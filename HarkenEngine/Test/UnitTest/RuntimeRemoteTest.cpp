#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Common/ObjectIdTestSupport.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "Common/TestUtil.h"
#include "MonitorCaptureHubTestHooks.h"
#include "harken_remote/Protocol.h"
#include <gtest/gtest.h>
#include <harken/Harken.h>

#include <array>
#include <chrono>
#include <functional>
#include <thread>

namespace {
using namespace harken::remote;

VnRemoteConfig RemoteConfig() {
    VnRemoteConfig config{};
    config.structSize = sizeof(config);
    config.bindAddress = "127.0.0.1";
    config.applicationName = "Remote runtime tests";
    return config;
}

VnRemoteStatus Status() {
    VnRemoteStatus status{};
    status.structSize = sizeof(status);
    EXPECT_EQ(VN_OK, Vn_RemoteGetStatus(&status));
    return status;
}

bool WaitUntil(const std::function<bool()> &predicate, bool pump = true,
               std::uint32_t timeoutMs = 2000) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < limit) {
        if (predicate())
            return true;
        if (pump)
            Vn_Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

TEST(RuntimeRemoteProtocol, ExplicitLittleEndianHeaderAndStrictLengths) {
    Frame frame{Message::Ping, 0x12345678, {}};
    auto bytes = EncodeFrame(frame);
    ASSERT_EQ(16u, bytes.size());
    EXPECT_EQ('V', bytes[0]);
    EXPECT_EQ('N', bytes[1]);
    EXPECT_EQ('R', bytes[2]);
    EXPECT_EQ('M', bytes[3]);
    EXPECT_EQ(0x78, bytes[12]);
    EXPECT_EQ(0x12, bytes[15]);
    Header header;
    ASSERT_TRUE(DecodeHeader(bytes.data(), bytes.size(), header));
    EXPECT_EQ(Message::Ping, header.type);
    EXPECT_FALSE(DecodeHeader(bytes.data(), 15, header));
    bytes[6] = 0x10;
    EXPECT_FALSE(DecodeHeader(bytes.data(), bytes.size(), header));
    bytes = EncodeFrame(frame);
    bytes[10] = 1;
    EXPECT_FALSE(DecodeHeader(bytes.data(), bytes.size(), header));
}

TEST(RuntimeRemoteProtocol, CommandsRoundTripWithGenerationAndNoTrailingBytes) {
    Command command;
    command.operation = Operation::SetGlobalVariable;
    command.expectedGeneration = 123;
    command.target = harkentest::ObjectId(0x1020304050607080ull);
    command.value = 0.25f;
    Frame frame = MakeFrame(Message::Command, 7, command);
    Command decoded;
    ASSERT_TRUE(DecodePayload(frame, decoded));
    EXPECT_EQ(command.target, decoded.target);
    EXPECT_EQ(123u, decoded.expectedGeneration);
    EXPECT_FLOAT_EQ(0.25f, decoded.value);
    frame.payload.push_back(0);
    EXPECT_FALSE(DecodePayload(frame, decoded));
    frame.payload.resize(3);
    EXPECT_FALSE(DecodePayload(frame, decoded));
}

TEST(RuntimeRemoteProtocol, CatalogRejectsImpossibleRowCount) {
    Writer writer;
    writer.I32(0);
    writer.U64(1);
    writer.U32(0xffffffff);
    writer.U32(0);
    writer.U32(0xffffffff);
    Catalog catalog;
    Reader reader(writer.bytes);
    EXPECT_FALSE(Decode(reader, catalog));
}

TEST(RuntimeRemote, DefaultOffStatusAndReleaseLifecycle) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    EXPECT_EQ(VN_REMOTE_STOPPED, Status().state);
    EXPECT_EQ(0u, Status().tcpPort);
    const auto config = RemoteConfig();
    ASSERT_EQ(VN_OK, Vn_RemoteStart(&config));
    const auto running = Status();
    EXPECT_EQ(VN_REMOTE_LISTENING, running.state);
    EXPECT_GT(running.tcpPort, 0u);
    EXPECT_EQ(0u, running.discoveryPort);
    EXPECT_NE(0u, running.instanceId);
    EXPECT_NE(0u, running.processId);
    EXPECT_NE('\0', running.engineVersion[0]);
    EXPECT_NE('\0', running.buildId[0]);
    EXPECT_EQ(VN_IN_USE, Vn_RemoteStart(&config));
    EXPECT_EQ(VN_OK, Vn_RemoteStop());
    EXPECT_EQ(VN_OK, Vn_RemoteStop());
    EXPECT_EQ(VN_REMOTE_STOPPED, Status().state);
    EXPECT_EQ(VN_OK, Vn_RemoteStart(&config));
    EXPECT_EQ(VN_OK, Vn_Term());
    EXPECT_EQ(VN_REMOTE_STOPPED, Status().state);
    EXPECT_EQ(0u, Status().tcpPort);
}

TEST(RuntimeRemote, WrongThreadRejectedAndInvalidConfigDoesNotAffectAudio) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto config = RemoteConfig();
    config.tcpPort = 65536;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RemoteStart(&config));
    EXPECT_EQ(VN_REMOTE_FAILED, Status().state);
    EXPECT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(VN_OK, Vn_RemoteStop());
    config.tcpPort = 0;
    VnResult otherResult = VN_OK;
    std::thread other([&] { otherResult = Vn_RemoteStart(&config); });
    other.join();
    EXPECT_EQ(VN_MONITOR_WRONG_THREAD, otherResult);
    EXPECT_EQ(VN_REMOTE_STOPPED, Status().state);
}

#if defined(_WIN32)

class Client {
  public:
    SOCKET socket = INVALID_SOCKET;
    std::vector<std::uint8_t> received;
    ~Client() { Close(); }
    void Close() {
        if (socket != INVALID_SOCKET)
            closesocket(socket);
        socket = INVALID_SOCKET;
    }
    bool Connect(std::uint32_t port) {
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (::connect(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
            return false;
        u_long nonblocking = 1;
        return ioctlsocket(socket, FIONBIO, &nonblocking) == 0;
    }
    bool Send(const Frame &frame) { return SendBytes(EncodeFrame(frame)); }
    bool SendBytes(const std::vector<std::uint8_t> &bytes) {
        return send(socket, reinterpret_cast<const char *>(bytes.data()),
                    static_cast<int>(bytes.size()), 0) == static_cast<int>(bytes.size());
    }
    bool Read(Frame &frame) {
        std::array<std::uint8_t, 65536> buffer{};
        const int size = recv(socket, reinterpret_cast<char *>(buffer.data()),
                              static_cast<int>(buffer.size()), 0);
        if (size > 0)
            received.insert(received.end(), buffer.begin(), buffer.begin() + size);
        Header header;
        if (received.size() < kHeaderBytes ||
            !DecodeHeader(received.data(), received.size(), header) ||
            received.size() < kHeaderBytes + header.payloadBytes)
            return false;
        frame = {header.type,
                 header.requestId,
                 {received.begin() + kHeaderBytes,
                  received.begin() + kHeaderBytes + header.payloadBytes}};
        received.erase(received.begin(), received.begin() + kHeaderBytes + header.payloadBytes);
        return true;
    }
    bool Welcome(Identity &identity, std::uint32_t major = kProtocolMajor) {
        Hello hello;
        hello.major = major;
        hello.clientName = "Runtime tests";
        if (!Send(MakeFrame(Message::Hello, 1, hello)))
            return false;
        Frame frame;
        return WaitUntil([&] { return Read(frame); }, false) && frame.type == Message::Welcome &&
               DecodePayload(frame, identity);
    }
};

TEST(RuntimeRemote, TcpHandshakeSnapshotAndCommandsUseLogicPump) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto config = RemoteConfig();
    ASSERT_EQ(VN_OK, Vn_RemoteStart(&config));
    Client client;
    ASSERT_TRUE(client.Connect(Status().tcpPort));
    Identity identity;
    ASSERT_TRUE(client.Welcome(identity));
    ASSERT_EQ(VN_OK, identity.result);
    EXPECT_FALSE(harken::monitor::DrainRunningForTests());
    EXPECT_FALSE(harken::monitor::CaptureResourcesAllocatedForTests());
    EXPECT_EQ(Status().instanceId, identity.instanceId);
    EXPECT_NE(0u, identity.sessionId);
    Command command;
    command.operation = Operation::SetBusVolume;
    command.target = VN_BUS_MASTER;
    command.value = -6;
    ASSERT_TRUE(client.Send(MakeFrame(Message::Command, 2, command)));
    Frame frame;
    EXPECT_FALSE(WaitUntil([&] { return client.Read(frame); }, false, 100));
    EXPECT_EQ(0u, Status().receivedCommands);
    ASSERT_TRUE(WaitUntil([&] { return client.Read(frame); }));
    ASSERT_EQ(Message::CommandResult, frame.type);
    CommandResult result;
    ASSERT_TRUE(DecodePayload(frame, result));
    EXPECT_EQ(VN_OK, result.result);
    EXPECT_EQ(1u, Status().receivedCommands);
    ASSERT_TRUE(client.Send({Message::SnapshotRequest, 3, {}}));
    ASSERT_TRUE(WaitUntil([&] { return client.Read(frame); }));
    Snapshot snapshot;
    ASSERT_TRUE(DecodePayload(frame, snapshot));
    EXPECT_EQ(VN_OK, snapshot.result);
    ASSERT_TRUE(client.Send({Message::CatalogRequest, 4, {}}));
    ASSERT_TRUE(WaitUntil([&] { return client.Read(frame); }));
    Catalog catalog;
    ASSERT_TRUE(DecodePayload(frame, catalog));
    EXPECT_EQ(VN_OK, catalog.result);
    EXPECT_EQ(0u, catalog.offset);
    command.operation = Operation::StopAll;
    command.expectedGeneration = catalog.generation + 1;
    ASSERT_TRUE(client.Send(MakeFrame(Message::Command, 5, command)));
    ASSERT_TRUE(WaitUntil([&] { return client.Read(frame); }));
    ASSERT_TRUE(DecodePayload(frame, result));
    EXPECT_EQ(VN_STALE_HANDLE, result.result);
}

TEST(RuntimeRemote, EventCaptureDeliversGameOperationsAndSealedTail) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto config = RemoteConfig();
    ASSERT_EQ(VN_OK, Vn_RemoteStart(&config));
    Client client;
    ASSERT_TRUE(client.Connect(Status().tcpPort));
    Identity identity;
    ASSERT_TRUE(client.Welcome(identity));
    ASSERT_NE(0u, identity.capabilities & kCapabilityEventCapture);
    ASSERT_TRUE(client.Send({Message::EventStart, 2, {}}));
    Frame frame;
    ASSERT_TRUE(WaitUntil([&] { return client.Read(frame); }));
    ASSERT_EQ(Message::EventStatus, frame.type);
    EventCaptureStatus started;
    ASSERT_TRUE(DecodePayload(frame, started));
    ASSERT_EQ(VN_OK, started.result);
    ASSERT_NE(0u, started.sessionEpoch);
    ASSERT_EQ(VN_MONITOR_EVENT_SESSION_LIVE, started.state);
    ASSERT_EQ(VN_OK, Vn_StopAll(17));
    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = harkentest::ObjectId(551991);
    VnActionFiringHandle firing = 0;
    const auto refused = Vn_PostAction(&post, &firing);
    ASSERT_NE(VN_OK, refused);
    const auto diagnosticTime = harken::LogTimestampNs();
    engine.Engine()->logicLogRing.Push({VN_LOG_WARN, harken::kLogCodePackClosureRefused,
                                        131, 1, diagnosticTime});
    Writer seal;
    seal.U64(started.sessionEpoch);
    ASSERT_TRUE(client.Send({Message::EventSeal, 3, std::move(seal.bytes)}));
    bool stopped = false, sawStop = false, sawFailure = false, sawWarning = false;
    ASSERT_TRUE(WaitUntil([&] {
        while (client.Read(frame)) {
            if (frame.type == Message::EventStatus) {
                EventCaptureStatus status;
                EXPECT_TRUE(DecodePayload(frame, status));
                EXPECT_EQ(VN_OK, status.result);
                stopped |= status.state == VN_MONITOR_EVENT_SESSION_DRAINED;
            } else if (frame.type == Message::EventRecords) {
                EXPECT_FALSE(stopped);
                MonitorRecords records;
                EXPECT_TRUE(DecodePayload(frame, records));
                EXPECT_EQ(started.sessionEpoch, records.sessionEpoch);
                for (std::size_t offset = 0; offset < records.bytes.size();) {
                    Reader header(records.bytes.data() + offset, records.bytes.size() - offset);
                    std::uint32_t kind = 0, version = 0, flags = 0, stream = 0;
                    std::uint64_t length = 0;
                    if (!header.U32(kind) || !header.U32(version) || !header.U32(flags) ||
                        !header.U32(stream) || !header.U64(length) || length == 0 ||
                        length > records.bytes.size() - offset) return false;
                    if (kind == VN_MONITOR_RECORD_EVENT_DETAIL) {
                        VnMonitorEventDetail detail{};
                        std::memcpy(&detail, records.bytes.data() + offset +
                            VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES, sizeof(detail));
                        sawStop |= detail.category == VN_MONITOR_EVENT_CATEGORY_OPERATION &&
                            detail.code == VN_MONITOR_OPERATION_STOP_ALL && detail.arg0 == 17;
                        sawFailure |= detail.category == VN_MONITOR_EVENT_CATEGORY_OPERATION &&
                            detail.code == VN_MONITOR_OPERATION_POST_ACTION && detail.result == refused;
                        sawWarning |= detail.category == VN_MONITOR_EVENT_CATEGORY_DIAGNOSTIC &&
                            detail.level == VN_LOG_WARN && detail.timestampNs == diagnosticTime && detail.arg0 == 131;
                    }
                    offset += length;
                }
            }
        }
        return stopped;
    }));
    EXPECT_TRUE(sawStop);
    EXPECT_TRUE(sawFailure);
    EXPECT_TRUE(sawWarning);
    EXPECT_EQ(VN_REMOTE_CONNECTED, Status().state);
    EXPECT_EQ(0u, Status().receivedCommands);
}

TEST(RuntimeRemote, EmptyEventCaptureCanRestartImmediatelyAfterSeal) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto config = RemoteConfig();
    ASSERT_EQ(VN_OK, Vn_RemoteStart(&config));
    Client client;
    ASSERT_TRUE(client.Connect(Status().tcpPort));
    Identity identity;
    ASSERT_TRUE(client.Welcome(identity));
    ASSERT_TRUE(client.Send({Message::EventStart, 2, {}}));
    Frame frame;
    ASSERT_TRUE(WaitUntil([&] { return client.Read(frame) && frame.requestId == 2; }));
    EventCaptureStatus started;
    ASSERT_TRUE(DecodePayload(frame, started));
    ASSERT_EQ(VN_OK, started.result);
    ASSERT_EQ(VN_MONITOR_EVENT_SESSION_LIVE, started.state);

    Writer seal;
    seal.U64(started.sessionEpoch);
    auto requests = EncodeFrame({Message::EventSeal, 3, std::move(seal.bytes)});
    const auto restart = EncodeFrame({Message::EventStart, 4, {}});
    requests.insert(requests.end(), restart.begin(), restart.end());
    ASSERT_TRUE(client.SendBytes(requests));
    ASSERT_TRUE(WaitUntil([&] { return client.Read(frame) && frame.requestId == 3; }));
    EventCaptureStatus stopped;
    ASSERT_TRUE(DecodePayload(frame, stopped));
    ASSERT_EQ(VN_OK, stopped.result);
    ASSERT_EQ(VN_MONITOR_EVENT_SESSION_DRAINED, stopped.state);
    ASSERT_TRUE(WaitUntil([&] { return client.Read(frame) && frame.requestId == 4; }));
    EventCaptureStatus restarted;
    ASSERT_TRUE(DecodePayload(frame, restarted));
    ASSERT_EQ(VN_OK, restarted.result);
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_LIVE, restarted.state);
    EXPECT_NE(started.sessionEpoch, restarted.sessionEpoch);
}

TEST(RuntimeRemote, MissingClientCapabilityDoesNotNegotiateEventCapture) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto config = RemoteConfig();
    ASSERT_EQ(VN_OK, Vn_RemoteStart(&config));
    Client client;
    ASSERT_TRUE(client.Connect(Status().tcpPort));
    Hello hello;
    hello.capabilities &= ~kCapabilityEventCapture;
    ASSERT_TRUE(client.Send(MakeFrame(Message::Hello, 1, hello)));
    Frame frame;
    ASSERT_TRUE(WaitUntil([&] { return client.Read(frame); }));
    Identity identity;
    ASSERT_TRUE(DecodePayload(frame, identity));
    EXPECT_EQ(0u, identity.capabilities & kCapabilityEventCapture);
    EXPECT_NE(0u, identity.capabilities & kCapabilitySnapshot);
}

TEST(RuntimeRemote, VersionMismatchAndMalformedPeerDoNotBreakNextConnection) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto config = RemoteConfig();
    ASSERT_EQ(VN_OK, Vn_RemoteStart(&config));
    Client wrong;
    ASSERT_TRUE(wrong.Connect(Status().tcpPort));
    Identity identity;
    ASSERT_TRUE(wrong.Welcome(identity, kProtocolMajor + 1));
    EXPECT_EQ(VN_MONITOR_INCOMPATIBLE, identity.result);
    wrong.Close();
    ASSERT_TRUE(WaitUntil([&] { return Status().clientCount == 0; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Client malformed;
    ASSERT_TRUE(malformed.Connect(Status().tcpPort));
    ASSERT_TRUE(malformed.Welcome(identity));
    const auto firstEpoch = identity.sessionId;
    Writer invalid;
    invalid.U32(kMagic);
    invalid.U32(kMaxPayloadBytes + 1);
    invalid.U16(20);
    invalid.U16(0);
    invalid.U32(2);
    ASSERT_TRUE(malformed.SendBytes(invalid.bytes));
    ASSERT_TRUE(WaitUntil([&] { return Status().clientCount == 0; }));
    Client next;
    ASSERT_TRUE(next.Connect(Status().tcpPort));
    ASSERT_TRUE(next.Welcome(identity));
    EXPECT_NE(firstEpoch, identity.sessionId);
    EXPECT_EQ(VN_OK, Vn_Update());
}

TEST(RuntimeRemote, QueuedOldSessionCommandNeverRunsAfterReconnect) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto config = RemoteConfig();
    ASSERT_EQ(VN_OK, Vn_RemoteStart(&config));
    Client first;
    ASSERT_TRUE(first.Connect(Status().tcpPort));
    Identity identity;
    ASSERT_TRUE(first.Welcome(identity));
    Command command;
    command.operation = Operation::SetBusVolume;
    command.target = VN_BUS_MASTER;
    command.value = -12;
    ASSERT_TRUE(first.Send(MakeFrame(Message::Command, 2, command)));
    std::this_thread::sleep_for(std::chrono::milliseconds(75));
    first.Close();
    ASSERT_TRUE(WaitUntil([&] { return Status().clientCount == 0; }, false));
    Client next;
    ASSERT_TRUE(next.Connect(Status().tcpPort));
    ASSERT_TRUE(next.Welcome(identity));
    EXPECT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, Status().receivedCommands);
}

TEST(RuntimeRemote, InboundQueueOverflowDiscardsCommandsAndAllowsReconnect) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto config = RemoteConfig();
    ASSERT_EQ(VN_OK, Vn_RemoteStart(&config));
    const auto port = Status().tcpPort;
    const auto master = session.Engine()->buses.FindIndex(VN_BUS_MASTER);
    ASSERT_NE(harken::kInvalidBusIndex, master);
    const auto initialVolume =
        session.Engine()->busTargets.Reduce(master, harken::kBusTargetBusVolume);
    Client first;
    ASSERT_TRUE(first.Connect(port));
    Identity identity;
    ASSERT_TRUE(first.Welcome(identity));
    ASSERT_EQ(VN_OK, identity.result);
    const auto firstEpoch = identity.sessionId;
    Command command;
    command.operation = Operation::SetBusVolume;
    command.target = VN_BUS_MASTER;
    command.value = -12;
    constexpr std::uint32_t queuedCommands = 64;
    for (std::uint32_t i = 0; i < queuedCommands; ++i)
        ASSERT_TRUE(first.Send(MakeFrame(Message::Command, i + 2, command)));
    ASSERT_TRUE(first.Send({Message::Ping, queuedCommands + 2, {}}));
    Frame frame;
    ASSERT_TRUE(WaitUntil([&] { return first.Read(frame); }, false));
    ASSERT_EQ(Message::Pong, frame.type);
    ASSERT_EQ(queuedCommands + 2, frame.requestId);
    ASSERT_EQ(1u, Status().clientCount);
    EXPECT_EQ(0u, Status().receivedCommands);
    ASSERT_TRUE(first.Send(MakeFrame(Message::Command, queuedCommands + 3, command)));
    ASSERT_TRUE(WaitUntil([&] { return Status().clientCount == 0; }, false));
    EXPECT_EQ(VN_REMOTE_LISTENING, Status().state);
    EXPECT_EQ(0u, Status().sessionId);
    ASSERT_EQ(VN_OK, Vn_Update());
    harkentest::Capture captured;
    harkentest::PumpAndCapture(session.Engine(), 1, captured);
    EXPECT_FALSE(captured.left.empty());
    EXPECT_EQ(0u, Status().receivedCommands);
    EXPECT_FLOAT_EQ(initialVolume,
                    session.Engine()->busTargets.Reduce(master, harken::kBusTargetBusVolume));
    first.Close();
    Client next;
    ASSERT_TRUE(next.Connect(port));
    ASSERT_TRUE(next.Welcome(identity));
    ASSERT_EQ(VN_OK, identity.result);
    EXPECT_NE(firstEpoch, identity.sessionId);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, Status().receivedCommands);
    command.value = -6;
    ASSERT_TRUE(next.Send(MakeFrame(Message::Command, 2, command)));
    ASSERT_TRUE(WaitUntil([&] { return next.Read(frame); }));
    ASSERT_EQ(Message::CommandResult, frame.type);
    CommandResult result;
    ASSERT_TRUE(DecodePayload(frame, result));
    EXPECT_EQ(VN_OK, result.result);
    EXPECT_EQ(1u, Status().receivedCommands);
    EXPECT_FLOAT_EQ(-6.0f,
                    session.Engine()->busTargets.Reduce(master, harken::kBusTargetBusVolume));
}

TEST(RuntimeRemote, PortConflictFailsIndependentlyAndCanRetry) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto config = RemoteConfig();
    ASSERT_EQ(VN_OK, Vn_RemoteStart(&config));
    const auto port = Status().tcpPort;
    WSADATA network{};
    ASSERT_EQ(0, WSAStartup(MAKEWORD(2, 2), &network));
    ASSERT_EQ(VN_OK, Vn_RemoteStop());
    SOCKET occupied = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    ASSERT_EQ(0, bind(occupied, reinterpret_cast<const sockaddr *>(&address), sizeof(address)));
    config.tcpPort = port;
    EXPECT_EQ(VN_DEVICE_ERROR, Vn_RemoteStart(&config));
    EXPECT_EQ(VN_REMOTE_FAILED, Status().state);
    EXPECT_EQ(VN_DEVICE_ERROR, Status().lastResult);
    EXPECT_EQ(VN_OK, Vn_Update());
    closesocket(occupied);
    WSACleanup();
    EXPECT_EQ(VN_OK, Vn_RemoteStart(&config));
    EXPECT_EQ(VN_REMOTE_LISTENING, Status().state);
}

TEST(RuntimeRemote, RemoteCaptureEmitsActualPackedRecordsAndStops) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto config = RemoteConfig();
    ASSERT_EQ(VN_OK, Vn_RemoteStart(&config));
    Client client;
    ASSERT_TRUE(client.Connect(Status().tcpPort));
    Identity identity;
    ASSERT_TRUE(client.Welcome(identity));
    MonitorRequest request;
    request.streamMask = VN_MONITOR_STREAM_TIME_SERIES;
    ASSERT_TRUE(client.Send(MakeFrame(Message::MonitorStart, 2, request)));
    Frame frame;
    ASSERT_TRUE(WaitUntil([&] { return client.Read(frame); }));
    ASSERT_EQ(Message::MonitorStatus, frame.type);
    MonitorStatus monitor;
    ASSERT_TRUE(DecodePayload(frame, monitor));
    ASSERT_EQ(VN_OK, monitor.result);
    EXPECT_NE(0u, monitor.sessionEpoch);
    MonitorRecords records;
    ASSERT_TRUE(WaitUntil([&] {
        harkentest::Capture captured;
        harkentest::PumpAndCapture(session.Engine(), 1, captured);
        if (!client.Read(frame) || frame.type != Message::MonitorRecords)
            return false;
        return DecodePayload(frame, records) && records.recordCount > 0;
    }));
    EXPECT_EQ(monitor.sessionEpoch, records.sessionEpoch);
    EXPECT_GE(records.bytes.size(),
              static_cast<std::size_t>(VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES));
    ASSERT_TRUE(client.Send({Message::MonitorStop, 3, {}}));
    ASSERT_TRUE(WaitUntil([&] {
        return client.Read(frame) && frame.type == Message::MonitorStatus && frame.requestId == 3;
    }));
    ASSERT_TRUE(DecodePayload(frame, monitor));
    EXPECT_EQ(VN_OK, monitor.result);
    EXPECT_EQ(0u, monitor.sessionEpoch);
}

TEST(RuntimeRemote, DuplicateCommandIsRejectedBeforeSecondExecution) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto config = RemoteConfig();
    ASSERT_EQ(VN_OK, Vn_RemoteStart(&config));
    Client client;
    ASSERT_TRUE(client.Connect(Status().tcpPort));
    Identity identity;
    ASSERT_TRUE(client.Welcome(identity));
    Command command;
    command.operation = Operation::StopAll;
    const auto request = MakeFrame(Message::Command, 2, command);
    ASSERT_TRUE(client.Send(request));
    Frame frame;
    ASSERT_TRUE(WaitUntil([&] { return client.Read(frame); }));
    ASSERT_EQ(Message::CommandResult, frame.type);
    ASSERT_TRUE(client.Send(request));
    ASSERT_TRUE(WaitUntil([&] { return Status().clientCount == 0; }));
    EXPECT_EQ(1u, Status().receivedCommands);
    EXPECT_EQ(VN_OK, Vn_Update());
}

TEST(RuntimeRemote, IncompleteFrameTimeoutDisconnectsAndLeavesEngineRunning) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto config = RemoteConfig();
    config.requestTimeoutMs = 150;
    ASSERT_EQ(VN_OK, Vn_RemoteStart(&config));
    Client client;
    ASSERT_TRUE(client.Connect(Status().tcpPort));
    Identity identity;
    ASSERT_TRUE(client.Welcome(identity));
    auto partial = EncodeFrame({Message::SnapshotRequest, 2, {}});
    partial.resize(8);
    ASSERT_TRUE(client.SendBytes(partial));
    ASSERT_TRUE(WaitUntil([&] { return Status().clientCount == 0; }));
    EXPECT_EQ(VN_REMOTE_LISTENING, Status().state);
    EXPECT_EQ(VN_OK, Vn_Update());
}

TEST(RuntimeRemote, MulticastDiscoveryReturnsAuthoritativeEndpointIdentity) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto config = RemoteConfig();
    config.bindAddress = "0.0.0.0";
    config.flags = VN_REMOTE_DISCOVERY_ENABLED;
    ASSERT_EQ(VN_OK, Vn_RemoteStart(&config));
    const auto running = Status();
    SOCKET query = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_NE(INVALID_SOCKET, query);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &local.sin_addr);
    ASSERT_EQ(0, bind(query, reinterpret_cast<const sockaddr *>(&local), sizeof(local)));
    ASSERT_EQ(0,
              setsockopt(query, IPPROTO_IP, IP_MULTICAST_IF,
                         reinterpret_cast<const char *>(&local.sin_addr), sizeof(local.sin_addr)));
    u_long nonblocking = 1;
    ASSERT_EQ(0, ioctlsocket(query, FIONBIO, &nonblocking));
    int localBytes = sizeof(local);
    ASSERT_EQ(0, getsockname(query, reinterpret_cast<sockaddr *>(&local), &localBytes));
    Identity foreign;
    foreign.instanceId = running.instanceId ^ 1u;
    foreign.tcpPort = running.tcpPort;
    const auto foreignBytes = EncodeFrame(MakeFrame(Message::Announcement, 123, foreign));
    ASSERT_EQ(static_cast<int>(foreignBytes.size()),
              sendto(query, reinterpret_cast<const char *>(foreignBytes.data()),
                     static_cast<int>(foreignBytes.size()), 0,
                     reinterpret_cast<const sockaddr *>(&local), sizeof(local)));
    sockaddr_in group{};
    group.sin_family = AF_INET;
    group.sin_port = htons(kDiscoveryPort);
    inet_pton(AF_INET, kDiscoveryGroup, &group.sin_addr);
    const auto bytes = EncodeFrame({Message::Discover, 123, {}});
    ASSERT_EQ(static_cast<int>(bytes.size()),
              sendto(query, reinterpret_cast<const char *>(bytes.data()),
                     static_cast<int>(bytes.size()), 0, reinterpret_cast<const sockaddr *>(&group),
                     sizeof(group)));
    std::array<std::uint8_t, 2048> reply{};
    int count = 0;
    Header header;
    Identity identity;
    const bool received = WaitUntil([&] {
        count = recvfrom(query, reinterpret_cast<char *>(reply.data()),
                         static_cast<int>(reply.size()), 0, nullptr, nullptr);
        if (count <= 0 || !DecodeHeader(reply.data(), count, header) ||
            header.type != Message::Announcement || header.requestId != 123u)
            return false;
        Reader reader(reply.data() + kHeaderBytes, count - kHeaderBytes);
        return Decode(reader, identity) && identity.instanceId == running.instanceId;
    });
    closesocket(query);
    ASSERT_TRUE(received);
    EXPECT_EQ(Message::Announcement, header.type);
    EXPECT_EQ(123u, header.requestId);
    EXPECT_EQ(running.instanceId, identity.instanceId);
    EXPECT_EQ(running.tcpPort, identity.tcpPort);
    EXPECT_EQ(0u, identity.sessionId);
    EXPECT_EQ("0.0.0.0", identity.bindAddress);
}

TEST(RuntimeRemote, SecondControllerReceivesBusyWithoutDisturbingFirst) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto config = RemoteConfig();
    ASSERT_EQ(VN_OK, Vn_RemoteStart(&config));
    Client first;
    ASSERT_TRUE(first.Connect(Status().tcpPort));
    Identity identity;
    ASSERT_TRUE(first.Welcome(identity));
    Client second;
    ASSERT_TRUE(second.Connect(Status().tcpPort));
    Hello hello;
    hello.clientName = "Second Studio";
    ASSERT_TRUE(second.Send(MakeFrame(Message::Hello, 1, hello)));
    Frame frame;
    ASSERT_TRUE(WaitUntil([&] { return second.Read(frame); }));
    EXPECT_EQ(Message::Error, frame.type);
    EXPECT_EQ(0u, frame.requestId);
    Reader reader(frame.payload);
    std::int32_t result = 0;
    ASSERT_TRUE(reader.I32(result));
    EXPECT_EQ(VN_IN_USE, result);
    EXPECT_EQ(identity.sessionId, Status().sessionId);
    ASSERT_TRUE(first.Send({Message::SnapshotRequest, 2, {}}));
    ASSERT_TRUE(WaitUntil([&] { return first.Read(frame); }));
    EXPECT_EQ(Message::Snapshot, frame.type);
}

TEST(RuntimeRemote, ImmediateReconnectProcessesOldFinBeforeNewHello) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto config = RemoteConfig();
    ASSERT_EQ(VN_OK, Vn_RemoteStart(&config));
    std::uint64_t previousEpoch = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        Client client;
        ASSERT_TRUE(client.Connect(Status().tcpPort));
        Identity identity;
        ASSERT_TRUE(client.Welcome(identity));
        ASSERT_EQ(VN_OK, identity.result);
        EXPECT_GT(identity.sessionId, previousEpoch);
        previousEpoch = identity.sessionId;
        Command command;
        command.operation = Operation::SetBusVolume;
        command.target = VN_BUS_MASTER;
        command.value = -12;
        ASSERT_TRUE(client.Send(MakeFrame(Message::Command, 2, command)));
        client.Close();
    }
    ASSERT_TRUE(WaitUntil([&] { return Status().clientCount == 0; }, false));
    EXPECT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, Status().receivedCommands);
}

#endif
}
