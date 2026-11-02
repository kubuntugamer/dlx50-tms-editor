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
#include <QFile>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QNetworkProxy>
#include <QTcpSocket>
#include <QUdpSocket>

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

// ---------------------------------------------------------------------
// DL-series network protocol (issues #7 and #8)
//
// Reverse-engineered black-box: the scale's own beacons gave the ports,
// and the message templates were recovered from the .NET user-string
// heap of the OEM's management program. Everything on the wire is ASCII,
// TAB-separated and CRLF-terminated:
//
//   request : "UPL\t<SECTION>\t\r\n"      read-only — the scale sends,
//                                          the PC only receives
//   response: "\r\nDWL\t<SECTION>\t\r\n"
//             "<SECTION>\t<fields>\t\r\n"    zero or more data rows
//             "END\t<SECTION>\t\r\n"
//
// The scale keeps the connection open after answering, so the reader
// stops at the END marker instead of waiting for the peer to close.
// An unknown section name is answered with an empty DWL/END pair, which
// is exactly what an empty section looks like in a real export.
//
// Discovery is passive, the way the vendor's utility does it: the scale
// beacons to UDP 33594 roughly every 10 s ("PTR\t\r\n") and to UDP 33584
// roughly every 60 s ("UDP\t<port>\t<station>\t\r\n"). Listening on both
// yields the scale's address within one beacon interval; the second
// beacon also carries its TCP port (Spec166, default 33581).
// ---------------------------------------------------------------------

namespace DlNetwork {

constexpr int kDefaultPort = 33581;   // Spec166, the scale's server port
constexpr int kBeaconPort  = 33594;   // destination of the 10 s beacon
constexpr int kInfoPort    = 33584;   // destination of the 60 s beacon
constexpr int kDiscoverMs  = 12000;   // must exceed the 10 s beacon gap
constexpr int kSectionMs   = 30000;   // PLU takes ~6 s; headroom for a slow one
constexpr int kPollMs      = 250;     // slice used while waiting on two sockets

// The file's own header/footer. The scale exposes no section for it, and
// Save() only rewrites an existing "END<TAB>ECS" line rather than adding
// one, so a retrieved file has to carry it itself. 254 is what this
// device family's exports start with (A_000.TMS and its .prebar both
// begin "ECS\tVER\t254\t"); the OEM's generic template ships 100.
constexpr const char* kEcsHeader = "ECS\tVER\t254\t\r\n";
constexpr const char* kEcsFooter = "\r\nEND\tECS\t\r\n";

// Section order copied from a real device export (see
// docs/dlx50-tms-format.md): DPT, CLS, PLU, UNT, BAR, LAB, SAL, REP, SCF,
// SCP, TMS, TMT, TMR, INF, TIM, BMP — exactly the order the editor reads.
const QStringList& sectionOrder()
{
    static const QStringList order = {
        "DPT", "CLS", "PLU", "UNT", "BAR", "LAB", "SAL",
        "REP", "SCF", "SCP", "TMS", "TMT", "TMR",
        "INF", "TIM", "BMP"
    };
    return order;
}

struct Endpoint {
    QString host;
    int port = kDefaultPort;
    bool heardBeacon = false;
};

// "UDP\t33581\t254\t\r\n" -> 33581. Returns false for anything else.
bool parseBeacon(const QByteArray& datagram, int* port)
{
    if (!datagram.startsWith("UDP\t"))
        return false;
    const QList<QByteArray> fields = datagram.split('\t');
    if (fields.size() < 2)
        return false;
    bool ok = false;
    const int candidate = QString::fromLatin1(fields.at(1).trimmed()).toInt(&ok);
    if (!ok || candidate <= 0 || candidate > 65535)
        return false;
    *port = candidate;
    return true;
}

// Wait for a scale beacon and fill in where to connect.
bool discover(Endpoint* ep, int timeoutMs, QString* err)
{
    QUdpSocket ptrSock, infoSock;
    const auto mode = QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint;
    const bool ptrBound  = ptrSock.bind(QHostAddress::Any, kBeaconPort, mode);
    const bool infoBound = infoSock.bind(QHostAddress::Any, kInfoPort, mode);
    if (!ptrBound && !infoBound) {
        if (err) {
            *err = QString("cannot listen on UDP %1/%2 for the scale's "
                           "beacon — both ports are already in use")
                       .arg(kBeaconPort).arg(kInfoPort);
        }
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        const qint64 remaining = timeoutMs - timer.elapsed();
        const int slice = static_cast<int>(qMin<qint64>(kPollMs, remaining));
        for (QUdpSocket* sock : {&ptrSock, &infoSock}) {
            if (sock->state() != QAbstractSocket::BoundState)
                continue;
            if (!sock->waitForReadyRead(slice))
                continue;
            while (sock->hasPendingDatagrams()) {
                const QNetworkDatagram dgram = sock->receiveDatagram();
                const QHostAddress from = dgram.senderAddress();
                if (from.isNull())
                    continue;
                ep->heardBeacon = true;
                // Qt reports an IPv4 peer as ::ffff:a.b.c.d — show the
                // plain dotted form the rest of the app uses.
                bool isV4 = false;
                const quint32 v4 = from.toIPv4Address(&isV4);
                ep->host = isV4 ? QHostAddress(v4).toString() : from.toString();
                int port = 0;
                if (parseBeacon(dgram.data(), &port))
                    ep->port = port;
                return true;   // any beacon names the scale; the port is
                               // Spec166's default unless the 60 s beacon
                               // said otherwise
            }
        }
    }

    if (err) {
        *err = QString("no scale beacon heard on UDP %1/%2 within %3 ms "
                       "— is the scale powered and on this network?")
                   .arg(kBeaconPort).arg(kInfoPort).arg(timeoutMs);
    }
    return false;
}

// One read-only UPL exchange. Fails unless the response is framed exactly
// as the protocol says, so a truncated or garbled answer is never mistaken
// for a good one.
bool fetchSection(const Endpoint& ep, const QString& section,
                  QByteArray* out, QString* err)
{
    const QByteArray tag = section.toLatin1();
    const QByteArray request    = "UPL\t" + tag + "\t\r\n";
    const QByteArray startMark  = "\r\nDWL\t" + tag + "\t\r\n";
    const QByteArray endMark    = "END\t" + tag + "\t\r\n";

    QTcpSocket sock;
    // A scale is a local device, not an HTTP endpoint — never let a system
    // proxy setting redirect this somewhere else.
    sock.setProxy(QNetworkProxy::NoProxy);
    sock.connectToHost(ep.host, ep.port);
    if (!sock.waitForConnected(kSectionMs)) {
        if (err) {
            *err = QString("could not connect to scale at %1:%2: %3")
                       .arg(ep.host).arg(ep.port).arg(sock.errorString());
        }
        return false;
    }

    if (sock.write(request) != request.size()
            || !sock.waitForBytesWritten(kSectionMs)) {
        if (err)
            *err = QString("could not send %1 to the scale: %2")
                       .arg(QString::fromLatin1(request.trimmed()))
                       .arg(sock.errorString());
        return false;
    }

    QByteArray buf;
    QElapsedTimer timer;
    timer.start();
    for (;;) {
        // Read until the END marker arrives, the peer goes away, or the
        // deadline passes. Deliberately no short idle cut-off: the scale
        // pauses between row batches (PLU takes ~6 s end to end), and an
        // idle timeout there would truncate a perfectly good response.
        const qint64 remaining = kSectionMs - timer.elapsed();
        if (remaining <= 0)
            break;
        if (sock.waitForReadyRead(static_cast<int>(remaining))
                || sock.bytesAvailable() > 0) {
            buf += sock.readAll();
            if (buf.contains(endMark))
                break;
            continue;
        }
        if (buf.contains(endMark))
            break;
        if (sock.state() == QAbstractSocket::UnconnectedState)
            break;
    }
    buf += sock.readAll();
    sock.disconnectFromHost();

    if (!buf.startsWith(startMark)) {
        if (err) {
            *err = buf.isEmpty()
                ? QString("the scale sent no data for section %1").arg(section)
                : QString("section %1 did not start with DWL: %2")
                      .arg(section,
                           QString::fromLatin1(buf.left(60)));
        }
        return false;
    }
    if (!buf.endsWith(endMark)) {
        if (err) {
            *err = QString("section %1 was truncated (%2 bytes, no END "
                           "marker)").arg(section).arg(buf.size());
        }
        return false;
    }

    *out = buf;
    return true;
}

} // namespace DlNetwork

class NetworkTransportBackend : public TransportBackend {
public:
    QString name() const override {
        return _tr("Network (DL-series scale)");
    }

    bool isUsable() const override {
        // Reachability is established from the scale's beacon at list time,
        // so the code path is always present; a scale that is offline shows
        // up as an Unreachable result rather than a greyed-out entry.
        return true;
    }

    TransportResult listFiles(QList<TransportFile>* out) const override {
        TransportResult r;
        DlNetwork::Endpoint ep;
        QString err;
        if (!ensureEndpoint(&ep, &err)) {
            r.status = TransportResult::Status::Unreachable;
            r.detail = err;
            return r;
        }

        // The network protocol has no file listing — it streams the scale's
        // live configuration as sections, which assemble into one .TMS.
        // That single logical file is what we offer here.
        if (out) {
            TransportFile f;
            f.remotePath = QStringLiteral("LIVE");
            f.localName  = QStringLiteral("SCALE_LIVE.TMS");
            f.size = 0;      // not known until the sections are fetched
            out->append(f);
        }
        r.status = TransportResult::Status::Ok;
        r.detail = QString("scale beacon heard from %1 (TCP %2)")
                       .arg(ep.host).arg(ep.port);
        return r;
    }

    TransportResult retrieveFile(const QString& remotePath,
                                 const QString& localPath) const override {
        TransportResult r;
        if (!remotePath.isEmpty() && remotePath != QLatin1String("LIVE")) {
            r.status = TransportResult::Status::NotSupported;
            r.detail = QString("the network transport only offers the scale's "
                               "live configuration (LIVE), not '%1'")
                           .arg(remotePath);
            return r;
        }

        DlNetwork::Endpoint ep;
        QString err;
        if (!ensureEndpoint(&ep, &err)) {
            r.status = TransportResult::Status::Unreachable;
            r.detail = err;
            return r;
        }

        const QStringList sections = DlNetwork::sectionOrder();

        // Assemble the whole file in memory first (tens of KB at most), so
        // a failure part-way through never leaves a half-written file on
        // disk. Every section block already begins with the blank line that
        // separates blocks, and the header/footer supply the rest — the
        // result has exactly the shape a device export has.
        QByteArray content(DlNetwork::kEcsHeader);
        qint64 total = 0;
        bool first = true;
        for (const QString& section : sections) {
            QByteArray block;
            if (!DlNetwork::fetchSection(ep, section, &block, &err)) {
                // A failure on the very first section usually means the
                // cached address went stale (scale rebooted / DHCP moved
                // it), so drop it and let the next attempt rediscover.
                if (first)
                    m_haveEndpoint = false;
                r.status = TransportResult::Status::ProtocolError;
                r.detail = err;
                return r;
            }
            content += block;
            total += block.size();
            first = false;
        }
        content += QByteArray(DlNetwork::kEcsFooter);

        QFile file(localPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            r.status = TransportResult::Status::IoError;
            r.detail = QString("cannot write %1: %2")
                           .arg(localPath, file.errorString());
            return r;
        }
        if (file.write(content) != content.size()) {
            const QString why = file.errorString();
            file.close();
            QFile::remove(localPath);
            r.status = TransportResult::Status::IoError;
            r.detail = QString("short write while saving %1: %2")
                           .arg(localPath, why);
            return r;
        }
        file.close();

        r.status = TransportResult::Status::Ok;
        r.detail = QString("retrieved %1 sections (%2 bytes) from %3")
                       .arg(sections.size()).arg(total).arg(ep.host);
        return r;
    }

    TransportResult uploadFile(const QString&,
                               const QString&) const override {
        TransportResult r;
        r.status = TransportResult::Status::NotSupported;
        r.detail = _tr("writing to the scale is not implemented — only the "
                      "read-only UPL direction has been reverse-engineered, "
                      "and an unverified write could corrupt the scale's "
                      "live data (#7)");
        return r;
    }

private:
    bool ensureEndpoint(DlNetwork::Endpoint* ep, QString* err) const {
        if (m_haveEndpoint) {
            *ep = m_endpoint;
            return true;
        }
        if (!DlNetwork::discover(&m_endpoint, DlNetwork::kDiscoverMs, err))
            return false;
        m_haveEndpoint = true;
        *ep = m_endpoint;
        return true;
    }

    // Cached so listFiles() and retrieveFile() do not each pay the up-to
    // 10 s wait for the next beacon.
    mutable DlNetwork::Endpoint m_endpoint;
    mutable bool m_haveEndpoint = false;
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
