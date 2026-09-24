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
| Diagnostics (read-only) | USART3 | PD8 | PD9 | 115200 (ST-LINK USB VCP) |

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
- **Nothing is lost silently.** Every drop is counted in `mc_status` and
  reported as an `[MC]` line. `[MC]` replies have their own reserved PC
  queue space, so they get through even when the PC link is congested.
- **Nothing blocks.** Reception is DMA into ring buffers that never stop,
  even on framing or noise errors. All routing runs in the main loop. The
  watchdog resets the board if the main loop ever stalls for about 1 s, and
  the next boot reports why.
- **Binary protocol frames are not forwarded.** They are the
  DataController's experimental COBS protocol, delimited by `0x00`. v1 is
  text only. Frames from either direction are discarded and counted, and
  a `0x00` byte is never sent to a DataController. See
  [docs/binary-protocol.md](docs/binary-protocol.md) for the plan.

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
| `mc_help` | Command summary |
| `mc_ping` | `[MC] pong` (lets the GUI detect a MasterController) |
| `mc_version` | Firmware version and build time |
| `mc_status` | Uptime, reset cause, worst main-loop time, then one line per port: online/offline, time since last byte, bytes/lines, queue depth, drops, framing/noise errors, DMA restarts, TX stalls |
| `mc_reset_stats` | Zero all counters |

Unsolicited `[MC]` lines:
- A boot banner that includes the reset cause (`POWER_ON`, `WATCHDOG`,
  `HARDFAULT`, `ERROR_HANDLER`, `BROWNOUT`, …).
- `I: DCk online`, `W: DCk offline: silent for … ms` and
  `W: DCk not detected`. DataControllers print `PZT Temp` every 500 ms,
  so 3 s of silence means a DataController has gone.
- Rate-limited warnings about drops or rejected lines.

Every `[MC]` line is also mirrored to the **ST-LINK virtual COM port**
(USB, 115200). You can watch it in a terminal without disturbing the GUI
link.

## LEDs and button

| | Meaning |
|---|---|
| Green LD1 | 1 Hz heartbeat. The main loop is running. |
| Yellow LD2 | Flickers with routed traffic |
| Red LD3 | A DataController is offline, or something was dropped in the last 2 s |
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
breakpoints don't cause resets.

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

## Commissioning checklist

1. Flash, then open the ST-LINK VCP at 115200. You should see
   `[MC] MasterController 1.0.0 ready (reset: …)`.
2. Open the laptop adapter at **921600** and send `mc_ping`. Expect
   `[MC] pong`.
3. Connect DataControllers one at a time. Each should produce
   `[MC] I: DCk online`, followed by `[DC-k] PZT Temp: …` lines every
   500 ms.
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
