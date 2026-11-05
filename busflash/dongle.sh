#!/bin/sh
# One-time / rescue flashing of a Photon feeder through the programming dongle (ROM UART
# bootloader), with STM32_Programmer_CLI instead of the CubeProgrammer GUI.
# Same settings as the Opulo docs: UART, 9600 baud, even parity.  Enter the bootloader as the
# docs describe (hold BOOT while inserting the dongle, or both buttons until red/blue blink).
#
#   dongle.sh ports                       candidate dongle ports (the LumenPnP motherboard is excluded)
#   dongle.sh backup  [--port P]          save UID + the complete 32 KB flash (exact rollback)
#   dongle.sh flash IMAGE [--port P]      backup, write + verify IMAGE at 0x08000000, start it
#   dongle.sh restore BACKUP [--port P]   write a backup back (e.g. the original firmware)
#   options: --no-backup (flash only), --baud N (default 9600)
# Backups: $FEEDER_BACKUP_DIR (default ~/claude/opulo/_feeder-backups), named <uuid>-<time>.bin;
# the uuid is the feeder's hardware id as OpenPnP shows it.
set -e
CLI=${STM32_CLI:-/opt/st/stm32cubeclt_1.21.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI}
BK=${FEEDER_BACKUP_DIR:-$HOME/claude/opulo/_feeder-backups}
FLASH_SIZE=32768
BAUD=9600
PORT=""
BACKUP=1

die() { echo "dongle.sh: $*" >&2; exit 1; }

ports() {   # ttyACM/ttyUSB devices that are not the LumenPnP motherboard (0483:5740)
    for d in /dev/ttyACM* /dev/ttyUSB*; do
        [ -e "$d" ] || continue
        props=$(udevadm info -q property -n "$d" 2>/dev/null || true)
        vid=$(echo "$props" | sed -n 's/^ID_VENDOR_ID=//p'); pid=$(echo "$props" | sed -n 's/^ID_MODEL_ID=//p')
        [ "$vid:$pid" = "0483:5740" ] && continue
        echo "$d $vid:$pid $(echo "$props" | sed -n 's/^ID_MODEL=//p')"
    done
}

pick_port() {
    [ -n "$PORT" ] && return
    n=$(ports | wc -l)
    [ "$n" -eq 1 ] || { ports >&2; die "found $n candidate ports - give --port /dev/ttyXXX"; }
    PORT=$(ports | cut -d' ' -f1)
}

cli() {   # run STM32_Programmer_CLI connected to the feeder's ROM bootloader
    "$CLI" -c port="$PORT" br="$BAUD" P=EVEN "$@" 2>&1 | sed 's/\x1b\[[0-9;]*m//g'
}

uuid() {  # 96-bit UID as Photon/OpenPnP show it (ArduinoUniqueID: big endian per 32-bit word)
    out=$(cli -r32 0x1FFFF7AC 12)
    echo "$out" | grep -qiE "error|fail" && { echo "$out" >&2; die "cannot read the UID (bootloader not entered?)"; }
    echo "$out" | sed -n 's/.*0x1FFFF7AC[^:]*:\s*//Ip' | head -1 | tr -d ' ' | tr 'A-F' 'a-f'
}

backup() {
    mkdir -p "$BK"
    id=$(uuid)
    [ ${#id} -eq 24 ] || die "unexpected UID '$id'"
    f="$BK/$id-$(date +%Y%m%d-%H%M%S-%N).bin"     # unique: a stale file must never pass as backup
    rm -f "$f"
    out=$(cli -u 0x08000000 $FLASH_SIZE "$f")
    if [ ! -s "$f" ] || [ "$(stat -c %s "$f")" -ne $FLASH_SIZE ]; then
        echo "$out" >&2
        die "backup failed (read protected?). Nothing written. Use --no-backup to flash anyway."
    fi
    sha256sum "$f" > "$f.sha256"
    echo "backup: $f (uuid $id)"
}

check_image() {
    [ -f "$1" ] || die "no such file: $1"
    sz=$(stat -c %s "$1")
    [ "$sz" -le $FLASH_SIZE ] || die "$1 is $sz B > $FLASH_SIZE B"
    if [ "$sz" -eq $FLASH_SIZE ]; then
        magic=$(dd if="$1" bs=1 skip=31744 count=4 2>/dev/null)
        [ "$magic" = "PBL1" ] && echo "image: factory image (app + bus loader)" || echo "image: full 32 KB image (backup?)"
    else
        echo "image: $sz B application only (no bus loader - bus updates will not work afterwards)"
    fi
}

write() {
    out=$(cli -w "$1" 0x08000000 -v)
    echo "$out" | tail -4
    echo "$out" | grep -qiE "verif.*(success|ok)|Download verified successfully" || die "write/verify FAILED - the feeder stays in the bootloader; run the command again"
    cli -g 0x08000000 > /dev/null || true
    echo "written and verified: $1 -> feeder started"
}

CMD=$1; [ -n "$CMD" ] && shift
ARG=""
while [ $# -gt 0 ]; do
    case $1 in
        --port) PORT=$2; shift 2 ;;
        --baud) BAUD=$2; shift 2 ;;
        --no-backup) BACKUP=0; shift ;;
        *) ARG=$1; shift ;;
    esac
done
[ -x "$CLI" ] || die "STM32_Programmer_CLI not found ($CLI)"
case $CMD in
    ports) ports ;;
    backup) pick_port; backup ;;
    flash) [ -n "$ARG" ] || die "usage: dongle.sh flash IMAGE"; check_image "$ARG"; pick_port
           [ $BACKUP = 1 ] && backup; write "$ARG" ;;
    restore) [ -n "$ARG" ] || die "usage: dongle.sh restore BACKUP.bin"; check_image "$ARG"; pick_port; write "$ARG" ;;
    *) sed -n '2,15p' "$0"; exit 2 ;;
esac
