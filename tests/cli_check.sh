#!/usr/bin/env bash
# End-to-end check of the headless CLI on scratch copies of reference files.
# Usage: tests/cli_check.sh <tms_editor binary> [reference.TMS]
#        ROUNDTRIP_CHECK=<roundtrip_check binary> adds the byte-exact
#        open->save round-trip on every fixture (CI sets it).
#
# Without a reference the script generates a synthetic layout-A .TMS itself,
# so it runs on any machine (no scale file needed).  Either way it also
# generates a layout-B file at the end — the other real-world row shapes (BAR
# rows with no blank field after the row index, PLU rows whose name follows
# the number directly) — so both layouts stay covered.  See issue #9.
set -u
E="$1"
REF="${2:-}"
N="--no-update-check"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fail=0

ok()   { echo "ok  : $1"; }
bad()  { echo "FAIL: $1"; fail=1; }

if [ -n "$REF" ]; then
  cp "$REF" "$WORK/ref.TMS"
else
  # Synthetic layout-A fixture: BAR rows as
  #   BAR <idx> <blank> 0 0 0 0 <barcode>, PLU rows with a code field.
  printf 'HEADER\tDLX50\r\n'                         > "$WORK/ref.TMS"
  printf 'DWL\tPLU\t\r\n'                            >> "$WORK/ref.TMS"
  printf 'PLU\t1\t1001\t0\t0\t799,2\tBeef Tendon\t0\t0\t\r\n' >> "$WORK/ref.TMS"
  printf 'END\tPLU\t\r\n\r\n'                       >> "$WORK/ref.TMS"
  printf 'DWL\tDPT\t\r\nDPT\t1\tFresh Fruit\t\r\nEND\tDPT\t\r\n\r\n' >> "$WORK/ref.TMS"
  printf 'DWL\tCLS\t\r\nEND\tCLS\t\r\n\r\n'         >> "$WORK/ref.TMS"
  printf 'DWL\tUNT\t\r\nEND\tUNT\t\r\n\r\n'         >> "$WORK/ref.TMS"
  printf 'DWL\tBAR\t\r\n'                            >> "$WORK/ref.TMS"
  printf 'BAR\t1\t\t0\t0\t0\t0\tB201E500L500A000A000A001\t\r\n' >> "$WORK/ref.TMS"
  printf 'END\tBAR\t\r\n\r\n'                       >> "$WORK/ref.TMS"
  printf 'DWL\tTMS\t\r\nEND\tTMS\t\r\n\r\n'         >> "$WORK/ref.TMS"
  printf 'DWL\tTMT\t\r\nEND\tTMT\t\r\n\r\n'         >> "$WORK/ref.TMS"
  printf 'DWL\tINF\t\r\nEND\tINF\t\r\n\r\n'         >> "$WORK/ref.TMS"
  printf 'END\tECS\t\r\n'                            >> "$WORK/ref.TMS"
fi

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

# 10) Layout B: the other real-world row shapes (issue #9).  BAR rows have no
#     blank field after the row index, so the barcode sits at offset+5 — a
#     fixed offset+6 read the wrong field (blank barcode column) and wrote a
#     second one (two barcodes in the row).
printf 'HEADER\tDLX50\r\n'                           > "$WORK/layoutB.TMS"
printf 'DWL\tPLU\t\r\n'                              >> "$WORK/layoutB.TMS"
printf 'PLU\t1\tWidget Alpha\t1\t0\t0\t0\t0\t0\t\r\n' >> "$WORK/layoutB.TMS"
printf 'PLU\t2\tWidget Beta\t1\t0\t0\t0\t0\t0\t\r\n'  >> "$WORK/layoutB.TMS"
printf 'END\tPLU\t\r\n\r\n'                         >> "$WORK/layoutB.TMS"
printf 'DWL\tBAR\t\r\n'                              >> "$WORK/layoutB.TMS"
printf 'BAR\t1\t0\t0\t0\t0\tB000L000A000A000A000\t\r\n' >> "$WORK/layoutB.TMS"
printf 'BAR\t2\t0\t0\t0\t0\tB001L001A000A000A000\t\r\n' >> "$WORK/layoutB.TMS"
printf 'END\tBAR\t\r\n\r\n'                         >> "$WORK/layoutB.TMS"
printf 'DWL\tDPT\t\r\nEND\tDPT\t\r\n\r\n'           >> "$WORK/layoutB.TMS"
printf 'DWL\tCLS\t\r\nEND\tCLS\t\r\n\r\n'           >> "$WORK/layoutB.TMS"
printf 'DWL\tUNT\t\r\nEND\tUNT\t\r\n\r\n'           >> "$WORK/layoutB.TMS"
printf 'DWL\tTMS\t\r\nEND\tTMS\t\r\n\r\n'           >> "$WORK/layoutB.TMS"
printf 'DWL\tTMT\t\r\nEND\tTMT\t\r\n\r\n'           >> "$WORK/layoutB.TMS"
printf 'DWL\tINF\t\r\nEND\tINF\t\r\n\r\n'           >> "$WORK/layoutB.TMS"
printf 'END\tECS\t\r\n'                              >> "$WORK/layoutB.TMS"

cp "$WORK/layoutB.TMS" "$WORK/lb.TMS"
"$E" "$WORK/lb.TMS" --list BAR $N >/dev/null || bad "layout B: list BAR exit"
"$E" "$WORK/lb.TMS" --list BAR $N | tail -1 | cut -f2 | grep -q '^B001L001A000A000A000$' \
  && ok "layout B: barcode shown in Barcode data column" || bad "layout B: barcode column empty"
cmp -s "$WORK/layoutB.TMS" "$WORK/lb.TMS" && ok "layout B: --list leaves the file unchanged" || bad "layout B: --list modified the file"
"$E" "$WORK/lb.TMS" --add BAR 3 "" "B002L002A000A000A000" $N >/dev/null || bad "layout B: add BAR exit"
grep -q $'^BAR\t3\t0\t0\t0\t0\tB002L002A000A000A000\t' "$WORK/lb.TMS" \
  && ok "layout B: barcode stored in its own field" || bad "layout B: barcode in the wrong field"
grep -q $'^BAR\t2\t0\t0\t0\t0\tB001L001A000A000A000\t\r$' "$WORK/lb.TMS" \
  && ok "layout B: earlier rows keep exactly one barcode" || bad "layout B: an earlier row changed"
"$E" "$WORK/lb.TMS" --delete-row BAR 2 $N >/dev/null || bad "layout B: delete-row exit"
cmp -s "$WORK/layoutB.TMS" "$WORK/lb.TMS" \
  && ok "layout B: add+delete-row is byte-identical" || bad "layout B: add+delete-row changed the file"
"$E" "$WORK/lb.TMS" --list PLU $N | sed -n 2p | cut -f3 | grep -q '^Widget Alpha$' \
  && ok "layout B: PLU name column" || bad "layout B: PLU name column"
cp "$WORK/layoutB.TMS" "$WORK/lbplu.TMS"
"$E" "$WORK/lbplu.TMS" --add PLU 3 "" "Third Widget" $N >/dev/null || bad "layout B: add PLU exit"
"$E" "$WORK/lbplu.TMS" --delete PLU 3 $N >/dev/null || bad "layout B: delete PLU by number"
cmp -s "$WORK/layoutB.TMS" "$WORK/lbplu.TMS" \
  && ok "layout B: PLU add+delete is byte-identical" || bad "layout B: PLU add+delete changed the file"

# 11) Issue #10: a product code the file cannot store must fail loudly
#     instead of exiting 0 and silently dropping the value.
cp "$WORK/layoutB.TMS" "$WORK/lbcode.TMS"
if "$E" "$WORK/lbcode.TMS" --add PLU 3 4126 "Coded Widget" $N > /dev/null 2> "$WORK/err.txt"; then
  bad "layout B: --add with a product code should have failed"
else
  ok "layout B: --add with a product code refused"
fi
cmp -s "$WORK/layoutB.TMS" "$WORK/lbcode.TMS" \
  && ok "layout B: refused add left the file unchanged" || bad "layout B: refused add wrote to the file"
grep -qi "product code" "$WORK/err.txt" \
  && ok "layout B: refusal names the missing code field" || bad "layout B: refusal message unclear"

cp "$WORK/layoutB.TMS" "$WORK/lbcsv.TMS"
printf 'code,name\n5001,Csv Item\n' > "$WORK/b.csv"
if "$E" "$WORK/lbcsv.TMS" --add PLU --csv "$WORK/b.csv" $N > /dev/null 2>&1; then
  bad "layout B: --csv carrying a code column should have failed"
else
  ok "layout B: --csv carrying a code column refused"
fi
cmp -s "$WORK/layoutB.TMS" "$WORK/lbcsv.TMS" \
  && ok "layout B: refused csv left the file unchanged" || bad "layout B: refused csv wrote to the file"

# 12) Optional byte-exact round-trip guard: set ROUNDTRIP_CHECK to the
#     roundtrip_check binary and every fixture above is also verified to
#     survive open->save byte-for-byte.  CI does this on every push:
#       ROUNDTRIP_CHECK=./build/roundtrip_check tests/cli_check.sh ./build/tms_editor
if [ -n "${ROUNDTRIP_CHECK:-}" ]; then
  for f in ref.TMS layoutB.TMS; do
    if out="$("$ROUNDTRIP_CHECK" "$WORK/$f" "$WORK/rt-$f" 2>&1)"; then
      ok "round-trip byte-identical ($f)"
    else
      bad "round-trip changed $f"
      echo "$out"
    fi
  done
fi

exit $fail
