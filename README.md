# TMS / DLX-50-style Editor

A cross-platform Rust desktop editor and CLI for DETECTO `.TMS` scale files.
The desktop UI uses egui/eframe; the application has no Qt/KDE runtime or
build dependency. It reads and edits PLU, DPT, CLS, UNT, BAR, TMS, TMR, INF,
TMT, REP, and SCP sections.

## File-format compatibility

The editor preserves hidden row fields and the device-specific CRLF, trailing
tabs, and section structure on save. It recognizes the two observed PLU row
layouts and the two BAR barcode offsets. The file-format notes are in
[docs/dlx50-tms-format.md](docs/dlx50-tms-format.md).

Save and Save As copy an existing target to `<file>.BAK` before writing.
Do not load and save files to a scale until you have verified them against
your own backup and hardware: the scale silently ignores some incompatible
PLU schemas.

## Desktop app

- Open, edit, add and delete rows across section tabs.
- **Ctrl+S** saves in place after making a `.BAK`; **Ctrl+Shift+S** opens Save As.
- The read-only scale probe sends the current 8-byte handshake and captures
  the response; it does not send transfer, write, delete, or reboot commands.
- USB, serial, retrieve, and upload are not implemented until the hardware
  protocol is known. The probe itself remains available without the cable.
- Help → Check for Updates checks GitHub Releases. A download is installed
  only after you explicitly confirm, and a published `SHA256SUMS.txt` must
  download and match before installation. Linux installation uses `pkexec`;
  Windows uses a validated helper script after the app exits.

## CLI

The same Rust library powers headless file editing; CLI invocations do not
open a window or perform update checks.

```sh
dlx50-tms-editor FILE.TMS --list PLU
dlx50-tms-editor FILE.TMS --add PLU 9 4099 "Beef Tendon"
dlx50-tms-editor FILE.TMS --add PLU --csv items.csv --price 799,2
dlx50-tms-editor FILE.TMS --add DPT 9 "New Department"
dlx50-tms-editor FILE.TMS --delete PLU 4099
dlx50-tms-editor FILE.TMS --set PLU 0 2 "Edited Name"
dlx50-tms-editor FILE.TMS --delete-row PLU 0
dlx50-tms-editor FILE.TMS --set PLU 0 2 "Saved As" --save-as copy.TMS
dlx50-tms-editor --probe-scale 10.86.215.95 9100 10000
```

`--list` prints a header and tab-separated rows. Mutations back up to
`FILE.TMS.BAK` before saving. Exit codes: `0` success, `2` usage error,
`3` file/load/probe connection error, `4` edit operation error. Other
options: `--version`, `--help`, `--no-update-check`.

CSV rows are `code,name` (or `code,value` for other sections); a first-line
header is optional.

## Distribution

- **Linux:** releases include an AppImage for portable use across common
  distributions and an optional Debian package. AppImage removes the Qt/KDE
  package dependency, but still relies on compatible host libc, graphics
  drivers, and a Wayland or X11 session.
- **Windows:** releases include a portable zip containing the app executable.

The CI builds Linux on Ubuntu 22.04 to target an older glibc baseline, runs the
Rust tests and CLI compatibility suite, and smoke-tests the GUI using both a
headless Wayland compositor and Xvfb/X11.
Windows CI builds and tests the portable executable.

## Build from source

Install the stable Rust toolchain. On Linux, the egui window backend also
needs the Wayland and X11 development libraries (`pkg-config`, Wayland, X11,
`libxkbcommon`, EGL/OpenGL). It supports Wayland sessions and X11-based GUI
automation.

```sh
cargo test --all-targets
cargo build --release
./target/release/dlx50-tms-editor path/to/file.TMS
```

`CMakeLists.txt` is retained only as a compatibility wrapper for developers
who still invoke CMake; it delegates to Cargo and does not locate or link Qt.

## Disclaimer

Not affiliated with, endorsed by, or connected to Detecto. “Detecto” and
“DLX-50” are trademarks of their respective owners. This is an independent,
clean-room compatibility tool developed without access to the original
DLX-50 software. The format was derived from publicly observable `.TMS`
data. The project’s legal analysis is in [LEGAL_BRIEF.md](LEGAL_BRIEF.md),
with cited authorities in [CITATIONS.md](CITATIONS.md).

## License

MIT — see [LICENSE](LICENSE).
