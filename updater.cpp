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

    const bool restarted = QProcess::startDetached(
        QCoreApplication::applicationFilePath(),
        QCoreApplication::arguments().mid(1));
    if (parent) {
        QMessageBox::information(parent, tr("Update installed"),
            restarted
                ? tr("The update is installed. The editor will now restart.")
                : tr("The update is installed. Please restart the editor."));
    }
    if (restarted)
        QCoreApplication::quit();
}

// ---------------------------------------------------------------------
// Windows: the running exe locks tms_editor.exe, so the folder cannot be
// overwritten while we are running. Stage a small helper .bat next to the
// exe that (1) waits for this process to exit, (2) unpacks the zip over the
// folder, (3) starts the updated exe — then quit the running app cleanly so
// the file lock is released.
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

    // Paths are inlined in plain text instead of using cmd %VARS%, and any
    // quoting stays inside Explore-Archive's single quotes.
    QString bat;
    bat += QStringLiteral("@echo off\r\n");
    bat += QStringLiteral("rem dlx50 self-update helper (auto-deleted)\r\n");
    bat += QStringLiteral("timeout /t 2 /nobreak >nul\r\n");
    bat += QStringLiteral("if exist \"%1\" rmdir /s /q \"%1\"\r\n").arg(unzipDir);
    bat += QStringLiteral("powershell -NoProfile -ExecutionPolicy Bypass "
                          "-Command \"Expand-Archive -Force -LiteralPath '%1' "
                          "-DestinationPath '%2'\"\r\n").arg(filePath, unzipDir);
    // The release zip's top-level folder is tms_editor/: copy every file and
    // subfolder (DLLs, plugins/…) over the running folder.
    bat += QStringLiteral("xcopy \"%1\\*\" \"%2\\\" /E /Y /I /Q /R /H\r\n")
               .arg(unzipDir + QStringLiteral("/tms_editor"), appDir);
    bat += QStringLiteral("rmdir /s /q \"%1\"\r\n").arg(updateDir);
    bat += QStringLiteral("start \"\" \"%1\"\r\n").arg(self);

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
