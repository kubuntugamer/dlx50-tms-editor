# DLX-50 `.TMS` row format

Everything below was checked against a real DETECTO DLX-50 over USB: a file
built by this editor was loaded onto the scale, labels were printed from it,
and the scale's own export was diffed byte-for-byte against the file that went
in. Where something is inferred rather than proven, it says so.

## Sections

A `.TMS` file is a sequence of blocks, each opened by a three-letter marker
line (`PLU`, `BAR`, `UNT`, `TMS`, `INF`, …) and closed by `END<TAB>ECS<TAB>`.
Lines are CRLF-terminated and **every line ends with a trailing tab**, including
the markers; empty sections are just marker + closer. The file ends with
`END<TAB>ECS<TAB>\r\n`.

## Two PLU row schemas exist — mixing them fails silently

| | layout A | layout B |
|---|---|---|
| written by | the DLX-50 itself (observed on a device export) | another tool/version (observed on a second USB stick) |
| fields per row | **65** | **45** |
| product name | index **15** | index **2** |
| item code / unit / price | indices **2 / 4 / 5** | absent |

**A file whose `PLU` rows are in the other schema is accepted by the scale and
then ignored.** The transfer reports success, the rows never reach the device's
memory, and keying a PLU number beyond the ones already stored produces two
fast beeps with no display — the owner's manual FAQ calls this "entering a PLU
number that has not been saved on the scale". There is no error text and no
partial load: the export afterwards is byte-identical to what the scale held
before.

So the row shape is the whole game. When building rows for a device, clone a
row the device itself wrote rather than re-deriving the columns.

## Layout A field map (0-based)

Taken from a device-written row; the manual supplies the meaning of a few of
them, the rest is observed shape.

| idx | content | notes |
|---|---|---|
| 0 | `PLU` | section tag |
| 1 | PLU number | what `Sale <n>` addresses |
| 2 | item code | manual: "used for the first part of the default barcode" |
| 3 | empty | `1` in layout-B rows; meaning unknown |
| 4 | unit | `6` = weighable lbs, `2` = piece count (manual) |
| 5 | unit price | manual: *"Each PLU must have a Unit Price other than $0.00"* |
| 6, 7 | `0,0` | unknown (tare / secondary price slots) |
| 8–13 | `0` | unknown |
| 14 | `9` | print-format / barcode-format selector (manual); same in every observed row |
| 15 | product name | blanks continue to index 22 |
| 23 | `1` or `0` | both shapes load and print correctly on hardware |
| 37, 42, 47, 52 | `127` | present on some rows, absent on others; harmless — a row cloned from either shape prints |
| 64 | line terminator | rows are CRLF with a trailing tab like the rest of the file |

Rows are stored by the device **verbatim** — after loading a file and
re-exporting it, the `PLU` and `BAR` sections came back byte-identical. The
scale only rewrote the sale log (`REP`/`RES`/`REE`) and the clock (`TIM`).

## Price encoding

`<digits>,<decimals>`, written unpadded:

- `799,2` → $7.99
- `100,2` → $1.00
- `79,2` → $0.79 *(never `079,2` — the device writes `55,2`, `0,2`)*
- `0,0` → no price

A `$1.00` unit price was confirmed to render as `$1.00/lb` on a printed label,
so the digits are cents and the number after the comma is the decimal count.

## Barcode printed on the label

The firmware generates the default label barcode itself — you do **not** need
to add `BAR` rows for labels to print a barcode. The observed layout is:

```
2 004057 000554
│ └──┬──┘ └──┬──┘
│    │       └─ total × 1000 (0.554 at $1.00/lb)
│    └─ item code, zero-padded to 6 digits
└─ constant leading digit
```

Decoded on hardware two ways: `$1.00/lb` on a 0.55 lb test label gave
`000554`, and a `0.551 lb × $1.49` label gave `000821`.

## The `BAR` section

Rows have the shape `BAR<TAB>index<TAB><TAB>0<TAB>0<TAB>0<TAB>0<TAB><template><TAB>`,
e.g. `BAR	9			0	0	0	0	B501L700A000A000A000A000	`. These are
**custom barcode format definitions**, not per-item barcodes — a file with only
the device's original `BAR` rows printed correct item-specific barcodes for
every new PLU added. The semantics of the `B…L…A…` templates are still unknown.

## Before loading a file onto a device

1. Round-trip it: open → save must be byte-identical. The Rust library test
   suite covers representative device-shaped fixtures with `cargo test --lib`.
2. Confirm the schema: `dlx50-tms-editor FILE.TMS --list PLU` should show the name in
   the *Product name* column and the price in *Price (as stored)* for every
   row. A name showing up under a code-like column means layout B.
3. Check no price is `0,0` — the manual says such a PLU is not valid.

## USB transfer key sequences (8529-0638-0M)

- **Retrieve from scale:** `Prog → 4 → 1`, enter the three-digit file number,
  `Amend` → written to `<stick>/JHSCALE/A_xxx.TMS`.
- **Load onto scale:** `Prog → 4 → 1`, enter the file number, `Accu/Confirm` →
  wait for **"Download Finish"**, `Sale` to return to weighing mode.

Files live in the `JHSCALE` folder as `A_xxx.TMS` (FAT32 stick).
