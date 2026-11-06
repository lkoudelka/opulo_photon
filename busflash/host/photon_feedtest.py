#!/usr/bin/env python3
"""Measure how a Photon feeder moves: duration of each feed, result, retries of the firmware.

    photon_feedtest.py --addr 50 [--mm 4] [--count 3] [--back] [--restart] [--log FILE]

--restart first restarts the feeder application through its bus loader (enter + RESET, nothing
is erased or written), so the feeds start from the power-on state (e.g. beefy mode off).

MOVES THE TAPE of that one feeder (--count feeds of --mm forward; --back also feeds back the
same distance afterwards).  A feed command is sent exactly once (never retried), so a lost
reply can never cause a second feed.  While feeding, the firmware blocks and does not answer;
the time from the feed command's reply to the first MOVE_FEED_STATUS answer is the motion
duration (resolution about one bus round trip, ~60 ms).  OpenPnP must be disconnected.
"""
import argparse
import logging
import sys
import time

import photon_busflash as pb

MOVE_FORWARD, MOVE_BACKWARD, MOVE_STATUS = 0x04, 0x05, 0x06
STATUS = {0x00: "OK", 0x01: "WRONG_FEEDER_ID", 0x03: "UNINITIALIZED", 0x04: "FEEDING_IN_PROGRESS",
          0x05: "FAIL", 0x06: "COULDNT_REACH"}


def feed(bus, addr, tenths, forward=True):
    t0 = time.monotonic()
    r = bus.transact(addr, MOVE_FORWARD if forward else MOVE_BACKWARD, bytes([tenths]), retries=0)
    t_reply = time.monotonic()
    if not r:
        return {"ok": False, "why": "no reply to the feed command (NOT repeated)"}
    if r[0] != 0:
        return {"ok": False, "why": "refused: %s" % STATUS.get(r[0], hex(r[0]))}
    expected = (r[1] << 8 | r[2]) if len(r) >= 3 else None
    polls, st = 0, None
    while time.monotonic() - t_reply < 15.0:
        polls += 1
        st = bus.transact(addr, MOVE_STATUS, retries=0)
        if st:
            break
    t_done = time.monotonic()
    return {"ok": bool(st) and st[0] == 0, "status": STATUS.get(st[0], hex(st[0])) if st else "no status",
            "duration_ms": (t_done - t_reply) * 1000, "expected_ms": expected, "polls": polls,
            "cmd_rtt_ms": (t_reply - t0) * 1000}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--addr", type=int, required=True)
    ap.add_argument("--mm", type=float, default=4.0)
    ap.add_argument("--count", type=int, default=1)
    ap.add_argument("--back", action="store_true", help="afterwards feed back the same total distance")
    ap.add_argument("--restart", action="store_true", help="restart the feeder application first (bus loader)")
    ap.add_argument("--port")
    ap.add_argument("--log")
    a = ap.parse_args(argv)
    handlers = [logging.StreamHandler()]
    if a.log:
        handlers.append(logging.FileHandler(a.log))
    logging.basicConfig(level=logging.DEBUG if a.log else logging.INFO, format="%(asctime)s %(levelname)s %(message)s",
                        handlers=handlers)
    handlers[0].setLevel(logging.INFO)
    pb.flog.addHandler(handlers[-1])
    tenths = int(round(a.mm * 10))
    bus = pb.MarlinBus(a.port or pb.find_port())
    try:
        fd = pb.Feeder(bus, a.addr)
        info = fd.probe()
        if not info or info.mode != "app":
            raise SystemExit("addr %d: no feeder application answering" % a.addr)
        pb.report("addr %d: %s" % (a.addr, info.describe()))
        if a.restart:
            if not info.entry:
                raise SystemExit("addr %d has no bus-update entry: --restart needs the bus loader" % a.addr)
            fd.vendor(pb.VENDOR_PBL_ENTER, pb.ENTER_MAGIC)
            time.sleep(0.2)
            st = fd.pbl_status()
            pb.report("addr %d: in bus loader (PBL v%d, state 0x%02X) -> RESET (nothing erased)" % (a.addr, st[0], st[1]))
            try:
                fd.pbl(pb.BL_RESET, retries=1)
            except pb.BusError:
                pass
            time.sleep(pb.T_BOOT)
            info = None
            for _ in range(5):
                info = fd.probe()
                if info and info.mode == "app":
                    break
                time.sleep(0.5)
            if not info or info.mode != "app":
                raise SystemExit("addr %d: application did not come back after the restart" % a.addr)
            pb.report("addr %d: restarted: %s" % (a.addr, info.describe()))
        res = []
        for i in range(a.count):
            r = feed(bus, a.addr, tenths)
            res.append(r)
            pb.report("feed %d: +%.1f mm  %s" % (i + 1, a.mm, r if not r["ok"] else
                      "OK  %.0f ms (firmware timeout %s ms, %d status polls)" % (r["duration_ms"], r["expected_ms"], r["polls"])))
            if not r["ok"]:
                break
            time.sleep(0.3)
        if a.back and res and res[0]["ok"]:
            for i in range(len([r for r in res if r["ok"]])):
                r = feed(bus, a.addr, tenths, forward=False)
                pb.report("back %d: -%.1f mm  %s" % (i + 1, a.mm, "OK  %.0f ms" % r["duration_ms"] if r["ok"] else r))
                time.sleep(0.3)
        ok = [r["duration_ms"] for r in res if r["ok"]]
        if ok:
            pb.report("forward feeds: %d OK, duration min %.0f / mean %.0f / max %.0f ms" % (
                len(ok), min(ok), sum(ok) / len(ok), max(ok)))
        return 0 if len(ok) == len(res) else 1
    finally:
        bus.close()


if __name__ == "__main__":
    sys.exit(main())
