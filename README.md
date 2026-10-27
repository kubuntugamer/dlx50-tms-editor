<header omitted>

A Linux‑ **and** Windows‑desktop Qt 6 (C++) editor for DETECTO `.TMS` scale
files (e.g. the DL‑1030). Reads and edits every section — PLU, DPT, CLS,
UNT, BAR, TMS, TMR, INF, TMT, REP, SCP — in a tabbed, table‑based UI.

## Editing products (add / delete rows)

Every section tab has **Add row** and **Delete selected** buttons above the
table:

- **Add row** clones the last row of that section as an editable draft —
  the next free number in the first column, the name preset to
  `NEW PRODUCT` (PLU) and the product/barcode code cleared so a new
  product can't inherit the template's code. All hidden fields (the ~60
  price/weight/telemetry values on a PLU row) are preserved so the scale
  still accepts the row. Sections with no rows get a blank first row
  (PLU stays clone-only — its rows need all 60+ fields).
- **Delete selected** removes whole rows (click the row number to select)
  from the table *and* the saved file
- Remember to **Save** — row changes are not written until you do.

## Self-update (Linux + Windows)

The app can update itself from the repo's GitHub Releases:

- **Help → Check for Updates…** compares the running version against the
  latest release tag and offers to download and install a newer one.
- A silent check also runs ~3 s after startup; it is skipped with the
  `--no-update-check` flag or a saved "skip and stop asking" choice, and
  it never dialogs when there is nothing newer or when offline.
- Downloads are verified against the release's `SHA256SUMS.txt` before
  anything is touched; a missing or unfetchable checksum file aborts the
  install instead of risking a bad binary.
- **Linux:** the `.deb` is installed via `pkexec` — an explicit
  privilege-escalation prompt; nothing is written before you authorise.
  After install the updated binary relaunches (also from a dev build:
  it relaunches the installed copy, not itself).
- **Windows:** a helper script waits for the running exe to exit,
  unpacks the new zip over the install folder, and relaunches it.
- `--version` prints the running version (dev builds report `0.0.0`;
  release builds carry their tag). `Help → About` shows it too.

## Saving (DLX‑50 behaviour)

- **Ctrl+S (Save)** — copies the current file to `<file>.BAK`, then writes
  your edited tables back over the original filename. No dialog.
- **Ctrl+Shift+S (Save As)** — pick a destination (still backs up if the
  target already exists).

## Get the Windows app (no build tools needed)

1. Open the repo's **Releases** page.
2. Download `tms_editor-windows-x64.zip`.
3. Unzip anywhere and double‑click `tms_editor.exe`.

Everything it needs (Qt DLLs, the Windows platform plugin, MinGW runtime)
is bundled in the zip. It's a native Windows GUI app — Win32 windowing,
DWM‑composited, no console window.

## Get the Linux app (.deb, Ubuntu/Debian)

1. Open the repo's **Releases** page.
2. Download `dlx50-tms-editor_*_amd64.deb`.
3. Install and run:

```bash
sudo apt install ./dlx50-tms-editor_*_amd64.deb
dlx50-tms-editor path/to/file.TMS
```

The installer also puts a **TMS / DLX‑50‑style Editor** launcher in your
application menu. The package depends only on the distro's own Qt 6 packages
(`libqt6widgets6`, `qt6-qpa-plugins`, …), so apt pulls in everything the app
needs. It is built on Ubuntu 24.04 LTS, so the same `.deb` installs on every
newer Ubuntu/Debian release as well.

## Build it yourself

### Linux

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/tms_editor path/to/file.TMS
```

### Windows (Qt Creator 6 or plain CMake)

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
windeployqt --release --no-translations build/tms_editor.exe
build\tms_editor.exe path\to\file.TMS
```

### Cross‑compile Windows .exe from Linux

Requires `x86_64-w64-mingw32-g++` and Qt 6.8.3 for `win64_mingw`
(e.g. via `aqtinstall`).

```bash
cmake -S . -B build-win \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-win --parallel
```

## Continuous builds & releases

Every push runs `.github/workflows/build-windows.yml` on both platforms and
**launches the app on each runner to verify it actually starts and opens a
window** — a green run means it built *and ran*:

| Platform | Runner | Proof | Artifact (every run) |
|---|---|---|---|
| Windows | `windows-latest` | exe launched, real Win32 window | `tms_editor-windows-x64.zip` |
| Linux | `ubuntu-24.04` | launched under Xvfb, window title checked, `.deb` installed and re-launched | `dlx50-tms-editor-linux-amd64` (`.deb`) |

Grab artifacts from the run's **Artifacts** section — no tag needed.
Tagging a version (`git tag v1.0 && git push --tags`) publishes a **GitHub
Release with both the zip and the .deb** attached.

> The Linux build intentionally targets **Ubuntu 24.04 (noble)** — the
> oldest supported release — so binaries run forward on newer releases
> (25.04, 26.10, …). A build made on a newer release would *not* run on
> older ones (glibc/Qt symbol floors), and the `.deb` uses package names
> that resolve on both (noble's `t64` packages provide the plain names).

## Disclaimer

Not affiliated with, endorsed by, or connected to Detecto. "Detecto" and
"DLX-50" are trademarks of their respective owners. This is an independent,
clean-room compatibility tool developed **without any access to the original
DLX-50 software** — the format support was derived from the publicly
observable structure of `.TMS` data files.

A full statement of the project's independent-development and
non-infringement analysis (statutes and case law) is maintained in
[LEGAL_BRIEF.md](LEGAL_BRIEF.md), with every cited authority linked to a
free full-text source in [CITATIONS.md](CITATIONS.md).

## License

MIT — see [LICENSE](LICENSE). Qt is used under its LGPL-3.0/GPL-3.0 terms;
the deployed Windows build ships the Qt libraries alongside the executable.
