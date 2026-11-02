// End-to-end check for the DL-series network transport (issues #7 and #8).
//
// Usage: network_check <reference.TMS> [output.TMS]
//
// There is no scale on the build machine, so this runs the real code path
// against a fake one: a thread listens on an ephemeral TCP port and answers
// UPL requests with the section blocks parsed out of a real device export,
// while a second thread beacons to UDP 33594/33584 exactly the way the
// scale does. Everything the backend does — discovery, framing, section
// order, assembly, header/footer — is the production code in transport.cpp.
//
// The reference must be a complete export (all 16 sections) for the strong
// byte-for-byte check to apply; a partial fixture still checks framing.

#include <QCoreApplication>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "transport.h"

namespace {

int g_failures = 0;
void ok(const char* what)   { std::printf("ok  : %s\n", what); }
void bad(const char* what)  { std::printf("FAIL: %s\n", what); ++g_failures; }
void check(bool cond, const char* what) { cond ? ok(what) : bad(what); }

const char* kHeader = "ECS\tVER\t254\t\r\n";
const char* kFooter = "\r\nEND\tECS\t\r\n";

// A wire-perfect section block: the empty answer a scale gives for a section
// with no rows. Used both for sections the reference does not contain and as
// the shape every block is held to.
QByteArray emptyBlock(const QByteArray& tag)
{
    return "\r\nDWL\t" + tag + "\t\r\nEND\t" + tag + "\t\r\n";
}

// Split a device export into its DWL…END blocks, keyed by section tag. Each
// block is framed exactly as the scale sends it: a leading blank line, the
// DWL marker, the rows, the END marker, CRLF throughout.
QHash<QByteArray, QByteArray> parseBlocks(const QByteArray& fixture,
                                          QStringList* order)
{
    QHash<QByteArray, QByteArray> blocks;
    const QList<QByteArray> rawLines = fixture.split('\n');
    QList<QByteArray> lines;
    lines.reserve(rawLines.size());
    for (QByteArray l : rawLines) {
        if (l.endsWith('\r'))
            l.chop(1);
        lines.append(l);
    }

    int start = -1;
    QByteArray tag;
    for (int i = 0; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i);
        if (line.startsWith("DWL\t")) {
            tag = line.split('\t').value(1);
            start = i;
        } else if (line.startsWith("END\t")) {
            const QByteArray endTag = line.split('\t').value(1);
            if (endTag == "ECS")
                break;                      // end of file, not a section
            if (start < 0 || endTag != tag) {
                start = -1;
                continue;                   // malformed — ignore
            }
            QByteArray block = "\r\n";
            for (int j = start; j <= i; ++j) {
                block += lines.at(j);
                block += "\r\n";
            }
            blocks.insert(endTag, block);
            if (order)
                order->append(QString::fromLatin1(endTag));
            start = -1;
        }
    }
    return blocks;
}

struct FakeScale {
    QHash<QByteArray, QByteArray> blocks;   // immutable once started
    mutable std::mutex mtx;
    bool listening = false;
    quint16 port = 0;
    std::vector<QByteArray> requests;       // UPL requests, in arrival order
    std::atomic<bool> stop{false};
};

// Answers one UPL request per connection, the way fetchSection() drives it:
// one request line, one framed section response, connection closed.
void runFakeScale(FakeScale* fs)
{
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        std::fprintf(stderr, "FAIL: fake scale could not listen on a local port\n");
        ++g_failures;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(fs->mtx);
        fs->port = server.serverPort();
        fs->listening = true;
    }

    while (!fs->stop.load()) {
        if (!server.waitForNewConnection(200))
            continue;
        QTcpSocket* conn = server.nextPendingConnection();
        if (!conn)
            continue;

        QByteArray req;
        while (!req.contains("\r\n") && !fs->stop.load()) {
            if (!conn->waitForReadyRead(2000))
                break;
            req += conn->readAll();
        }

        // "UPL\t<TAG>\t\r\n" -> section tag
        const QList<QByteArray> fields = req.split('\t');
        const bool wellFormed = req.startsWith("UPL\t") && fields.size() >= 2;
        const QByteArray tag = wellFormed ? fields.at(1) : QByteArray();

        {
            std::lock_guard<std::mutex> lock(fs->mtx);
            fs->requests.push_back(req);
        }

        if (wellFormed) {
            QByteArray resp = fs->blocks.value(tag);
            if (resp.isEmpty())
                resp = emptyBlock(tag);     // empty section, framed properly
            conn->write(resp);
            conn->waitForBytesWritten(2000);
        }
        conn->disconnectFromHost();
        conn->waitForDisconnected(500);
        delete conn;
    }
    server.close();
}

// Beacons to both of the scale's ports until told to stop.
void runBeacon(FakeScale* fs)
{
    QUdpSocket sock;
    while (!fs->stop.load()) {
        quint16 port = 0;
        {
            std::lock_guard<std::mutex> lock(fs->mtx);
            port = fs->port;
        }
        if (port != 0) {
            const QByteArray datagram =
                "UDP\t" + QByteArray::number(port) + "\t254\t\r\n";
            for (int beaconPort : {33594, 33584})
                sock.writeDatagram(datagram, QHostAddress::LocalHost,
                                   quint16(beaconPort));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

TransportBackend* networkBackend()
{
    for (TransportBackend* b : TransportRegistry::availableBackends()) {
        if (b->name().contains(QLatin1String("Network")))
            return b;
    }
    return nullptr;
}

// Wait until the fake scale reports it is listening (or give up).
bool waitListening(FakeScale* fs, int timeoutMs)
{
    for (int waited = 0; waited < timeoutMs; waited += 25) {
        {
            std::lock_guard<std::mutex> lock(fs->mtx);
            if (fs->listening)
                return true;
        }
        if (fs->stop.load())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

void shutdown(FakeScale* fs, std::thread* beacon, std::thread* scale)
{
    fs->stop = true;
    if (beacon && beacon->joinable())
        beacon->join();
    if (scale && scale->joinable())
        scale->join();
}

} // namespace

// The production section order lives in transport.cpp; declaring it here
// lets the test assert it against a real device export instead of keeping a
// second copy of the list that could silently drift.
namespace DlNetwork {
const QStringList& sectionOrder();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: network_check <reference.TMS> [output.TMS]\n");
        return 2;
    }

    QFile in(QString::fromLocal8Bit(argv[1]));
    if (!in.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "error: cannot read %s\n", argv[1]);
        return 2;
    }
    const QByteArray fixture = in.readAll();
    in.close();

    QStringList fixtureOrder;
    const QHash<QByteArray, QByteArray> blocks =
        parseBlocks(fixture, &fixtureOrder);
    check(!blocks.isEmpty(), "reference parses into DWL…END blocks");

    const QStringList sectionOrder = DlNetwork::sectionOrder();
    check(fixtureOrder == sectionOrder,
          "reference section order matches the order the code requests");

    // --- start the fake scale ---------------------------------------------
    FakeScale fs;
    fs.blocks = blocks;
    std::thread scaleThread(runFakeScale, &fs);
    const bool listening = waitListening(&fs, 5000);
    check(listening, "fake scale is listening");
    if (!listening) {
        shutdown(&fs, nullptr, &scaleThread);
        return g_failures ? g_failures : 1;
    }
    std::thread beaconThread(runBeacon, &fs);

    // --- list ---------------------------------------------------------------
    TransportBackend* backend = networkBackend();
    check(backend != nullptr, "network backend is registered");
    if (!backend) {
        shutdown(&fs, &beaconThread, &scaleThread);
        return g_failures;
    }

    QList<TransportFile> files;
    const TransportResult listResult = backend->listFiles(&files);
    check(listResult.status == TransportResult::Status::Ok,
          "listFiles() succeeds once the beacon is heard");
    check(files.size() == 1
              && files.first().localName == QLatin1String("SCALE_LIVE.TMS"),
          "listFiles() offers exactly the live configuration");

    // --- retrieve -----------------------------------------------------------
    const QString outPath =
        argc >= 3 ? QString::fromLocal8Bit(argv[2])
                  : QDir(QDir::tempPath())
                        .filePath(QStringLiteral("network_check_out.TMS"));
    QFile::remove(outPath);

    const TransportResult getResult =
        backend->retrieveFile(QStringLiteral("LIVE"), outPath);
    check(getResult.status == TransportResult::Status::Ok,
          "retrieveFile(LIVE) succeeds");

    // Expected bytes: the header, every section block in the production
    // order, the footer — i.e. exactly the shape of a device export.
    QByteArray expected(kHeader);
    for (const QString& section : sectionOrder) {
        const QByteArray tag = section.toLatin1();
        const QByteArray block = blocks.value(tag);
        check(block.isEmpty()
                  || (block.startsWith("\r\nDWL\t" + tag + "\t\r\n")
                      && block.endsWith("END\t" + tag + "\t\r\n")),
              "reference block for the section is framed like the wire");
        expected += block.isEmpty() ? emptyBlock(tag) : block;
    }
    expected += QByteArray(kFooter);

    QByteArray actual;
    {
        QFile out(outPath);
        if (out.open(QIODevice::ReadOnly))
            actual = out.readAll();
    }
    check(!actual.isEmpty() && actual == expected,
          "assembled file matches header + sections + footer");
    check(actual == fixture,
          "retrieved file is byte-identical to the device export");

    // --- protocol shape ------------------------------------------------------
    std::vector<QByteArray> requests;
    {
        std::lock_guard<std::mutex> lock(fs.mtx);
        requests = fs.requests;
    }
    bool orderOk = int(requests.size()) == sectionOrder.size();
    for (int i = 0; orderOk && i < sectionOrder.size(); ++i) {
        const QByteArray want =
            "UPL\t" + sectionOrder.at(i).toLatin1() + "\t\r\n";
        if (requests[size_t(i)] != want)
            orderOk = false;
    }
    check(orderOk, "every section was requested once, in device order");

    bool framingOk = int(requests.size()) == sectionOrder.size();
    for (const QByteArray& req : requests) {
        const QList<QByteArray> f = req.split('\t');
        if (!req.startsWith("UPL\t") || !req.endsWith("\t\r\n")
                || f.size() != 3 || f.at(1).isEmpty()) {
            framingOk = false;
            break;
        }
    }
    check(framingOk, "requests are exactly UPL<TAB><TAG><TAB>CRLF");

    // --- the write direction stays off ---------------------------------------
    const TransportResult up =
        backend->uploadFile(outPath, QStringLiteral("LIVE"));
    check(up.status == TransportResult::Status::NotSupported,
          "uploadFile() refuses — only the read direction is reverse-engineered");

    const TransportResult bogus =
        backend->retrieveFile(QStringLiteral("A_999.TMS"), outPath + ".x");
    check(bogus.status == TransportResult::Status::NotSupported,
          "retrieveFile() rejects a path that is not the live configuration");

    shutdown(&fs, &beaconThread, &scaleThread);

    if (argc < 3)
        QFile::remove(outPath);

    std::printf("%d failure(s)\n", g_failures);
    return g_failures;
}