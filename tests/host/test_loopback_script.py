"""Self-test for tools/loopback_test.py: it must FAIL when the status report
is missing, incomplete, or shows loss, and when a stop is overtaken.

    python tests/host/test_loopback_script.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
import loopback_test as lt  # noqa: E402

GOOD_ROWS = (
    ["[MC] status up=5s reset=X loop_max=0ms mc_lost=0 busy=0",
     "[MC] PC  P 921600 baud rx=1 lines=1 tx=1 q=0/256 overlong=0 binary=0 garbled=0 "
     "uart_err_lines=0 fe=0 ne=0 overruns=0 stalls=0"]
    + [f"[MC] DC{k} D online @1,@2 last_rx=1ms rx=1 lines=1 up_drop=0 tx=1 q=0/96 "
       f"rejected=0 purged={28 if k == 1 else 0} overlong=0 binary=0 fe=0 ne=0 "
       f"overruns=0 dma_restarts=0 stalls=0" for k in range(1, 7)]
)


class FakeLink:
    """Answers mc_status with the given rows; echoes nothing else."""
    def __init__(self, status_rows, stop_lines=None):
        self.rows, self.pending = status_rows, []
        self.stop_lines = stop_lines

    def send(self, line):
        if line == "mc_status":
            self.pending += self.rows
        elif line == "@1 stop" and self.stop_lines is not None:
            self.pending += self.stop_lines

    def collect(self, duration, want=None):
        got = [p for p in self.pending if not p.startswith("[MC]")]
        mc = [p for p in self.pending if p.startswith("[MC]")]
        self.pending = []
        return got, mc


def expect(name, result, want):
    ok = result == want
    print(f"{'ok  ' if ok else 'FAIL'} {name}: {'PASS' if result else 'FAIL'} (want {'PASS' if want else 'FAIL'})")
    return ok


def main():
    ok = True
    ok &= expect("complete clean report", lt.counters_test(FakeLink(GOOD_ROWS)), True)
    ok &= expect("no status at all", lt.counters_test(FakeLink([])), False)
    ok &= expect("DC6 row missing", lt.counters_test(FakeLink(GOOD_ROWS[:-1])), False)
    bad = [r.replace("overruns=0", "overruns=3") if r.startswith("[MC] DC3") else r for r in GOOD_ROWS]
    ok &= expect("DC3 overruns", lt.counters_test(FakeLink(bad)), False)
    bad = [r.replace("rejected=0", "rejected=1") if r.startswith("[MC] DC5") else r for r in GOOD_ROWS]
    ok &= expect("DC5 rejected", lt.counters_test(FakeLink(bad)), False)
    bad = [r.replace("garbled=0", "garbled=2") for r in GOOD_ROWS]
    ok &= expect("PC garbled", lt.counters_test(FakeLink(bad)), False)
    bad = [r.replace("mc_lost=0", "mc_lost=1") for r in GOOD_ROWS]
    ok &= expect("summary mc_lost", lt.counters_test(FakeLink(bad)), False)
    bad = [r.replace("purged=0", "purged=4") if r.startswith("[MC] DC2") else r for r in GOOD_ROWS]
    ok &= expect("DC2 purged (only DC1 allowed)", lt.counters_test(FakeLink(bad)), False)
    bad = [r.replace(" uart_err_lines=0", "") for r in GOOD_ROWS]
    ok &= expect("PC field missing", lt.counters_test(FakeLink(bad)), False)

    q = lambda i: f"[DC-1] @1 queued-{i}-" + "y" * 200  # noqa: E731
    good_stop = [q(0), q(1), "[DC-1] @1 stop",
                 f"[MC] W: '@1 stop' discarded {lt.QUEUED - 2} queued line(s) for DC1"]
    ok &= expect("stop overtakes", lt.stop_test(FakeLink([], good_stop)), True)
    late = [q(0), "[DC-1] @1 stop", q(1),
            f"[MC] W: '@1 stop' discarded {lt.QUEUED - 2} queued line(s) for DC1"]
    ok &= expect("queued line after stop", lt.stop_test(FakeLink([], late)), False)
    lost = [q(0), "[DC-1] @1 stop", "[MC] W: '@1 stop' discarded 3 queued line(s) for DC1"]
    ok &= expect("lines unaccounted for", lt.stop_test(FakeLink([], lost)), False)
    ok &= expect("stop missing", lt.stop_test(FakeLink([], [q(0)])), False)

    print("\nself-test", "PASSED" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
