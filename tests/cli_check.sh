#!/usr/bin/env bash
# End-to-end check of the headless CLI on a scratch copy of the reference file.
# Usage: tests/cli_check.sh <tms_editor binary> <reference.TMS>
set -u
E="$1"
REF="$2"
N="--no-update-check"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fail=0

ok()   { echo "ok  : $1"; }
bad()  { echo "FAIL: $1"; fail=1; }

cp "$REF" "$WORK/ref.TMS"

# 1) PLU add then delete returns the file to byte-identical state.
cp "$WORK/ref.TMS" "$WORK/plu.TMS"
"$E" "$WORK/plu.TMS" --add PLU 9 4099 "Beef Tendon" $N >/dev/null || bad "PLU add exit"
"$E" "$WORK/plu.TMS" --list PLU $N | grep -q "4099" && ok "PLU add visible in --list" || bad "PLU add not in --list"
"$E" "$WORK/plu.TMS" --delete PLU 4099 $N >/dev/null || bad "PLU delete exit"
cmp -s "$WORK/ref.TMS" "$WORK/plu.TMS" && ok "PLU add+delete is byte-identical" || bad "PLU add+delete changed the file"

# 2) BAR add shows the barcode string in the barcode column.
cp "$WORK/ref.TMS" "$WORK/bar.TMS"
"$E" "$WORK/bar.TMS" --add BAR 10 "" "B201E500L500A000A000A999" $N >/dev/null || bad "BAR add exit"
"$E" "$WORK/bar.TMS" --list BAR $N | tail -1 | grep -q $'^10\tB201E500L500A000A000A999$' \
  && ok "BAR add: barcode shown in Barcode data column" || bad "BAR barcode not in column 2"
grep -q $'^BAR\t10\t\t0\t0\t0\t0\tB201E500L500A000A000A999\t' "$WORK/bar.TMS" \
  && ok "BAR add: stored in the file's BAR layout" || bad "BAR row not in file layout"

# 3) DPT add lands as a new DPT row.
cp "$WORK/ref.TMS" "$WORK/dpt.TMS"
"$E" "$WORK/dpt.TMS" --add DPT 9 "" "New Dept" $N >/dev/null || bad "DPT add exit"
"$E" "$WORK/dpt.TMS" --list DPT $N | tail -1 | grep -q $'^9\tNew Dept\t*$' \
  && ok "DPT add visible in --list" || bad "DPT add not in --list"

# 4) Reads leave the file untouched.
cp "$WORK/ref.TMS" "$WORK/read.TMS"
for s in PLU DPT CLS UNT BAR TMS TMT INF; do "$E" "$WORK/read.TMS" --list "$s" $N >/dev/null || bad "list $s exit"; done
cmp -s "$WORK/ref.TMS" "$WORK/read.TMS" && ok "--list leaves the file unchanged" || bad "--list modified the file"

exit $fail
