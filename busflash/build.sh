#!/bin/sh
# Build the Photon application (this branch, with the bus-loader entry), the
# PBL bus loader and the one-time factory image; check all sizes.
#
#   busflash/build.sh v1.0.7-pbl1
#
# Needs PlatformIO (`uv tool install platformio`) and the STM32CubeCLT GCC
# (bootloader/Makefile, TOOLS=...).  Outputs go to busflash/out/.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
VER=${1:?usage: build.sh VERSION_STRING   (e.g. v1.0.7-pbl1)}
PIO=${PIO:-pio}
PY=${PY:-python3}
OUT="$HERE/out"

cd "$HERE/../photon"
# board_upload.maximum_size = 31744 in platformio.ini -> link fails instead of
# silently overlapping the PBL page
VERSION_STRING="$VER" "$PIO" run -e photon-bmp

mkdir -p "$OUT"
cp .pio/build/photon-bmp/firmware.bin "$OUT/photon-$VER.bin"
cp .pio/build/photon-bmp/firmware.elf "$OUT/photon-$VER.elf"
make -C "$HERE/bootloader"

SZ=$(stat -c %s "$OUT/photon-$VER.bin")
echo "app: $SZ / 31744 B  (headroom $((31744 - SZ)) B)"
"$PY" "$HERE/host/photon_busflash.py" info "$OUT/photon-$VER.bin"
"$PY" "$HERE/host/photon_busflash.py" make-factory "$OUT/photon-$VER.bin" \
    "$HERE/bootloader/build/pbl.bin" "$OUT/photon-$VER-factory.bin"
(cd "$OUT" && sha256sum "photon-$VER.bin" "photon-$VER-factory.bin") | tee "$OUT/photon-$VER.sha256"
sha256sum "$HERE/bootloader/build/pbl.bin" | tee -a "$OUT/photon-$VER.sha256"
