# MasterController

Firmware for a **NUCLEO-H7A3ZI-Q** (STM32H7A3ZIT6Q) that lets one laptop UART
control **12 EVS2 boards** behind **6 F401 DataControllers**.

```
                          ┌──────────── DC1 ── EVS2 @1,  @2
                          ├──────────── DC2 ── EVS2 @3,  @4
 Laptop ── USB-UART ── MasterController ─── DC3 ── EVS2 @5,  @6
          921600 8N1      ├──────────── DC4 ── EVS2 @7,  @8
                          ├──────────── DC5 ── EVS2 @9,  @10
                          └──────────── DC6 ── EVS2 @11, @12
                              57600 8N1 each
```

It is a pure pass-through router. It does not take part in pump, defog or
sequence logic. Each DataController still runs its current firmware
unchanged and sees exactly the traffic it would get from a laptop today.

## Wiring

| Link | UART | MC TX | MC RX | Baud |
|---|---|---|---|---|
| Laptop (external USB-UART, 3.3 V) | USART2 | PA2 | PA3 | 921600 |
| DC1 → EVS2 @1, @2 | USART10 | PE3 | PE2 | 57600 |
| DC2 → EVS2 @3, @4 | UART7 | PF7 | PF6 | 57600 |
| DC3 → EVS2 @5, @6 | UART4 | PA0 | PA1 | 57600 |
| DC4 → EVS2 @7, @8 | UART5 | PB13 | PB12 | 57600 |
| DC5 → EVS2 @9, @10 | UART9 | PD15 | PD14 | 57600 |
| DC6 → EVS2 @11, @12 | USART6 | PC6 | PC7 | 57600 |
| Diagnostics + commands | USART3 | PD8 | PD9 | 115200 (ST-LINK USB VCP, no wiring) |

**Soldering to the morpho through-holes (CN11/CN12)?** Open
[docs/Morpho_wiring_guide.html](docs/Morpho_wiring_guide.html). It shows
the exact holes for every link, taken from ST UM2408 Table 19, with a
top/solder-side toggle and a continuity checklist.
- Arduino labels are not the MCU pin names: **A0 is PA3, not PA0**.
- **DC4's TX (PB13) is not on the morpho rows on a stock board.** CN12-30
  carries PE8 by default. Take PB13 from Zio **CN7 pin 5 (D18)**.

For each DataController, the MC's **TX** pin goes to the F401's USART1 **RX
(PA10)**, and the F401's USART1 **TX (PA9)** goes to the MC's **RX** pin.
Join the grounds. Every RX pin has an internal pull-up, so an unplugged
DataController reads as idle rather than noise.

Pins deliberately left alone on this board:
- PD8/PD9 (ST-LINK VCP)
- PA9–PA12 (USB OTG)
- PB0, PE1, PB14 (LEDs)
- PC13 (button)
- PA13, PA14, PB3 (SWD/SWO)
- PH0/PH1 and PC14/PC15 (clock pins)

## Addressing (laptop → boards)

| You send | Goes to | As |
|---|---|---|
| `@n <cmd>` (n = 1…12) | EVS2 *n*, i.e. DataController ⌈n/2⌉ | `@1 <cmd>` for odd n, `@2 <cmd>` for even n |
| `#k <cmd>` (k = 1…6) | DataController *k* only | `<cmd>` unchanged (e.g. `#3 enable_pump`, `#2 lockout++`) |
| `<cmd>` | all six DataControllers | `<cmd>` unchanged (each DC passes it to both boards, as today) |
| `mc_…` | the MasterController itself | see below |

`@0 …` lines (e.g. `@0 play_all`) are DataController pump commands, so
they are broadcast to all six DataControllers unchanged.

The MC rejects malformed addresses with an `[MC] E:` line and sends
nothing. This covers `@13`, `@01`, `@1` with no command, `#7`, and similar.

### Running with fewer than six DataControllers

Any number of DataControllers from 0 to 6 can be connected, on any ports,
and plugged or unplugged while running. Addresses are fixed by port:
DataController *k* always owns `@(2k−1)`/`@2k`, however many others are
connected. Each port is in one of three states:

| State | Meaning | What you get |
|---|---|---|
| **live** | Traffic in the last 3 s (every F401 prints `PZT Temp` every 500 ms) | Normal routing |
| **absent** | No traffic at all since boot: nothing plugged in | Quiet: no warning and no red LED. Running with fewer than six is normal. |
| **lost** | Was live, then went silent: a cable or DataController failed | `[MC] W: DC3 (port 3, …) lost: silent for … ms`, and the red LED lights |

A DataController that reappears is reported as `I: DCk online`, or
`I: DCk back online` if it had been lost.

- **Addressed commands** (`@n`, `#k`) to an absent or lost DataController
  are **refused with a reply** instead of vanishing:
  - `[MC] E: DC6 (port 6) not connected, not sent: @11 start_sweep`
  - `[MC] E: DC3 (port 3) lost, silent for 5 s, not sent: @5 get volt`

  Each refused command gets its own reply. Only a flood of more than 10
  per second is summarised.
- **Stops always go out**, even to absent or lost ports. A silent
  DataController may still be running, and a stop must never be held back
  on a guess. To a lost one they go out with a warning.
- **Broadcasts go to every port**, connected or not, with no reply about
  absent ones. If a lost DataController exists, the broadcast still goes to
  it and the laptop is told:
  `[MC] W: broadcast also sent to lost DataController(s) DC3: …`.
- **`dc_status`** is answered by the MasterController itself, never
  forwarded. It lists every DataController with its port, UART and
  addresses:

  ```
  [MC] dc_status: 4 live, 1 absent, 1 lost
  [MC]   live   DC1 (port 1, USART10 PE3/PE2, @1,@2) last_rx=212ms
  [MC]   live   DC2 (port 2, UART7 PF7/PF6, @3,@4) last_rx=180ms
  [MC]   live   DC4 (port 4, UART5 PB13/PB12, @7,@8) last_rx=95ms
  [MC]   live   DC5 (port 5, UART9 PD15/PD14, @9,@10) last_rx=402ms
  [MC]   absent DC6 (port 6, USART6 PC6/PC7, @11,@12)
  [MC]   lost   DC3 (port 3, UART4 PA0/PA1, @5,@6) silent for 42 s
  ```

  A DataController whose port is not healthy (see
  [Self-healing ports](#self-healing-ports)) also shows that state, for
  example `[FAULTED: tx_stalls]`.

Lines may end in CR, LF or CRLF. Empty lines are ignored. Everything sent
downstream ends in CRLF.

A laptop line containing non-printable bytes (anything outside ASCII
0x20–0x7E, except tab) is rejected rather than forwarded. Such bytes come
from connect glitches or baud mismatches; tab is allowed.

## Tagging (boards → laptop)

Output from DataController *k* is renumbered so the laptop sees one
12-board system:

| DataController *k* sends | Laptop receives |
|---|---|
| `[ESV2-1] …` / `[ESV2-2] …` | `[ESV2-(2k−1)] …` / `[ESV2-2k] …` |
| `TX to ESV2-1: …` / `TX to ESV2-2: …` | `TX to ESV2-(2k−1): …` / `TX to ESV2-2k: …` |
| `Board1 Temp: …` / `Board2 Temp: …` | `Board(2k−1) Temp: …` / `Board2k Temp: …` |
| anything else (`PZT Temp: …`, `I: …`, `[ESV2-0] …`) | `[DC-k] …` |

Lines produced by the MasterController itself start with `[MC] `
(`I:` info, `W:` warning, `E:` error).

## Behaviour guarantees

- **Whole lines only.** Lines are queued, sent and dropped as complete
  units. Lines from six DataControllers never interleave mid-line.
- **All-or-nothing broadcast.** A broadcast is only accepted if all six
  DataController queues can take it. Otherwise nobody gets it and the
  laptop receives `[MC] E: busy (DCk queue full), not sent: …`. Because
  all six ports send at once, the DataControllers receive it within
  microseconds of each other.
- **Stop always wins.** `stop`, `cancel` and `@0 stop_all` (addressed or
  broadcast) go out as soon as the line currently on the wire finishes.
  - Any queued, unsent lines for the same boards are discarded first, so
    nothing queued earlier can restart them. The laptop is told how many:
    `[MC] W: 'stop' discarded 3 queued line(s) for DC2`.
  - Queue space is reserved, so a stop still fits when the queue is
    otherwise full.
  - Urgent lines are never discarded by a later stop. Several stops in a
    row go out in the order they were sent.
  - Discarding is per DataController. For example, `@1 stop` also discards
    a queued broadcast on DC1, even if that broadcast has already reached
    the other five DataControllers. The discard is always reported.
- **Damaged input is never forwarded.** If the laptop link reports a
  framing or noise error, the UART has dropped a byte. That byte might
  have been a line ending, which merges two commands, or a letter, which
  could turn `stop` into something else. The error flag doesn't say where
  the byte was, so the MC plays it safe:
  - It discards every line holding a byte received before the error was
    seen, however much input was buffered at the time.
  - It also discards a line that begins right at that point within 5 ms,
    since the lost byte may have been its first character.
  - A command typed afterwards is unaffected.
  - Discards are reported as `[MC] E: line received with UART errors, not
    sent: …` (rate-limited) and counted in `uart_err_lines=`.
  - Errors on DataController links are counted and warned about, at most
    once per second per port.
- **Lost input is never glued together.** If a receive DMA restarts or a
  ring overflows, input is lost at an unknown point. The partial line is
  dropped, and everything up to the next line ending is discarded, so the
  tail of a line can never be forwarded as a command. This applies on
  every port.
- **Nothing is lost silently.** Every drop is counted in `mc_status` and
  reported as an `[MC]` line. `[MC]` replies have their own reserved PC
  queue space, so they get through even when the PC link is congested.
- **Nothing blocks.** Reception is DMA into ring buffers that never stop,
  even on framing or noise errors.
  - A stopped DMA stream is restarted, including re-initialising its
    handle if needed. If that keeps happening, or fails, the port
    supervisor takes over (see [Self-healing ports](#self-healing-ports)).
  - All routing runs in the main loop. The watchdog resets the board if the
    main loop ever stalls for about 1 s, and the next boot reports why.
  - Each DataController ring holds more than the watchdog timeout of input.
  - The laptop ring holds about 0.5 s. A longer stall is reported, and the
    affected partial line is discarded rather than joined to the next one.
  - If a transmission ever stalls, it is aborted and followed by CRLF, so
    a partial line cannot merge into the next command.
- **Binary protocol frames are not forwarded.** They are the
  DataController's experimental COBS protocol, delimited by `0x00`. v1 is
  text only. Frames from either direction are discarded and counted, and
  a `0x00` byte is never sent to a DataController. See
  [docs/binary-protocol.md](docs/binary-protocol.md) for the plan.

### Self-healing ports

Each UART port (the laptop link and all six DataController links) has a
health supervisor. A **hard fault** means the port stopped carrying
traffic:
- 3 transmit stalls within 10 s;
- the driver refusing to transmit for 50 ms;
- 3 receive-DMA restarts within 10 s, or one restart that fails.

Recovery climbs a ladder, one step at a time:

| State | How it gets there | What happens |
|---|---|---|
| **OK** | Normal operation | — |
| **PROBATION** | A hard fault on a healthy port | The UART is **rebuilt from scratch**: de-initialised, its RCC reset pulsed, then re-initialised with its original settings and reception restarted. It then has 3 s to prove itself by sending a line (an empty line is sent if nothing is queued). |
| **OK again** | Probation passes | `[MC] I: DCk recovered (…)` |
| **FAULTED** | Any hard fault during probation, or nothing sent | The port is **quarantined** (see below). The laptop is told which boards are unavailable. |
| **PROBATION** | Probe timer fires on a faulted port | The port is rebuilt and tried again. The retry delay doubles each time: 5 s, 10 s, 20 s … up to 60 s. |
| **DEGRADED** | 100+ line errors in 1 s | Reported, and the port is rebuilt at most once a minute. It is **never quarantined**, because a noisy port still carries traffic. It goes back to OK when the errors stop. |

While a DataController port is quarantined:
- The MC stops driving it.
- Its unsent ordinary lines are discarded and reported.
- Commands addressed to its boards are refused:
  `[MC] E: DC3 is faulted (tx_stalls), not sent: …`.
- **Broadcasts still go to the healthy DataControllers**, and the laptop is
  told which were skipped:
  `[MC] W: sent to 5 of 6 DataControllers (faulted: DC3): start_defog`.
  All-or-nothing still applies across the healthy ones.
- Stops sent to it stay queued. They go out first, ahead of anything else,
  if it recovers.

A few more rules:
- **The laptop link is never quarantined**, since it is the only way in.
  It is rebuilt and retried the same way, but keeps routing.
- **Lines cut by a rebuild are not forwarded.** Bytes arriving right after
  a rebuild may be the tail of a line that was cut off, so they are
  discarded up to the next line ending. A line that starts after a quiet
  period (one full-length line time: about 6 ms on the laptop link, 89 ms
  on a DC link) is kept.
- **Three or more DataController ports faulted at once** points at the
  board itself, such as its clock or power. The MC resets once, and the
  next boot reports `reset: PORT_FAULTS`. If the same thing happens again
  before a power cycle, it stays up and keeps probing instead of looping
  through resets.
- **Manual recovery:** `mc_recover <1..6|pc|all>` rebuilds a port on
  demand, or makes a faulted port probe immediately.

### Capacity

| Queue | Slots (whole lines) | Notes |
|---|---|---|
| Laptop → each DC | 96 | Minus 4 reserved for stop/cancel. Roughly 50 KB of upload frames can be buffered per DC. |
| All DCs → laptop | 256 | Minus 4 reserved for `[MC]` replies. |
| Line length | 512 characters | Longer lines are discarded whole, never truncated. |

All six DataControllers together can deliver at most about 6 × 5.8 KB/s.
The 921600 laptop link carries about 92 KB/s, so upstream congestion should
never happen in practice.

## MasterController commands

| Command | Reply |
|---|---|
| `dc_status` | Which DataControllers are live, absent and lost, with port numbers (see above) |
| `mc_help` | Command summary |
| `mc_ping` | `[MC] pong` (lets the GUI detect a MasterController) |
| `mc_version` | Firmware version and build time |
| `mc_status` | Uptime, reset cause, worst main-loop time, then one line per port: live/absent/lost, time since last byte, bytes/lines, queue depth, drops, framing/noise errors, DMA restarts, TX stalls, `health=` state (with the fault reason), recoveries and quarantines |
| `mc_reset_stats` | Zero all counters |
| `mc_recover <1..6\|pc\|all>` | Rebuild a port now, or probe a faulted port now |
| `mc_pace [1..6] <0..100>` | Quiet gap (ms) after each line sent to a DataController: all six, or one. `mc_pace` alone lists them. |

### Pacing towards the DataControllers

By default the MC sends queued lines to a DataController back to back, at
the full 57600 line rate. That is the heaviest load an F401 sees: it
handles one line per main-loop pass and adds its own `TX to ESV2-n` echo
and `[ESV2-n]` prefixes towards the MC.

`mc_pace` adds a quiet gap after each line, per DataController.
- **Stops are never delayed.** `stop`, `cancel`, `@0 stop_all` and the CRLF
  that ends an aborted line never wait for the gap.
- **The gap is shown per port** as `pace=` in `mc_status`.
- **The default is 0.** It is set in RAM only, so it resets at boot. The
  compile-time default is `MC_DC_LINE_GAP_MS`.
- **Measured on the bench:** 20 ms gives 19.5 ms per line and 50 ms gives
  50.0 ms per line, with short lines.
- **Choosing a value:** tune it during end-to-end tests with real F401s by
  watching their drop and bad-frame counters.

Unsolicited `[MC]` lines:
- A boot banner that includes the reset cause (`POWER_ON`, `WATCHDOG`,
  `HARDFAULT`, `ERROR_HANDLER`, `BROWNOUT`, …).
- `I: DCk … online`, `I: DCk … back online` and `W: DCk … lost: silent
  for … ms`.
- One `I: DataControllers live: …; absent (not connected): …` summary,
  3 s after boot.
- Rate-limited warnings about drops or rejected lines.

Every `[MC]` line is also mirrored to the **ST-LINK virtual COM port**
(USB, 115200). You can watch it in a terminal without disturbing the GUI
link.

The ST-LINK port also **accepts commands**, exactly like the laptop link.
Any line typed there is routed the same way, for example `dc_status`,
`#3 test` or `@5 get volt`. It is a backup way in, and it needs no wiring:
just the Nucleo's USB cable. Replies from the DataControllers still go to
the laptop link; the ST-LINK port gets only the `[MC]` lines.

Like every other port, it receives through circular DMA (DMA2 Stream0),
never one byte per interrupt. CubeMX uses DMA1 only; if DMA2 Stream0 is
ever assigned in the `.ioc`, move the VCP stream in `mc_platform.c`.

## LEDs and button

| | Meaning |
|---|---|
| Green LD1 | 1 Hz heartbeat. The main loop is running. |
| Yellow LD2 | Flickers with routed traffic |
| Red LD3 | A DataController is **lost** (absent ones don't count), any port is not healthy, or something was dropped in the last 2 s |
| Blue button B1 | Prints a full `mc_status` report |

## Building

This is a normal STM32CubeIDE project, generated with STM32CubeMX 6.17.0
and STM32Cube H7 firmware package V1.13.0. Import this directory as an
existing project and build **Debug** or **Release**. Both build with 0
errors and 0 warnings.

To build without the IDE:

```bash
stm32cubeidec.exe --launcher.suppressErrors -nosplash -application org.eclipse.cdt.managedbuilder.core.headlessbuild -data <workspace> -import <this folder> -build MasterController/Debug
```

In Debug builds the watchdog is frozen while the core is halted, so
breakpoints don't cause resets. **Flash the Release build for deployment.**

A fault always resets the board, whether or not a debugger was attached.
To inspect a fault, set a breakpoint on `mc_board_fatal`.

### Host unit tests

The routing core (`mc_txq`, `mc_line`, `mc_router`, `mc_app`) has no HAL
dependency. It is tested on the PC against a fake platform:

```bash
make -C tests/host run
```

Any C11 gcc works (MinGW on Windows). The `make.exe` bundled with
STM32CubeIDE works too.

## Code layout

| File | Role |
|---|---|
| `Core/Inc/mc_config.h` | Every tunable: sizes, timeouts, baud rates |
| `Core/Src/mc_router.c` | Pure rules: address parsing, stop detection, retagging |
| `Core/Src/mc_line.c` | Bounded line assembler; drops overlong lines and binary frames |
| `Core/Src/mc_txq.c` | Whole-line TX queue with urgent insertion and tag-based purge |
| `Core/Src/mc_app.c` | Application: routing, admission, health, `mc_*` commands |
| `Core/Src/mc_platform.c` | STM32 layer: DMA RX, IT TX, error counting, reset cause, LEDs, watchdog |
| `Core/Src/main.c` | CubeMX-generated; calls `mc_board_init()` / `mc_board_poll()` from USER CODE blocks |

You can regenerate the project from `MasterController.ioc` in CubeMX.
The only edits to generated files are inside `USER CODE` blocks:
- `main.c`: init, poll, `Error_Handler`
- `stm32h7xx_it.c`: fault handlers

If you ever enable USART3 interrupts or TX DMA in CubeMX, remove the
matching hand-written handler in `mc_platform.c`. Otherwise the link fails
with a duplicate symbol.

### Settings in the .ioc that the code relies on or overrides

- **DMA disable on RX error is enabled in the .ioc.** `mc_platform.c`
  clears it at runtime (`CR3.DDRE`), so a framing error cannot stop
  reception. Setting it to *Disable* in CubeMX would make the two agree.
- **Clock:** the system clock is 280 MHz from **HSI** via PLL1. This is
  well within UART tolerance at room temperature. For the tightest baud
  accuracy over temperature, switch PLL1 to **HSE bypass**, which is the
  8 MHz clock from the ST-LINK, in CubeMX.
- **Caches:** only the instruction cache is on. The data cache stays off
  so DMA buffers need no cache maintenance.
- **Unused peripherals:** TIM1 and the RTC are initialised but unused. The
  RTC's backup domain holds the fault reason across resets.

## Loopback test (no DataControllers needed)

1. Put a jumper from TX to RX on every DataController port:
   PE3→PE2, PF7→PF6, PA0→PA1, PB13→PB12, PD15→PD14, PC6→PC7.
2. Run:

   ```bash
   python tools/loopback_test.py COM7
   ```

   Replace COM7 with the laptop adapter's port. The script needs pyserial.

It checks:
- Every `@1`…`@12` and `#1`…`#6` route.
- Broadcast lines arrive at all six DataControllers exactly once.
- Back-to-back bursts to all 12 boards arrive whole and in order.
- A stop overtakes and discards queued lines: every queued line arrives before it or is reported discarded, and none arrives after it.
- The full status report (summary plus all seven ports) is present, and every loss and error counter is zero.

The same script runs against the **host simulator**, which is the real
routing core with emulated wire speeds and looped-back DC ports:

```bash
make -C tests/host mc_sim
```

```bash
tests/host/mc_sim 5555
```

```bash
python tools/loopback_test.py socket://localhost:5555
```

## Commissioning checklist

1. Flash, then open the ST-LINK VCP at 115200. You should see
   `[MC] MasterController 1.0.0 ready (reset: …)`.
2. Open the laptop adapter at **921600** and send `mc_ping`. Expect
   `[MC] pong`.
3. Connect DataControllers one at a time. Each should produce
   `[MC] I: DCk (port k, …) online`, followed by `[DC-k] PZT Temp: …`
   lines every 500 ms. `dc_status` should list it as live.
4. Run `mc_status`. For every connected port, `fe=` and `ne=` should stay
   at 0. Rising counts point to wiring, grounding or baud problems.
5. Send `@1 <harmless get command>` and check it reaches only DC1 board 1.
   Do the same for `@2`, `@12`, `#3 …` and a broadcast.
6. Scope a broadcast on two DataController TX lines to confirm they start
   together.
7. Queue a few commands, send `stop`, and confirm the discard report and
   that the stop reaches every board.

## PyGUI changes needed to use all 12 boards

- Set `BAUD_DEFAULT` to 921600.
- PyGUI currently only recognises `[ESV2-1]`/`[ESV2-2]` and addresses
  `@1`/`@2`. It needs to handle `[ESV2-1]`…`[ESV2-12]`, `@1`…`@12`,
  `BoardN Temp`, and the `[DC-k] PZT Temp` lines (one thermocouple per
  DataController).
- Lines starting `[MC] ` can be shown in a log or ignored.
