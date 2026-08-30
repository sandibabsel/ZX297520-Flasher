#!/bin/sh
# End-to-end smoke test against the simulated device. No hardware needed.
set -e

cd "$(dirname "$0")/.."
PASS=0
FAIL=0

ok()   { PASS=$((PASS+1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  FAIL %s\n' "$1"; }
check() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (got '$2', want '$3')"; fi; }

TMP=$(mktemp -d)
cleanup() {
  rc=$?
  if [ -n "$SIMPID" ]; then kill "$SIMPID" 2>/dev/null || true; fi
  rm -rf "$TMP"
  exit $rc
}
trap cleanup EXIT

echo "== offline =="

# known-good vectors computed from the reference partition.bin
check "xor checksum round trip" \
  "$(./zxdl mkptable examples/partition_wf7520.ini "$TMP/pt.bin" >/dev/null && \
     ./zxdl ptable "$TMP/pt.bin" | grep -c 'MISMATCH')" "0"

check "partition entry count" \
  "$(./zxdl ptable "$TMP/pt.bin" | awk '/^entries/{print $2}')" "12"

check "generated table is 512 bytes" \
  "$(stat -c %s "$TMP/pt.bin")" "512"

# CRC32 self-check: the protocol variant of "123456789" (no final XOR).
printf '123456789' > "$TMP/check"
check "protocol crc32 vs standard" \
  "$(./zxdl crc "$TMP/check" | awk '{print $2}')" "CBF43926"

echo "== against simulated device =="

./simdev > "$TMP/pts" 2>"$TMP/sim.log" &
SIMPID=$!
sleep 0.4
PTS=$(cat "$TMP/pts")
if [ -z "$PTS" ]; then
  bad "simulator failed to start"
  echo "Result: $PASS passed, $FAIL failed"
  exit 1
fi
ok "simulator on $PTS"

check "getvar plat" \
  "$(./zxdl -p "$PTS" getvar plat 2>/dev/null | grep -c 'OKAY WF7520')" "1"

# write a random blob, read it back, compare
dd if=/dev/urandom of="$TMP/blob" bs=1000 count=10 2>/dev/null
./zxdl -p "$PTS" -y write testpart "$TMP/blob" >/dev/null 2>&1 \
  && ok "write accepted (crc verified by device)" \
  || bad "write rejected"

./zxdl -p "$PTS" read testpart 0 10000 "$TMP/back" >/dev/null 2>&1 || true
if cmp -s "$TMP/blob" "$TMP/back"; then
  ok "read back matches what was written"
else
  bad "read back differs"
fi

./zxdl -p "$PTS" ptupload "$TMP/pt.bin" -y >/dev/null 2>&1 \
  && ok "partition table accepted by device" \
  || bad "partition table rejected"

./zxdl -p "$PTS" -y erase auto >/dev/null 2>&1 \
  && ok "erase auto" || bad "erase auto"

./zxdl -p "$PTS" reboot >/dev/null 2>&1 \
  && ok "reboot" || bad "reboot"

echo
echo "Result: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
