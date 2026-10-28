#pragma once

// Transport abstraction (issue #7): common interface the editor can use to
// list/retrieve .TMS files from the scale and save edited files back, over
// USB, serial, and/or network as the hardware supports.
//
// This header only; the implementations live in transport_*.cpp files that
// are added to CMakeLists.txt when the relevant hardware/OS support is ready.

#include <QString>
#include <QStringList>
#include <QMap>
#include <QPair>

class QTcpSocket;

// One file available on the scale, as reported by a transport.
struct TransportFile {
    QString remotePath;   // path/name as the scale sees it
    QString localName;    // friendly name for the UI (e.g. A_000.TMS)
    qint64 size = 0;      // size in bytes, when known
};

// Result of a single transport operation.
struct TransportResult {
    enum class Status {
        Ok,
        NotSupported,     // this transport backend does not implement the op yet
        Unreachable,      // could not connect to the scale
        ProtocolError,    // the scale returned something unexpected
        AccessDenied,     // authenticated/authorised refused
        IoError,          // local read/write failure
    };
    Status status = Status::Ok;
    QString detail;       // human-readable message for the status bar / logs
};

// Forward declaration of the transport factory; concrete backends register
// themselves and the editor picks one based on what the user asked for and
// what the machine actually has.
class TransportBackend {
public:
    virtual ~TransportBackend() = default;

    // Human-readable name shown in the UI, e.g. "USB attached storage",
    // "Serial (COM3, 9600 8N1)", "Network (10.86.215.95:9100)".
    virtual QString name() const = 0;

    // Whether this backend is usable on this machine right now (ports present,
    // devices mounted, network reachable). Cheap — used to grey out disabled
    // options in the UI.
    virtual bool isUsable() const = 0;

    // List .TMS files the scale reports as available for retrieval.
    virtual TransportResult listFiles(QList<TransportFile>* out) const = 0;

    // Retrieve a remote .TMS file into a local path. The caller is responsible
    // for the local path; this only performs the transfer.
    virtual TransportResult retrieveFile(const QString& remotePath,
                                        const QString& localPath) const = 0;

    // Upload a local .TMS file to the scale at the given remote path.
    virtual TransportResult uploadFile(const QString& localPath,
                                      const QString& remotePath) const = 0;
};

// Registry for the transport backends. Backends call registerBackend()
// during static initialisation (or from a plugin), and the editor calls
// availableBackends() to populate the transport picker.
class TransportRegistry {
public:
    static void registerBackend(TransportBackend* backend);
    static QList<TransportBackend*> availableBackends();
    static void clear();   // for tests only

private:
    static QList<TransportBackend*>& backends();
};

// ---------------------------------------------------------------------
// Network scale probe (issue #8): read-only discovery of what a scale
// agent exposes on the LAN, before any transfer code is written.
//
// Usage (CLI, headless):
//   dlx50-tms-editor --probe-scale HOST PORT [TIMEOUT_MS]
//
// This is intentionally read-only: it opens a TCP connection, sends a
// minimal handshake / probe, prints whatever the scale returns, and
// closes without sending any write/delete/reboot/transfer command.
// Results are meant to be pasted into the parent transport issue (#7)
// as implementation notes.
// ---------------------------------------------------------------------

struct NetworkProbeResult {
    bool connected = false;
    QString host;
    int port = 0;
    int timeoutMs = 5000;

    // Whatever the scale sent back, captured raw.
    QString rawResponse;          // text-ish capture (UTF-8, lossily)
    QByteArray rawBytes;          // exact bytes received

    // Lines the probe emitted for readability.
    QStringList lines;

    // Implementation notes filled in by the probe based on what it saw.
    QString summary;              // one-paragraph summary for the issue
    QStringList observations;     // bullet points
};

// Run a read-only probe against a scale agent endpoint. Returns the result;
// a failed connection is still a valid result (connected == false) — the
// caller decides whether that is "no scale here" or "wrong port".
NetworkProbeResult probeScale(const QString& host, int port,
                              int timeoutMs = 5000);
