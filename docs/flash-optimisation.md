# Photon firmware: memory review and optimisations (8/12 mm feeders, STM32F031x6)

*Branch `feature/flash-diet`, based on release **v1.0.7** (= upstream `main` 8bf76b1). Reviewed 2026-11-05.*

**Constraints given for this work:**
- no functional change;
- no big loss of readability, scalability or updatability;
- no rewrite into LL drivers or assembly.

## 1. Method

- **Reproducible baseline.** The v1.0.7 source builds **byte-identical** to the official `photon-lumenpnp-v1.0.7.bin`, with the pinned platform `ststm32@19.7.1` and the Arduino core `~4.21200.0`. Every number below is an exact measurement, not an estimate.
- **Where the flash goes** (v1.0.7, from the linker map):

  | Part | Size |
  |---|---|
  | STM32 core HAL/LL wrapper (UART, timers/PWM, RCC, GPIO) | 13.9 KB |
  | Arduino core | 3.6 KB |
  | App: PhotonFeeder | 3.4 KB |
  | App: main | 1.5 KB |
  | App: protocol | 1.0 KB |
  | Libraries: RS485 | 2.5 KB |
  | Libraries: OneWire | 1.3 KB |
  | newlib | 1.2 KB |
  | libgcc | 0.8 KB |

- **Behavioural equivalence for source changes** (`tools/equivalence/run.sh <base-rev>`):
  - `PhotonFeeder.cpp` is compiled on the PC against a mock Arduino API with a deterministic motor/encoder model.
  - It runs the same scenarios for the base revision and the working tree: 5 friction levels × beefy on/off × 18 forward distances and 10 mixed forward/backward moves, a stuck motor (retries → COULDNT_REACH), the vendor options, the LEDs, `"v1.0.7"` and `""` (debug) version builds.
  - It compares the complete trace of pin writes, PWM values and delays with virtual timestamps (64,465 lines).
  - Self-check: a 1 ms peel-timing change and a stall-step change are both detected.
- **Build-option changes** (core / compiler) are checked with symbol-table diffs, plus the hardware regression list in §4.

## 2. Implemented (each one commit, measured on v1.0.7)

| # | Commit | Flash | RAM | Verification |
|---|---|---|---|---|
| 1 | Share the common blocks of the three forward-feed sequences and the four retry loops (named helpers; phase logic unchanged) | **−692 B** | 0 | Equivalence: identical |
| 2 | Version text as `const char*` instead of Arduino `String` | **−500 B** | 0 | Equivalence: identical (both version paths) |
| 3 | `-D HWSERIAL_NONE`: no unused default `Serial1` (same USART1 as the bus object) | **−340 B** | **−344 B** | nm diff: only `Serial1`, its init and the unused constructor removed |
| 4 | `PhotonFeeder::set_rgb()` made `static` (fixes the call through a null pointer in `setup()`) | −56 B | 0 | Equivalence: identical |
| 5 | `-flto`, the core's own "Smallest (-Os) with LTO" option | **−4,788 B** | +156 B (core keeps `serial_debug`) | **Needs HW regression (§4)** |
| | **Total** | **30,840 → 24,464 B (−6,376 B, −20.7 %)** | **1,932 → 1,744 B** | |

**With the bus-loader entry** (`dev` = flash-diet + bus-loader), the app is **24,740 B against the 31,744 B cap: 7,004 B free** instead of 692 B.

Readability is preserved or improved:
- Each sequence still reads top-down.
- The drive phases are where they were.
- The repeated 25-line comment blocks are now one comment.
- A wrong comment ("ONLY USED FOR MOVEMENTS >= 2mm and <= 4mm" on the < 2 mm sequence) is fixed.

**Commits 1–4 are independent of the compiler.** If LTO is rejected after the hardware test, they still save 1,588 B flash and 344 B RAM.

## 3. Review findings (not changed: they would alter behaviour, or need a decision)

| Finding | Where | Effect | Proposal |
|---|---|---|---|
| `millis()` comparisons written as `start + timeout > millis()` are not wrap-safe | all feed loops, buttons | After 49.7 days of uptime, one feed or button press can misbehave at the wrap | `millis() - start < timeout` (separate commit, HW test) |
| `UNINITIALIZED_FEEDERS_RESPOND` falls through into `default:` (missing `break`) | PhotonFeederProtocol.cpp | Harmless today (`default` does nothing) | Add `break` |
| Unused parameter `first_attempt`, unused locals `signed_mm`, `first_time_phase_two` (LT2), `len` | PhotonFeeder.cpp | Noise only (the compiler removes them) | Clean up with the next functional change |
| `checkLoaded()` and `lifetime()` are dead code (call commented out / never called) | PhotonFeeder.cpp, main.cpp | Not linked (no flash cost) | Keep, or move behind `#ifdef` as test tools |
| `volatile int coast / test` "breakpoint" locals in the settle path | PhotonFeeder.cpp | Few bytes; debugging aid | Keep, or wrap in `#ifdef PHOTON_DEBUG_COAST` |
| Floating ~RE/DE pins (hardware) handled by a GPIO-lock "HACK" before the ROM-bootloader jump | bootloader.cpp | Documented workaround | Hardware pull resistors on a respin |

## 4. Hardware regression before `main` (mandatory for commit 5, recommended for all)

1. Feed forward 1, 2, 4, 8, 12, 20 mm, ×20 each: position error compared with v1.0.7. Also backward 2 and 4 mm.
2. Thick tape: beefy auto-switch after a failed feed; manual beefy toggle (double click on the top button, LED green/red).
3. Buttons:
   - short and long press, both directions;
   - both buttons for 2 s: version blink;
   - 4 s: red/blue blink;
   - 6 s: ROM bootloader entry (dongle flashing still works).
4. LEDs at boot: red when there is no slot, blue when the slot is unprogrammed.
5. OpenPnP: feeder search, initialize, feed in a job, slot address programming.
6. 1-wire floor address read on 3 different slots.
7. Long run: 1,000 feeds without COULDNT_REACH on good tape.

## 5. Further candidates (not implemented)

| Candidate | Est. saving | Why not now |
|---|---|---|
| Replace the RS485 / Packetizer library by a minimal Photon framer | 1–1.5 KB | Third-party dependency, shared with Marlin; changes the bus code |
| `analogWrite` → direct HardwareTimer/LL PWM | 1–2 KB | Excluded: LL rewrite |
| OneWire → small DS28E07-only routine | ~0.6 KB | Rewrite of a working driver |
| newlib stdio (`__sf`, 312 B RAM) | 312 B RAM | Pulled in by newlib re-entrancy (malloc); needs libc-level hacks |
| `__divsi3` removal (rounding division) | ~0.4 KB | Small gain, less readable |

## 6. Reproduce

```
cd photon && VERSION_STRING=v1.0.7 pio run -e photon-bmp      # size printed at the end
tools/equivalence/run.sh v1.0.7                              # behavioural equivalence vs the release
```
