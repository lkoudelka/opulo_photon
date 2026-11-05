#!/bin/sh
# dongle.sh against a fake STM32_Programmer_CLI (call log + simulated outputs).
HERE=$(cd "$(dirname "$0")" && pwd); W=$(mktemp -d ${SIM_WORKDIR:+-p "$SIM_WORKDIR"}); PASS=0; FAIL=0
ok() { if eval "$2"; then PASS=$((PASS+1)); echo "  [PASS] $1"; else FAIL=$((FAIL+1)); echo "  [FAIL] $1"; fi; }
cat > "$W/fakecli" <<'F'
#!/bin/sh
echo "$*" >> "$FAKE_LOG"
case "$*" in
  *-r32*) [ -n "$FAKE_NOUID" ] && { echo "Error: no response"; exit 1; }
          echo "0x1FFFF7AC : 00350031 3335510A 34363537" ;;
  *-u*) [ -n "$FAKE_RDP" ] && { echo "Error: read protected"; exit 1; }
        f=$(echo "$*" | awk '{print $NF}'); head -c 32768 /dev/urandom > "$f"; echo "Upload completed" ;;
  *-w*) [ -n "$FAKE_BADVERIFY" ] && { echo "Error: data mismatch found"; exit 1; }
        echo "Download in Progress:"; echo "File download complete"; echo "Download verified successfully" ;;
  *-g*) echo "Start operation achieved successfully" ;;
esac
F
chmod +x "$W/fakecli"
export STM32_CLI="$W/fakecli" FEEDER_BACKUP_DIR="$W/bk" FAKE_LOG="$W/log"
head -c 31744 /dev/zero > "$W/factory.bin"; printf 'PBL1' >> "$W/factory.bin"; head -c 1020 /dev/zero >> "$W/factory.bin"
head -c 24740 /dev/zero > "$W/app.bin"
out=$("$HERE/../dongle.sh" flash "$W/factory.bin" --port /dev/ttyFAKE 2>&1); rc=$?
ok "flash factory image succeeds" '[ $rc -eq 0 ]'
ok "factory image recognised" 'echo "$out" | grep -q "factory image"'
ok "backup named by UUID, 32 KB, with sha256" 'ls "$W/bk"/0035003133355100a34363537* >/dev/null 2>&1 || ls "$W/bk"/003500313335510a34363537-*.bin >/dev/null 2>&1'
f=$(ls "$W/bk"/*.bin | head -1); ok "backup size 32768" '[ "$(stat -c %s "$f")" -eq 32768 ] && [ -f "$f.sha256" ]'
ok "order: UID, backup, write+verify, go; 9600 even parity" 'grep -n "" "$W/log" | tr "\n" " " | grep -qE "1:.*port=/dev/ttyFAKE br=9600 P=EVEN -r32.* 2:.*-u 0x08000000 32768.* 3:.*-w .*factory.bin 0x08000000 -v.* 4:.*-g 0x08000000"'
: > "$W/log"; out=$(FAKE_RDP=1 "$HERE/../dongle.sh" flash "$W/app.bin" --port /dev/ttyFAKE 2>&1); rc=$?
ok "read-protected: fails, nothing written" '[ $rc -ne 0 ] && ! grep -q -- "-w " "$W/log" && echo "$out" | grep -q "Nothing written"'
: > "$W/log"; out=$("$HERE/../dongle.sh" flash "$W/app.bin" --port /dev/ttyFAKE --no-backup 2>&1); rc=$?
ok "--no-backup flashes without reading; app-only image warned" '[ $rc -eq 0 ] && ! grep -q -- "-u " "$W/log" && echo "$out" | grep -q "no bus loader"'
: > "$W/log"; out=$(FAKE_BADVERIFY=1 "$HERE/../dongle.sh" restore "$f" --port /dev/ttyFAKE 2>&1); rc=$?
ok "verify failure reported" '[ $rc -ne 0 ] && echo "$out" | grep -q "FAILED"'
out=$(FAKE_NOUID=1 "$HERE/../dongle.sh" backup --port /dev/ttyFAKE 2>&1); rc=$?
ok "no bootloader: clear message" '[ $rc -ne 0 ] && echo "$out" | grep -q "bootloader not entered"'
head -c 40000 /dev/zero > "$W/big.bin"; out=$("$HERE/../dongle.sh" flash "$W/big.bin" --port /dev/ttyFAKE 2>&1); rc=$?
ok "oversized image refused" '[ $rc -ne 0 ]'
echo; echo "$PASS/$((PASS+FAIL)) checks passed"; [ $FAIL -eq 0 ]
