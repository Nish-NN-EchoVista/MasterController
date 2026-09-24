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
        ok = False
        print(f"  FAIL {len(busy)} busy report(s): lines were refused (reduce --burst)")

    ok &= stop_test(link)
    ok &= counters_test(link)

    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


QUEUED = 30


def stop_test(link):
    """A stop must overtake the queue: every queued line either arrives
    before the stop or is discarded (and reported), none arrives after it,
    and the two together account for every line sent."""
    print("stop: overtakes and purges queued lines")
    for i in range(QUEUED):
        link.send(f"@1 queued-{i}-" + "y" * 200)
    link.send("@1 stop")
    got, mc = link.collect(4.0)
    ok = True
    if "[DC-1] @1 stop" not in got:
        print("  FAIL stop not delivered")
        return False
    at = got.index("[DC-1] @1 stop")
    before = [g for g in got[:at] if " queued-" in g]
    after = [g for g in got[at + 1:] if " queued-" in g]
    purged = sum(int(m.group(1)) for line in mc
                 if (m := re.search(r"discarded (\d+) queued line\(s\) for DC1", line)))
    if after:
        ok = False
        print(f"  FAIL {len(after)} queued line(s) arrived after the stop, e.g. {after[0][:40]}")
    if len(before) + purged != QUEUED:
        ok = False
        print(f"  FAIL {len(before)} delivered + {purged} discarded != {QUEUED} queued")
    if ok:
        print(f"  PASS stop delivered after {len(before)} line(s); {purged} discarded; none after it")
    return ok


# Every field each status row must carry, and the ones that must be zero.
SUMMARY_ZERO = ("mc_lost", "busy")
PC_FIELDS = ("rx", "lines", "tx", "overlong", "binary", "garbled",
             "uart_err_lines", "fe", "ne", "overruns", "stalls")
PC_ZERO = ("overlong", "binary", "garbled", "uart_err_lines", "fe", "ne",
           "overruns", "stalls")
DC_FIELDS = ("rx", "lines", "up_drop", "tx", "rejected", "purged", "overlong",
             "binary", "fe", "ne", "overruns", "dma_restarts", "stalls")
DC_ZERO = ("up_drop", "rejected", "overlong", "binary", "fe", "ne",
           "overruns", "dma_restarts", "stalls")


def fields(line):
    return {k: int(v) for k, v in re.findall(r"\b([a-z_]+)=(\d+)\b", line)}


def counters_test(link):
    """Require the complete status report and fail on any loss or error."""
    print("counters")
    link.send("mc_status")
    _, mc = link.collect(2.0)
    rows = {}
    for m in mc:
        if m.startswith("[MC] status "):
            rows["summary"] = m
        elif (r := re.match(r"\[MC\] (PC|DC[1-6]) ", m)):
            rows[r.group(1)] = m
    for m in rows.values():
        print("   ", m)

    ok = True
    expected = ["summary", "PC"] + [f"DC{k}" for k in range(1, NUM_DC + 1)]
    missing = [r for r in expected if r not in rows]
    if missing:
        print(f"  FAIL status report incomplete, missing: {missing}")
        return False

    def check(row, need, zero, allow=()):
        nonlocal ok
        f = fields(rows[row])
        absent = [k for k in need if k not in f]
        if absent:
            ok = False
            print(f"  FAIL {row}: missing field(s) {absent}")
        bad = {k: f[k] for k in zero if f.get(k, 0) and k not in allow}
        if bad:
            ok = False
            print(f"  FAIL {row}: nonzero {bad}")

    check("summary", SUMMARY_ZERO, SUMMARY_ZERO)
    check("PC", PC_FIELDS, PC_ZERO)
    for k in range(1, NUM_DC + 1):
        # purged is expected on DC1 only, from the stop test.
        check(f"DC{k}", DC_FIELDS, DC_ZERO + (() if k == 1 else ("purged",)))
    if ok:
        print("  PASS complete report, no loss or error counters")
    return ok


if __name__ == "__main__":
    sys.exit(main())
