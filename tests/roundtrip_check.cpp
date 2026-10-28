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

static QByteArray readBytes(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return f.readAll();
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <reference.TMS> [scratch-dir]\n", argv[0]);
        return 2;
    }

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
