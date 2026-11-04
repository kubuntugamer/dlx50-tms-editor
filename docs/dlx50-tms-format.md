# DL-series `.TMS` row format

Everything below was checked against a real DETECTO DL-series over USB: a file
built by this editor was loaded onto the scale, labels were printed from it,
and the scale's own export was diffed byte-for-byte against the file that went
in. Where something is inferred rather than proven, it says so.

## Sections

A `.TMS` file is a sequence of blocks, each opened by a three-letter marker
line (`PLU`, `BAR`, `UNT`, `TMS`, `INF`, …) and closed by `END<TAB><TAG><TAB>`.
Lines are CRLF-terminated and **every line ends with a trailing tab**, including
the markers; empty sections are just marker + closer. The file ends with
`END<TAB>ECS<TAB>\r\n`.

## Two PLU row schemas exist — mixing them fails silently

| | layout A | layout B |
|---|---|---|
| written by | the DL-series itself (observed on a device export) | another tool/version (observed on a second USB stick) |
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
e.g. `BAR\t9\t\t\t0\t0\t0\t0\tB501L700A000A000A000A000\t`. These are
**custom barcode format definitions**, not per-item barcodes — a file with only
the device's original `BAR` rows printed correct item-specific barcodes for
every new PLU added. The semantics of the `B…L…A…` templates are still unknown.

## Protecting a retrieval before you trust it

1. Round-trip it: open → save must be byte-identical. The Rust library test
   suite covers representative device-shaped fixtures with `cargo test --lib`.
2. Confirm the schema: `dlx50-tms-editor FILE.TMS --list PLU` should show the name in
   the *Product name* column and the price in *Price (as stored)* for every
   row. A name showing up under a code-like column means layout B.
3. Check no price is `0,0` — the manual says such a PLU is not valid.

## USB transfer key sequences (8529-0638-0M, manual p. 6-7)

- **Retrieve from scale:** `Prog → 4 → 1`, enter the three-digit file number,
  `Amend` → written to `<stick>/JHSCALE/A_xxx.TMS`.
- **Load onto scale:** `Prog → 4 → 1`, enter the file number, `Accu/Confirm` →
  wait for **"Download Finish"**, `Sale` to return to weighing mode.

Files live in the `JHSCALE` folder as `A_xxx.TMS` (FAT32 stick).

## Wavetable emulator validation against docs

The wavetable emulator is judged against this doc's observed data — the default
label barcode, the price encoding, and the section shape — not against vendor
code (which was never available). The relevant expectations are:

- The default barcode on a printed label is computed from the item code and the
  total × 1000, with a constant leading digit of `2`.
- Price is stored as `<digits>,<decimals>` and confirmed on hardware as cents +
  decimal count.
- Section frames are `\r\nDWL\t<TAG>\t\r\n` … `END\t<TAG>\t\r\n`.

The fragment-level pipeline (decode frame edges → align to per-voice sequence →
apply the WAV/PC6 mix model) yields an expected LE-measure C.
That value is the target the emulator must reproduce.

Note: the exact C is computed only after the fragment data and weight model are
available; right now it is recorded as a required target, not as a number.
The required target is also recorded in [prototype-log.txt](prototype-log.txt),
where its numeric value is marked pending. The exact numeric C was not computed
and is not claimed anywhere in this repo.

## C Python prototype result

The C Python prototype was verified against the same expectations. Its output
matched the expected barcode layout and price interpretation on the defined
test cases; the detailed log is at `prototype-log.txt`.

Verified cases:
- item code 004057, total 0.554 lb at $1.00/lb → barcode tail 000554
- item code 004057, total 0.551 lb at $1.49/lb → barcode tail 000821
- price 799,2 → $7.99
- price 100,2 → $1.00
- price 0,0 → no price

## Live network retrieval

The Rust `ScaleTransport` (read-only) discovers a powered scale by passively
listening for its UDP beacon, then requests each section with `UPL\t<TAG>\t\r\n`.
The scale answers with `\r\nDWL\t<TAG>\t\r\n` … `END\t<TAG>\t\r\n` and keeps
the TCP connection open; the reader stops at the END marker. Every section in
device order is assembled, the ECS header and footer are added, and the file is
written — a failure part-way through never leaves a half-written file.

`--scale-live FILE.TMS` runs this against the first scale heard.

Writing uses the same block shape in the other direction: the PC sends
`\r\nDWL\t<TAG>\t\r\n`, the rows, then `END\t<TAG>\t\r\n`, and the scale applies
it without answering. This is confirmed on a real DL-series — after a full-file
write, a fresh retrieval is byte-identical except for the `TIM` row — but no
vendor session was ever captured, so `--scale-upload FILE.TMS --allow-write`
reads every section back and compares before it reports success. `TIM`
(yy mm dd hh mm ss) is never written: a saved file's stale copy would reset the
device clock. `REP` is skipped for the opposite reason — the scale keeps its
sale log there (`REP`/`RES`/`REE` records) and a write clears it instead of
storing the rows, so uploads and dry runs leave it alone.

Discovery and file assembly both live in the Rust `ScaleTransport` in
`src/lib.rs` (`discover`, `fetch_section`, `retrieve`).

`--scale-upload FILE.TMS --dry-run` runs the same section-by-section
comparison the upload verifies against *after* writing, and reports it instead
of writing: each section comes back as unchanged, would be replaced (with row
counts), or would be cleared because the file carries no rows for it. For a
changed section it names the first five differing rows by id and shows which
fields differ (`field 5: "100,2" -> "349,2"`), so a price typo is visible
before it reaches the scale.

## The `TMS` section is the Spec table

`TMS` rows are `TMS<TAB><spec><TAB><value>` where the number is the scale's
`Spec` number, 0–499 (`UPL <TAB>TMS` answers with 500 rows on the store's
DL-series). The mapping is documented in the Digi SM-100 service manual §6.4.1
and the SM-110 Spec table, which agree with each other and with this project's
existing `Spec166` name for the TCP port.

| Spec | Meaning | Values | The store's scale |
|---|---|---|---|
| 48 | scale number (4-line / RS-232C) | 1–999999 | 10 |
| 49 | client/server interface | 0 none, 1 Ethernet coax, 2 Ethernet twisted | 1 |
| 50 | client/server | 0 client, 1 server | 0 |
| 51 | baud rate, RS-232C | 0=1200 1=2400 2=4800 3=9600 4=19200 5=38400 6=76800 | **1 → 2400** |
| 52 | data length | 0=7 bits, 1=8 bits | **0 → 7 bits** |
| 53 | parity | 0 none, 1 odd, 2 even | **2 → even** |
| 54 | stop bits | 0=1 bit, 1=2 bits | **0 → 1 bit** |
| 60 | SIO job, RS-232C port | 0 none, 1 FIS 30000 #1, 2 FL-1, 3 Point 'N' Shop, 4 barcode | **1 → FIS 30000 #1** |
| 166–169 | service TCP ports | | 33581–33584 |
| 376–379 | beacon ports | | 33591–33594 |
| 401 | RTS/CTS handshaking | 0 off, 1 on | 0 |

So the store's serial port is configured for **2400 baud, 7 data bits, even
parity, 1 stop bit**, and it is bound to the `FIS 30000 #1` POS job rather than
a generic output mode. That is why it answers neither a passive listen nor a
generic `ENQ`/`W`/`S` demand poll (checked at every baud/parity combination,
plus every 7-bit poll byte at 2400 7E1), and why it never streams.

The RS-232 wiring is straight-through, not null-modem: scale pin 2 is `RXD` and
pin 3 `TXD`, so they meet the PC's 2/3 the other way round (SM-100 §6.5.2).
With the cable in the scale's RS-232 socket the adapter should see the scale's
`DTR` on pins 4 and 7 as `DSR`/`CTS`; seeing neither suggests the cable is in
the cash-drawer socket instead.

## A live round-trip, measured

Retrieve → push the same file back → retrieve again, against the store's
scale (Nov 2026):

- 15 of 16 sections came back byte-identical (`PLU` 99 rows, `BAR` 9, `DPT`,
  `CLS`, `UNT`, `SCP` 148, `TMS` 500, `TMT` 28, `LAB`, `SAL`, `REP`, `SCF`,
  `TMR`, `BMP`, `INF`).
- Only `TIM` differed (`16:40:44` → `16:41:56`): the clock runs on, and uploads
  deliberately never write it.
- The write reported `every section read back matches what was sent`. That
  read-back is the only proof available, because the write framing has never
  been captured from a vendor session.

One caveat seen in practice: the scale drops off the network when it sleeps or
is powered down (`ping` 100% loss, ARP `INCOMPLETE`), so a retrieve that fails
to hear a beacon is usually the device being off, not the transport.
