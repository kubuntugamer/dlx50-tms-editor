// Headless round-trip check for the device-exact writer.
//
// Usage: roundtrip_check <reference.TMS> [scratch-dir]
//
// Copies the reference file into a scratch directory (default: ./roundtrip_scratch
// inside the current directory), opens the copy in the editor, saves it, and
// verifies the saved bytes equal the reference bytes. The reference file and
// its .BAK are never written to; the .BAK made by save lands in the scratch
// directory only.

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cstdio>

#include "tmseditorwindow.h"
#include "updater.h"

static QByteArray readBytes(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return f.readAll();
}

// Issue #12: the Windows self-update helper is a batch file the app builds
// by hand, so its safety is decided by text we can assert on without ever
// running an update: every path must travel as a quoted cmd argument or an
// environment variable, and every fallible step must be followed by an
// errorlevel check that leads to a message instead of to the relaunch.
static int updateHelperCheck()
{
    int failures = 0;
    const auto check = [&failures](bool cond, const char* what) {
        std::printf("%s: %s\n", cond ? "ok  " : "FAIL", what);
        if (!cond)
            ++failures;
    };

    // An install path carrying everything cmd.exe and PowerShell trip over:
    // a space, an apostrophe, a command separator and a percent sign.
    Updater::UpdateHelperPaths p;
    p.appDir       = QStringLiteral("C:\\Users\\O'Brien & Sons\\100% legit\\dlx50");
    p.updateDir    = p.appDir + QStringLiteral("\\_update");
    p.unzipDir     = p.updateDir + QStringLiteral("\\new");
    p.persistedZip = p.updateDir + QStringLiteral("\\tms_editor-windows-x64.zip");
    p.selfExe      = p.appDir + QStringLiteral("\\tms_editor.exe");

    const QString escapedRoot =
        QStringLiteral("C:\\Users\\O'Brien & Sons\\100%% legit\\dlx50");

    QString script, error;
    check(Updater::buildWindowsUpdateHelper(p, &script, &error),
          "path with space, apostrophe, & and % is accepted");

    check(script.contains(QStringLiteral("set \"TMS_UPDATE_ZIP=") + escapedRoot
                          + QStringLiteral("\\_update\\tms_editor-windows-x64.zip\"")),
          "zip path travels in an environment variable, % doubled");
    check(script.contains(QStringLiteral("$env:TMS_UPDATE_ZIP"))
              && script.contains(QStringLiteral("$env:TMS_UPDATE_UNZIP")),
          "PowerShell reads its paths from the environment");
    check(!script.contains(QStringLiteral("-LiteralPath '")),
          "no path is inlined into PowerShell single quotes");
    check(script.contains(QStringLiteral("xcopy \"") + escapedRoot
                          + QStringLiteral("\\_update\\new/tms_editor\\*\" \"")
                          + escapedRoot + QStringLiteral("\"")),
          "copy source and target are single quoted cmd arguments");
    check(script.contains(QStringLiteral("if errorlevel 1 goto :fail_clean"))
              && script.contains(QStringLiteral("if errorlevel 1 goto :fail_unpack"))
              && script.contains(QStringLiteral("if errorlevel 1 goto :fail_copy")),
          "clean, unpack and copy all check their exit code");

    const int firstFail = script.indexOf(QStringLiteral("\r\n:fail_"));
    check(firstFail > 0
              && !script.mid(firstFail).contains(QStringLiteral("start \"\"")),
          "failure path never relaunches the editor");
    check(firstFail > 0
              && script.mid(firstFail).contains(QStringLiteral("pause >nul")),
          "failure path stays open with a message");

    Updater::UpdateHelperPaths bad = p;
    bad.appDir = QStringLiteral("C:\\Program\" Files\\dlx50");
    script.clear();
    error.clear();
    check(!Updater::buildWindowsUpdateHelper(bad, &script, &error)
              && error.contains(QStringLiteral("C:\\Program\" Files\\dlx50")),
          "path with a double quote is refused and named in the error");
    check(script.isEmpty(),
          "refused path produces no script");

    Updater::UpdateHelperPaths ctrl = p;
    ctrl.selfExe = p.appDir + QChar('\n') + QStringLiteral("\\tms_editor.exe");
    check(!Updater::buildWindowsUpdateHelper(ctrl, nullptr, nullptr),
          "path with a control character is refused");

    if (failures) {
        std::printf("FAIL: %d update helper check(s) failed\n", failures);
        return 1;
    }
    std::printf("ok  : update helper script checks (issue #12)\n");
    return 0;
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <reference.TMS> [scratch-dir]\n"
                             "       %s --update-helper-check\n",
                     argv[0], argv[0]);
        return 2;
    }

    if (QString::fromLocal8Bit(argv[1]) == QLatin1String("--update-helper-check"))
        return updateHelperCheck();

    const QString reference = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
    const QString scratchDir = argc >= 3
        ? QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath()
        : QDir::current().filePath(QStringLiteral("roundtrip_scratch"));

    if (!QDir().mkpath(scratchDir)) {
        std::fprintf(stderr, "cannot create scratch dir %s\n", qPrintable(scratchDir));
        return 2;
    }

    const QByteArray before = readBytes(reference);
    if (before.isEmpty()) {
        std::fprintf(stderr, "cannot read reference %s\n", qPrintable(reference));
        return 2;
    }

    const QString copy = QDir(scratchDir).filePath(QFileInfo(reference).fileName());
    QFile::remove(copy);
    if (!QFile::copy(reference, copy)) {
        std::fprintf(stderr, "cannot copy reference to %s\n", qPrintable(copy));
        return 2;
    }

    // Offscreen platform: no display needed, and no update check.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    char arg0[] = "roundtrip_check";
    char arg1[] = "--no-update-check";
    char* av[] = { arg0, arg1, nullptr };
    int ac = 2;
    QApplication app(ac, av);

    {
        TmsEditorWindow w(copy);
        w.saveFile();
    }

    const QByteArray after = readBytes(copy);
    const bool identical = (after == before);

    if (identical) {
        std::printf("ok  : open->save is byte-identical (%lld bytes)\n",
                    static_cast<long long>(before.size()));
        return 0;
    }

    int diff = -1;
    const int n = qMin(after.size(), before.size());
    for (int i = 0; i < n; ++i) {
        if (after.at(i) != before.at(i)) { diff = i; break; }
    }
    if (diff == -1) {
        std::printf("FAIL: size differs: reference=%lld saved=%lld\n",
                    static_cast<long long>(before.size()),
                    static_cast<long long>(after.size()));
    } else {
        std::printf("FAIL: first difference at byte %d: reference=0x%02x saved=0x%02x\n",
                    diff,
                    static_cast<unsigned char>(before.at(diff)),
                    static_cast<unsigned char>(after.at(diff)));
    }
    return 1;
}
