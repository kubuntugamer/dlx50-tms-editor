// Self-update (issue #3) — see updater.h.

#include "updater.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>

// GitHub repo whose Releases feed the updater.
static const char* kRepoApi =
    "https://api.github.com/repos/kubuntugamer/dlx50-tms-editor/releases/latest";

QUrl Updater::releaseApiUrl()
{
    const QString override = qEnvironmentVariable("TMS_UPDATE_API_URL");
    if (!override.isEmpty())
        return QUrl(override);          // testing hook
    return QUrl(QString::fromLatin1(kRepoApi));
}

static const char* kChecksumsAsset = "SHA256SUMS.txt";

Updater::Platform Updater::platform()
{
#if defined(Q_OS_WINDOWS)
    return Platform::Windows;
#elif defined(Q_OS_LINUX)
    return Platform::Linux;
#else
    return Platform::Unknown;
#endif
}

QString Updater::platformAssetSuffix()
{
    switch (platform()) {
    case Platform::Windows: return QStringLiteral("tms_editor-windows-x64.zip");
    case Platform::Linux:   return QStringLiteral(".deb");
    default:                return QString();
    }
}

QString Updater::currentVersion()
{
    return QStringLiteral(APP_VERSION);
}

int Updater::compareVersions(const QString& a, const QString& b)
{
    const auto strip = [](QString v) {
        v = v.trimmed();
        if (v.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
            v.remove(0, 1);
        return v.trimmed();
    };
    const QStringList pa = strip(a).split('.');
    const QStringList pb = strip(b).split('.');

    const int n = qMax(pa.size(), pb.size());
    for (int i = 0; i < n; ++i) {
        bool okA = false, okB = false;
        const int va = (i < pa.size()) ? pa.at(i).toInt(&okA) : 0;
        const int vb = (i < pb.size()) ? pb.at(i).toInt(&okB) : 0;
        if (okA && okB) {
            if (va != vb) return va < vb ? -1 : 1;
            continue;
        }
        // Non-numeric segments (git shas, "~git"): plain string order.
        const int c = QString::compare(pa.value(i), pb.value(i));
        if (c != 0) return c < 0 ? -1 : 1;
    }
    return 0;
}

bool Updater::startupCheckAllowed()
{
    const QStringList args = QCoreApplication::arguments();
    for (const QString& arg : args) {
        if (arg.compare(QStringLiteral("--no-update-check"),
                        Qt::CaseInsensitive) == 0)
            return false;
    }
    QSettings settings;
    return settings.value(QStringLiteral("autoUpdateCheck"), true).toBool();
}

void Updater::setStartupCheckEnabled(bool enabled)
{
    QSettings settings;
    settings.setValue(QStringLiteral("autoUpdateCheck"), enabled);
}

Updater::Updater(QObject* parent)
    : QObject(parent)
{
}

QNetworkReply* Updater::getViaNetwork(const QUrl& url)
{
    QNetworkRequest request(url);
    // Follow GitHub's redirects (browser_download_url goes to a CDN host)
    // but never downgrade security.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("dlx50-tms-editor-updater/%1")
                          .arg(currentVersion()));
    return m_network.get(request);
}

void Updater::checkForUpdates(QWidget* parent, bool interactive)
{
    m_parentWindow = parent;
    if (m_releaseReply)                 // a check is already in flight
        return;

    QNetworkReply* reply = getViaNetwork(releaseApiUrl());
    m_releaseReply = reply;
    connect(reply, &QNetworkReply::finished,
            this, &Updater::handleReleaseReply);
    Q_UNUSED(interactive);
}

void Updater::handleReleaseReply()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply || reply != m_releaseReply)
        return;
    m_releaseReply = nullptr;
    reply->deleteLater();

    QWidget* parent = m_parentWindow;
    const bool interactive = (parent != nullptr && parent->isVisible());

    if (reply->error() != QNetworkReply::NoError) {
        if (interactive)
            QMessageBox::warning(parent, tr("Update check failed"),
                tr("Could not check for updates:\n%1").arg(reply->errorString()));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(),
                                                      &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (interactive)
            QMessageBox::warning(parent, tr("Update check failed"),
                tr("Could not parse the release list."));
        return;
    }

    const QJsonObject obj = doc.object();
    ReleaseInfo info;
    info.tag = obj.value(QStringLiteral("tag_name")).toString();

    // Trim GitHub's auto-generated "Full Changelog:" footer from notes.
    const QRegularExpression changelogFooter(
        QStringLiteral("(?m)^Full Changelog:.*\\n?"));
    QString notes = obj.value(QStringLiteral("body")).toString();
    notes.replace(changelogFooter, QString());
    info.notes = notes.trimmed();

    // Pick the artifact for this platform, plus the checksum file if present.
    const QString checksumName = QString::fromLatin1(kChecksumsAsset);
    const QJsonArray assets = obj.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue& av : assets) {
        const QJsonObject asset = av.toObject();
        const QString name = asset.value(QStringLiteral("name")).toString();
        const QString url  = asset.value(QStringLiteral("browser_download_url")).toString();

        if (name.compare(checksumName, Qt::CaseInsensitive) == 0) {
            info.checksumsUrl = url;
            continue;
        }
        // Windows: exact portable zip name. Linux: any release .deb.
        if (platform() == Platform::Windows
                && name.compare(platformAssetSuffix(), Qt::CaseInsensitive) == 0) {
            info.assetName = name;
            info.assetUrl  = url;
        } else if (platform() == Platform::Linux
                   && name.endsWith(platformAssetSuffix(), Qt::CaseInsensitive)) {
            info.assetName = name;
            info.assetUrl  = url;
        }
    }

    if (info.tag.isEmpty() || info.assetUrl.isEmpty()
            || compareVersions(info.tag, currentVersion()) <= 0) {
        if (interactive)
            QMessageBox::information(parent, tr("Up to date"),
                tr("You are running the latest version (v%1).")
                    .arg(currentVersion()));
        return;
    }

    offerUpdate(parent, info);
}

void Updater::offerUpdate(QWidget* parent, const ReleaseInfo& release)
{
    QString text = tr("<h3>A new version is available: %1</h3>"
                      "<p>Currently running: v%2</p>")
                       .arg(release.tag.toHtmlEscaped(),
                            currentVersion());
    if (!release.checksumsUrl.isEmpty())
        text += tr("<p>The download will be verified against the release's "
                   "SHA256SUMS before installing.</p>");
    else
        text += tr("<p><b>Note:</b> this release has no SHA256SUMS.txt, so the "
                   "download cannot be checksum-verified.</p>");

    QMessageBox box(QMessageBox::Information, tr("Update available"),
                    text, QMessageBox::NoButton, parent);
    box.setTextFormat(Qt::RichText);
    if (!release.notes.isEmpty())
        box.setDetailedText(release.notes);

    QPushButton* updateBtn = box.addButton(tr("Download && install"),
                                           QMessageBox::AcceptRole);
    QPushButton* cancelBtn = box.addButton(QMessageBox::Cancel);
    QPushButton* skipBtn = nullptr;
    if (parent && parent->isVisible())
        skipBtn = box.addButton(tr("Skip this and future update checks"),
                                QMessageBox::ActionRole);
    box.exec();

    if (box.clickedButton() == skipBtn) {
        setStartupCheckEnabled(false);
        return;
    }
    if (box.clickedButton() == cancelBtn || box.clickedButton() != updateBtn)
        return;

    startDownload(parent, release);
}

void Updater::startDownload(QWidget* parent, const ReleaseInfo& release)
{
    m_parentWindow = parent;
    m_currentRelease = release;

    m_progress = new QProgressDialog(
        tr("Downloading %1 …").arg(release.assetName),
        tr("Cancel"), 0, 0, parent);
    m_progress->setWindowModality(Qt::WindowModal);
    m_progress->setMinimumDuration(0);
    m_progress->setValue(0);

    // Keep the staging dir alive across the network stages; the .deb has to
    // stay on disk until dpkg has run, the zip until the helper unpacks it.
    m_staging = std::make_unique<QTemporaryDir>();

    QNetworkReply* reply = getViaNetwork(QUrl(release.assetUrl));
    m_assetReply = reply;
    connect(reply, &QNetworkReply::downloadProgress,
            this, &Updater::handleAssetProgress);
    connect(reply, &QNetworkReply::finished,
            this, &Updater::handleAssetReply);
    connect(m_progress, &QProgressDialog::canceled,
            reply, &QNetworkReply::abort);
}

void Updater::handleAssetProgress(qint64 received, qint64 total)
{
    if (!m_progress)
        return;
    if (total > 0) {
        m_progress->setRange(0, 100);
        m_progress->setValue(int(received * 100 / total));
    } else {
        m_progress->setRange(0, 0);      // indeterminate
    }
}

void Updater::handleAssetReply()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply || reply != m_assetReply)
        return;
    m_assetReply = nullptr;
    reply->deleteLater();

    const ReleaseInfo release = m_currentRelease;
    QWidget* parent = m_parentWindow;
    if (m_progress) { m_progress->reset(); m_progress->deleteLater(); m_progress = nullptr; }

    if (reply->error() != QNetworkReply::NoError) {
        if (parent)
            QMessageBox::warning(parent, tr("Download failed"),
                tr("The update could not be downloaded: %1")
                    .arg(reply->errorString()));
        return;
    }

    m_assetBytes = reply->readAll();

    if (!m_staging->isValid()) {
        if (parent)
            QMessageBox::warning(parent, tr("Update failed"),
                tr("Could not create a staging directory."));
        return;
    }
    m_stagedPath = m_staging->path() + QLatin1Char('/') + release.assetName;
    QFile out(m_stagedPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_stagedPath.clear();
        if (parent)
            QMessageBox::warning(parent, tr("Update failed"),
                tr("Could not write %1: %2").arg(m_stagedPath, out.errorString()));
        return;
    }
    out.write(m_assetBytes);
    out.close();

    verifyChecksum(parent, release, m_stagedPath, m_assetBytes);
}

void Updater::verifyChecksum(QWidget* parent, const ReleaseInfo& release,
                             const QString& filePath,
                             const QByteArray& assetBytes)
{
    if (release.checksumsUrl.isEmpty()) {
        // Nothing to verify against — the offer dialog already warned, and
        // the user confirmed the install anyway.
        install(parent, release, filePath);
        return;
    }

    QNetworkReply* reply = getViaNetwork(QUrl(release.checksumsUrl));
    m_shaReply = reply;
    connect(reply, &QNetworkReply::finished,
            this, &Updater::handleChecksumReply);
    Q_UNUSED(filePath);
    Q_UNUSED(assetBytes);
}

void Updater::handleChecksumReply()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply || reply != m_shaReply)
        return;
    m_shaReply = nullptr;
    reply->deleteLater();

    const ReleaseInfo release = m_currentRelease;
    const QString filePath = m_stagedPath;
    QWidget* parent = m_parentWindow;

    if (reply->error() != QNetworkReply::NoError) {
        if (parent)
            QMessageBox::warning(parent, tr("Update failed"),
                tr("Downloaded %1 but could not fetch SHA256SUMS.txt to "
                   "verify it. Install was cancelled; the file is at:\n%2")
                    .arg(release.assetName, filePath));
        return;
    }

    QString expected;
    if (!parseChecksumsFile(reply->readAll(), release.assetName, &expected)
            || expected.isEmpty()) {
        if (parent)
            QMessageBox::warning(parent, tr("Update failed"),
                tr("SHA256SUMS.txt does not contain an entry for %1. Install "
                   "was cancelled; the file is at:\n%2")
                    .arg(release.assetName, filePath));
        return;
    }

    const QByteArray actual =
        QCryptographicHash::hash(m_assetBytes, QCryptographicHash::Sha256)
            .toHex();
    if (QString::fromLatin1(actual) != expected) {
        if (parent)
            QMessageBox::critical(parent, tr("Update failed"),
                tr("Checksum mismatch for %1 — refusing to install. "
                   "(expected %2, got %3)")
                    .arg(release.assetName, expected,
                         QString::fromLatin1(actual)));
        return;
    }

    install(parent, release, filePath);
}

bool Updater::parseChecksumsFile(const QByteArray& sums,
                                 const QString& assetName,
                                 QString* sha256Out)
{
    // sha256sum format: "<64 hex chars> <name>" — text mode uses two spaces
    // before the name, binary mode a space + '*' before it.
    static const QRegularExpression hex(
        QStringLiteral("^[0-9a-fA-F]{64}$"));
    const QList<QByteArray> lines = sums.split('\n');
    for (const QByteArray& rawLine : lines) {
        const QByteArray line = rawLine.trimmed();
        if (line.size() < 65 || line.at(64) != ' ')
            continue;
        const QString hexStr = QString::fromLatin1(line.left(64));
        if (!hexStr.contains(hex))
            continue;
        int i = 64;
        while (i < line.size() && line.at(i) == ' ')
            ++i;
        QString name = QString::fromLatin1(line.mid(i));
        if (name.startsWith(QLatin1Char('*')))
            name.remove(0, 1);
        if (name.compare(assetName, Qt::CaseInsensitive) == 0) {
            if (sha256Out)
                *sha256Out = hexStr.toLower();
            return true;
        }
    }
    return false;
}

void Updater::install(QWidget* parent, const ReleaseInfo& release,
                      const QString& filePath)
{
    Q_UNUSED(release);
    switch (platform()) {
    case Platform::Linux:
        installLinuxDeb(parent, filePath);
        break;
    case Platform::Windows:
        installWindowsZip(parent, filePath);
        break;
    default:
        if (parent)
            QMessageBox::warning(parent, tr("Update"),
                tr("Automatic install is only wired up for Linux and Windows "
                   "builds. The download is at:\n%1").arg(filePath));
        break;
    }
}

// ---------------------------------------------------------------------
// Linux: install the .deb with pkexec — polkit shows an explicit
// privilege-escalation prompt and dpkg only runs after the user authorises.
// Nothing is installed silently. On success, relaunch the installed binary
// (dpkg has replaced both /usr/bin/dlx50-tms-editor and its /opt target).
// ---------------------------------------------------------------------
void Updater::installLinuxDeb(QWidget* parent, const QString& filePath)
{
    QProcess pkexec;
    pkexec.setProgram(QStringLiteral("pkexec"));
    pkexec.setArguments({ QStringLiteral("dpkg"),
                          QStringLiteral("-i"),
                          filePath });

    pkexec.start();

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&pkexec, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            &loop, &QEventLoop::quit);
    connect(&pkexec, &QProcess::errorOccurred,
            &loop, &QEventLoop::quit);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(10 * 60 * 1000);       // auth + dpkg can take a while
    loop.exec();

    const bool timedOut = !timeout.isActive();
    if (timedOut) {
        timeout.stop();
        pkexec.kill();
        pkexec.waitForFinished(2000);
    }

    if (pkexec.state() != QProcess::NotRunning && !timedOut) {
        // errorOccurred with the process still up is treated as a failure.
        pkexec.kill();
        pkexec.waitForFinished(2000);
    }

    if (pkexec.error() == QProcess::FailedToStart) {
        if (parent)
            QMessageBox::warning(parent, tr("Update failed"),
                tr("pkexec is not available, so the .deb cannot be installed "
                   "with a privilege-escalation prompt.\n"
                   "The package is parked at:\n%1\n"
                   "Install it manually with:\n  sudo dpkg -i %1")
                    .arg(filePath));
        return;
    }

    if (pkexec.exitStatus() != QProcess::NormalExit
            || pkexec.exitCode() != 0) {
        // polkit returns 126 when the user dismisses the authentication
        // prompt — that is just "not now", so no error dialog here.
        const int code = pkexec.exitCode();
        if (parent) {
            if (timedOut)
                QMessageBox::information(parent, tr("Update"),
                    tr("Installation timed out. Nothing was changed."));
            else if (code != 126)
                QMessageBox::information(parent, tr("Not installed"),
                    tr("Installation was not completed (exit %1).\n"
                       "The package is parked at:\n%2").arg(code).arg(filePath));
        }
        return;
    }

    // Choose what to relaunch. dpkg replaced the packaged binary in place,
    // so a .deb-installed copy restarts itself. When we are running a dev
    // build (e.g. a CMake build tree), relaunching self would come back as
    // the same un-updated binary — prefer the freshly installed one.
    QString relaunchPath = QCoreApplication::applicationFilePath();
    const QString usrBin = QStringLiteral("/usr/bin/dlx50-tms-editor");
    const QString optBin = QStringLiteral("/opt/dlx50-tms-editor/tms_editor");
    if (relaunchPath != usrBin && relaunchPath != optBin) {
        if (QFile::exists(usrBin))
            relaunchPath = usrBin;
        else if (QFile::exists(optBin))
            relaunchPath = optBin;
        else
            relaunchPath.clear();
    }

    if (relaunchPath.isEmpty()) {
        if (parent)
            QMessageBox::information(parent, tr("Update installed"),
                tr("The update is installed. Please restart the editor."));
        return;
    }

    const bool restarted = QProcess::startDetached(
        relaunchPath, QCoreApplication::arguments().mid(1));
    if (parent) {
        QMessageBox::information(parent, tr("Update installed"),
            restarted
                ? tr("The update is installed. The editor will now restart.")
                : tr("The update is installed. Please restart the editor."));
    }
    if (restarted)
        QCoreApplication::quit();
}

namespace {

// Issue #12: everything below is pasted into a cmd.exe batch file, so a path
// has to survive cmd's own parsing. Inside a double-quoted argument cmd takes
// &, |, <, >, ^, (, ), !, ; and , as plain text, and a % can be neutralised
// by doubling it — but a double quote would end the argument and a control
// character would corrupt the script line, so those two are refused outright
// instead of guessed at.
bool helperPathIsUsable(const QString& path)
{
    if (path.isEmpty())
        return false;
    for (const QChar c : path) {
        if (c == QLatin1Char('"') || c.unicode() < 0x20)
            return false;
    }
    return true;
}

// Escapes a path for a batch file: a lone % would be read as the start of a
// variable reference, so it is doubled (batch collapses %% back to %).
QString cmdEscape(const QString& path)
{
    QString escaped = path;
    escaped.replace(QLatin1String("%"), QLatin1String("%%"));
    return escaped;
}

// The same path as one double-quoted cmd.exe argument, which is what keeps
// the metacharacters literal.
QString cmdQuote(const QString& path)
{
    return QLatin1Char('"') + cmdEscape(path) + QLatin1Char('"');
}

} // namespace

// Builds the Windows self-update helper (issue #12). Split out of
// installWindowsZip so the headless test can inspect the script the app
// would really write. Three rules the script follows:
//   * cmd gets every path as a double-quoted argument with % doubled;
//   * PowerShell gets its two paths through environment variables instead of
//     having them pasted inside its own quoting, so no ' or " can break out;
//   * every step that can fail (clean, unpack, copy) ends in an errorlevel
//     check that jumps to a message and exits non-zero — the relaunch only
//     exists in the success fall-through.
bool Updater::buildWindowsUpdateHelper(const UpdateHelperPaths& p,
                                       QString* script, QString* error)
{
    const QStringList all{ p.appDir, p.unzipDir, p.persistedZip,
                           p.updateDir, p.selfExe };
    for (const QString& value : all) {
        if (helperPathIsUsable(value))
            continue;
        if (error) {
            *error = tr("The update helper was not written: the path\n"
                        "  %1\n"
                        "contains a double quote or a control character, "
                        "which cannot be passed safely to a batch script.\n\n"
                        "Move the editor to a folder without those "
                        "characters and update again.").arg(value);
        }
        return false;
    }

    // No trailing backslash on the copy target: "C:\dir\" would hand xcopy
    // an escaped quote instead of a closing one. /I tells xcopy the target is
    // a directory, so it does not prompt for it.
    const QString zipEnv   = cmdEscape(p.persistedZip);
    const QString unzipEnv = cmdEscape(p.unzipDir);
    const QString srcGlob  = cmdQuote(p.unzipDir + QStringLiteral("/tms_editor")
                                      + QStringLiteral("\\*"));
    const QString appDir   = cmdQuote(p.appDir);
    const QString unzipDir = cmdQuote(p.unzipDir);
    const QString updateDir = cmdQuote(p.updateDir);
    const QString zip      = cmdQuote(p.persistedZip);
    const QString self     = cmdQuote(p.selfExe);

    QString s;
    s += QStringLiteral("@echo off\r\n");
    s += QStringLiteral("rem dlx50 self-update helper (auto-deleted)\r\n");
    s += QStringLiteral("rem Paths ride in environment variables and quoted\r\n");
    s += QStringLiteral("rem arguments only, never inlined (issue #12).\r\n");
    s += QStringLiteral("timeout /t 2 /nobreak >nul\r\n");
    s += QStringLiteral("setlocal\r\n");
    s += QStringLiteral("set \"TMS_UPDATE_ZIP=%1\"\r\n").arg(zipEnv);
    s += QStringLiteral("set \"TMS_UPDATE_UNZIP=%1\"\r\n").arg(unzipEnv);
    s += QStringLiteral("\r\n");
    s += QStringLiteral("if exist %1 (\r\n").arg(unzipDir);
    s += QStringLiteral("    rmdir /s /q %1\r\n").arg(unzipDir);
    s += QStringLiteral("    if errorlevel 1 goto :fail_clean\r\n");
    s += QStringLiteral(")\r\n");
    s += QStringLiteral(
        "powershell -NoProfile -ExecutionPolicy Bypass -Command "
        "\"Expand-Archive -Force -LiteralPath $env:TMS_UPDATE_ZIP "
        "-DestinationPath $env:TMS_UPDATE_UNZIP\"\r\n");
    s += QStringLiteral("if errorlevel 1 goto :fail_unpack\r\n");
    s += QStringLiteral("xcopy %1 %2 /E /Y /I /Q /R /H\r\n")
             .arg(srcGlob, appDir);
    s += QStringLiteral("if errorlevel 1 goto :fail_copy\r\n");
    s += QStringLiteral("rmdir /s /q %1\r\n").arg(updateDir);
    s += QStringLiteral(
        "if errorlevel 1 echo NOTE: staging folder left behind: %1\r\n")
        .arg(updateDir);
    s += QStringLiteral("start \"\" %1\r\n").arg(self);
    s += QStringLiteral("exit /b 0\r\n");
    s += QStringLiteral("\r\n");
    s += QStringLiteral(":fail_clean\r\n");
    s += QStringLiteral(
        "echo The update was NOT installed: the staging folder could not be cleared.\r\n");
    s += QStringLiteral("echo   %1\r\n").arg(unzipDir);
    s += QStringLiteral("goto :fail_end\r\n");
    s += QStringLiteral(":fail_unpack\r\n");
    s += QStringLiteral(
        "echo The update was NOT installed: the downloaded archive could not be unpacked.\r\n");
    s += QStringLiteral("echo   %1\r\n").arg(zip);
    s += QStringLiteral("goto :fail_end\r\n");
    s += QStringLiteral(":fail_copy\r\n");
    s += QStringLiteral("echo The update was NOT installed: copying the new files into\r\n");
    s += QStringLiteral("echo   %1\r\n").arg(appDir);
    s += QStringLiteral(
        "echo failed. The editor was NOT restarted and the installed files may be incomplete.\r\n");
    s += QStringLiteral("goto :fail_end\r\n");
    s += QStringLiteral(":fail_end\r\n");
    s += QStringLiteral("echo.\r\n");
    s += QStringLiteral("echo Nothing was restarted. The downloaded archive is still at\r\n");
    s += QStringLiteral("echo   %1\r\n").arg(zip);
    s += QStringLiteral("echo Press any key to close this window.\r\n");
    s += QStringLiteral("pause >nul\r\n");
    s += QStringLiteral("exit /b 1\r\n");

    if (script)
        *script = s;
    return true;
}

// ---------------------------------------------------------------------
// Windows: the running exe locks tms_editor.exe, so the folder cannot be
// overwritten while we are running. Stage a small helper .bat next to the
// exe that (1) waits for this process to exit, (2) unpacks the zip over the
// folder, (3) starts the updated exe — then quit the running app cleanly so
// the file lock is released. The script itself comes from
// buildWindowsUpdateHelper (issue #12).
// ---------------------------------------------------------------------
void Updater::installWindowsZip(QWidget* parent, const QString& filePath)
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString self   = QCoreApplication::applicationFilePath();

    // Keep the helper next to the exe (not in %TEMP%): it has to survive
    // after this process exits and until the new exe is running.
    const QString updateDir = appDir + QStringLiteral("/_update");
    QDir().mkpath(updateDir);
    const QString unzipDir = updateDir + QStringLiteral("/new");

    // The staged zip lives in a QTemporaryDir that is removed when this
    // process exits — and the helper only starts unpacking AFTER we exit,
    // so the archive must be copied somewhere persistent first.
    const QString persistedZip = updateDir
        + QStringLiteral("/") + QFileInfo(filePath).fileName();
    if (QFile::exists(persistedZip))
        QFile::remove(persistedZip);
    if (!QFile::copy(filePath, persistedZip)) {
        if (parent)
            QMessageBox::warning(parent, tr("Update failed"),
                tr("Could not stage the update next to the editor."));
        return;
    }

    Updater::UpdateHelperPaths helperPaths;
    helperPaths.appDir       = appDir;
    helperPaths.unzipDir     = unzipDir;
    helperPaths.persistedZip = persistedZip;
    helperPaths.updateDir    = updateDir;
    helperPaths.selfExe      = self;

    QString bat;
    QString buildError;
    if (!buildWindowsUpdateHelper(helperPaths, &bat, &buildError)) {
        // Refused rather than guessed at: an unquoted install path is not
        // worth running arbitrary cmd.exe lines over. Nothing was launched,
        // so the editor keeps running and the staged zip is cleaned up by
        // the next attempt.
        if (parent)
            QMessageBox::warning(parent, tr("Update failed"), buildError);
        return;
    }

    const QString batPath = updateDir + QStringLiteral("/tms_editor_update.bat");
    {
        QFile f(batPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (parent)
                QMessageBox::warning(parent, tr("Update failed"),
                    tr("Could not write the update helper: %1")
                        .arg(f.errorString()));
            return;
        }
        f.write(bat.toUtf8());
    }

    const bool started = QProcess::startDetached(
        QStringLiteral("cmd.exe"),
        { QStringLiteral("/c"), batPath },
        appDir);

    if (!started) {
        if (parent)
            QMessageBox::warning(parent, tr("Update failed"),
                tr("Could not start the update helper."));
        return;
    }

    if (parent)
        QMessageBox::information(parent, tr("Update installed"),
            tr("The update is staged. The editor will now close and relaunch "
               "by itself."));
    QCoreApplication::quit();            // releases the exe lock for the helper
}
