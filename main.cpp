#include <QApplication>
#include "tmseditorwindow.h"

int main(int argc, char *argv[])
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

    QString filePath;
    if (argc > 1)
        filePath = argv[1];

    TmsEditorWindow window(filePath);
    window.show();

    return app.exec();
}
