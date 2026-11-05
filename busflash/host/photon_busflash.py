#!/usr/bin/env python3
"""photon-busflash - update Photon feeder firmware over the LumenPnP RS-485 bus.

Feeders stay mounted.  The PC talks to the LumenPnP motherboard (Marlin) over
USB and every feeder packet is sent with the existing ``M485 <hex>`` G-code,
so no extra hardware is needed.

Requirements on the feeder (one-time manual flash, see README):
  * application with the bus-update entry (VENDOR_OPTIONS 0xBF / 0x20, 0x21)
  * PBL bus loader in the last flash page (0x08007C00)

OpenPnP (or anything else using the serial port) must be disconnected.

Sub-commands:
  scan                         list feeders on the bus
  info IMAGE                   check an image before flashing
  flash IMAGE --addr N [N..]   update selected feeders
  flash IMAGE --all            update every feeder that needs it
  recover IMAGE --addr N       finish a feeder that is stuck in PBL (magenta LED)
  make-factory APP PBL OUT     build the one-time image for the dongle procedure
"""

from __future__ import annotations

import argparse
import dataclasses
import logging
import re
import sys
import time
import zlib

log = logging.getLogger("busflash")

# --- protocol constants (mirror of bootloader/pbl.h) -----------------------
APP_MAX = 31 * 1024
PAGE = 1024
PBL_BASE_OFF = APP_MAX                  # offset of PBL in a 32 KB factory image
PBL_HEADER_MAGIC = b"PBL1"
ENTER_MAGIC = b"PBL!"
WRITE_MAX = 24

ST_ARMED, ST_COMMITTED, ST_RECOVERY = 0x01, 0x02, 0x04
BL_STATUS, BL_ERASE, BL_WRITE, BL_COMMIT, BL_RESET = 0xB0, 0xB1, 0xB2, 0xB3, 0xB4
BL_E_CMD = 0x15
ERRORS = {0x00: "OK", 0x05: "FLASH", 0x10: "RANGE", 0x11: "ALIGN", 0x12: "NOT_ERASED",
          0x13: "VERIFY", 0x14: "CRC", 0x15: "UNKNOWN_CMD"}

# Photon application commands
GET_FEEDER_ID, INITIALIZE_FEEDER, VENDOR_OPTIONS = 0x01, 0x02, 0xBF
VENDOR_PBL_INFO, VENDOR_PBL_ENTER = 0x20, 0x21

# timing (host side waits; long PBL operations run after their reply)
T_ERASE = 0.045                         # page erase <= 40 ms (datasheet max)
T_COMMIT = 0.40                         # CRC + page 0 erase/program
T_ENTER = 0.10                          # app -> PBL jump
T_BOOT = 1.5                            # reset -> application answering


class BusError(Exception):
    pass


def crc8(data: bytes) -> int:
    """Photon CRC-8: poly 0x07, init 0 (same as OpenPnP / leash)."""
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc ^= 0x1070 << 3
            crc <<= 1
    return (crc >> 8) & 0xFF


# --- Marlin transport --------------------------------------------------------
REPLY_RE = re.compile(r"rs485-reply:\s*([0-9A-Fa-f]+|TIMEOUT)")


class MarlinBus:
    def __init__(self, port: str, baud: int = 115200):
        import serial                    # pyserial, imported lazily for `info`
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.pid = 0
        self.stats = {"packets": 0, "retries": 0}
        time.sleep(0.2)
        self.ser.reset_input_buffer()
        self.gcode("M115", timeout=5.0)  # Marlin answers -> link is alive

    def close(self):
        self.ser.close()

    def gcode(self, line: str, timeout: float = 3.0) -> list[str]:
        self.ser.write((line + "\n").encode())
        lines, buf, end = [], b"", time.monotonic() + timeout
        while time.monotonic() < end:
            buf += self.ser.read(self.ser.in_waiting or 1)   # return as soon as data is there
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                s = raw.decode(errors="replace").strip()
                if not s:
                    continue
                if s.startswith("ok"):
                    return lines
                if s.startswith("echo:busy"):
                    end = time.monotonic() + timeout
                    continue
                if s.lower().startswith("error"):
                    raise BusError(f"Marlin: {s} (for '{line}')")
                lines.append(s)
        raise BusError(f"no 'ok' from Marlin for '{line}'")

    def transact(self, addr: int, cmd: int, payload: bytes = b"",
                 retries: int = 5, retry_delay: float = 0.03) -> bytes | None:
        """Send one Photon packet; return reply payload (status + data) or
        None after `retries` timeouts."""
        for attempt in range(retries + 1):
            self.pid = (self.pid + 1) & 0xFF
            hdr = bytes([addr, 0x00, self.pid, len(payload) + 1])
            body = bytes([cmd]) + payload
            pkt = hdr + bytes([crc8(hdr + body)]) + body
            if len(pkt) > 32:
                raise ValueError("packet > 32 B (Marlin RS485_SEND_BUFFER_SIZE)")
            self.stats["packets"] += 1
            lines = self.gcode("M485 " + pkt.hex().upper())
            r = None
            for s in lines:
                m = REPLY_RE.search(s)
                if m:
                    r = m.group(1)
                elif "rs485-timeout" in s:
                    r = "TIMEOUT"
            if r and r != "TIMEOUT":
                rb = bytes.fromhex(r)
                ok = (len(rb) >= 6 and rb[0] == 0 and rb[1] == addr and
                      rb[2] == self.pid and rb[3] == len(rb) - 5 and
                      crc8(rb[:4] + rb[5:]) == rb[4])
                if ok:
                    return rb[5:]
                log.debug("bad reply %s", r)
            if attempt < retries:
                self.stats["retries"] += 1
                time.sleep(retry_delay)
        return None


# --- feeder operations ---------------------------------------------------------
@dataclasses.dataclass
class FeederInfo:
    addr: int
    mode: str                 # "app" | "pbl"
    uuid: bytes = b""
    version: str = ""
    entry: bool = False       # app has the bus-update entry
    pbl_version: int = 0      # 0 = PBL not installed
    pbl_state: int = 0
    pbl_err: int = 0

    def describe(self) -> str:
        if self.mode == "pbl":
            st = []
            if self.pbl_state & ST_RECOVERY:
                st.append("recovery")
            if self.pbl_state & ST_ARMED:
                st.append("app-erased")
            if self.pbl_state & ST_COMMITTED:
                st.append("committed")
            return (f"addr {self.addr:3d}  IN BUS LOADER v{self.pbl_version} "
                    f"[{', '.join(st) or 'idle'}] err={ERRORS.get(self.pbl_err, hex(self.pbl_err))}")
        upd = ("bus-update READY" if self.entry and self.pbl_version else
               "no PBL installed" if self.entry else "no bus-update entry (old fw)")
        return f"addr {self.addr:3d}  uuid {self.uuid.hex()}  fw {self.version or '?':16s} {upd}"


class Feeder:
    def __init__(self, bus: MarlinBus, addr: int):
        self.bus, self.addr = bus, addr

    # application side
    def app(self, cmd, payload=b"", retries=3):
        return self.bus.transact(self.addr, cmd, payload, retries=retries)

    def vendor(self, sub: int, args: bytes = b"") -> bytes | None:
        opts = (bytes([sub]) + args).ljust(20, b"\0")
        return self.app(VENDOR_OPTIONS, opts)

    def read_version(self) -> str:
        out = b""
        for chunk in range(16):
            r = self.vendor(chunk)
            if not r or r[0] != 0:
                break
            out += r[1:21]
            if b"\0" in r[1:21]:
                break
        return out.split(b"\0", 1)[0].decode(errors="replace")

    # bus-loader side
    def pbl(self, cmd, payload=b"", retries=5) -> bytes:
        r = self.bus.transact(self.addr, cmd, payload, retries=retries,
                              retry_delay=T_ERASE)
        if r is None:
            raise BusError(f"addr {self.addr}: no reply to PBL command 0x{cmd:02X}")
        return r

    def pbl_status(self, retries=5) -> tuple[int, int, int, int]:
        r = self.pbl(BL_STATUS, retries=retries)
        if len(r) < 9:
            raise BusError(f"addr {self.addr}: short PBL status {r.hex()}")
        # status, ver, state, err, addr, crc32
        return r[1], r[2], r[3], int.from_bytes(r[5:9], "little")

    def probe(self, quick=False) -> FeederInfo | None:
        r = self.app(GET_FEEDER_ID, retries=0 if quick else 2)
        if r is None:
            return None
        if r[0] == BL_E_CMD and len(r) == 1:            # PBL answers app cmds with E_CMD
            self.pbl_status()                           # clears the E_CMD our probe caused
            ver, state, err, _ = self.pbl_status()
            return FeederInfo(self.addr, "pbl", pbl_version=ver, pbl_state=state, pbl_err=err)
        if r[0] != 0 or len(r) < 13:
            return None
        info = FeederInfo(self.addr, "app", uuid=r[1:13])
        r = self.app(INITIALIZE_FEEDER, info.uuid)
        if not r or r[0] != 0:
            raise BusError(f"addr {self.addr}: INITIALIZE failed")
        info.version = self.read_version()
        r = self.vendor(VENDOR_PBL_INFO)
        if r and r[0] == 0 and r[1:4] == b"PBL":
            info.entry = True
            info.pbl_version = r[5]
        return info


# --- image handling -------------------------------------------------------------
VERSION_RE = re.compile(rb"(v\d+\.\d+\.\d+[0-9A-Za-z.+\-]*)\x00")


@dataclasses.dataclass
class Image:
    data: bytes
    version: str
    has_entry: bool
    problems: list

    @classmethod
    def load(cls, path: str) -> "Image":
        data = open(path, "rb").read()
        problems = []
        if len(data) > APP_MAX:
            problems.append(f"size {len(data)} B > {APP_MAX} B: would overwrite the bus "
                            "loader page (is this a factory image?)")
        if len(data) >= 8:
            sp = int.from_bytes(data[0:4], "little")
            rv = int.from_bytes(data[4:8], "little")
            if not (0x20000000 < sp <= 0x20002000):
                problems.append(f"initial SP 0x{sp:08X} not in SRAM: not a Photon .bin?")
            if not (rv & 1 and 0x08000000 <= rv < 0x08000000 + len(data)):
                problems.append(f"reset vector 0x{rv:08X} outside image")
        else:
            problems.append("file too short")
        if len(data) & 1:
            data += b"\xff"
        m = VERSION_RE.search(data)
        return cls(data, m.group(1).decode() if m else "", ENTER_MAGIC in data, problems)


# --- the update procedure --------------------------------------------------------
def flash_one(bus: MarlinBus, addr: int, img: Image, allow_no_entry=False) -> FeederInfo:
    fd = Feeder(bus, addr)
    t0 = time.monotonic()
    info = fd.probe()
    if info is None:
        raise BusError(f"addr {addr}: no feeder answers")
    if info.mode == "app":
        log.info("addr %d: %s", addr, info.describe())
        if not info.entry:
            raise BusError(f"addr {addr}: firmware '{info.version}' has no bus-update entry; "
                           "one-time manual flash of the factory image needed")
        if not info.pbl_version:
            raise BusError(f"addr {addr}: bus loader (PBL) not installed; "
                           "one-time manual flash of the factory image needed")
        if not img.has_entry and not allow_no_entry:
            raise BusError("image has no bus-update entry: after flashing, this feeder could "
                           "only be updated manually again (use --allow-no-entry to accept)")
        r = fd.vendor(VENDOR_PBL_ENTER, ENTER_MAGIC)
        if r is not None and r[0] != 0:
            raise BusError(f"addr {addr}: feeder refused to enter the bus loader (status {r[0]})")
        time.sleep(T_ENTER)
    else:
        log.info("addr %d: already in bus loader (recovery), continuing", addr)

    ver, state, err, _ = fd.pbl_status(retries=10)
    log.info("addr %d: PBL v%d state=0x%02X err=%s", addr, ver, state, ERRORS.get(err, err))

    data = img.data
    npages = (len(data) + PAGE - 1) // PAGE
    # Page 0 first: replaces the app vectors by the recovery stub.  If the
    # stub is already there (recovery), keep it: no unprotected window.
    for p in range(1 if state & ST_ARMED else 0, npages):
        fd.pbl(BL_ERASE, bytes([p]))
        time.sleep(T_ERASE)
    _, state, err, _ = fd.pbl_status()
    if err or not state & ST_ARMED:
        raise BusError(f"addr {addr}: erase failed (err={ERRORS.get(err, err)}, state=0x{state:02X})")

    sent = 0
    for off in range(0, len(data), WRITE_MAX):
        chunk = data[off:off + WRITE_MAX]
        if chunk.count(0xFF) == len(chunk):
            continue                         # erased flash / RAM shadow are 0xFF already
        r = fd.pbl(BL_WRITE, off.to_bytes(2, "little") + chunk)
        if r[0] != 0:
            raise BusError(f"addr {addr}: write @0x{off:04X} failed ({ERRORS.get(r[0], r[0])})")
        sent += 1
    log.info("addr %d: %d data packets written", addr, sent)

    _, _, err, _ = fd.pbl_status()
    if err:
        raise BusError(f"addr {addr}: error before commit ({ERRORS.get(err, err)})")
    crc = zlib.crc32(data)
    fd.pbl(BL_COMMIT, len(data).to_bytes(2, "little") + crc.to_bytes(4, "little"))
    time.sleep(T_COMMIT)
    _, state, err, got = fd.pbl_status(retries=20)
    if err or not state & ST_COMMITTED or got != crc:
        raise BusError(f"addr {addr}: commit failed (err={ERRORS.get(err, err)}, "
                       f"crc feeder 0x{got:08X} host 0x{crc:08X}); feeder stays in the "
                       "bus loader, run `recover`")
    try:
        fd.pbl(BL_RESET, retries=1)
    except BusError:
        pass                                 # reply may be lost in the reset
    time.sleep(T_BOOT)

    after = None
    for _ in range(5):
        after = fd.probe()
        if after and after.mode == "app":
            break
        time.sleep(0.5)
    if not after or after.mode != "app":
        raise BusError(f"addr {addr}: application did not come back after reset")
    if info.mode == "app" and after.uuid != info.uuid:
        raise BusError(f"addr {addr}: UUID changed ({info.uuid.hex()} -> {after.uuid.hex()})?!")
    log.info("addr %d: DONE in %.1f s -> %s", addr, time.monotonic() - t0, after.describe())
    return after


def scan(bus: MarlinBus, first: int, last: int) -> list[FeederInfo]:
    found = []
    for a in range(first, last + 1):
        try:
            info = Feeder(bus, a).probe(quick=True)
        except BusError as e:
            log.warning("%s", e)
            continue
        if info:
            found.append(info)
            print(info.describe(), flush=True)
    return found


# --- CLI --------------------------------------------------------------------------
def find_port() -> str:
    from serial.tools import list_ports
    ports = [p.device for p in list_ports.comports() if (p.vid, p.pid) == (0x0483, 0x5740)]
    if len(ports) != 1:
        raise SystemExit(f"cannot auto-detect the LumenPnP port (found {ports}); use --port")
    return ports[0]


def cmd_info(a):
    img = Image.load(a.image)
    print(f"{a.image}: {len(img.data)} B (max {APP_MAX}), version '{img.version or '?'}', "
          f"bus-update entry: {'yes' if img.has_entry else 'NO'}, crc32 0x{zlib.crc32(img.data):08X}")
    for p in img.problems:
        print("  PROBLEM:", p)
    return 1 if img.problems else 0


def cmd_make_factory(a):
    app = Image.load(a.app)
    pbl = open(a.pbl, "rb").read()
    if app.problems:
        raise SystemExit("app image: " + "; ".join(app.problems))
    if not app.has_entry:
        raise SystemExit("app image has no bus-update entry (PBL!): wrong build?")
    if len(pbl) > PAGE or pbl[:4] != PBL_HEADER_MAGIC:
        raise SystemExit("PBL image must be <= 1024 B and start with 'PBL1'")
    out = app.data.ljust(APP_MAX, b"\xff") + pbl.ljust(PAGE, b"\xff")
    open(a.out, "wb").write(out)
    print(f"{a.out}: {len(out)} B = app '{app.version}' ({len(app.data)} B) + PBL v{pbl[4]} "
          f"({len(pbl)} B) -> flash at 0x08000000 with the normal dongle procedure")
    return 0


def cmd_bus(a):
    bus = MarlinBus(a.port or find_port(), a.baud)
    try:
        if a.cmd == "scan":
            scan(bus, a.first, a.last)
            return 0
        img = Image.load(a.image)
        if img.problems:
            raise SystemExit("image rejected: " + "; ".join(img.problems))
        if a.cmd == "recover":
            targets = a.addr
        elif a.all:
            targets = []
            for f in scan(bus, a.first, a.last):
                if f.mode == "pbl" or a.force or f.version != img.version:
                    if f.mode == "app" and not (f.entry and f.pbl_version):
                        log.warning("addr %d skipped: not bus-updatable yet", f.addr)
                        continue
                    targets.append(f.addr)
        else:
            targets = a.addr
        if not targets:
            print("nothing to do")
            return 0
        print(f"image '{img.version}' ({len(img.data)} B) -> feeders {targets}")
        if a.dry_run:
            return 0
        if not a.yes and input("proceed? [y/N] ").strip().lower() != "y":
            return 1
        failed = []
        for t in targets:
            try:
                flash_one(bus, t, img, a.allow_no_entry)
            except BusError as e:
                log.error("%s", e)
                failed.append(t)
        log.info("packets %d, retries %d", bus.stats["packets"], bus.stats["retries"])
        if failed:
            print(f"FAILED: {failed}  (a feeder with a magenta LED is waiting in the bus "
                  "loader: run `recover IMAGE --addr N`)")
            return 2
        print("all done")
        return 0
    finally:
        bus.close()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="Marlin serial port (default: auto-detect 0483:5740)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--log", help="also write a debug log to this file")
    ap.add_argument("-v", "--verbose", action="store_true")
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("scan"); s.add_argument("--first", type=int, default=1); s.add_argument("--last", type=int, default=50)
    s = sub.add_parser("info"); s.add_argument("image")
    for name in ("flash", "recover"):
        s = sub.add_parser(name)
        s.add_argument("image")
        g = s.add_mutually_exclusive_group(required=True)
        g.add_argument("--addr", type=int, nargs="+")
        if name == "flash":
            g.add_argument("--all", action="store_true", help="every feeder whose version differs")
            s.add_argument("--force", action="store_true", help="with --all: also same version")
        s.add_argument("--first", type=int, default=1); s.add_argument("--last", type=int, default=50)
        s.add_argument("--yes", action="store_true"); s.add_argument("--dry-run", action="store_true")
        s.add_argument("--allow-no-entry", action="store_true")
    s = sub.add_parser("make-factory"); s.add_argument("app"); s.add_argument("pbl"); s.add_argument("out")
    a = ap.parse_args(argv)
    if a.cmd in ("flash", "recover") and not hasattr(a, "all"):
        a.all = False
        a.force = False

    handlers = [logging.StreamHandler()]
    if a.log:
        handlers.append(logging.FileHandler(a.log))
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s", handlers=handlers)
    if a.log:
        handlers[1].setLevel(logging.DEBUG)
        logging.getLogger().setLevel(logging.DEBUG)
        handlers[0].setLevel(logging.DEBUG if a.verbose else logging.INFO)

    if a.cmd == "info":
        return cmd_info(a)
    if a.cmd == "make-factory":
        return cmd_make_factory(a)
    return cmd_bus(a)


if __name__ == "__main__":
    sys.exit(main())
