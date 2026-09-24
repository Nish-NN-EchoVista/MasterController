#!/usr/bin/env python3
"""
MasterController loopback commissioning test.

Hardware: NO DataControllers connected. On every DataController port, put a
jumper from the MC's TX pin to its own RX pin:

    DC1 PE3->PE2   DC2 PF7->PF6   DC3 PA0->PA1
    DC4 PB13->PB12 DC5 PD15->PD14 DC6 PC6->PC7

Whatever the MC sends to DataController k then comes straight back as if
DataController k had printed it, tagged "[DC-k] ...". This exercises every
route, queue and DMA path at full speed without any other hardware.

    pip install pyserial
    python tools/loopback_test.py COM7            (laptop adapter port)
    python tools/loopback_test.py COM7 --rounds 200 --burst 40

Exit code 0 means every check passed.
"""
import argparse
import re
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")

NUM_DC = 6


class Link:
    def __init__(self, port, baud):
        # serial_for_url also accepts socket://host:port (tests/host/mc_sim).
        self.s = serial.serial_for_url(port, baudrate=baud, timeout=0.05)
        self.buf = b""

    def send(self, line):
        self.s.write(line.encode("ascii") + b"\r\n")

    def lines(self, duration):
        """Yield decoded lines received within `duration` seconds."""
        end = time.monotonic() + duration
        while time.monotonic() < end:
            self.buf += self.s.read(4096)
            while b"\r\n" in self.buf:
                raw, self.buf = self.buf.split(b"\r\n", 1)
                yield raw.decode("ascii", "replace")

    def collect(self, duration, want=None):
        """Collect non-[MC] lines; stop early once `want` lines have arrived."""
        got, mc = [], []
        for line in self.lines(duration):
            (mc if line.startswith("[MC]") else got).append(line)
            if want is not None and len(got) >= want:
                break
        return got, mc


def expect_set(name, got, expected):
    missing = sorted(set(expected) - set(got))
    extra = sorted(set(got) - set(expected))
    dup = len(got) - len(set(got))
    ok = not missing and not extra and dup == 0
    print(f"  {'PASS' if ok else 'FAIL'} {name}: {len(got)}/{len(expected)} lines"
          + (f", missing {missing[:3]}" if missing else "")
          + (f", unexpected {extra[:3]}" if extra else "")
          + (f", {dup} duplicated" if dup else ""))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--rounds", type=int, default=50)
    ap.add_argument("--burst", type=int, default=20,
                    help="lines per board sent back-to-back in the burst test")
    a = ap.parse_args()

    link = Link(a.port, a.baud)
    ok = True

    print("ping")
    link.send("mc_reset_stats")
    link.send("mc_ping")
    _, mc = link.collect(1.0)
    ok &= any("pong" in m for m in mc)
    print(f"  {'PASS' if ok else 'FAIL'} [MC] pong")

    print("addressing: every @n reaches only its DataController, as @1/@2")
    expected, got = [], []
    for r in range(a.rounds):
        this_round = []
        for n in range(1, 2 * NUM_DC + 1):
            tag = f"t{r}-{n}"
            link.send(f"@{n} {tag}")
            dc, board = (n + 1) // 2, 1 if n % 2 else 2
            this_round.append(f"[DC-{dc}] @{board} {tag}")
        g, _ = link.collect(2.0, want=len(this_round))
        got += g
        expected += this_round
    ok &= expect_set("addressed lines", got, expected)

    print("#k raw: reaches only DataController k, unchanged")
    for k in range(1, NUM_DC + 1):
        link.send(f"#{k} raw{k}")
    got, _ = link.collect(2.0, want=NUM_DC)
    ok &= expect_set("raw lines", got, [f"[DC-{k}] raw{k}" for k in range(1, NUM_DC + 1)])

    print("broadcast: each line reaches all six DataControllers exactly once")
    expected = []
    for r in range(a.rounds):
        link.send(f"bc{r}")
        expected += [f"[DC-{k}] bc{r}" for k in range(1, NUM_DC + 1)]
    got, _ = link.collect(5.0, want=len(expected))
    ok &= expect_set("broadcast lines", got, expected)

    print("burst: back-to-back lines to every board arrive whole and in order")
    expected = []
    for i in range(a.burst):
        for n in range(1, 2 * NUM_DC + 1):
            payload = f"burst-{n}-{i}-" + "x" * 60
            link.send(f"@{n} {payload}")
            expected.append(f"[DC-{(n + 1) // 2}] @{1 if n % 2 else 2} {payload}")
    got, mc = link.collect(10.0, want=len(expected))
    ok &= expect_set("burst lines", got, expected)
    for n in range(1, 2 * NUM_DC + 1):
        seq = [int(m.group(1)) for line in got
               if (m := re.search(rf" burst-{n}-(\d+)-", line))]
        if seq != sorted(seq):
            ok = False
            print(f"  FAIL burst order for @{n}")
    busy = [m for m in mc if "busy" in m]
    if busy:
        print(f"  note: {len(busy)} busy report(s) - increase pacing or reduce --burst")

    print("stop: overtakes and purges queued lines")
    for i in range(30):
        link.send(f"@1 queued-{i}-" + "y" * 200)
    link.send("@1 stop")
    got, mc = link.collect(3.0)
    stop_seen = "[DC-1] @1 stop" in got
    purge = [m for m in mc if "discarded" in m]
    ok &= stop_seen
    print(f"  {'PASS' if stop_seen else 'FAIL'} stop delivered; "
          f"{purge[0] if purge else 'no purge report (queue may have drained first)'}")

    print("counters")
    link.send("mc_status")
    _, mc = link.collect(1.5)
    for m in mc:
        print("   ", m)
    for m in mc:
        if re.match(r"\[MC\] (PC|DC\d)", m):
            bad = {k: int(v) for k, v in re.findall(r"\b(fe|ne|up_drop|stalls|dma_restarts)=(\d+)", m)
                   if int(v)}
            if bad:
                ok = False
                print(f"  FAIL nonzero error counters: {bad} in: {m[:40]}")

    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
