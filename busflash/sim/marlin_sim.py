#!/usr/bin/env python3
"""Marlin + Photon feeder bus simulator for photon_busflash.py.

Presents a pseudo-terminal that behaves like the LumenPnP motherboard for the
G-codes the tool uses (M115, M485) and routes M485 packets to simulated
feeders:
  * application  = Python model of Photon firmware (old: no bus entry;
                   new: VENDOR 0x20/0x21 as in app-patch/0001-bus-loader-entry.patch)
  * bus loader   = the REAL bootloader/pbl_core.c compiled for Linux (pbl_sim),
                   operating on a 32 KB flash file per feeder

Faults: --drop P   lose a reply with probability P (feeder did execute)
        --cut N    "power cut" the feeder at its N-th packet in the bus loader
"""
import argparse
import os
import random
import re
import select
import subprocess
import sys
import time
import tty

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "host"))
from photon_busflash import crc8, APP_MAX  # noqa: E402

RECOVERY_MAGIC = (0x52424C50).to_bytes(4, "little")
REPLY_TIMEOUT = 0.050                    # Marlin setMaxReadTimeout(50000) us
VERSION_RE = re.compile(rb"(v\d+\.\d+\.\d+[0-9A-Za-z.+\-]*)\x00")


def frame(addr, pid, status, data=b""):
    payload = bytes([status]) + data
    hdr = bytes([0, addr, pid, len(payload)])
    return hdr + bytes([crc8(hdr + payload)]) + payload


class SimFeeder:
    def __init__(self, addr, flash_path, uuid, pbl_sim, cut_at=0):
        self.addr, self.path, self.uuid, self.pbl_sim = addr, flash_path, uuid, pbl_sim
        self.proc = None
        self.cut_at, self.pbl_packets = cut_at, 0
        self.boot()

    def flash(self):
        with open(self.path, "rb") as f:
            return f.read()

    def boot(self):
        """Power-on: decide what the MCU runs from the flash content."""
        f = self.flash()
        self.initialized = False
        sp, rv = int.from_bytes(f[0:4], "little"), int.from_bytes(f[4:8], "little")
        if f[8:12] == RECOVERY_MAGIC:
            self.start_pbl("reset")
        elif 0x20000000 < sp <= 0x20002000 and rv & 1 and 0x08000000 <= rv < 0x08000000 + APP_MAX:
            self.mode = "app"
            m = VERSION_RE.search(f[:APP_MAX])
            self.version = m.group(1) if m else b"?"
            self.has_entry = b"PBL!" in f[:APP_MAX]
        else:
            self.mode = "dead"           # bricked: needs the dongle
        log(f"feeder {self.addr}: boot -> {self.mode}"
            + (f" {self.version.decode()} entry={self.has_entry}" if self.mode == "app" else ""))

    def start_pbl(self, how):
        self.mode = "pbl"
        self.proc = subprocess.Popen([self.pbl_sim, self.path, str(self.addr), how],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        os.set_blocking(self.proc.stdout.fileno(), False)

    def pbl_version(self, f):
        return f[APP_MAX + 4] if f[APP_MAX:APP_MAX + 4] == b"PBL1" else 0

    # -- packet handling ------------------------------------------------------
    def handle(self, pkt):
        if self.mode == "pbl" and self.proc.poll() is not None:
            log(f"feeder {self.addr}: PBL exited ({self.proc.returncode}) -> reset")
            self.proc = None
            self.boot()
        if self.mode == "pbl":
            return self.handle_pbl(pkt)
        if self.mode == "app":
            return self.handle_app(pkt)
        return None

    def handle_pbl(self, pkt):
        self.pbl_packets += 1
        if self.cut_at and self.pbl_packets == self.cut_at:
            log(f"feeder {self.addr}: *** POWER CUT at PBL packet {self.pbl_packets} ***")
            self.proc.kill()
            self.proc.wait()
            self.proc = None
            self.cut_at = 0
            time.sleep(0.05)
            self.boot()
            return None
        self.proc.stdin.write(pkt)
        self.proc.stdin.flush()
        buf, end = b"", time.monotonic() + REPLY_TIMEOUT
        fd = self.proc.stdout.fileno()
        while time.monotonic() < end:
            r, _, _ = select.select([fd], [], [], max(0, end - time.monotonic()))
            if r:
                chunk = os.read(fd, 64)
                if not chunk:
                    break
                buf += chunk
                if len(buf) >= 5 and len(buf) >= 5 + buf[3]:
                    return buf[:5 + buf[3]]
        return None

    def handle_app(self, pkt):
        if len(pkt) < 6 or crc8(pkt[:4] + pkt[5:]) != pkt[4] or len(pkt) != 5 + pkt[3]:
            return None
        pid, cmd, body = pkt[2], pkt[5], pkt[6:]
        a = self.addr
        if cmd == 0x01:
            return frame(a, pid, 0, self.uuid)
        if cmd == 0x02:
            if body[:12] != self.uuid:
                return frame(a, pid, 1, self.uuid)
            self.initialized = True
            return frame(a, pid, 0, self.uuid)
        if cmd == 0x03:
            return frame(a, pid, 0, b"\x01")
        if cmd == 0xBF:
            if not self.initialized:
                return frame(a, pid, 3, self.uuid)
            sub, opts = body[0], body[:20]
            if sub <= 0x0F:
                s = self.version + b"\0"
                return frame(a, pid, 0, s[sub * 20:sub * 20 + 20].ljust(20, b"\0"))
            if sub in (0x20, 0x21) and self.has_entry:
                blv = self.pbl_version(self.flash())
                info = (b"PBL" + bytes([1, blv])).ljust(20, b"\0")
                enter = sub == 0x21 and blv and opts[1:5] == b"PBL!"
                if sub == 0x21 and not enter:
                    return frame(a, pid, 5, info)
                reply = frame(a, pid, 0, info)
                if enter:
                    self.start_pbl("app")
                    log(f"feeder {a}: app -> PBL")
                return reply
            return frame(a, pid, 0, bytes(20))       # old firmware: OK + zeros
        return None                                   # unknown: Photon ignores it


def log(msg):
    print(f"[sim {time.strftime('%H:%M:%S')}] {msg}", file=sys.stderr, flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--feeder", action="append", default=[],
                    help="ADDR:FLASHFILE[:UUIDHEX] (flash file = 32 KB image)")
    ap.add_argument("--pbl-sim", default=os.path.join(os.path.dirname(__file__), "pbl_sim"))
    ap.add_argument("--port-file", required=True, help="write the pty path here")
    ap.add_argument("--drop", type=float, default=0.0)
    ap.add_argument("--cut", default="", help="ADDR:N power cut at the N-th PBL packet")
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    random.seed(a.seed)
    cut = dict([tuple(map(int, a.cut.split(":")))]) if a.cut else {}

    feeders = {}
    for spec in a.feeder:
        parts = spec.split(":")
        addr = int(parts[0])
        uuid = bytes.fromhex(parts[2]) if len(parts) > 2 else bytes([0xA0 + addr] * 12)
        feeders[addr] = SimFeeder(addr, parts[1], uuid, a.pbl_sim, cut.get(addr, 0))

    master, slave = os.openpty()
    tty.setraw(slave)
    with open(a.port_file, "w") as f:
        f.write(os.ttyname(slave))
    log(f"Marlin sim on {os.ttyname(slave)} with feeders {sorted(feeders)}")

    buf = b""
    while True:
        r, _, _ = select.select([master], [], [], 1.0)
        if not r:
            continue
        try:
            buf += os.read(master, 1024)
        except OSError:
            break
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.decode().strip()
            out = []
            if line.startswith("M485"):
                hexs = line[4:].strip()
                if len(hexs) % 2 or len(hexs) > 64:
                    out.append("Error:String too long or odd")
                else:
                    pkt = bytes.fromhex(hexs)
                    f = feeders.get(pkt[0])
                    rep = f.handle(pkt) if f else None
                    if rep is not None and random.random() < a.drop:
                        rep = None
                    time.sleep(0.004)                 # ~USB + 57600 Bd transfer time
                    out.append("rs485-reply: " + (rep.hex().upper() if rep else "TIMEOUT"))
            elif line.startswith("M115"):
                out.append("FIRMWARE_NAME:Marlin SIMULATOR (LumenPnP bus sim)")
            out.append("ok")
            os.write(master, ("\n".join(out) + "\n").encode())


if __name__ == "__main__":
    main()
