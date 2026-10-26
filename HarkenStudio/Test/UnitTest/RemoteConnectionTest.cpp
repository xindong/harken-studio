#include "UnitTest/StudioObjectIdTestSupport.h"
#include "RuntimeBridge/RemoteConnection.h"
#include "Monitor/EventCaptureController.h"
#include "MonitorRecordPack.h"
#include "MonitorExecutionCodec.h"

#include <harken_remote/Protocol.h>
#include <harken/Harken.h>

#include <gtest/gtest.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QNetworkDatagram>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QUdpSocket>

#include <cstring>
#include <functional>

namespace {

namespace Wire = harken::remote;
using HarkenEditor::Bridge::RemoteConnection;
using HarkenEditor::Bridge::RemoteConnectionState;

void ensureRemoteTestApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char executable[] = "harken_editor_tests";
    static char* argv[] = {executable, nullptr};
    static QApplication application(argc, argv);
}

bool waitUntil(const std::function<bool()>& done, int timeoutMs = 2000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!done() && elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return done();
}

QByteArray bytesFor(const Wire::Frame& frame)
{
    const auto bytes = Wire::EncodeFrame(frame);
    return QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
}

class RemotePeer
{
public:
    QTcpServer server;
    QPointer<QTcpSocket> client;
    QByteArray incoming;
    Wire::Identity identity;
    QVector<Wire::Command> commands;
    std::function<void(const Wire::Frame&, quint32)> catalogResponder;
    std::function<void(const Wire::Frame&)> eventResponder;

    RemotePeer()
    {
        identity.capabilities &= ~Wire::kCapabilityExecutionObservation;
        identity.instanceId = 42;
        identity.sessionId = 11;
        identity.applicationName = "Remote game";
        identity.engineVersion = "9.2.1";
        identity.sampleRate = 48000;
        identity.blockFrames = 512;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            client = server.nextPendingConnection();
            incoming.clear();
            QObject::connect(client, &QTcpSocket::readyRead, &server, [this] { receive(); });
        });
        EXPECT_TRUE(server.listen(QHostAddress::LocalHost, 0));
        identity.tcpPort = server.serverPort();
    }

    void send(const Wire::Frame& frame)
    {
        if (client)
            client->write(bytesFor(frame));
    }

    void receive()
    {
        incoming += client->readAll();
        while (incoming.size() >= Wire::kHeaderBytes) {
            Wire::Header header;
            ASSERT_TRUE(Wire::DecodeHeader(incoming.data(), incoming.size(), header));
            const qsizetype size = Wire::kHeaderBytes + header.payloadBytes;
            if (incoming.size() < size)
                return;
            Wire::Frame frame;
            frame.type = header.type;
            frame.requestId = header.requestId;
            const auto* data = reinterpret_cast<const std::uint8_t*>(incoming.constData()) + Wire::kHeaderBytes;
            frame.payload.assign(data, data + header.payloadBytes);
            incoming.remove(0, size);
            respond(frame);
        }
    }

    void respond(const Wire::Frame& frame)
    {
        if (frame.type == Wire::Message::Hello) {
            send(Wire::MakeFrame(Wire::Message::Welcome, frame.requestId, identity));
        } else if (frame.type == Wire::Message::CatalogRequest) {
            Wire::Reader reader(frame.payload);
            std::uint32_t offset = 0;
            ASSERT_TRUE(reader.U32(offset));
            ASSERT_TRUE(reader.Done());
            if (catalogResponder) {
                catalogResponder(frame, offset);
                return;
            }
            Wire::Catalog catalog;
            catalog.generation = 1;
            catalog.offset = offset;
            catalog.totalCount = 2;
            catalog.entries.push_back({Wire::CatalogKind::Action, harkentest::ObjectId(1001), "Start music"});
            catalog.entries.push_back({Wire::CatalogKind::Variable, harkentest::ObjectId(2001), "Intensity"});
            send(Wire::MakeFrame(Wire::Message::Catalog, frame.requestId, catalog));
        } else if (frame.type == Wire::Message::EventStart || frame.type == Wire::Message::EventStartV2 ||
                   frame.type == Wire::Message::EventSeal) {
            if (eventResponder) eventResponder(frame);
        } else if (frame.type == Wire::Message::Command) {
            Wire::Command command;
            ASSERT_TRUE(Wire::DecodePayload(frame, command));
            commands.push_back(command);
            Wire::CommandResult result;
            result.operation = command.operation;
            send(Wire::MakeFrame(Wire::Message::CommandResult, frame.requestId, result));
        } else if (frame.type == Wire::Message::Ping)
            send({Wire::Message::Pong, frame.requestId, {}});
    }
};

Wire::MonitorRecords eventRecords(quint64 epoch, quint64 firstSequence, int count,
                                  quint64 firstTimestamp)
{
    Wire::MonitorRecords records;
    records.sessionEpoch = epoch;
    records.recordCount = count;
    constexpr auto bytes = harken::monitor::PackedRecordBytes(sizeof(VNX::Monitor::MonitorEventDetail));
    records.bytes.resize(bytes * count);
    for (int i = 0; i < count; ++i) {
        VnMonitorRecordHeader header{};
        harken::monitor::FillSyntheticHeader(&header, firstSequence + i, bytes,
            sizeof(VNX::Monitor::MonitorEventDetail), VN_MONITOR_RECORD_EVENT_DETAIL, 0);
        header.flags = VN_MONITOR_RECORD_FLAG_OPTIONAL;
        header.streamKind = VN_MONITOR_STREAM_EVENT;
        header.monitorSessionEpoch = epoch;
        auto* packed = records.bytes.data() + i * bytes;
        harken::monitor::PackHeader(packed, header);
        VNX::Monitor::MonitorEventDetail detail{};
        detail.structSize = sizeof(detail);
        detail.category = VN_MONITOR_EVENT_CATEGORY_DIAGNOSTIC;
        detail.level = VN_LOG_WARN;
        detail.timestampNs = firstTimestamp + i;
        std::memcpy(packed + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES, &detail, sizeof(detail));
    }
    return records;
}

TEST(EventCaptureController, RestartRejectsPreviousEpochRecords)
{
    ensureRemoteTestApp();
    using namespace VNX::Monitor;
    RemotePeer peer;
    RemoteConnection connection;
    EventCaptureController capture(&connection);
    connection.connectTarget(QStringLiteral("127.0.0.1"), peer.server.serverPort());
    ASSERT_TRUE(waitUntil([&] { return connection.state() == RemoteConnectionState::Connected; }));
    quint64 epoch = 80;
    peer.eventResponder = [&](const Wire::Frame& request) {
        Wire::EventCaptureStatus status;
        status.sessionEpoch = epoch;
        status.startedTimestampNs = 100;
        if (request.type == Wire::Message::EventStart) {
            peer.send(Wire::MakeFrame(Wire::Message::EventStatus, request.requestId, status));
        } else {
            status.state = VN_MONITOR_EVENT_SESSION_SEALED;
            status.stoppedTimestampNs = 300;
            peer.send(Wire::MakeFrame(Wire::Message::EventStatus, request.requestId, status));
            status.state = VN_MONITOR_EVENT_SESSION_DRAINED;
            peer.send(Wire::MakeFrame(Wire::Message::EventStatus, 0, status));
        }
    };
    capture.start();
    ASSERT_TRUE(waitUntil([&] { return capture.state() == EventCaptureState::Capturing; }));
    peer.send(Wire::MakeFrame(Wire::Message::EventRecords, 0, eventRecords(epoch, 1, 1, 200)));
    ASSERT_TRUE(waitUntil([&] { return capture.rowCount() == 1; }));
    capture.stop();
    ASSERT_TRUE(waitUntil([&] { return capture.state() == EventCaptureState::Stopped; }));
    ++epoch;
    capture.start();
    ASSERT_TRUE(waitUntil([&] { return capture.state() == EventCaptureState::Capturing; }));
    peer.send(Wire::MakeFrame(Wire::Message::EventRecords, 0, eventRecords(epoch - 1, 10, 1, 500)));
    peer.send(Wire::MakeFrame(Wire::Message::EventRecords, 0, eventRecords(epoch, 1, 1, 400)));
    ASSERT_TRUE(waitUntil([&] { return capture.rowCount() == 1; }));
    EXPECT_EQ(400u, capture.eventAt(0).detail.timestampNs);
}

TEST(EventCaptureController, MalformedPayloadPreservesPublishedHistoryAndAccounting)
{
    ensureRemoteTestApp();
    using namespace VNX::Monitor;
    RemotePeer peer;
    peer.identity.capabilities |= Wire::kCapabilityExecutionObservation;
    RemoteConnection connection;
    EventCaptureController capture(&connection);
    connection.connectTarget(QStringLiteral("127.0.0.1"), peer.server.serverPort());
    ASSERT_TRUE(waitUntil([&] { return connection.canCaptureEvents(); }));
    peer.eventResponder = [&](const Wire::Frame& request) {
        Wire::EventCaptureStatusV2 status;
        status.base.sessionEpoch = 99;
        status.base.startedTimestampNs = 100;
        status.acceptedExecutionFactVersion = 2;
        status.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS | VN_MONITOR_PERM_OBSERVE_STATE | VN_MONITOR_PERM_SYMBOLS;
        status.acceptedPermissionMask = status.requestedPermissionMask;
        status.acceptedStreamKindMask = VN_MONITOR_STREAM_EVENT | VN_MONITOR_STREAM_STATE;
        if (request.type == Wire::Message::EventSeal) {
            status.base.state = VN_MONITOR_EVENT_SESSION_DRAINED;
            status.base.stoppedTimestampNs = 100000;
        }
        peer.send(Wire::MakeFrame(Wire::Message::EventStatusV2, request.requestId, status));
    };
    const auto variableRecords = [](quint64 firstSequence, int count) {
        Wire::MonitorRecords records;
        records.sessionEpoch = 99;
        records.recordCount = count;
        constexpr auto bytes = harken::monitor::kActivityRecordBytes;
        records.bytes.resize(bytes * count);
        for (int i = 0; i < count; ++i) {
            harken::MonitorExecutionFact source{};
            source.engine = 42;
            source.sourceSequence = firstSequence + i;
            source.kind = VN_MONITOR_FACT_VARIABLE_RESULT;
            source.contentKind = VN_MONITOR_CONTENT_VARIABLE;
            source.timestamp = firstSequence + i + 100;
            source.body.execution.identity.object = harkentest::ObjectId(7);
            source.body.execution.activity.variableOutcome = VN_VARIABLE_OUTCOME_APPLIED;
            source.body.execution.activity.variableTicket = firstSequence + i;
            for (const auto field : {VN_MONITOR_FACT_FIELD_CONTENT_OBJECT,
                    VN_MONITOR_FACT_FIELD_TIMESTAMP, VN_MONITOR_FACT_FIELD_ACTIVITY_VARIABLE})
                EXPECT_TRUE(harken::monitor::SetSourceField(source, field));
            auto* packed = records.bytes.data() + i * bytes;
            EXPECT_EQ(bytes, harken::monitor::PackExecutionFact(packed, bytes, source, 1, 1));
            harken::monitor::WriteU64(packed + 64, records.sessionEpoch);
        }
        return records;
    };
    capture.start();
    ASSERT_TRUE(waitUntil([&] { return capture.state() == EventCaptureState::Capturing; }));
    quint64 sequence = 1;
    for (int batch = 0; batch < 110; ++batch) {
        peer.send(Wire::MakeFrame(Wire::Message::EventRecords, 0,
                                  variableRecords(sequence, 100)));
        sequence += 100;
        ASSERT_TRUE(waitUntil([&] {
            return capture.rowCount() > 0 &&
                capture.eventAt(capture.rowCount() - 1).header.sourceSequence == sequence - 1;
        }));
    }
    EXPECT_EQ(11000u, capture.trimmedRecords() + capture.rowCount());
    const int retainedRows = capture.rowCount();
    auto invalid = variableRecords(sequence, 1);
    invalid.bytes.pop_back();
    peer.send(Wire::MakeFrame(Wire::Message::EventRecords, 0, invalid));
    ASSERT_TRUE(waitUntil([&] { return capture.state() == EventCaptureState::Failed; }));
    EXPECT_EQ(retainedRows, capture.rowCount());
    EXPECT_EQ(11000u, capture.trimmedRecords() + capture.rowCount());
}

TEST(RemoteConnection, CatalogRefreshPublishesGenerationWithCompleteEntries)
{
    ensureRemoteTestApp();
    RemotePeer peer;
    RemoteConnection connection;
    connection.connectTarget(QStringLiteral("127.0.0.1"), peer.server.serverPort());
    ASSERT_TRUE(waitUntil([&] { return connection.catalog().size() == 2; }));
    quint32 continuationRequest = 0;
    peer.catalogResponder = [&](const Wire::Frame& frame, quint32 offset) {
        if (offset != 0) {
            continuationRequest = frame.requestId;
            return;
        }
        Wire::Catalog catalog;
        catalog.generation = 2;
        catalog.totalCount = 2;
        catalog.entries.push_back({Wire::CatalogKind::Action, harkentest::ObjectId(1002), "New music"});
        peer.send(Wire::MakeFrame(Wire::Message::Catalog, frame.requestId, catalog));
    };
    connection.refreshCatalog();
    ASSERT_TRUE(waitUntil([&] { return continuationRequest != 0; }));
    ASSERT_EQ(connection.catalog().size(), 2);
    EXPECT_EQ(connection.catalog().first().id, harkentest::StudioObjectId(1001u));
    connection.postAction(connection.catalog().first().id);
    ASSERT_TRUE(waitUntil([&] { return !connection.commandPending(); }));
    ASSERT_EQ(peer.commands.size(), 1);
    EXPECT_EQ(peer.commands.last().expectedGeneration, 1u);
    EXPECT_EQ(peer.commands.last().target, harkentest::StudioObjectId(1001u));
    connection.setParameter(connection.catalog().last().id, 0.4f);
    ASSERT_TRUE(waitUntil([&] { return !connection.commandPending(); }));
    ASSERT_EQ(peer.commands.size(), 2);
    EXPECT_EQ(peer.commands.last().expectedGeneration, 1u);

    Wire::Catalog finalPage;
    finalPage.generation = 2;
    finalPage.offset = 1;
    finalPage.totalCount = 2;
    finalPage.entries.push_back({Wire::CatalogKind::Variable, harkentest::ObjectId(2002), "New intensity"});
    peer.send(Wire::MakeFrame(Wire::Message::Catalog, continuationRequest, finalPage));
    ASSERT_TRUE(waitUntil([&] { return connection.catalog().first().id == harkentest::StudioObjectId(1002); }));
    connection.postAction(connection.catalog().first().id);
    ASSERT_TRUE(waitUntil([&] { return !connection.commandPending(); }));
    ASSERT_EQ(peer.commands.size(), 3);
    EXPECT_EQ(peer.commands.last().expectedGeneration, 2u);
    EXPECT_EQ(peer.commands.last().target, harkentest::StudioObjectId(1002u));
    connection.setParameter(connection.catalog().last().id, 0.6f);
    ASSERT_TRUE(waitUntil([&] { return !connection.commandPending(); }));
    ASSERT_EQ(peer.commands.size(), 4);
    EXPECT_EQ(peer.commands.last().expectedGeneration, 2u);
    EXPECT_EQ(peer.commands.last().target, harkentest::StudioObjectId(2002u));
}

TEST(RemoteConnection, DiscoveryRejectsPreviousRequestAnnouncements)
{
    ensureRemoteTestApp();
    QUdpSocket server;
    ASSERT_TRUE(server.bind(QHostAddress::LocalHost, 0));
    QNetworkDatagram request;
    quint32 requestId = 0;
    QObject::connect(&server, &QUdpSocket::readyRead, &server, [&] {
        while (server.hasPendingDatagrams()) {
            request = server.receiveDatagram();
            Wire::Header header;
            ASSERT_TRUE(Wire::DecodeHeader(request.data().data(), request.data().size(), header));
            requestId = header.requestId;
            Wire::Identity identity;
            identity.instanceId = 89;
            identity.tcpPort = 34567;
            identity.bindAddress = "127.0.0.1";
            server.writeDatagram(bytesFor(Wire::MakeFrame(Wire::Message::Announcement, requestId, identity)),
                                 request.senderAddress(), request.senderPort());
        }
    });
    RemoteConnection connection;
    connection.discover(server.localPort());
    ASSERT_TRUE(waitUntil([&] { return connection.discoveredTargets().size() == 1; }));

    const auto previousRequestId = requestId;

    connection.discover(server.localPort());
    ASSERT_TRUE(waitUntil([&] { return connection.discoveredTargets().size() == 1; }));
    EXPECT_NE(requestId, previousRequestId);
    Wire::Identity stale;
    stale.instanceId = 90;
    stale.tcpPort = 34568;
    stale.bindAddress = "127.0.0.1";
    server.writeDatagram(bytesFor(Wire::MakeFrame(Wire::Message::Announcement, previousRequestId, stale)),
                         request.senderAddress(), request.senderPort());
    stale.instanceId = 91;
    server.writeDatagram(bytesFor(Wire::MakeFrame(Wire::Message::Announcement, requestId, stale)),
                         request.senderAddress(), request.senderPort());
    ASSERT_TRUE(waitUntil([&] { return connection.discoveredTargets().size() == 2; }));
    EXPECT_EQ(connection.discoveredTargets().last().instanceId, QString::number(stale.instanceId, 16));
}

}
