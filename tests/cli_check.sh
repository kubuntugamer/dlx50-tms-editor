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

# 5) --set edits one cell; --list shows the new value.
cp "$WORK/ref.TMS" "$WORK/set.TMS"
"$E" "$WORK/set.TMS" --set PLU 0 2 "Edited Name" $N >/dev/null || bad "--set exit"
"$E" "$WORK/set.TMS" --list PLU $N | sed -n 2p | cut -f3 | grep -q '^Edited Name$' \
  && ok "--set: cell edited and shown in --list" || bad "--set did not change the cell"

# 6) --set with a bad row is refused and does not change the file.
cp "$WORK/ref.TMS" "$WORK/badset.TMS"
"$E" "$WORK/badset.TMS" --set PLU 999 2 "x" $N >/dev/null 2>&1 && bad "--set bad row should fail" || ok "--set bad row refused"
cmp -s "$WORK/ref.TMS" "$WORK/badset.TMS" && ok "--set bad row left file unchanged" || bad "--set bad row changed the file"

# 7) --delete-row removes one row by index.
cp "$WORK/ref.TMS" "$WORK/delrow.TMS"
"$E" "$WORK/delrow.TMS" --delete-row PLU 0 $N >/dev/null || bad "--delete-row exit"
[ "$("$E" "$WORK/delrow.TMS" --list PLU $N | wc -l)" -eq "$("$E" "$WORK/ref.TMS" --list PLU $N | wc -l)" ] \
  && bad "--delete-row did not remove a row" || ok "--delete-row removed one row"

# 8) --save-as writes elsewhere and leaves the source unchanged.
cp "$WORK/ref.TMS" "$WORK/src.TMS"
"$E" "$WORK/src.TMS" --set PLU 0 2 "Saved As" --save-as "$WORK/out.TMS" $N >/dev/null || bad "--save-as exit"
cmp -s "$WORK/ref.TMS" "$WORK/src.TMS" && ok "--save-as left the source unchanged" || bad "--save-as changed the source"
"$E" "$WORK/out.TMS" --list PLU $N | sed -n 2p | cut -f3 | grep -q '^Saved As$' \
  && ok "--save-as wrote the edit to the new file" || bad "--save-as output missing edit"

# 9) --add PLU with --price stores the price in the price column.
cp "$WORK/ref.TMS" "$WORK/price.TMS"
"$E" "$WORK/price.TMS" --add PLU 9 4099 "Priced Item" --price 123,4 $N >/dev/null || bad "--add --price exit"
"$E" "$WORK/price.TMS" --list PLU $N | tail -1 | cut -f4 | grep -q '^123,4$' \
  && ok "--add --price: price stored" || bad "--add --price not stored"

# 9b) --add PLU --csv with --price stores the price on each row.
cp "$WORK/ref.TMS" "$WORK/csvprice.TMS"
printf 'code,name\n5001,Csv Item\n' > "$WORK/items.csv"
"$E" "$WORK/csvprice.TMS" --add PLU --csv "$WORK/items.csv" --price 55,5 $N >/dev/null || bad "--add --csv --price exit"
"$E" "$WORK/csvprice.TMS" --list PLU $N | tail -1 | cut -f4 | grep -q '^55,5$' \
  && ok "--add --csv --price: price stored" || bad "--add --csv --price not stored"

exit $fail
