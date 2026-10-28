// Transport abstraction + read-only network probe (issues #7 and #8).
//
// The probe is written with Qt sockets rather than raw POSIX sockets on
// purpose: the same file is compiled for the Windows release build (see
// .github/workflows/build-windows.yml), where <sys/socket.h> does not
// exist. QTcpSocket's blocking waitFor* calls keep it usable from the
// headless CLI, which never enters an event loop.

#include "transport.h"

#include <QAbstractSocket>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QNetworkProxy>
#include <QTcpSocket>

#include <cstring>   // memcpy (ProbePayload)

static inline QString _tr(const char* s) { return QCoreApplication::translate("transport", s); }

// ---------------------------------------------------------------------
// TransportRegistry
// ---------------------------------------------------------------------

QList<TransportBackend*>& TransportRegistry::backends() {
    static QList<TransportBackend*> s_backends;
    return s_backends;
}

void TransportRegistry::registerBackend(TransportBackend* backend) {
    if (!backend) return;
    auto& b = backends();
    if (!b.contains(backend))
        b.append(backend);
}

QList<TransportBackend*> TransportRegistry::availableBackends() {
    return backends();
}

void TransportRegistry::clear() {
    backends().clear();
}

// ---------------------------------------------------------------------
// Network probe (issue #8): read-only discovery of a LAN scale agent.
//
// Runs without an event loop (works in the headless CLI and in
// roundtrip_check's offscreen mode) because it only uses QTcpSocket's
// blocking waitFor* calls.
//
// Strategy: connect, send a small probe message, read with a short deadline,
// capture whatever comes back, close. No write/delete/reboot/transfer
// command is ever sent.
//
// The probe message itself is a placeholder until we know the real protocol
// from the scale's response. Right now it is a minimal 8-byte handshake
// probe that any LAN scale agent endpoint should either answer or reject in
// a parseable way. The exact bytes will be adjusted once we have one real
// response to compare against.
// ---------------------------------------------------------------------

namespace {

// A minimal probe payload. This is intentionally generic: a short handshake
// that asks the agent to identify itself. The value will be updated once a
// real scale response is captured and we can match the protocol.
//
// Format (tentative, to be verified against the first real response):
//   <4 bytes: probe magic "SCAL"> <1 byte: probe command 0x01 = "who are you">
//   <3 bytes: padding>
struct ProbePayload {
    static QByteArray make() {
        QByteArray out;
        out.resize(8);
        const char magic[] = "SCAL";
        memcpy(out.data(), magic, 4);
        out[4] = static_cast<char>(0x01);   // probe command: identify
        out[5] = static_cast<char>(0x00);
        out[6] = static_cast<char>(0x00);
        out[7] = static_cast<char>(0x00);
        return out;
    }
};

// After the first byte arrives, keep draining in short slices so a response
// that is written in several TCP segments still lands in one capture, but
// never hold the caller longer than this idle window per slice.
constexpr int kIdleSliceMs = 150;

} // namespace

NetworkProbeResult probeScale(const QString& host, int port, int timeoutMs) {
    NetworkProbeResult r;
    r.host = host;
    r.port = port;
    r.timeoutMs = timeoutMs;

    if (port <= 0 || port > 65535) {
        r.summary = QString("invalid port %1 — must be 1..65535").arg(port);
        r.observations.append(r.summary);
        return r;
    }
    if (host.trimmed().isEmpty()) {
        r.summary = QStringLiteral("no host given");
        r.observations.append(r.summary);
        return r;
    }

    QTcpSocket socket;
    // A scale agent is a local device, not an HTTP endpoint — never let a
    // system/proxy setting redirect this probe somewhere else.
    socket.setProxy(QNetworkProxy::NoProxy);

    QElapsedTimer deadline;
    deadline.start();

    // --- connect (read-only: nothing is sent until this succeeds) -------
    socket.connectToHost(host, port);
    if (!socket.waitForConnected(timeoutMs)) {
        if (socket.error() == QAbstractSocket::SocketTimeoutError) {
            r.summary = QString("connect to %1:%2 timed out after %3 ms")
                            .arg(host).arg(port).arg(timeoutMs);
        } else {
            r.summary = QString("connect to %1:%2 failed: %3")
                            .arg(host).arg(port).arg(socket.errorString());
        }
        r.observations.append(r.summary);
        return r;
    }

    r.connected = true;
    r.observations.append(QString("connected to %1:%2").arg(host).arg(port));

    // --- send the probe payload -----------------------------------------
    {
        const QByteArray probe = ProbePayload::make();
        const qint64 written = socket.write(probe);
        if (written != probe.size()) {
            r.summary = QString("probe send failed: %1").arg(socket.errorString());
            r.observations.append(r.summary);
        } else if (!socket.waitForBytesWritten(timeoutMs)) {
            r.summary = QString("probe send timed out after %1 ms: %2")
                            .arg(timeoutMs).arg(socket.errorString());
            r.observations.append(r.summary);
        } else {
            r.observations.append(
                QString("sent %1-byte probe payload").arg(probe.size()));
        }
    }

    // --- read whatever comes back, until close / deadline / idle --------
    QByteArray received;
    bool sawData = false;
    bool peerClosed = false;
    while (true) {
        const qint64 remaining = timeoutMs - deadline.elapsed();
        if (remaining <= 0)
            break;

        const int slice = sawData ? static_cast<int>(qMin<qint64>(kIdleSliceMs, remaining))
                                  : static_cast<int>(remaining);
        if (socket.waitForReadyRead(slice)) {
            received += socket.readAll();
            sawData = true;
            continue;
        }
        if (socket.bytesAvailable() > 0) {
            received += socket.readAll();
            sawData = true;
            continue;
        }
        if (socket.state() == QAbstractSocket::UnconnectedState) {
            peerClosed = true;          // peer closed the connection
            break;
        }
        break;                          // nothing more within this slice
    }
    if (socket.state() == QAbstractSocket::UnconnectedState)
        peerClosed = true;
    received += socket.readAll();

    r.rawBytes = received;
    r.rawResponse = QString::fromUtf8(received);

    // --- classify what we saw (tentative, refined once a real response
    //     from the scale has been captured) ------------------------------
    if (received.isEmpty()) {
        if (peerClosed) {
            r.summary = QString("connected to %1:%2 but the scale closed the "
                                "connection without sending any data")
                            .arg(host).arg(port);
            r.observations.append(r.summary);
            r.observations.append(
                _tr("no response payload captured — this may be the wrong "
                    "port, the wrong probe command, or an endpoint that "
                    "requires authentication first."));
        } else {
            r.summary = QString("connected to %1:%2 but the scale sent no data "
                                "within the %3 ms timeout")
                            .arg(host).arg(port).arg(timeoutMs);
            r.observations.append(r.summary);
            r.observations.append(_tr("no response payload captured"));
        }
        return r;
    }

    r.observations.append(QString("received %1 byte(s)").arg(received.size()));

    QString text = r.rawResponse;
    if (text.isEmpty()) {
        // Not valid UTF-8 — fall back to a placeholder; the caller still
        // prints the exact hex below.
        text = QString("[binary payload, %1 bytes — see hex]").arg(received.size());
    }
    r.lines = text.split('\n', Qt::KeepEmptyParts);
    for (QString& l : r.lines) l = l.trimmed();

    QStringList notes;
    notes.append(QString("scale agent at %1:%2 responded with %3 byte(s)")
                     .arg(host).arg(port).arg(received.size()));

    if (received.size() <= 256) {
        notes.append(QString("raw payload (hex): %1")
                         .arg(QString::fromLatin1(received.toHex())));
    } else {
        notes.append(QString("raw payload too large to inline (>256 bytes); "
                             "first 256 hex: %1")
                         .arg(QString::fromLatin1(received.left(256).toHex())));
    }

    if (!r.rawResponse.isEmpty()) {
        notes.append(QString("decoded as UTF-8 (%1):").arg(r.rawResponse.size()));
        for (const QString& l : r.lines)
            notes.append(QString("  | %1").arg(l));
    }

    const QString upper = r.rawResponse.toUpper();
    if (upper.contains("ERROR") || upper.contains("ERR")) {
        notes.append(_tr("scale returned an error-style payload — likely an "
                         "unsupported command or missing authentication."));
    } else if (upper.contains("TMS") || upper.contains("DLX")) {
        notes.append(_tr("payload mentions TMS/DLX — likely relevant to the "
                         "transfer protocol we are trying to discover."));
    }

    r.summary = notes.join("\n");
    r.observations = notes;
    return r;
}

// ---------------------------------------------------------------------
// Placeholder backends (stub enough to compile and for the UI to list
// "not implemented yet" transports without crashing). Real USB/serial/
// network transfer backends go in transport_usb.cpp, transport_serial.cpp,
// transport_network.cpp once the protocols are known.
// ---------------------------------------------------------------------

class NetworkTransportBackend : public TransportBackend {
public:
    QString name() const override {
        return _tr("Network (scale agent TCP)");
    }

    bool isUsable() const override {
        // Network is always "usable" in the sense that the code path exists;
        // actual reachability is discovered at connect time.
        return true;
    }

    TransportResult listFiles(QList<TransportFile>* out) const override {
        TransportResult r;
        r.status = TransportResult::Status::NotSupported;
        r.detail = _tr("network file listing not implemented yet — "
                      "discover the scale's protocol first (#8)");
        return r;
    }

    TransportResult retrieveFile(const QString& remotePath,
                                 const QString& localPath) const override {
        TransportResult r;
        r.status = TransportResult::Status::NotSupported;
        r.detail = _tr("network file retrieval not implemented yet — "
                      "discover the scale's protocol first (#8)");
        return r;
    }

    TransportResult uploadFile(const QString& localPath,
                               const QString& remotePath) const override {
        TransportResult r;
        r.status = TransportResult::Status::NotSupported;
        r.detail = _tr("network file upload not implemented yet — "
                      "discover the scale's protocol first (#8)");
        return r;
    }
};

// Stubs for USB and serial so the registry can list them. Real ones go in
// separate files once the hardware transport is known.
class UsbTransportBackend : public TransportBackend {
public:
    QString name() const override {
        return _tr("USB attached storage");
    }
    bool isUsable() const override { return false; }
    TransportResult listFiles(QList<TransportFile>*) const override {
        TransportResult r;
        r.status = TransportResult::Status::NotSupported;
        r.detail = _tr("USB transport not implemented yet");
        return r;
    }
    TransportResult retrieveFile(const QString&, const QString&) const override {
        TransportResult r;
        r.status = TransportResult::Status::NotSupported;
        r.detail = _tr("USB transport not implemented yet");
        return r;
    }
    TransportResult uploadFile(const QString&, const QString&) const override {
        TransportResult r;
        r.status = TransportResult::Status::NotSupported;
        r.detail = _tr("USB transport not implemented yet");
        return r;
    }
};

class SerialTransportBackend : public TransportBackend {
public:
    QString name() const override {
        return _tr("Serial (RS-232 / COM / /dev/tty*)");
    }
    bool isUsable() const override { return false; }
    TransportResult listFiles(QList<TransportFile>*) const override {
        TransportResult r;
        r.status = TransportResult::Status::NotSupported;
        r.detail = _tr("serial transport not implemented yet");
        return r;
    }
    TransportResult retrieveFile(const QString&, const QString&) const override {
        TransportResult r;
        r.status = TransportResult::Status::NotSupported;
        r.detail = _tr("serial transport not implemented yet");
        return r;
    }
    TransportResult uploadFile(const QString&, const QString&) const override {
        TransportResult r;
        r.status = TransportResult::Status::NotSupported;
        r.detail = _tr("serial transport not implemented yet");
        return r;
    }
};

// Static registration — these appear in the UI as "not yet implemented"
// options until real backends are added. The network backend is the first
// one we care about for issue #8.
namespace {
    UsbTransportBackend   g_usbBackend;
    SerialTransportBackend g_serialBackend;
    NetworkTransportBackend g_networkBackend;
    struct Register {
        Register() {
            TransportRegistry::registerBackend(&g_usbBackend);
            TransportRegistry::registerBackend(&g_serialBackend);
            TransportRegistry::registerBackend(&g_networkBackend);
        }
    } g_register;
}
