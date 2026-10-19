#include "UnitTest/StudioObjectIdTestSupport.h"
#include "ActionManager.h"
#include "Core/ToastManager.h"
#include "Dialogs/RemoteConnectDialog.h"
#include "MainWindow.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "RuntimeBridge/RemoteConnection.h"
#include "Monitor/EventCaptureController.h"
#include "Widgets/Monitor/MonitorPanel.h"
#include "MonitorRecordPack.h"
#include "UndoCommands.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include "Widgets/PlatformBar/PlatformBar.h"
#include "Widgets/Common/TableWidget.h"

#include <harken_remote/Protocol.h>
#include <harken/Harken.h>

#include <gtest/gtest.h>

#include <QAction>
#include <QAbstractProxyModel>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QPointer>
#include <QPersistentModelIndex>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTableView>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUdpSocket>

#include <functional>
#include <memory>

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
    bool fragmentWelcome = false;
    bool replyToCommands = true;
    bool invalidHeader = false;
    bool skipWelcome = false;
    bool busy = false;
    int catalogPages = 0;
    int snapshots = 0;
    bool paginateCatalog = false;
    std::function<void(const Wire::Frame&, quint32)> catalogResponder;
    std::function<void(const Wire::Frame&)> eventResponder;
    quint64 block = 40;

    RemotePeer()
    {
        identity.capabilities &= ~Wire::kCapabilityExecutionObservation;
        identity.instanceId = 42;
        identity.sessionId = 11;
        identity.applicationName = "Remote game";
        identity.machineName = "Test machine";
        identity.engineVersion = "9.2.1";
        identity.buildId = "test-build";
        identity.platform = "Windows";
        identity.processId = 123;
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
            Wire::Hello hello;
            ASSERT_TRUE(Wire::DecodePayload(frame, hello));
            EXPECT_EQ(hello.major, Wire::kProtocolMajor);
            if (busy) {
                Wire::Writer writer;
                writer.I32(VN_IN_USE);
                send({Wire::Message::Error, 0, std::move(writer.bytes)});
                client->disconnectFromHost();
                return;
            }
            if (skipWelcome)
                return;
            if (invalidHeader) {
                Wire::Writer writer;
                writer.U32(Wire::kMagic);
                writer.U32(Wire::kMaxPayloadBytes + 1);
                writer.U16(static_cast<std::uint16_t>(Wire::Message::Welcome));
                writer.U16(0);
                writer.U32(frame.requestId);
                client->write(reinterpret_cast<const char*>(writer.bytes.data()), writer.bytes.size());
                return;
            }
            const auto reply = Wire::MakeFrame(Wire::Message::Welcome, frame.requestId, identity);
            if (fragmentWelcome) {
                const auto packed = bytesFor(reply);
                client->write(packed.first(5));
                const QPointer<QTcpSocket> receiver = client;
                QTimer::singleShot(5, &server, [receiver, packed] {
                    if (receiver)
                        receiver->write(packed.sliced(5));
                });
            } else
                send(reply);
        } else if (frame.type == Wire::Message::CatalogRequest) {
            ++catalogPages;
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
            if (offset == 0)
                catalog.entries.push_back({Wire::CatalogKind::Action, harkentest::ObjectId(1001), "Start music"});
            if (!paginateCatalog || offset != 0)
                catalog.entries.push_back({Wire::CatalogKind::Variable, harkentest::ObjectId(2001), "Intensity"});
            send(Wire::MakeFrame(Wire::Message::Catalog, frame.requestId, catalog));
        } else if (frame.type == Wire::Message::SnapshotRequest) {
            ++snapshots;
            Wire::Snapshot snapshot;
            snapshot.blockIndex = ++block;
            snapshot.activeVoices = 3;
            snapshot.busCount = 1;
            snapshot.callbackCpuUs = 17;
            snapshot.updateCpuUs = 23;
            snapshot.commandBackpressure = 2;
            snapshot.buses.push_back({harkentest::ObjectId(44), 1, 0.75f});
            send(Wire::MakeFrame(Wire::Message::Snapshot, frame.requestId, snapshot));
        } else if (frame.type == Wire::Message::MonitorStart || frame.type == Wire::Message::MonitorStop) {
            Wire::MonitorStatus status;
            if (frame.type == Wire::Message::MonitorStart) {
                Wire::MonitorRequest request;
                ASSERT_TRUE(Wire::DecodePayload(frame, request));
                status.streamMask = request.streamMask;
                status.sessionEpoch = 11;
                status.planGeneration = 1;
            }
            send(Wire::MakeFrame(Wire::Message::MonitorStatus, frame.requestId, status));
        } else if (frame.type == Wire::Message::EventStart || frame.type == Wire::Message::EventStartV2 ||
                   frame.type == Wire::Message::EventSeal) {
            if (eventResponder) eventResponder(frame);
        } else if (frame.type == Wire::Message::Command) {
            Wire::Command command;
            ASSERT_TRUE(Wire::DecodePayload(frame, command));
            commands.push_back(command);
            if (replyToCommands) {
                Wire::CommandResult result;
                result.operation = command.operation;
                if (command.operation == Wire::Operation::PostAction)
                    result.handle = 7001;
                send(Wire::MakeFrame(Wire::Message::CommandResult, frame.requestId, result));
            }
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
        detail.category = VN_MONITOR_EVENT_CATEGORY_OPERATION;
        detail.code = VN_MONITOR_OPERATION_STOP_ALL;
        detail.timestampNs = firstTimestamp + i;
        std::memcpy(packed + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES, &detail, sizeof(detail));
    }
    return records;
}

TEST(RemoteConnection, ExecutionCaptureNegotiatesAndSealsWithoutLegacyFallback)
{
    ensureRemoteTestApp();
    using HarkenEditor::Bridge::RemoteEventCaptureState;
    for (const bool reject : {false, true}) {
        SCOPED_TRACE(reject);
        RemotePeer peer;
        peer.identity.capabilities = Wire::kCapabilityExecutionObservation;
        RemoteConnection connection;
        int starts = 0;
        peer.eventResponder = [&](const Wire::Frame& request) {
            EXPECT_NE(request.type, Wire::Message::EventStart);
            Wire::EventCaptureStatusV2 status;
            status.base.sessionEpoch = 79;
            status.base.startedTimestampNs = 100;
            status.acceptedExecutionFactVersion = 1;
            status.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
            status.acceptedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
            status.acceptedStreamKindMask = VN_MONITOR_STREAM_EVENT;
            status.supportedFactKinds[0] = UINT64_C(1) << VN_MONITOR_FACT_REQUEST_SUBMITTED;
            status.identityUnavailableCount = 3;
            if (request.type == Wire::Message::EventStartV2) {
                ++starts;
                Wire::EventCaptureRequestV2 decoded;
                ASSERT_TRUE(Wire::DecodePayload(request, decoded));
                EXPECT_EQ(decoded.executionFactVersion, 1u);
                EXPECT_EQ(decoded.requestedStreamKindMask, VN_MONITOR_STREAM_EVENT);
                EXPECT_EQ(decoded.requestedPermissionMask, VN_MONITOR_PERM_OBSERVE_EVENTS);
                EXPECT_EQ(decoded.requestSymbols, 0u);
                if (reject) status.base.result = VN_MONITOR_UNSUPPORTED;
            } else {
                EXPECT_EQ(request.type, Wire::Message::EventSeal);
                status.base.state = VN_MONITOR_EVENT_SESSION_DRAINED;
                status.base.stoppedTimestampNs = 200;
                status.base.sourceDroppedRecords = 7;
                status.base.transportDroppedRecords = 5;
            }
            peer.send(Wire::MakeFrame(Wire::Message::EventStatusV2, request.requestId, status));
        };
        connection.connectTarget(QStringLiteral("127.0.0.1"), peer.server.serverPort());
        ASSERT_TRUE(waitUntil([&] { return connection.canCaptureEvents(); }));
        connection.startEventCapture();
        ASSERT_TRUE(waitUntil([&] {
            return connection.eventCaptureStatus().state ==
                (reject ? RemoteEventCaptureState::Failed : RemoteEventCaptureState::Capturing);
        }));
        EXPECT_EQ(starts, 1);
        EXPECT_EQ(connection.state(), RemoteConnectionState::Connected);
        if (reject) {
            EXPECT_EQ(connection.eventCaptureStatus().result, VN_MONITOR_UNSUPPORTED);
            continue;
        }
        EXPECT_EQ(connection.eventCaptureStatus().acceptedExecutionFactVersion, 1u);
        EXPECT_EQ(connection.eventCaptureStatus().identityUnavailableCount, 3u);
        EXPECT_EQ(connection.eventCaptureStatus().supportedFactKinds[0], UINT64_C(1) << VN_MONITOR_FACT_REQUEST_SUBMITTED);
        connection.stopEventCapture();
        ASSERT_TRUE(waitUntil([&] { return connection.eventCaptureStatus().state == RemoteEventCaptureState::Stopped; }));
        EXPECT_EQ(connection.eventCaptureStatus().sourceDroppedRecords, 7u);
        EXPECT_EQ(connection.eventCaptureStatus().transportDroppedRecords, 5u);
    }
}

TEST(EventCaptureController, RemoteTailRestartFailureAndEpochIsolation)
{
    ensureRemoteTestApp();
    using namespace VNX::Monitor;
    RemotePeer peer;
    RemoteConnection connection;
    EventCaptureController capture(&connection);
    capture.setLocalRuntimeAvailable(true);
    connection.connectTarget(QStringLiteral("127.0.0.1"), peer.server.serverPort());
    ASSERT_TRUE(waitUntil([&] { return connection.state() == RemoteConnectionState::Connected; }));
    quint64 epoch = 80;
    bool refuseStart = false;
    peer.eventResponder = [&](const Wire::Frame& request) {
        Wire::EventCaptureStatus status;
        status.sessionEpoch = epoch;
        status.startedTimestampNs = 100;
        if (request.type == Wire::Message::EventStart) {
            if (refuseStart) status.result = VN_IN_USE;
            peer.send(Wire::MakeFrame(Wire::Message::EventStatus, request.requestId, status));
        } else {
            peer.send(Wire::MakeFrame(Wire::Message::EventRecords, 0, eventRecords(epoch, 3, 1, 250)));
            status.state = VN_MONITOR_EVENT_SESSION_SEALED;
            status.stoppedTimestampNs = 300;
            peer.send(Wire::MakeFrame(Wire::Message::EventStatus, request.requestId, status));
            status.state = VN_MONITOR_EVENT_SESSION_DRAINED;
            peer.send(Wire::MakeFrame(Wire::Message::EventStatus, 0, status));
        }
    };
    capture.start();
    ASSERT_TRUE(waitUntil([&] { return capture.state() == EventCaptureState::Capturing; }));
    EXPECT_EQ(EventCaptureTarget::Remote, capture.target());
    EXPECT_FALSE(connection.captureEnabled());
    peer.send(Wire::MakeFrame(Wire::Message::EventRecords, 0, eventRecords(epoch, 1, 1, 200)));
    peer.send(Wire::MakeFrame(Wire::Message::EventRecords, 0, eventRecords(epoch, 2, 1, 150)));
    ASSERT_TRUE(waitUntil([&] { return capture.rowCount() == 2; }));
    EXPECT_EQ(150u, capture.eventAt(0).detail.timestampNs);
    capture.stop();
    EXPECT_EQ(EventCaptureState::Stopping, capture.state());
    ASSERT_TRUE(waitUntil([&] { return capture.state() == EventCaptureState::Stopped; }));
    EXPECT_EQ(3, capture.rowCount());
    EXPECT_EQ(250u, capture.eventAt(2).detail.timestampNs);
    EXPECT_EQ(RemoteConnectionState::Connected, connection.state());
    refuseStart = true;
    capture.start();
    ASSERT_TRUE(waitUntil([&] { return capture.state() == EventCaptureState::Failed; }));
    EXPECT_EQ(3, capture.rowCount());
    refuseStart = false;
    ++epoch;
    capture.start();
    ASSERT_TRUE(waitUntil([&] { return capture.state() == EventCaptureState::Capturing; }));
    EXPECT_EQ(0, capture.rowCount());
    peer.send(Wire::MakeFrame(Wire::Message::EventRecords, 0, eventRecords(epoch - 1, 10, 1, 500)));
    peer.send(Wire::MakeFrame(Wire::Message::EventRecords, 0, eventRecords(epoch, 1, 1, 400)));
    ASSERT_TRUE(waitUntil([&] { return capture.rowCount() == 1; }));
    EXPECT_EQ(400u, capture.eventAt(0).detail.timestampNs);
    peer.client->disconnectFromHost();
    ASSERT_TRUE(waitUntil([&] { return capture.state() == EventCaptureState::Failed; }));
    EXPECT_EQ(1, capture.rowCount());
    EXPECT_TRUE(capture.incomplete());
}

TEST(EventCaptureController, BoundsHistoryAndPreservesItOnMalformedPayload)
{
    ensureRemoteTestApp();
    using namespace VNX::Monitor;
    RemotePeer peer;
    RemoteConnection connection;
    EventCaptureController capture(&connection);
    connection.connectTarget(QStringLiteral("127.0.0.1"), peer.server.serverPort());
    ASSERT_TRUE(waitUntil([&] { return connection.canCaptureEvents(); }));
    VNX::MonitorPanel panel;
    panel.setCaptureController(&capture);
    panel.findChild<QToolButton*>(QStringLiteral("MonitorAutoScroll"))->setChecked(false);
    peer.eventResponder = [&](const Wire::Frame& request) {
        Wire::EventCaptureStatus status;
        status.sessionEpoch = 99;
        status.startedTimestampNs = 100;
        if (request.type == Wire::Message::EventSeal) {
            status.state = VN_MONITOR_EVENT_SESSION_DRAINED;
            status.stoppedTimestampNs = 100000;
        }
        peer.send(Wire::MakeFrame(Wire::Message::EventStatus, request.requestId, status));
    };
    capture.start();
    ASSERT_TRUE(waitUntil([&] { return capture.state() == EventCaptureState::Capturing; }));
    quint64 sequence = 1;
    QPersistentModelIndex selected;
    quint64 selectedSequence = 0;
    auto* proxy = qobject_cast<QAbstractProxyModel*>(panel.table()->model());
    ASSERT_NE(proxy, nullptr);
    for (int batch = 0; batch < 110; ++batch) {
        peer.send(Wire::MakeFrame(Wire::Message::EventRecords, 0,
                                  eventRecords(99, sequence, 100, sequence + 100)));
        sequence += 100;
        ASSERT_TRUE(waitUntil([&] {
            return capture.rowCount() > 0 &&
                capture.eventAt(capture.rowCount() - 1).header.sourceSequence == sequence - 1;
        }));
        ASSERT_LE(capture.rowCount(), EventCaptureController::kMaxRows);
        ASSERT_LE(capture.rowCount() * sizeof(CapturedEvent), EventCaptureController::kMaxBytes);
        ASSERT_EQ(panel.visibleRowCount(), capture.rowCount());
        if (batch == 50) {
            panel.table()->selectRow(panel.visibleRowCount() - 1);
            selected = panel.table()->currentIndex();
            selectedSequence = capture.eventAt(proxy->mapToSource(selected).row()).header.sourceSequence;
        }
    }
    ASSERT_TRUE(selected.isValid());
    EXPECT_EQ(selectedSequence, capture.eventAt(proxy->mapToSource(selected).row()).header.sourceSequence);
    EXPECT_EQ(selected, panel.table()->currentIndex());
    EXPECT_EQ(11000u, capture.trimmedRecords() + capture.rowCount());
    EXPECT_EQ(capture.trimmedRecords() + 1, capture.eventAt(0).header.sourceSequence);
    const int retainedRows = capture.rowCount();
    auto invalid = eventRecords(99, sequence, 1, sequence + 100);
    harken::monitor::WriteU32(invalid.bytes.data() + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES + 52, 193);
    peer.send(Wire::MakeFrame(Wire::Message::EventRecords, 0, invalid));
    ASSERT_TRUE(waitUntil([&] { return capture.state() == EventCaptureState::Failed; }));
    EXPECT_EQ(retainedRows, capture.rowCount());
    EXPECT_EQ(11000u, capture.trimmedRecords() + capture.rowCount());
    EXPECT_TRUE(capture.incomplete());
}

TEST(EventCaptureController, LocalCaptureStopsWhenRemoteTargetConnects)
{
    ensureRemoteTestApp();
    using namespace VNX::Monitor;
    VnConfig config{};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(VN_OK, Vn_Init(&config));
    const auto terminate = qScopeGuard([] { Vn_Term(); });
    RemotePeer peer;
    RemoteConnection connection;
    EventCaptureController capture(&connection);
    capture.setLocalRuntimeAvailable(true);
    capture.start();
    ASSERT_EQ(EventCaptureState::Capturing, capture.state());
    EXPECT_EQ(EventCaptureTarget::Local, capture.target());
    ASSERT_EQ(VN_OK, Vn_StopAll(19));
    const auto hasCompletedStopRequest = [&] {
        quint64 submitted = 0, returned = 0;
        for (int row = 0; row < capture.rowCount(); ++row) {
            const auto& event = capture.eventAt(row);
            if (!event.isExecution() || event.execution.request.operation != VN_MONITOR_OPERATION_STOP_ALL) continue;
            if (event.execution.factKind == VN_MONITOR_FACT_REQUEST_SUBMITTED)
                submitted = event.execution.request.sequence;
            if (event.execution.factKind == VN_MONITOR_FACT_REQUEST_RETURNED &&
                monitorFactHas(event.execution, kMonitorFactFieldResult) && event.execution.result == VN_OK)
                returned = event.execution.request.sequence;
        }
        return submitted != 0 && submitted == returned;
    };
    ASSERT_TRUE(waitUntil([&] {
        Vn_Update();
        capture.pollLocal();
        return hasCompletedStopRequest();
    }));
    connection.connectTarget(QStringLiteral("127.0.0.1"), peer.server.serverPort());
    ASSERT_TRUE(waitUntil([&] {
        Vn_Update();
        capture.pollLocal();
        return capture.state() == EventCaptureState::Stopped;
    }));
    EXPECT_TRUE(hasCompletedStopRequest());
    EXPECT_EQ(HarkenEditor::Bridge::RemoteEventCaptureState::Idle, connection.eventCaptureStatus().state);
    EXPECT_TRUE(capture.canStart());
}

TEST(EventCaptureController, UnsupportedRemoteDoesNotFallBackToLocal)
{
    ensureRemoteTestApp();
    using namespace VNX::Monitor;
    RemotePeer peer;
    peer.identity.capabilities &= ~Wire::kCapabilityEventCapture;
    RemoteConnection connection;
    EventCaptureController capture(&connection);
    capture.setLocalRuntimeAvailable(true);
    connection.connectTarget(QStringLiteral("127.0.0.1"), peer.server.serverPort());
    ASSERT_TRUE(waitUntil([&] { return connection.state() == RemoteConnectionState::Connected; }));
    EXPECT_FALSE(capture.canStart());
    capture.start();
    EXPECT_EQ(EventCaptureState::Unavailable, capture.state());
    EXPECT_EQ(0, capture.rowCount());
}

TEST(EventCaptureController, ProjectAndObjectIdentityGateNamesAndJumps)
{
    ensureRemoteTestApp();
    using namespace VNX::Monitor;
    RemotePeer peer;
    RemoteConnection connection;
    EventCaptureController capture(&connection);
    VNX::MonitorPanel panel;
    panel.setCaptureController(&capture);
    const QUuid project = QUuid::createUuid();
    const QUuid object = VNX::authoringUuid(harkentest::StudioObjectId(7));
    const auto objects = [&](const QUuid& projectGuid, const QString& name) {
        VNX::ProjectData data;
        data.projectId = projectGuid.toString(QUuid::WithoutBraces);
        data.root = std::make_shared<VNX::ProjectItem>(name, VNX::ProjectItem::Type::Action);
        data.root->objectID = 17;
        data.root->guid = object;
        return VNX::ProjectObjectIndex::fromProject(&data);
    };
    capture.setProjectObjects(objects(project, QStringLiteral("Engine Start")));
    int located = 0;
    panel.setJumpLocator([&](harkenstudio::ObjectId stableId) { EXPECT_EQ(stableId, harkentest::StudioObjectId(7u)); ++located; });
    quint64 epoch = 500;
    peer.eventResponder = [&](const Wire::Frame& request) {
        Wire::EventCaptureStatus status;
        status.sessionEpoch = epoch;
        status.startedTimestampNs = 100;
        if (request.type == Wire::Message::EventSeal) {
            status.state = VN_MONITOR_EVENT_SESSION_DRAINED;
            status.stoppedTimestampNs = 1000;
        }
        peer.send(Wire::MakeFrame(Wire::Message::EventStatus, request.requestId, status));
    };
    connection.connectTarget(QStringLiteral("127.0.0.1"), peer.server.serverPort());
    ASSERT_TRUE(waitUntil([&] { return capture.canStart(); }));
    capture.start();
    ASSERT_TRUE(waitUntil([&] { return capture.canStop(); }));
    const auto sendEvent = [&](quint64 sequence, const QUuid& sourceProject, quint64 fingerprint) {
        Wire::MonitorRecords records;
        records.sessionEpoch = epoch;
        records.recordCount = 1;
        constexpr auto bytes = harken::monitor::PackedRecordBytes(sizeof(VNX::Monitor::MonitorEventDetailV2));
        records.bytes.resize(bytes);
        VnMonitorRecordHeader header{};
        harken::monitor::FillSyntheticHeader(&header, sequence, bytes,
            sizeof(VNX::Monitor::MonitorEventDetailV2), VN_MONITOR_RECORD_EVENT_DETAIL, 0);
        header.recordVersion = 2;
        header.flags = VN_MONITOR_RECORD_FLAG_OPTIONAL;
        header.streamKind = VN_MONITOR_STREAM_EVENT;
        header.monitorSessionEpoch = epoch;
        header.stableId = harkentest::ObjectId(7);
        header.programSemanticFingerprint = fingerprint;
        harken::monitor::PackHeader(records.bytes.data(), header);
        VNX::Monitor::MonitorEventDetailV2 detail{};
        detail.structSize = sizeof(detail);
        detail.category = VN_MONITOR_EVENT_CATEGORY_OPERATION;
        detail.code = VN_MONITOR_OPERATION_POST_ACTION;
        detail.timestampNs = 200 + sequence;
        const auto guid = sourceProject.toRfc4122();
        std::memcpy(detail.projectGuid, guid.constData(), sizeof(detail.projectGuid));
        std::memcpy(records.bytes.data() + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES, &detail, sizeof(detail));
        peer.send(Wire::MakeFrame(Wire::Message::EventRecords, 0, records));
    };
    sendEvent(1, project, 91);
    sendEvent(2, QUuid::createUuid(), 91);
    sendEvent(3, project, 92);
    ASSERT_TRUE(waitUntil([&] { return capture.rowCount() == 3; }));
    EXPECT_EQ(capture.eventAt(0).objectName, QStringLiteral("Engine Start"));
    EXPECT_TRUE(capture.eventAt(1).objectName.isEmpty());
    EXPECT_EQ(capture.eventAt(2).objectName, QStringLiteral("Engine Start"));
    EXPECT_EQ(panel.table()->model()->index(0, 3).data().toString(), QStringLiteral("Engine Start"));
    auto* search = panel.findChild<QLineEdit*>(QStringLiteral("MonitorTextFilter"));
    search->setText(QStringLiteral("Engine Start"));
    EXPECT_EQ(panel.visibleRowCount(), 2);
    search->clear();
    panel.table()->selectRow(0);
    EXPECT_TRUE(panel.jumpButton()->isEnabled());
    EXPECT_TRUE(panel.jumpCurrentRow());
    EXPECT_EQ(located, 1);
    capture.stop();
    ASSERT_TRUE(waitUntil([&] { return capture.state() == EventCaptureState::Stopped; }));
    capture.setProjectObjects(objects(project, QStringLiteral("Changed")));
    EXPECT_TRUE(panel.jumpButton()->isEnabled());
    panel.jumpCurrentRow();
    EXPECT_EQ(located, 2);
    EXPECT_EQ(capture.eventAt(0).objectName, QStringLiteral("Engine Start"));
    EXPECT_EQ(capture.eventAt(2).objectName, QStringLiteral("Engine Start"));
    capture.setProjectObjects(objects(QUuid::createUuid(), QStringLiteral("Other project")));
    EXPECT_FALSE(panel.jumpButton()->isEnabled());
    EXPECT_FALSE(panel.jumpCurrentRow());
    EXPECT_EQ(located, 2);
    capture.setProjectObjects(objects(project, QStringLiteral("Renamed")));
    EXPECT_TRUE(panel.jumpButton()->isEnabled());
    EXPECT_EQ(capture.eventAt(0).objectName, QStringLiteral("Engine Start"));
    capture.setProjectObjects({});
    EXPECT_FALSE(panel.jumpButton()->isEnabled());
    EXPECT_FALSE(panel.jumpCurrentRow());
    EXPECT_EQ(located, 2);
    EXPECT_EQ(capture.rowCount(), 3);
}

TEST(RemoteConnection, HandshakeFragmentsCatalogPagesAndDifferentEngineVersion)
{
    ensureRemoteTestApp();
    RemotePeer peer;
    peer.fragmentWelcome = true;
    peer.paginateCatalog = true;
    RemoteConnection connection;
    connection.connectTarget(QStringLiteral("127.0.0.1"), peer.server.serverPort());
    ASSERT_TRUE(waitUntil([&] { return connection.catalog().size() == 2; }));
    EXPECT_EQ(connection.state(), RemoteConnectionState::Connected);
    EXPECT_EQ(connection.target().engineVersion, QStringLiteral("9.2.1"));
    EXPECT_EQ(connection.target().machine, QStringLiteral("Test machine"));
    EXPECT_EQ(connection.target().instanceId, QStringLiteral("2a"));
    EXPECT_EQ(peer.catalogPages, 2);
    EXPECT_FALSE(connection.captureEnabled());
    EXPECT_EQ(peer.snapshots, 0);
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

TEST(RemoteConnection, IncompatibleMajorRejectedBeforeCatalogOrCommands)
{
    ensureRemoteTestApp();
    RemotePeer peer;
    peer.identity.major = Wire::kProtocolMajor + 1;
    RemoteConnection connection;
    connection.connectTarget(QStringLiteral("127.0.0.1"), peer.server.serverPort());
    ASSERT_TRUE(waitUntil([&] { return connection.state() == RemoteConnectionState::Failed; }));
    EXPECT_EQ(peer.catalogPages, 0);
    EXPECT_TRUE(connection.catalog().isEmpty());
    EXPECT_FALSE(connection.canControl());
}

TEST(RemoteConnection, OversizedFrameRejectedWithoutWaitingForPayload)
{
    ensureRemoteTestApp();
    RemotePeer peer;
    peer.invalidHeader = true;
    RemoteConnection connection;
    connection.connectTarget(QStringLiteral("127.0.0.1"), peer.server.serverPort());
    ASSERT_TRUE(waitUntil([&] { return connection.state() == RemoteConnectionState::Failed; }));
}

TEST(RemoteConnection, BusyRuntimeHasExplicitConnectionFailure)
{
    ensureRemoteTestApp();
    RemotePeer peer;
    peer.busy = true;
    RemoteConnection connection;
    connection.connectTarget(QStringLiteral("127.0.0.1"), peer.server.serverPort());
    ASSERT_TRUE(waitUntil([&] { return connection.state() == RemoteConnectionState::Failed; }));
    EXPECT_FALSE(connection.canControl());
    EXPECT_EQ(peer.catalogPages, 0);
}

TEST(RemoteConnection, CaptureShowsActualSnapshotThenInvalidatesOnStopAndDisconnect)
{
    ensureRemoteTestApp();
    RemotePeer peer;
    RemoteConnection connection;
    connection.connectTarget(QStringLiteral("127.0.0.1"), peer.server.serverPort());
    ASSERT_TRUE(waitUntil([&] { return connection.state() == RemoteConnectionState::Connected; }));
    connection.setCaptureEnabled(true);
    ASSERT_TRUE(waitUntil([&] { return connection.snapshot().valid && !connection.capturePending(); }));
    EXPECT_EQ(connection.snapshot().activeVoices, 3u);
    EXPECT_EQ(connection.snapshot().callbackCpuUs, 17u);
    EXPECT_FLOAT_EQ(connection.snapshot().peak, 0.75f);
    const auto first = connection.snapshot().blockIndex;
    ASSERT_TRUE(waitUntil([&] { return connection.snapshot().blockIndex > first; }));
    connection.setCaptureEnabled(false);
    EXPECT_FALSE(connection.snapshot().valid);
    ASSERT_TRUE(waitUntil([&] { return !connection.capturePending(); }));
    connection.setCaptureEnabled(true);
    ASSERT_TRUE(waitUntil([&] { return connection.snapshot().valid; }));
    peer.client->abort();
    ASSERT_TRUE(waitUntil([&] { return connection.state() == RemoteConnectionState::Failed; }));
    EXPECT_FALSE(connection.snapshot().valid);
    EXPECT_FALSE(connection.captureEnabled());
    EXPECT_TRUE(connection.catalog().isEmpty());
}

TEST(RemoteConnection, RemoteCommandsHaveResultsAndNeverReplayAfterReconnect)
{
    ensureRemoteTestApp();
    RemotePeer peer;
    RemoteConnection connection;
    connection.connectTarget(QStringLiteral("127.0.0.1"), peer.server.serverPort());
    ASSERT_TRUE(waitUntil([&] { return connection.catalog().size() == 2; }));
    connection.postAction(harkentest::StudioObjectId(1001));
    ASSERT_TRUE(waitUntil([&] { return connection.lastPlaybackHandle() == 7001; }));
    EXPECT_EQ(peer.commands.last().operation, Wire::Operation::PostAction);
    connection.setParameter(harkentest::StudioObjectId(2001), 0.6f);
    ASSERT_TRUE(waitUntil([&] { return !connection.commandPending(); }));
    EXPECT_EQ(peer.commands.last().target, harkentest::StudioObjectId(2001u));
    EXPECT_FLOAT_EQ(peer.commands.last().value, 0.6f);
    connection.stopPlayback(7001);
    ASSERT_TRUE(waitUntil([&] { return !connection.commandPending(); }));
    EXPECT_EQ(peer.commands.last().operation, Wire::Operation::StopAction);
    EXPECT_EQ(connection.lastPlaybackHandle(), 0u);

    peer.replyToCommands = false;
    connection.postAction(harkentest::StudioObjectId(1001));
    ASSERT_TRUE(waitUntil([&] { return peer.commands.size() == 4; }));
    EXPECT_TRUE(connection.commandPending());
    connection.disconnectTarget();
    EXPECT_FALSE(connection.commandPending());
    peer.replyToCommands = true;
    connection.connectTarget(QStringLiteral("127.0.0.1"), peer.server.serverPort());
    ASSERT_TRUE(waitUntil([&] { return connection.catalog().size() == 2; }));
    EXPECT_EQ(peer.commands.size(), 4);
    EXPECT_EQ(connection.lastPlaybackHandle(), 0u);
}

TEST(RemoteConnection, DiscoveryCorrelatesRequestAndKeepsVersionIdentity)
{
    ensureRemoteTestApp();
    QUdpSocket server;
    ASSERT_TRUE(server.bind(QHostAddress::LocalHost, 0));
    QObject::connect(&server, &QUdpSocket::readyRead, &server, [&] {
        const auto datagram = server.receiveDatagram();
        Wire::Header header;
        ASSERT_TRUE(Wire::DecodeHeader(datagram.data().data(), datagram.data().size(), header));
        Wire::Identity identity;
        identity.instanceId = 77;
        identity.processId = 456;
        identity.tcpPort = 45678;
        identity.engineVersion = "1.2.3";
        identity.applicationName = "Discovery game";
        const auto response = bytesFor(Wire::MakeFrame(Wire::Message::Announcement, header.requestId, identity));
        server.writeDatagram(response, datagram.senderAddress(), datagram.senderPort());
        server.writeDatagram(response, datagram.senderAddress(), datagram.senderPort());
    });
    RemoteConnection connection;
    connection.discover(server.localPort());
    ASSERT_TRUE(waitUntil([&] { return !connection.discoveredTargets().isEmpty(); }));
    EXPECT_EQ(connection.discoveredTargets().size(), 1);
    EXPECT_EQ(connection.discoveredTargets().first().port, 45678);
    EXPECT_TRUE(connection.discoveredTargets().first().compatible);
}

TEST(RemoteConnection, SnapshotCaptureWorksWithoutMonitorOrControlCapability)
{
    ensureRemoteTestApp();
    RemotePeer peer;
    peer.identity.capabilities = Wire::kCapabilitySnapshot;
    RemoteConnection connection;
    connection.connectTarget(QStringLiteral("127.0.0.1"), peer.server.serverPort());
    ASSERT_TRUE(waitUntil([&] { return connection.state() == RemoteConnectionState::Connected; }));
    EXPECT_FALSE(connection.canControl());
    EXPECT_TRUE(connection.canCapture());
    EXPECT_EQ(peer.catalogPages, 0);
    connection.postAction(harkentest::StudioObjectId(1001));
    EXPECT_TRUE(peer.commands.isEmpty());
    connection.setCaptureEnabled(true);
    ASSERT_TRUE(waitUntil([&] { return connection.snapshot().valid; }));
    EXPECT_FALSE(connection.capturePending());
}

TEST(RemoteConnection, DiscoveryUsesExplicitTcpBindAddress)
{
    ensureRemoteTestApp();
    QUdpSocket server;
    ASSERT_TRUE(server.bind(QHostAddress::LocalHost, 0));
    QObject::connect(&server, &QUdpSocket::readyRead, &server, [&] {
        const auto datagram = server.receiveDatagram();
        Wire::Header header;
        ASSERT_TRUE(Wire::DecodeHeader(datagram.data().data(), datagram.data().size(), header));
        Wire::Identity identity;
        identity.instanceId = 88;
        identity.tcpPort = 34567;
        identity.bindAddress = "192.168.1.20";
        const auto response = bytesFor(Wire::MakeFrame(Wire::Message::Announcement, header.requestId, identity));
        server.writeDatagram(response, datagram.senderAddress(), datagram.senderPort());
    });
    RemoteConnection connection;
    connection.discover(server.localPort());
    ASSERT_TRUE(waitUntil([&] { return connection.discoveredTargets().size() == 1; }));
    EXPECT_EQ(connection.discoveredTargets().first().address, QStringLiteral("192.168.1.20"));
}

TEST(RemoteConnection, LoopbackMulticastDiscoversMultipleSharedPortListeners)
{
    ensureRemoteTestApp();
    QNetworkInterface loopback;
    for (const auto& interface : QNetworkInterface::allInterfaces()) {
        if ((interface.flags() & QNetworkInterface::IsLoopBack)
            && (interface.flags() & QNetworkInterface::IsUp)) {
            loopback = interface;
            break;
        }
    }
    ASSERT_TRUE(loopback.isValid());
    QUdpSocket first;
    QUdpSocket second;
    const auto mode = QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint;
    ASSERT_TRUE(first.bind(QHostAddress::AnyIPv4, 0, mode));
    ASSERT_TRUE(second.bind(QHostAddress::AnyIPv4, first.localPort(), mode));
    const QHostAddress group(QString::fromLatin1(Wire::kDiscoveryGroup));
    ASSERT_TRUE(first.joinMulticastGroup(group, loopback));
    ASSERT_TRUE(second.joinMulticastGroup(group, loopback));
    auto reply = [](QUdpSocket* server, std::uint64_t instance) {
        while (server->hasPendingDatagrams()) {
            const auto datagram = server->receiveDatagram();
            Wire::Header header;
            ASSERT_TRUE(Wire::DecodeHeader(datagram.data().data(), datagram.data().size(), header));
            Wire::Identity identity;
            identity.instanceId = instance;
            identity.tcpPort = 40000 + static_cast<std::uint32_t>(instance);
            identity.bindAddress = "127.0.0.1";
            const auto response = bytesFor(Wire::MakeFrame(Wire::Message::Announcement, header.requestId, identity));
            server->writeDatagram(response, datagram.senderAddress(), datagram.senderPort());
        }
    };
    QObject::connect(&first, &QUdpSocket::readyRead, &first, [&] { reply(&first, 1); });
    QObject::connect(&second, &QUdpSocket::readyRead, &second, [&] { reply(&second, 2); });
    RemoteConnection connection;
    connection.discover(first.localPort());
    ASSERT_TRUE(waitUntil([&] { return connection.discoveredTargets().size() == 2; }));
    EXPECT_NE(connection.discoveredTargets()[0].instanceId, connection.discoveredTargets()[1].instanceId);
}

TEST(RemoteConnection, InstanceAnnouncementsMergeAddressesAndPreferLoopback)
{
    ensureRemoteTestApp();
    QUdpSocket server;
    ASSERT_TRUE(server.bind(QHostAddress::LocalHost, 0));
    QObject::connect(&server, &QUdpSocket::readyRead, &server, [&] {
        while (server.hasPendingDatagrams()) {
            const auto datagram = server.receiveDatagram();
            Wire::Header header;
            ASSERT_TRUE(Wire::DecodeHeader(datagram.data().data(), datagram.data().size(), header));
            for (const auto* address : {"192.168.1.20", "127.0.0.1", "10.0.0.20"}) {
                Wire::Identity identity;
                identity.instanceId = 99;
                identity.processId = 500;
                identity.tcpPort = 34567;
                identity.bindAddress = address;
                const auto response = bytesFor(Wire::MakeFrame(Wire::Message::Announcement, header.requestId, identity));
                server.writeDatagram(response, datagram.senderAddress(), datagram.senderPort());
            }
        }
    });
    RemoteConnection connection;
    connection.discover(server.localPort());
    ASSERT_TRUE(waitUntil([&] {
        return connection.discoveredTargets().size() == 1
            && connection.discoveredTargets().first().address == QStringLiteral("127.0.0.1");
    }));
    EXPECT_EQ(connection.discoveredTargets().first().processId, 500u);
}

TEST(RemoteConnection, DialogKeepsSelectedInstanceAndConnectsItsLatestCompatibleEndpoint)
{
    ensureRemoteTestApp();
    RemotePeer peer;
    QUdpSocket server;
    ASSERT_TRUE(server.bind(QHostAddress::LocalHost, 0));
    QHostAddress receiver;
    quint16 receiverPort = 0;
    quint32 requestId = 0;
    Wire::Identity identity = peer.identity;
    identity.tcpPort = 34567;
    identity.bindAddress = "192.168.1.20";
    const auto announce = [&] {
        server.writeDatagram(bytesFor(Wire::MakeFrame(Wire::Message::Announcement, requestId, identity)),
                             receiver, receiverPort);
    };
    QObject::connect(&server, &QUdpSocket::readyRead, &server, [&] {
        while (server.hasPendingDatagrams()) {
            const auto datagram = server.receiveDatagram();
            Wire::Header header;
            ASSERT_TRUE(Wire::DecodeHeader(datagram.data().data(), datagram.data().size(), header));
            receiver = datagram.senderAddress();
            receiverPort = datagram.senderPort();
            requestId = header.requestId;
            announce();
        }
    });
    RemoteConnection connection;
    VNX::RemoteConnectDialog dialog(&connection);
    auto* targets = dialog.findChild<VNX::ProportionalTreeWidget*>(QStringLiteral("remoteTargets"));
    auto* connect = dialog.findChild<QPushButton*>(QStringLiteral("remoteConnect"));
    ASSERT_NE(targets, nullptr);
    ASSERT_NE(connect, nullptr);
    EXPECT_FALSE(connect->isEnabled());
    connection.discover(server.localPort());
    ASSERT_TRUE(waitUntil([&] { return targets->topLevelItemCount() == 1; }));
    targets->setCurrentItem(targets->topLevelItem(0));
    EXPECT_TRUE(connect->isEnabled());
    EXPECT_EQ(targets->currentItem()->text(2), QStringLiteral("192.168.1.20:34567"));

    identity.bindAddress = "127.0.0.1";
    identity.tcpPort = peer.server.serverPort();
    identity.major = Wire::kProtocolMajor + 1;
    announce();
    ASSERT_TRUE(waitUntil([&] { return !connection.discoveredTargets().first().compatible; }));
    ASSERT_NE(targets->currentItem(), nullptr);
    EXPECT_EQ(targets->selectedItems().size(), 1);
    EXPECT_EQ(targets->currentItem()->text(2),
              QStringLiteral("127.0.0.1:%1").arg(peer.server.serverPort()));
    EXPECT_FALSE(connect->isEnabled());
    identity.major = Wire::kProtocolMajor;
    announce();
    ASSERT_TRUE(waitUntil([&] { return connect->isEnabled(); }));
    connect->click();
    ASSERT_TRUE(waitUntil([&] { return connection.state() == RemoteConnectionState::Connected; }));
    EXPECT_EQ(connection.target().address, QStringLiteral("127.0.0.1"));
    EXPECT_EQ(connection.target().port, peer.server.serverPort());
    EXPECT_FALSE(connection.captureEnabled());
    EXPECT_EQ(peer.snapshots, 0);
    EXPECT_TRUE(peer.commands.isEmpty());
}

TEST(RemoteConnection, AsynchronousMonitorFailureKeepsSnapshotConnection)
{
    ensureRemoteTestApp();
    RemotePeer peer;
    RemoteConnection connection;
    connection.connectTarget(QStringLiteral("127.0.0.1"), peer.server.serverPort());
    ASSERT_TRUE(waitUntil([&] { return connection.state() == RemoteConnectionState::Connected; }));
    connection.setCaptureEnabled(true);
    ASSERT_TRUE(waitUntil([&] { return connection.snapshot().valid && !connection.capturePending(); }));
    Wire::MonitorStatus failed;
    failed.result = -1;
    peer.send(Wire::MakeFrame(Wire::Message::MonitorStatus, 0, failed));
    const auto previousBlock = connection.snapshot().blockIndex;
    ASSERT_TRUE(waitUntil([&] { return connection.snapshot().blockIndex > previousBlock; }));
    EXPECT_EQ(connection.state(), RemoteConnectionState::Connected);
    EXPECT_TRUE(connection.captureEnabled());
}

TEST(RemoteConnection, HandshakeTimeoutAndCancelledConnectionDoNotContaminateReconnect)
{
    ensureRemoteTestApp();
    RemotePeer peer;
    peer.skipWelcome = true;
    RemoteConnection connection;
    connection.connectTarget(QStringLiteral("127.0.0.1"), peer.server.serverPort());
    ASSERT_TRUE(waitUntil([&] { return connection.state() == RemoteConnectionState::Failed; }, 6500));
    peer.skipWelcome = false;
    peer.fragmentWelcome = true;
    connection.connectTarget(QStringLiteral("127.0.0.1"), peer.server.serverPort());
    ASSERT_TRUE(waitUntil([&] { return connection.state() == RemoteConnectionState::Handshaking; }));
    connection.disconnectTarget();
    connection.connectTarget(QStringLiteral("127.0.0.1"), peer.server.serverPort());
    ASSERT_TRUE(waitUntil([&] { return connection.catalog().size() == 2; }));
    EXPECT_EQ(connection.state(), RemoteConnectionState::Connected);
}

TEST(RemoteConnection, ManualConnectionValidatesEndpointAndEscapeDoesNotConnect)
{
    ensureRemoteTestApp();
    RemotePeer peer;
    RemoteConnection connection;
    VNX::RemoteConnectDialog dialog(&connection);
    auto* add = dialog.findChild<QToolButton*>(QStringLiteral("remoteAddConnection"));
    ASSERT_NE(add, nullptr);
    bool inspected = false;
    QTimer::singleShot(0, &dialog, [&] {
        auto* popup = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        ASSERT_NE(popup, nullptr);
        const auto dismiss = qScopeGuard([popup] { if (popup->isVisible()) popup->reject(); });
        EXPECT_EQ(popup->objectName(), QStringLiteral("remoteManualConnectDialog"));
        auto* address = popup->findChild<QLineEdit*>(QStringLiteral("remoteAddress"));
        auto* port = popup->findChild<QSpinBox*>(QStringLiteral("remotePort"));
        auto* connect = popup->findChild<QPushButton*>(QStringLiteral("remoteManualConnect"));
        ASSERT_NE(address, nullptr);
        ASSERT_NE(port, nullptr);
        ASSERT_NE(connect, nullptr);
        auto* buttons = popup->findChild<QDialogButtonBox*>();
        ASSERT_NE(buttons, nullptr);
        ASSERT_EQ(buttons->buttons().size(), 1);
        EXPECT_EQ(buttons->buttons().first(), connect);
        EXPECT_EQ(buttons->buttonRole(connect), QDialogButtonBox::AcceptRole);
        address->setText(QStringLiteral("127.0.0.1"));
        port->setValue(0);
        EXPECT_FALSE(connect->isEnabled());
        port->setValue(peer.server.serverPort());
        EXPECT_TRUE(connect->isEnabled());
        address->setText(QStringLiteral("   "));
        EXPECT_FALSE(connect->isEnabled());
        address->setText(QStringLiteral(" localhost "));
        EXPECT_TRUE(connect->isEnabled());
        EXPECT_EQ(connection.state(), RemoteConnectionState::Disconnected);
        inspected = true;
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(popup, &escape);
        EXPECT_FALSE(popup->isVisible());
    });
    add->click();
    EXPECT_TRUE(inspected);
    QCoreApplication::processEvents();
    EXPECT_EQ(connection.state(), RemoteConnectionState::Disconnected);
    EXPECT_TRUE(peer.client.isNull());
    EXPECT_TRUE(peer.commands.isEmpty());
}

TEST(RemoteConnection, ManualConnectionSurvivesRefreshAndClosingUntilDisconnected)
{
    ensureRemoteTestApp();
    QSettings settings;
    const QVariant savedHost = settings.value(QStringLiteral("remote/host"));
    const QVariant savedPort = settings.value(QStringLiteral("remote/port"));
    const auto restoreSettings = qScopeGuard([&] {
        if (savedHost.isValid()) settings.setValue(QStringLiteral("remote/host"), savedHost);
        else settings.remove(QStringLiteral("remote/host"));
        if (savedPort.isValid()) settings.setValue(QStringLiteral("remote/port"), savedPort);
        else settings.remove(QStringLiteral("remote/port"));
    });
    RemotePeer peer;
    RemoteConnection connection;
    VNX::RemoteConnectDialog dialog(&connection);
    auto* add = dialog.findChild<QToolButton*>(QStringLiteral("remoteAddConnection"));
    auto* targets = dialog.findChild<VNX::ProportionalTreeWidget*>(QStringLiteral("remoteTargets"));
    auto* connect = dialog.findChild<QPushButton*>(QStringLiteral("remoteConnect"));
    auto* disconnect = dialog.findChild<QPushButton*>(QStringLiteral("remoteDisconnect"));
    ASSERT_NE(add, nullptr);
    ASSERT_NE(targets, nullptr);
    ASSERT_NE(connect, nullptr);
    ASSERT_NE(disconnect, nullptr);
    bool submitted = false;
    QTimer::singleShot(0, &dialog, [&] {
        auto* popup = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        ASSERT_NE(popup, nullptr);
        const auto dismiss = qScopeGuard([popup] { if (popup->isVisible()) popup->reject(); });
        auto* address = popup->findChild<QLineEdit*>(QStringLiteral("remoteAddress"));
        auto* port = popup->findChild<QSpinBox*>(QStringLiteral("remotePort"));
        auto* submit = popup->findChild<QPushButton*>(QStringLiteral("remoteManualConnect"));
        ASSERT_NE(address, nullptr);
        ASSERT_NE(port, nullptr);
        ASSERT_NE(submit, nullptr);
        address->setText(QStringLiteral(" 127.0.0.1 "));
        port->setValue(peer.server.serverPort());
        ASSERT_TRUE(submit->isEnabled());
        submit->click();
        submitted = true;
    });
    add->click();
    ASSERT_TRUE(submitted);
    ASSERT_TRUE(waitUntil([&] { return connection.state() == RemoteConnectionState::Connected; }));
    EXPECT_EQ(settings.value(QStringLiteral("remote/host")).toString(), QStringLiteral("127.0.0.1"));
    EXPECT_EQ(settings.value(QStringLiteral("remote/port")).toUInt(), peer.server.serverPort());
    EXPECT_FALSE(connect->isEnabled());
    EXPECT_TRUE(disconnect->isEnabled());
    ASSERT_EQ(targets->topLevelItemCount(), 1);
    EXPECT_EQ(targets->topLevelItem(0)->text(0), QStringLiteral("Remote game"));
    QUdpSocket discovery;
    ASSERT_TRUE(discovery.bind(QHostAddress::LocalHost, 0));
    connection.discover(discovery.localPort());
    QCoreApplication::processEvents();
    EXPECT_EQ(targets->topLevelItemCount(), 1);
    EXPECT_FALSE(connection.captureEnabled());
    EXPECT_EQ(peer.snapshots, 0);
    EXPECT_TRUE(peer.commands.isEmpty());
    dialog.show();
    dialog.close();
    EXPECT_EQ(connection.state(), RemoteConnectionState::Connected);
    disconnect->click();
    EXPECT_EQ(connection.state(), RemoteConnectionState::Disconnected);
    EXPECT_FALSE(disconnect->isEnabled());
    EXPECT_EQ(targets->topLevelItemCount(), 0);
    bool remembered = false;
    QTimer::singleShot(0, &dialog, [&] {
        auto* popup = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        ASSERT_NE(popup, nullptr);
        const auto dismiss = qScopeGuard([popup] { if (popup->isVisible()) popup->reject(); });
        auto* address = popup->findChild<QLineEdit*>(QStringLiteral("remoteAddress"));
        auto* port = popup->findChild<QSpinBox*>(QStringLiteral("remotePort"));
        ASSERT_NE(address, nullptr);
        ASSERT_NE(port, nullptr);
        EXPECT_EQ(address->text(), QStringLiteral("127.0.0.1"));
        EXPECT_EQ(port->value(), peer.server.serverPort());
        remembered = true;
    });
    add->click();
    EXPECT_TRUE(remembered);
}

TEST(RemoteConnection, MainToolbarOpensWithoutProjectAndReflectsTheActualSession)
{
    ensureRemoteTestApp();
    VNX::ActionManager::instance().clearAllForTesting();
    if (VNX::ProjectManager::instance().isProjectOpen())
        VNX::ProjectManager::instance().closeProject();
    RemotePeer peer;
    auto window = std::make_unique<VNX::MainWindow>();
    const auto cleanup = qScopeGuard([&] {
        VNX::ProjectManager::instance().closeProject();
        QObject::disconnect(&VNX::UndoRedoManager::instance(), nullptr, window.get(), nullptr);
        VNX::ToastManager::instance().setParentWidget(nullptr);
        window.reset();
        VNX::ActionManager::instance().setTransactionCallbacks({});
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::UndoRedoManager::instance().clear();
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
    });
    auto* toolbar = window->findChild<VNX::PlatformBar*>(QStringLiteral("platformBar"));
    auto* button = window->findChild<QToolButton*>(QStringLiteral("remoteConnectButton"));
    ASSERT_NE(toolbar, nullptr);
    ASSERT_NE(button, nullptr);
    EXPECT_EQ(window->findChild<QAction*>(QStringLiteral("toolsMenu_remoteConnect")), nullptr);
    EXPECT_EQ(window->findChild<QToolBar*>(QStringLiteral("remoteToolBar")), nullptr);
    EXPECT_EQ(window->toolBarArea(toolbar), Qt::TopToolBarArea);
    EXPECT_TRUE(toolbar->isAncestorOf(button));
    EXPECT_FALSE(button->property("segmentedControl").toBool());
    EXPECT_FALSE(button->isChecked());
    EXPECT_EQ(button->toolButtonStyle(), Qt::ToolButtonIconOnly);
    window->show();
    QCoreApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QCoreApplication::processEvents();
    EXPECT_FALSE(VNX::ProjectManager::instance().isProjectOpen());
    EXPECT_TRUE(toolbar->isVisible());
    EXPECT_TRUE(button->isVisible());
    EXPECT_TRUE(button->isEnabled());
    button->click();
    auto* dialog = window->findChild<VNX::RemoteConnectDialog*>(QStringLiteral("remoteConnectDialog"));
    ASSERT_NE(dialog, nullptr);
    EXPECT_TRUE(dialog->isVisible());
    auto* connection = window->remoteConnection();
    ASSERT_NE(connection, nullptr);
    connection->connectTarget(QStringLiteral("127.0.0.1"), peer.server.serverPort());
    EXPECT_FALSE(button->isChecked());
    ASSERT_TRUE(waitUntil([&] { return connection->state() == RemoteConnectionState::Connected; }));
    EXPECT_TRUE(button->isChecked());
    button->click();
    EXPECT_TRUE(button->isChecked());
    dialog->close();
    EXPECT_EQ(connection->state(), RemoteConnectionState::Connected);
    connection->disconnectTarget();
    EXPECT_FALSE(button->isChecked());
}

}
