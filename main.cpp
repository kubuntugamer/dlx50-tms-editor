#include <QApplication>
#include <QFile>
#include <QTextStream>
#include "tmseditorwindow.h"
#include "updater.h"
#include "transport.h"

// -------------------------------------------------------------------
// Headless CLI manipulation mode (issue #6): --list / --add / --delete
// run the SAME load/clone/overlay/save machinery as the GUI, so every
// scripted or AI-driven edit is emitted by the device-exact writer.
// Syntax:
//   dlx50-tms-editor FILE.TMS --list TAG
//   dlx50-tms-editor FILE.TMS --add TAG [NUMBER] [CODE] [NAME]
//   dlx50-tms-editor FILE.TMS --add TAG --csv FILE       (code,name rows;
//                                    an optional header line is skipped)
//   dlx50-tms-editor FILE.TMS --delete TAG CODE
// Exit codes: 0 ok · 2 usage error · 3 file/load error · 4 op error.
//
// Network scale probe (issue #8): --probe-scale HOST PORT [TIMEOUT]
//   read-only discovery of what a LAN scale agent exposes; never sends
//   write/delete/reboot/transfer commands.
//   Exit codes: 0 probe ran and reported · 2 usage error · 3 probe error
// -------------------------------------------------------------------
static void cliUsage(QTextStream& out)
{
    out << "Usage:\n"
           "  dlx50-tms-editor FILE.TMS                     open the GUI\n"
           "  dlx50-tms-editor FILE.TMS --list TAG          print rows to stdout\n"
           "  dlx50-tms-editor FILE.TMS --add TAG [NUMBER] [CODE] [NAME]\n"
           "      add one row (clone of the last, auto-numbered). PLU: NUMBER\n"
           "      = PLU #, CODE = product code, NAME = product name.\n"
           "      PLU may also take --price VALUE (stored price, e.g. 799,2)\n"
           "  dlx50-tms-editor FILE.TMS --add TAG --csv FILE\n"
           "      bulk add: each line 'CODE,NAME' (header line skipped)\n"
           "  dlx50-tms-editor FILE.TMS --delete TAG CODE   delete matching rows\n"
           "  dlx50-tms-editor FILE.TMS --set TAG ROW COL VALUE\n"
           "      edit one cell (0-based ROW and COL, as shown by --list)\n"
           "  dlx50-tms-editor FILE.TMS --delete-row TAG ROW\n"
           "      delete one row by 0-based index\n"
           "  add --save-as PATH to write the result to PATH instead of FILE\n"
           "  dlx50-tms-editor --probe-scale HOST PORT [TIMEOUT_MS]\n"
           "      read-only network probe: connect, send an 8-byte handshake,\n"
           "      print what the scale answers (no write/delete/reboot sent)\n"
           "  --version | --no-update-check | --help\n"
        << Qt::endl;
}

static bool takesFileToken(const QString& a)
{
    return a.compare(QLatin1String("--no-update-check"), Qt::CaseInsensitive) == 0
        || a.compare(QLatin1String("--version"), Qt::CaseInsensitive) == 0;
}

static bool isProbeFlag(const QString& a)
{
    return a.compare(QLatin1String("--probe-scale"), Qt::CaseInsensitive) == 0;
}

int main(int argc, char* argv[])
{
    // No QT_QPA_PLATFORM forcing here on purpose: Qt6 picks the NATIVE
    // compositor/windowing system on its own —
    //   * Windows : Win32 + DWM (Desktop Window Manager)
    //   * Linux   : Wayland (or X11 only if that's the session)
    // The only exception is true headless (CI / servers) where no display
    // server exists at all; there we fall back to offscreen so the process
    // still runs.
#if defined(Q_OS_LINUX) || defined(Q_OS_UNIX)
    if (qgetenv("WAYLAND_DISPLAY").isEmpty() &&
        qgetenv("DISPLAY").isEmpty() &&
        qgetenv("QT_QPA_PLATFORM").isEmpty()) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
#endif

    QApplication app(argc, argv);

    app.setOrganizationName(QStringLiteral("kubuntugamer"));
    app.setApplicationName(QStringLiteral("dlx50-tms-editor"));

    // --version: print and exit (useful to tell a dev build 0.0.0 apart
    // from an installed updater target).
    for (int i = 1; i < argc; ++i) {
        if (QString(argv[i]).compare(QStringLiteral("--version"),
                                     Qt::CaseInsensitive) == 0) {
            QTextStream out(stdout);
            out << "dlx50-tms-editor " << Updater::currentVersion()
                << Qt::endl;
            return 0;
        }
    }

    // ---------- CLI manipulation mode detection + parsing ------------- //
    // Values are consumed positionally after each operation flag, stopping
    // at another flag; the leftover leading non-flag token is the file.
    QString opTag, opListFlag, csvPath, deleteCode, addPrice;
    QString setValue, saveAsPath;
    int setRow = -1, setCol = -1, deleteRow = -1;
    bool haveSet = false, haveDeleteRow = false;
    QVector<QString> addVals;           // NUMBER, CODE, NAME
    bool haveList = false, haveAdd = false, haveDelete = false;

    // Network probe (issue #8).
    bool haveProbe = false;
    QString probeHost;
    int probePort = -1;
    int probeTimeoutMs = 5000;

    for (int i = 1; i < argc; ++i) {
        const QString a = argv[i];
        if (a.compare(QLatin1String("--help"), Qt::CaseInsensitive) == 0) {
            QTextStream out(stdout);
            cliUsage(out);
            return 0;
        }
        if (a.compare(QLatin1String("--list"), Qt::CaseInsensitive) == 0) {
            haveList = true;
            if (i + 1 < argc && !QString(argv[i + 1]).startsWith('-'))
                opTag = argv[++i];
            continue;
        }
        if (a.compare(QLatin1String("--add"), Qt::CaseInsensitive) == 0) {
            haveAdd = true;
            if (i + 1 < argc && !QString(argv[i + 1]).startsWith('-'))
                opTag = argv[++i];
            while (i + 1 < argc) {
                const QString v = argv[i + 1];
                if (v.compare(QLatin1String("--csv"),
                              Qt::CaseInsensitive) == 0) {
                    if (i + 2 < argc)
                        csvPath = argv[i + 2];
                    i += 2;
                    continue;
                }
                if (v.compare(QLatin1String("--price"),
                              Qt::CaseInsensitive) == 0) {
                    if (i + 2 < argc)
                        addPrice = argv[i + 2];
                    i += 2;
                    continue;
                }
                if (v.startsWith('-') || addVals.size() >= 3)
                    break;
                addVals.append(v);
                ++i;
            }
            continue;
        }
        if (a.compare(QLatin1String("--delete"), Qt::CaseInsensitive) == 0) {
            haveDelete = true;
            if (i + 1 < argc && !QString(argv[i + 1]).startsWith('-'))
                opTag = argv[++i];
            if (i + 1 < argc && !QString(argv[i + 1]).startsWith('-'))
                deleteCode = argv[++i];
            continue;
        }
        // --set TAG ROW COL VALUE: edit one cell (0-based row/col).
        if (a.compare(QLatin1String("--set"), Qt::CaseInsensitive) == 0) {
            haveSet = true;
            if (i + 4 < argc) {
                opTag = argv[i + 1];
                setRow = QString(argv[i + 2]).toInt();
                setCol = QString(argv[i + 3]).toInt();
                setValue = argv[i + 4];
            }
            i += 4;
            continue;
        }
        // --delete-row TAG ROW: delete one row by 0-based index.
        if (a.compare(QLatin1String("--delete-row"), Qt::CaseInsensitive) == 0) {
            haveDeleteRow = true;
            if (i + 2 < argc) {
                opTag = argv[i + 1];
                deleteRow = QString(argv[i + 2]).toInt();
            }
            i += 2;
            continue;
        }
        if (isProbeFlag(a)) {
            haveProbe = true;
            if (i + 1 < argc && !QString(argv[i + 1]).startsWith('-'))
                probeHost = argv[++i];
            if (i + 1 < argc && !QString(argv[i + 1]).startsWith('-'))
                probePort = QString(argv[++i]).toInt();
            if (i + 1 < argc && !QString(argv[i + 1]).startsWith('-'))
                probeTimeoutMs = QString(argv[++i]).toInt();
            continue;
        }
        // --save-as PATH: write the result to PATH instead of the input file.
        if (a.compare(QLatin1String("--save-as"), Qt::CaseInsensitive) == 0) {
            if (i + 1 < argc)
                saveAsPath = argv[++i];
            continue;
        }
    }

    const bool cliMode = haveList || haveAdd || haveDelete || haveSet || haveDeleteRow || haveProbe;

    // Network probe (issue #8): it does not touch any file, so it runs
    // before the file-loading path and never opens the GUI.
    if (haveProbe) {
        if (probeHost.isEmpty() || probePort <= 0) {
            QTextStream err(stderr);
            err << "error: --probe-scale HOST PORT [TIMEOUT_MS] required\n" << Qt::endl;
            cliUsage(err);
            return 2;
        }
        QTextStream out(stdout);
        out << "dlx50-tms-editor network probe (issue #8)\n"
            << "  target : " << probeHost << ":" << probePort << "\n"
            << "  timeout: " << probeTimeoutMs << " ms\n"
            << "  mode   : read-only (no write/delete/reboot/transfer sent)\n"
            << Qt::endl;

        NetworkProbeResult result = probeScale(probeHost, probePort, probeTimeoutMs);

        out << "--- connection ---\n";
        out << "  connected : " << (result.connected ? "yes" : "no") << "\n";
        out << "  host      : " << result.host << "\n";
        out << "  port      : " << result.port << "\n";
        out << Qt::endl;

        out << "--- observations ---\n";
        for (const QString& o : result.observations)
            out << "  " << o << "\n";
        out << Qt::endl;

        out << "--- captured response ---\n";
        if (result.rawBytes.isEmpty()) {
            out << "  (no bytes received)\n";
        } else {
            out << "  bytes    : " << result.rawBytes.size() << "\n";
            out << "  hex       : " << result.rawBytes.toHex().constData() << "\n";
            if (!result.rawResponse.isEmpty()) {
                out << "  utf8      : \"" << result.rawResponse << "\"\n";
            } else if (!result.rawBytes.isEmpty()) {
                out << "  utf8      : (not valid UTF-8 — see hex above)\n";
            }
            if (!result.lines.isEmpty()) {
                out << "  lines     :\n";
                for (const QString& l : result.lines)
                    out << "    | " << l << "\n";
            }
        }
        out << Qt::endl;

        out << "--- summary for issue #8 ---\n";
        out << result.summary << "\n";
        out << Qt::endl;

        out << "--- next steps (paste into issue #7/8) ---\n";
        out << "  - target   : " << result.host << ":" << result.port << "\n";
        out << "  - connected: " << (result.connected ? "yes" : "no") << "\n";
        out << "  - bytes    : " << result.rawBytes.size() << "\n";
        if (!result.rawBytes.isEmpty())
            out << "  - hex      : " << result.rawBytes.toHex().constData() << "\n";
        out << "  - probe    : read-only, no write/delete/reboot/transfer sent\n";
        out << Qt::endl;

        return result.connected ? 0 : 3;
    }

    if (!cliMode) {
        // GUI mode: first non-flag argument is the file to open.
        QString filePath;
        for (int i = 1; i < argc; ++i) {
            const QString arg = argv[i];
            if (takesFileToken(arg))
                continue;
            if (filePath.isEmpty())
                filePath = arg;
        }

        TmsEditorWindow window(filePath);
        window.show();

        return app.exec();
    }

    // ---------------------- headless CLI execution ------------------- //
    // No event loop is ever entered here, so the ctor's QTimer-based
    // update-check singleShot can never fire — the headless run provably
    // touches no network and raises no dialogs.

    QString filePath;
    for (int i = 1; i < argc; ++i) {
        const QString a = argv[i];
        if (takesFileToken(a) || a.startsWith(QLatin1Char('-')))
            continue;
        filePath = a;
        break;
    }
    if (filePath.isEmpty()) {
        QTextStream err(stderr);
        err << "error: no .TMS file given\n" << Qt::endl;
        cliUsage(err);
        return 3;
    }
    if (opTag.isEmpty()) {
        QTextStream err(stderr);
        err << "error: missing section tag after the operation flag "
               "(PLU, DPT, CLS, UNT, BAR, TMS, TMR, INF, TMT, REP, SCP)\n"
            << Qt::endl;
        cliUsage(err);
        return 2;
    }

    TmsEditorWindow window(filePath);
    if (!window.fileLoaded()) {
        QTextStream err(stderr);
        err << "error: could not load " << filePath << Qt::endl;
        return 3;
    }

    // Reads: list, exit without touching the file.
    if (haveList && !haveAdd && !haveDelete && csvPath.isEmpty()) {
        QStringList lines;
        if (!window.listSection(opTag, &lines)) {
            QTextStream err(stderr);
            err << "error: no such section: " << opTag << Qt::endl;
            return 2;
        }
        QTextStream out(stdout);
        for (const QString& l : lines)
            out << l << Qt::endl;
        return 0;
    }

    // Mutations: perform, then Save (backs up to FILE.BAK first).
    int failures = 0;
    int successCount = 0;
    QString err;

    if (haveAdd) {
        if (!csvPath.isEmpty()) {
            QFile csv(csvPath);
            if (!csv.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream e(stderr);
                e << "error: cannot open csv " << csvPath << Qt::endl;
                return 3;
            }
            int lineNo = 0;
            while (!csv.atEnd()) {
                QString line = QString::fromUtf8(csv.readLine());
                line = line.trimmed();
                ++lineNo;
                if (line.isEmpty())
                    continue;
                // Split on commas honouring a simple "a,b" quoting.
                QStringList cells;
                bool inQ = false;
                QString cur;
                for (int p = 0; p < line.size(); ++p) {
                    const QChar c = line.at(p);
                    if (c == '"') { inQ = !inQ; continue; }
                    if (c == ',' && !inQ) { cells.append(cur); cur.clear(); }
                    else cur.append(c);
                }
                cells.append(cur);
                // Skip a header line (first non-numeric cell on line 1).
                if (lineNo == 1) {
                    bool firstNumeric = false;
                    cells.value(0).toInt(&firstNumeric);
                    if (!firstNumeric && !cells.value(1).toInt(nullptr))
                        continue;
                }
                // PLU csv: CODE,NAME. Other sections: CODE,VALUE.
                QString number, code, name;
                if (opTag == QLatin1String("PLU")) {
                    code = cells.value(0);
                    name = cells.value(1);
                } else {
                    code = cells.value(0);
                    name = cells.value(1);
                }
                const QString rowPrice = (opTag == QLatin1String("PLU")) ? addPrice : QString();
                if (window.addRowForCli(opTag, number, code, name, &err, rowPrice))
                    ++successCount;
                else {
                    ++failures;
                    QTextStream e(stderr);
                    e << "row " << lineNo << ": " << err << Qt::endl;
                }
            }
        } else {
            const QString number = addVals.value(0);
            const QString code   = addVals.value(1);
            const QString name   = addVals.value(2);
            // PLU: NUMBER=PLU#, CODE=product code, NAME=name. Other
            // sections have no separate product-code column, so a single
            // first value is the code — accept it in either position.
            QString effCode = code, effName = name;
            if (opTag != QLatin1String("PLU")) {
                effCode = !number.isEmpty() ? number : code;
                effName = name;
            }
            if (window.addRowForCli(opTag, number, effCode, effName, &err, addPrice))
                ++successCount;
            else {
                ++failures;
                QTextStream e(stderr);
                e << "error: " << err << Qt::endl;
            }
        }
    }

    if (haveDelete) {
        if (window.removeRowsByCode(opTag, deleteCode, &err)) {
            ++successCount;
        } else {
            ++failures;
            QTextStream e(stderr);
            e << "error: " << err << Qt::endl;
        }
    }

    if (haveSet) {
        if (window.setCellForCli(opTag, setRow, setCol, setValue, &err)) {
            ++successCount;
        } else {
            ++failures;
            QTextStream e(stderr);
            e << "error: " << err << Qt::endl;
        }
    }

    if (haveDeleteRow) {
        if (window.removeRowForCli(opTag, deleteRow, &err)) {
            ++successCount;
        } else {
            ++failures;
            QTextStream e(stderr);
            e << "error: " << err << Qt::endl;
        }
    }

    if (failures > 0)
        return 4;

    if (successCount == 0) {
        QTextStream e(stderr);
        e << "error: nothing to do\n" << Qt::endl;
        cliUsage(e);
        return 2;
    }

    if (!saveAsPath.isEmpty()) {
        if (!window.saveToPathForCli(saveAsPath, &err)) {
            QTextStream e(stderr);
            e << "error: " << err << Qt::endl;
            return 3;
        }
        QTextStream out(stdout);
        out << opTag << ": " << successCount << " change(s); saved " << saveAsPath << Qt::endl;
        return 0;
    }

    window.saveFile();

    QTextStream out(stdout);
    out << opTag << ": " << (haveDelete || haveDeleteRow ? "deleted " : haveSet ? "edited " : "added ")
        << successCount << " row(s); saved " << filePath
        << " (backup: " << filePath << ".BAK)" << Qt::endl;
    return 0;
}
