# Binary protocol through the MasterController (not in v1)

## Why v1 is text only

The F401 DataController has an experimental binary protocol, developed on
the `rebase_F401_hex` branch:
- COBS framing with `0x00` delimiters, CRC-32, and a 64-byte payload limit.
- Every frame carries source and destination node IDs:
  - `EV_NODE_GUI = 0`
  - `EV_NODE_CONTROLLER = 1`
  - `EV_NODE_BOARD1 = 2`
  - `EV_NODE_BOARD2 = 3`

Behind a MasterController there are six DataControllers, and every one of
them uses node IDs 1–3. Forwarding their frames unchanged would give the
GUI six indistinguishable "controller 1"s and "board 2"s.

v1 therefore:
- **drops** every binary frame arriving from a DataController, and counts
  it (`binary=` in `mc_status`);
- **drops** every binary frame arriving from the laptop and replies with
  `[MC] E: binary protocol frames are not supported …`;
- **never sends a `0x00` byte** to a DataController. On the F401, a `0x00`
  would open protocol frame collection.

## Proposed design: node renumbering at the MasterController

1. **Give the MC its own endpoint.** It implements the shared codec
   (`protocol/common`, the same C sources the F401 vendors). It becomes
   node 1 on the laptop side and answers HELLO / CAPABILITIES / HEARTBEAT
   itself.
2. **Use a global node map on the laptop side:**

   | Global node | Meaning |
   |---|---|
   | 0 | GUI |
   | 1 | MasterController |
   | 16 + k | DataController *k* (k = 1…6) |
   | 32 + n | EVS2 *n* (n = 1…12) |

3. **Route downstream (laptop → DC).** Decode and CRC-check each frame.
   Map the destination to (DataController *k*, local node 1/2/3). Rewrite
   the destination, re-encode with a new CRC, and queue it as one whole
   entry on DC *k*.
   - The source stays 0 (GUI).
   - Session and transaction IDs pass through unchanged, so end-to-end
     ACK correlation and retries still work.
4. **Route upstream (DC → laptop).** Decode the frame and check that the
   source is plausible for that port. Map the local source to the global
   node, re-encode, and queue it whole on the PC link.
5. **Keep queue behaviour.** Frames share the whole-entry queues with text
   lines, so a frame can never split a text line or the other way round.
   Log frames keep the F401's control-reserve rule: diagnostics may not
   use the reserve kept for control and heartbeat frames.
6. **Never acknowledge on anyone's behalf.** As on the F401, the MC never
   ACKs on behalf of a destination. Only the addressed node confirms
   acceptance.

PyGUI would need a target selector covering the global node map. That is
the only GUI change the binary path needs.

## Work needed

- Vendor `protocol/common` into this repo via `protocol/sync.py` (add a
  third firmware root).
- Add an `mc_bin.c` module: decoder per port, node-map rewrite, encoder.
- Change `mc_line` to hand complete frames to it instead of discarding
  them.
- Add host tests that reuse the F401's round-trip fixtures.
