#pragma once

// Self-update (issue #3): check GitHub Releases for a newer build, download
// it, verify its SHA-256 when the release ships a SHA256SUMS.txt, install it
// and relaunch. Platform behaviour:
//   * Linux   : download the .deb, install it with pkexec (an explicit
//               privilege-escalation prompt — never silent), then relaunch
//               the installed binary.
//   * Windows : download the portable zip, exit cleanly, and let a small
//               helper script unpack it over the running folder and start
//               the new exe (the running exe's file lock has to be released
//               before it can be replaced).

#include <QObject>
#include <QString>
#include <QNetworkAccessManager>

#include <memory>

class QWidget;
class QNetworkReply;
#include <QTemporaryDir>

class Updater : public QObject
{
    Q_OBJECT

public:
    enum class Platform { Linux, Windows, Unknown };

    explicit Updater(QObject* parent = nullptr);

    static Platform platform();
    static QString platformAssetSuffix();   // "tms_editor-windows-x64.zip" / ".deb"

    static QString currentVersion();        // APP_VERSION (0.0.0 in dev builds)

    // Silent when not interactive (no dialogs on "up to date"); asks for
    // confirmation before downloading/installing when a newer release exists.
    void checkForUpdates(QWidget* parent, bool interactive);

    // Startup auto-check opt-out: --no-update-check argument and/or a saved
    // "skip updates" choice in QSettings.
    static bool startupCheckAllowed();
    static void setStartupCheckEnabled(bool enabled);

    static int compareVersions(const QString& a, const QString& b);

    // Parses a sha256sum-format file ("<64 hex>  <name>") for one asset.
    static bool parseChecksumsFile(const QByteArray& sums,
                                   const QString& assetName,
                                   QString* sha256Out);

    // The Releases feed queried; TMS_UPDATE_API_URL overrides it for tests.
    static QUrl releaseApiUrl();

private slots:
    void handleReleaseReply();
    void handleAssetProgress(qint64 bytesReceived, qint64 bytesTotal);
    void handleAssetReply();
    void handleChecksumReply();

private:
    struct ReleaseInfo {
        QString tag;
        QString notes;
        QString assetName;
        QString assetUrl;
        QString checksumsUrl;    // SHA256SUMS.txt asset, when published
    };

    void offerUpdate(QWidget* parent, const ReleaseInfo& release);
    void startDownload(QWidget* parent, const ReleaseInfo& release);
    void verifyChecksum(QWidget* parent, const ReleaseInfo& release,
                        const QString& filePath, const QByteArray& assetBytes);
    void install(QWidget* parent, const ReleaseInfo& release, const QString& filePath);
    void installLinuxDeb(QWidget* parent, const QString& filePath);
    void installWindowsZip(QWidget* parent, const QString& filePath);

    QNetworkReply* getViaNetwork(const QUrl& url);

    QNetworkAccessManager m_network;
    QNetworkReply* m_releaseReply = nullptr;
    QNetworkReply* m_assetReply = nullptr;
    QNetworkReply* m_shaReply = nullptr;
    QWidget* m_parentWindow = nullptr;
    class QProgressDialog* m_progress = nullptr;

    // State carried across the network stages:
    ReleaseInfo m_currentRelease;              // release we are updating to
    std::unique_ptr<class QTemporaryDir> m_staging;   // must outlive the download
    QString m_stagedPath;
    QByteArray m_assetBytes;
};
