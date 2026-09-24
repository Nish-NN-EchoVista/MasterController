/*
 * mc_txq.c - Whole-line transmit queue. See mc_txq.h.
 */
#include "mc_txq.h"
#include <string.h>

void mc_txq_init(mc_txq_t *q, mc_slot_t *slots, uint16_t *order,
                 uint16_t *free_stack, uint16_t cap)
{
    q->slots = slots;
    q->order = order;
    q->free_stack = free_stack;
    q->cap = cap;
    q->head = 0;
    q->count = 0;
    q->head_locked = 0;
    q->free_count = cap;
    for (uint16_t i = 0; i < cap; ++i)
        free_stack[i] = (uint16_t)(cap - 1u - i);
}

uint16_t mc_txq_count(const mc_txq_t *q) { return q->count; }
uint16_t mc_txq_free(const mc_txq_t *q)  { return q->free_count; }

static uint16_t ring_at(const mc_txq_t *q, uint16_t pos)
{
    return (uint16_t)((q->head + pos) % q->cap);
}

int mc_txq_push(mc_txq_t *q, const uint8_t *data, uint16_t len,
                uint8_t tag, int urgent)
{
    if (!q->free_count || len == 0u || len > MC_SLOT_BYTES)
        return 0;

    uint16_t s = q->free_stack[--q->free_count];
    mc_slot_t *slot = &q->slots[s];
    memcpy(slot->data, data, len);
    slot->len = len;
    slot->tag = tag;

    if (!urgent || q->count == 0u) {
        q->order[ring_at(q, q->count)] = s;
    } else if (!q->head_locked) {
        /* Becomes the new head. */
        q->head = (uint16_t)((q->head + q->cap - 1u) % q->cap);
        q->order[q->head] = s;
    } else {
        /* Slide the locked head back one position and slot in behind it. */
        uint16_t locked = q->order[q->head];
        q->head = (uint16_t)((q->head + q->cap - 1u) % q->cap);
        q->order[q->head] = locked;
        q->order[ring_at(q, 1u)] = s;
    }
    ++q->count;
    return 1;
}

const mc_slot_t *mc_txq_head(const mc_txq_t *q)
{
    return q->count ? &q->slots[q->order[q->head]] : NULL;
}

void mc_txq_lock_head(mc_txq_t *q)
{
    if (q->count)
        q->head_locked = 1;
}

int mc_txq_head_locked(const mc_txq_t *q) { return q->head_locked; }

void mc_txq_pop(mc_txq_t *q)
{
    if (!q->count)
        return;
    q->free_stack[q->free_count++] = q->order[q->head];
    q->head = (uint16_t)((q->head + 1u) % q->cap);
    --q->count;
    q->head_locked = 0;
}

uint16_t mc_txq_purge(mc_txq_t *q, uint8_t mask)
{
    uint16_t kept = 0, removed = 0;
    for (uint16_t pos = 0; pos < q->count; ++pos) {
        uint16_t s = q->order[ring_at(q, pos)];
        int protect = (pos == 0u && q->head_locked);
        if (!protect && (q->slots[s].tag & mask)) {
            q->free_stack[q->free_count++] = s;
            ++removed;
        } else {
            /* kept <= pos, so this never overwrites an unread entry. */
            q->order[ring_at(q, kept)] = s;
            ++kept;
        }
    }
    q->count = kept;
    return removed;
}
