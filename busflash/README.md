# photon-busflash: Photon feeder firmware update over the LumenPnP bus

*Prepared 2026-11-05, before hardware access. Status: **simulation-verified, app built, not yet run on hardware**.*

**Scope: 8 mm / 12 mm feeders** (original architecture, STM32F031x6, 32 KB / 4 KB, firmware line `main`, release v1.0.7). The 16/24 mm "2u" feeders (STM32F030C8, 64 KB / 8 KB, branch `2u`, other pins, RS-485 on USART2) are **not supported**: all feeders in use here are first generation (F031).

Feeders stay mounted. The PC talks to the LumenPnP motherboard over USB, and each feeder packet goes through the existing Marlin G-code `M485 <hex>` (57600 8N1, Photon framing, ≤ 32 B per packet). **Marlin, OpenPnP and the release `.bin` files are not modified.**

```
PC ──USB── Marlin (M485) ──RS-485 bus── feeder app ──VENDOR 0xBF/0x21 "PBL!"──► PBL (last flash page)
 photon_busflash.py                                                              erase / write / commit
```

## 1. Components

| Path | What | State |
|---|---|---|
| `busflash/bootloader/` | **PBL**, the 1 KB bus loader for the last flash page (0x08007C00). Bare-register C, built with the local STM32CubeCLT GCC (`make`). | Builds, **1020 / 1024 B** |
| `photon/src/*`, `photon/platformio.ini` (branch `feature/bus-loader`) | Photon app change: VENDOR_OPTIONS sub-commands `0x20` (info) and `0x21` (enter PBL), plus a size guard in `platformio.ini`. +95 lines, source only. | **Built: 31,052 B** (v1.0.7 base 30,840 B, +204 B for the patch and the longer version string) |
| `busflash/host/photon_busflash.py` | Host tool: `scan`, `info`, `flash --addr/--all`, `recover`, `make-factory`. Needs pyserial; run it with `_tools/photon-busflash.sh`. | Tested in simulation |
| `busflash/sim/` | `pbl_sim` (the **same `pbl_core.c`** built for Linux, with mmap'ed flash) + `marlin_sim.py` (Marlin + bus + app model, fault injection) | Working |
| `busflash/tests/run_sim_tests.py` | End-to-end tests: real tool + real PBL core + simulated Marlin | **25/25 pass** |
| `busflash/build.sh` | Builds the app (PlatformIO), PBL and the factory image, then checks sizes | Works (reproducible: the unmodified v1.0.7 source builds byte-identical to the official release) |

## 2. Memory budget (the critical constraint)

**Feeder flash: 32 KB = 32 pages × 1 KB.**

| Region | Address | Size |
|---|---|---|
| Application, standard link address, released `.bin` unchanged | 0x08000000–0x08007BFF | **≤ 31,744 B** (31 pages) |
| PBL | 0x08007C00–0x08007FFF | 1,024 B, of which 1,020 B is used |

- **v1.0.7 release:** 30,840 B, which leaves 904 B below the PBL page.
- **Patched app** (`busflash/build.sh v1.0.7-pbl1`, measured): **31,052 B, 692 B headroom.** The same source without the patch builds to 30,848 B, so the patch costs **+204 B**. The link fails if the image exceeds 31,744 B (`board_upload.maximum_size`).
- Further feeder features are easier with the flash optimisations on branch `feature/flash-diet`.

**RAM.** PBL runs *instead of* the app, so the app's RAM is free:
- PBL uses 1,064 B of `.bss` (1 KB page-0 shadow, packet buffer, state).
- The stack sits at 0x20001000.
- Total is under 2 KB, inside the 4 KB of the F031. The v1.0.7 app itself uses 1,932 B static RAM.

**PBL size breakdown** (from `nm`):

| Part | Size |
|---|---|
| Main loop (RX, init, erase/program and CRC-32 inlined) | 592 B |
| Reply/TX | 104 B |
| Flash programming with read-back | 104 B |
| Register init table | 88 B |
| CRC-8 | 60 B |
| Flash wait | 32 B |
| Header + two entry stubs | 38 B |

With `-DPBL_HW_CRC` it uses the CRC peripheral and saves ~8 B, but that is not verified on silicon.

## 3. How an update works

1. **App side** (feeder still running the old app):
   1. `GET_FEEDER_ID`, then `INITIALIZE`.
   2. `VENDOR 0x20` reports whether the entry and PBL are present.
   3. `VENDOR 0x21 "PBL!"`: the app replies, waits 10 ms, runs `HAL_RCC_DeInit()` + `HAL_DeInit()` (the same clean-up as the existing ROM-bootloader jump) and jumps to PBL.
   4. The LED turns **magenta**.
2. **PBL** (commands `0xB0`–`0xB4`, unicast to the feeder's bus address):
   1. `ERASE 0`: page 0 is erased and a **recovery stub** is written (SP, reset → PBL, magic, bus address). From now on, a reset or power loss lands in PBL again.
   2. `ERASE 1..n`, then `WRITE` 24 B chunks. All-0xFF chunks are skipped. Page 0 data goes to a RAM shadow; everything else is programmed directly and read back.
   3. `COMMIT len crc32`: PBL computes the zlib CRC-32 of the final image (page 0 taken from RAM). Only if it matches does it erase page 0 and program it from RAM. This is the **only unprotected window, ~65 ms**.
   4. `RESET` starts the new app. The host checks UUID and version.
3. **Safety properties:**
   - PBL never erases or writes its own page (range checks).
   - WRITE is idempotent, so retried packets are harmless.
   - Long operations reply first, and their errors show up in the next reply.
   - The status byte of every reply is the last error.

**Time per feeder, estimated:**
- ~1,300 `M485` packets × measured RTT, plus ~3.5 s fixed.
- At 15 ms per round trip that's ≈ 25 s; at 40 ms it's ≈ 55 s.
- Measure the RTT on day 1 (study §9.4).

## 4. Compatibility (per the standing rule: nothing silent)

| Item | Effect | Breaking? |
|---|---|---|
| Release `.bin` files | Flashed byte-for-byte, same link address | No |
| Dongle procedure (docs.opulo.io) | Unchanged. The one-time migration uses the **same procedure**, just with the `*-factory.bin` file. | No |
| OpenPnP, Marlin, `machine.xml` | Unchanged | No |
| Photon protocol | Adds VENDOR sub-commands `0x20`/`0x21`. Old firmware answers them with OK + zeros, which the tool detects as "no entry". | No (additive) |
| Plain upstream image flashed later by dongle | The feeder works as before. The PBL page survives a normal CubeProgrammer download (but not a *full chip erase*, ⚠ verify B11). Without the entry patch, the bus update is unavailable until the next factory flash. | No |
| Plain upstream image flashed over the bus | Refused unless `--allow-no-entry`, because afterwards the feeder could only be updated manually again | No |
| **Memory contract: application ≤ 31,744 B** | Future Photon releases must leave the last page free. Today the app is 31,456 B. | **Constraint, needs your confirmation (D1)** |
| **VENDOR sub-command numbers 0x20/0x21** | If upstream ever uses them differently, there will be a conflict | **Coordinate upstream (D2)** |

**Decisions for you:**
- **D1:** accept the 31 KB application cap.
- **D2:** offer the patch upstream (photonfirmware/photon, MPL-2.0), or keep it as a fork rebased per release.
- **D3:** allow PlatformIO to be installed (`uv tool install platformio`) for `build-app.sh`.

## 5. Day-1 deployment runbook (once the PnP is connected)

**Prerequisites**
- OpenPnP closed or disconnected.
- `lsusb` shows `0483:5740`.
- `M115` firmware is Opulo Marlin with `M485`. The tool sends `M115` on connect; check the log.
- `busflash/build.sh v1.0.7-pbl1` has been run. It produces `busflash/out/photon-…-pbl1.bin` (bus updates) and `…-pbl1-factory.bin` (one-time dongle flash).

**Phase B: bench validation on ONE spare feeder (do not skip)**

| # | Test | Pass criterion |
|---|---|---|
| B1 | Flash `*-factory.bin` with the normal dongle procedure; run the feeder in OpenPnP | Feeds, LED, version shown; no behaviour change |
| B2 | `photon-busflash.sh scan` | Shows `bus-update READY` |
| B3 | `flash <same image> --addr N` | LED magenta during the update, `DONE`, version unchanged. Validates the jump, GPIO/USART set-up and RS-485 turnaround with Marlin. |
| B4 | Read RTT and retries from the log (`_logs/busflash-*.log`) | Retries < 1 %, RTT measured |
| B5 | Flash a *different* build (e.g. VERSION_STRING `…-pbl1b`) | New version reported |
| B6 | Pull the feeder or cut power in the middle of writing; re-insert | Magenta LED, `scan` shows `IN BUS LOADER … recovery`, `recover` completes |
| B7 | 20 consecutive updates | No E_VERIFY / NOT_ERASED. If they appear, raise `T_ERASE`. |
| B8 | Other feeders on the bus during the update | Unaffected; OpenPnP works with all of them afterwards |
| B9 | OpenPnP after update | Feeder found, initialized, feeds |
| B10 | Feeder moved to another slot while in PBL | Answers on its *old* address (documented limitation) |
| B11 | Dongle-flash a plain release `.bin` with CubeProgrammer defaults | `scan` shows "no bus-update entry". Check the PBL page with CubeProgrammer read: still `PBL1` at 0x08007C00. |

**Measure the bus first:** `photon-busflash.sh ping --addr N` gives the M485 round-trip time (min / median / max, lost replies) and the expected update time per feeder.

**Phase C: one-time fleet migration**
- Each feeder gets `*-factory.bin` with the documented dongle procedure, one last manual flash. `busflash/dongle.sh flash IMAGE` does it from the command line with the docs' settings (UART 9600, even parity):
  - It first saves the feeder's UID and **complete original firmware** (`~/claude/opulo/_feeder-backups/<uuid>-<time>.bin`, sha256), so `dongle.sh restore` can put back exactly what was there.
  - It then writes, verifies and starts the image. The motherboard port is never used.
  - Tested against a simulated programmer CLI (`busflash/tests/test_dongle.sh`).
- Check whether the dongle reaches the programming holes with the feeder mounted (study §9.5).

**Phase D: routine updates**
```
photon-busflash.sh scan
photon-busflash.sh flash out/photon-vX-pbl1.bin --all     # every feeder whose version differs
photon-busflash.sh flash out/photon-vX-pbl1.bin --addr 3 7
```

**Recovery**
- A magenta LED or `IN BUS LOADER` means `recover IMAGE --addr N`.
- If the feeder is dark or dead (power lost inside the ~65 ms commit window, or the feeder was moved), use the dongle + factory image as today.

## 6. Re-running the simulation

```
cd busflash
make -C bootloader                       # target build + size check
cc -O1 -std=gnu11 -o sim/pbl_sim sim/pbl_sim_main.c
python3 tests/run_sim_tests.py photon-lumenpnp-v1.0.7.bin   # needs pyserial
```

The simulation does **not** cover:
- real USART/GPIO register behaviour;
- the app's `HAL_DeInit` jump;
- Marlin's real RS-485 turnaround and timeouts;
- flash timing.

Those are the B-tests above.

## 7. Bench tools: logging and feed timing (2026-11-06)
- **Logging:** `photon_busflash.py --log FILE` (the `photon-busflash.sh` wrapper always passes one: `~/claude/opulo/_logs/busflash-*.log`) records **every G-code line and reply** (DEBUG) and every result line. Before 2026-11-06 the file stayed empty.
- **Feed timing:** `host/photon_feedtest.py --addr N [--mm 4] [--count 3] [--back] [--restart] [--log FILE]` measures how a feeder moves. OpenPnP must be disconnected.
  - It **moves the tape** of that one feeder. A feed command is sent exactly once (never retried), so a lost reply cannot cause a second feed.
  - The motion time is measured from the feed command's reply to the first answered `MOVE_FEED_STATUS` (resolution about one bus round trip, ~60 ms).
  - `--restart` restarts the feeder application through its bus loader (enter + RESET, nothing erased), so the feeds start from the power-on state.
- **Bench result:** v1.0.7 switches into its *beefy* mode (faster, more aggressive drive) after any feed that needed a retry, or by a double click on the top button, and stays there until power-off.

  | Feeder | 4 mm feed |
  |---|---|
  | 50, v1.0.7-pbl1, beefy | 2.2 s |
  | 50, v1.0.7-pbl1, after `--restart` | 3.14 s |
  | 48, official v1.0.7 | 3.35 s |

  The motion code of v1.0.7-pbl1 is instruction-identical to official v1.0.7 (only shifted constant addresses), so the difference is beefy mode, not the build.
