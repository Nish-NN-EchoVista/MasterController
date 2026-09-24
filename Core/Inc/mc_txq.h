/*
 * mc_txq.h - Whole-line transmit queue.
 *
 * Every entry is one complete line (terminator included). A transmission
 * always covers exactly one entry, so a receiver can never see half of one
 * line joined to another, whatever is purged or reordered around it.
 *
 * The entry at the head may be "locked" while the UART is sending it. A
 * locked head is never purged or displaced; urgent entries go directly
 * behind it.
 *
 * Main-loop only: interrupts never touch a queue (the UART ISR only clears
 * a busy flag), so no locking is needed.
 */
#ifndef MC_TXQ_H
#define MC_TXQ_H

#include <stdint.h>
#include <stddef.h>
#include "mc_config.h"

typedef struct {
    uint16_t len;
    uint8_t  tag;                  /* caller-defined; used by purge masks   */
    uint8_t  data[MC_SLOT_BYTES];
} mc_slot_t;

typedef struct {
    mc_slot_t *slots;              /* storage pool, cap entries             */
    uint16_t  *order;              /* ring of slot indices, cap entries     */
    uint16_t  *free_stack;         /* unused slot indices, cap entries      */
    uint16_t   cap;
    uint16_t   head;               /* index into order[]                    */
    uint16_t   count;
    uint16_t   free_count;
    uint8_t    head_locked;
} mc_txq_t;

void     mc_txq_init(mc_txq_t *q, mc_slot_t *slots, uint16_t *order,
                     uint16_t *free_stack, uint16_t cap);
uint16_t mc_txq_count(const mc_txq_t *q);
uint16_t mc_txq_free(const mc_txq_t *q);

/* Append (urgent = 0) or insert as the next entry to send (urgent = 1).
   Returns 1 on success, 0 if the queue is full or len is out of range.
   Entries are copied; the caller's buffer may be reused immediately.       */
int      mc_txq_push(mc_txq_t *q, const uint8_t *data, uint16_t len,
                     uint8_t tag, int urgent);

/* Head entry, or NULL if empty. */
const mc_slot_t *mc_txq_head(const mc_txq_t *q);
/* Mark the head as in transmission. No-op if empty. */
void     mc_txq_lock_head(mc_txq_t *q);
int      mc_txq_head_locked(const mc_txq_t *q);
/* Release the (locked or not) head after it has been sent. */
void     mc_txq_pop(mc_txq_t *q);

/* Remove every queued entry whose tag shares a bit with mask, except a
   locked head. Order of the survivors is preserved. Returns the number
   removed.                                                                   */
uint16_t mc_txq_purge(mc_txq_t *q, uint8_t mask);

#endif /* MC_TXQ_H */
