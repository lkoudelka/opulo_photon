#!/usr/bin/env python3
"""End-to-end tests: real host tool + real PBL core (host build) + Marlin/bus sim.

Usage: run_sim_tests.py RELEASE.bin     (e.g. photon v1.1.1 .bin = "old firmware")
"""
import os
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOOL = os.path.join(ROOT, "host", "photon_busflash.py")
SIM = os.path.join(ROOT, "sim", "marlin_sim.py")
PBL_BIN = os.path.join(ROOT, "bootloader", "build", "pbl.bin")
PY = sys.executable
APP_MAX = 31744

results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond)))
    print(f"  [{'PASS' if cond else 'FAIL'}] {name} {detail}")


def make_app(release, version, entry=True, size=31660):
    """Synthetic 'new firmware': release image, version string replaced
    (same length), bus-entry marker added, padded to a realistic size."""
    import re
    d = bytearray(release)
    m = re.search(rb"v\d+\.\d+\.\d+[0-9A-Za-z.+\-]*\0", bytes(d))
    assert m, "release version string not found"
    n = m.end() - m.start() - 1                 # keep the length: same layout
    d[m.start():m.end()] = version.encode().ljust(n, b"_")[:n] + b"\0"
    if entry:
        d += b"PBL!"
    d += bytes((7 * k) & 0xFF for k in range(max(0, size - len(d))))   # non-0xFF filler
    return bytes(d)


def flash_file(path, app, pbl=None):
    img = app.ljust(APP_MAX, b"\xff") + (pbl.ljust(1024, b"\xff") if pbl else b"\xff" * 1024)
    open(path, "wb").write(img)


class Sim:
    def __init__(self, work, feeders, extra=()):
        self.port_file = os.path.join(work, "port")
        if os.path.exists(self.port_file):
            os.remove(self.port_file)
        args = [PY, SIM, "--port-file", self.port_file] + list(extra)
        for f in feeders:
            args += ["--feeder", f]
        self.log = open(os.path.join(work, "sim.log"), "a")
        self.p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=self.log)
        for _ in range(100):
            if os.path.exists(self.port_file) and open(self.port_file).read():
                break
            time.sleep(0.05)
        self.port = open(self.port_file).read()

    def tool(self, *args):
        t0 = time.monotonic()
        r = subprocess.run([PY, TOOL, "--port", self.port] + list(args),
                           capture_output=True, text=True, timeout=600)
        return r.returncode, r.stdout + r.stderr, time.monotonic() - t0

    def stop(self):
        self.p.terminate()
        self.p.wait()
        self.log.close()


def main():
    release = open(sys.argv[1], "rb").read()
    pbl = open(PBL_BIN, "rb").read()
    work = tempfile.mkdtemp(prefix="busflash-sim-", dir=os.environ.get("SIM_WORKDIR"))
    print("work dir:", work)
    cur = make_app(release, "v1.1.8")       # installed via the one-time dongle flash
    new = make_app(release, "v1.1.9")       # the update
    noentry = make_app(release, "v1.2.0", entry=False)
    for n, d in (("new.bin", new), ("noentry.bin", noentry)):
        open(os.path.join(work, n), "wb").write(d)
    newbin = os.path.join(work, "new.bin")

    def fresh():
        flash_file(os.path.join(work, "f1.bin"), release)        # old fw, no PBL
        flash_file(os.path.join(work, "f2.bin"), cur, pbl)       # migrated
        flash_file(os.path.join(work, "f3.bin"), cur, pbl)       # migrated
        flash_file(os.path.join(work, "f4.bin"), cur)            # entry but no PBL
        return [f"{a}:{os.path.join(work, f'f{a}.bin')}" for a in (1, 2, 3, 4)]

    def app_region(a):
        return open(os.path.join(work, f"f{a}.bin"), "rb").read()[:len(new)]

    print("T1 info / image checks")
    rc, out, _ = subprocess.run([PY, TOOL, "info", newbin], capture_output=True, text=True).returncode, \
        subprocess.run([PY, TOOL, "info", newbin], capture_output=True, text=True).stdout, 0
    check("info accepts new image", rc == 0 and "v1.1.9" in out and "entry: yes" in out, out.strip())
    big = os.path.join(work, "big.bin")
    open(big, "wb").write(new.ljust(APP_MAX + 2, b"\0"))
    rc = subprocess.run([PY, TOOL, "info", big], capture_output=True).returncode
    check("info rejects image > 31744 B", rc != 0)
    fac = os.path.join(work, "factory.bin")
    rc = subprocess.run([PY, TOOL, "make-factory", newbin, PBL_BIN, fac], capture_output=True).returncode
    fb = open(fac, "rb").read()
    check("make-factory = 32 KB, PBL at 0x7C00", rc == 0 and len(fb) == 32768 and fb[APP_MAX:APP_MAX + 4] == b"PBL1")

    print("T2 scan + single flash (clean bus)")
    s = Sim(work, fresh())
    rc, out, _ = s.tool("scan", "--last", "6")
    check("scan finds 4 feeders", out.count("addr ") >= 4, "")
    check("scan: old fw flagged", "no bus-update entry" in out)
    check("scan: migrated feeder ready", "bus-update READY" in out)
    check("scan: missing PBL flagged", "no PBL installed" in out)
    rc, out, dt = s.tool("flash", newbin, "--addr", "2", "--yes")
    check("flash addr 2 succeeds", rc == 0 and "DONE" in out, f"({dt:.1f} s sim time)")
    check("addr 2 flash content == image", app_region(2) == new)
    check("addr 2 reports new version", "v1.1.9" in out)
    rc, out, _ = s.tool("flash", newbin, "--addr", "1", "--yes")
    check("old-fw feeder refused (needs manual flash)", rc != 0 and "no bus-update entry" in out)
    check("old-fw feeder untouched", open(os.path.join(work, "f1.bin"), "rb").read()[:len(release)] == release)
    rc, out, _ = s.tool("flash", newbin, "--addr", "4", "--yes")
    check("no-PBL feeder refused", rc != 0 and "not installed" in out)
    rc, out, _ = s.tool("flash", os.path.join(work, "noentry.bin"), "--addr", "3", "--yes")
    check("image without entry refused w/o flag", rc != 0 and "allow-no-entry" in out)
    s.stop()

    print("T3 --all with 10 % lost replies")
    s = Sim(work, fresh(), ["--drop", "0.10", "--seed", "7"])
    rc, out, dt = s.tool("flash", newbin, "--all", "--last", "6", "--yes")
    check("--all updates 2 and 3, skips 1 and 4", rc == 0 and app_region(2) == new and app_region(3) == new
          and "addr 1 skipped" in out and "addr 4 skipped" in out, f"({dt:.1f} s)")
    retr = [l for l in out.splitlines() if "retries" in l]
    check("retries happened and were absorbed", retr and " 0" not in retr[-1].split("retries")[-1], retr[-1] if retr else "")
    s.stop()

    print("T4 power cut in the middle of writing -> recovery")
    s = Sim(work, fresh(), ["--cut", "3:400"])
    rc, out, _ = s.tool("flash", newbin, "--addr", "3", "--yes")
    check("interrupted update reported", rc != 0)
    rc, out, _ = s.tool("scan", "--first", "3", "--last", "3")
    check("feeder 3 boots into PBL (recovery stub)", "IN BUS LOADER" in out and "recovery" in out, out.strip())
    rc, out, _ = s.tool("recover", newbin, "--addr", "3", "--yes")
    check("recover completes", rc == 0 and app_region(3) == new)
    s.stop()

    print("T5 power cut right after ERASE(0) -> still recoverable")
    s = Sim(work, fresh(), ["--cut", "2:4"])
    rc, out, _ = s.tool("flash", newbin, "--addr", "2", "--yes")
    rc, out, _ = s.tool("recover", newbin, "--addr", "2", "--yes")
    check("recover after early cut", rc == 0 and app_region(2) == new)
    s.stop()

    print("T6 corrupted data detected by commit CRC")
    s = Sim(work, fresh())
    # flash once OK, then corrupt one byte in page 5 behind the loader's back
    # while it is in PBL: simulate by flipping a byte between WRITE and COMMIT
    # is not possible from outside, so check the CRC path directly:
    import importlib.util
    spec = importlib.util.spec_from_file_location("bf", TOOL)
    bf = importlib.util.module_from_spec(spec)
    sys.modules["bf"] = bf
    spec.loader.exec_module(bf)
    bus = bf.MarlinBus(s.port)
    fd = bf.Feeder(bus, 2)
    info = fd.probe()
    fd.vendor(bf.VENDOR_PBL_ENTER, bf.ENTER_MAGIC)
    time.sleep(0.1)
    for p in range(31):
        fd.pbl(bf.BL_ERASE, bytes([p]))
        time.sleep(bf.T_ERASE)
    fd.pbl_status()
    fd.pbl(bf.BL_WRITE, (2048).to_bytes(2, "little") + b"\x11\x22")
    fd.pbl(bf.BL_COMMIT, (len(new)).to_bytes(2, "little") + zlib.crc32(new).to_bytes(4, "little"))
    time.sleep(bf.T_COMMIT)
    ver, state, err, crc = fd.pbl_status(retries=20)
    check("wrong image -> E_CRC, not committed", err == 0x14 and not state & bf.ST_COMMITTED,
          f"err=0x{err:02X} state=0x{state:02X}")
    r = fd.pbl(bf.BL_ERASE, bytes([31]))
    ver, state, err, crc = fd.pbl_status()
    check("ERASE of the PBL page refused (E_RANGE)", err == 0x10)
    r = fd.pbl(bf.BL_WRITE, (APP_MAX - 2).to_bytes(2, "little") + b"\0\0\0\0")
    check("WRITE crossing into PBL page refused", r[0] == 0x10)
    bus.close()
    pbl_page = open(os.path.join(work, "f2.bin"), "rb").read()[APP_MAX:]
    check("PBL page intact", pbl_page.startswith(pbl))
    rc, out, _ = s.tool("recover", newbin, "--addr", "2", "--yes")
    check("recover after failed commit", rc == 0 and app_region(2) == new)
    s.stop()

    ok = sum(1 for _, r in results if r)
    print(f"\n{ok}/{len(results)} checks passed")
    return 0 if ok == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
