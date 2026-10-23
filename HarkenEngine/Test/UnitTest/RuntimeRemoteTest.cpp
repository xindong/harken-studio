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
#include "MonitorCaptureHub.h"
#include "MonitorIdentityCut.h"
#include "harken_remote/Protocol.h"
#include "../../Source/Runtime/remote/src/RemoteRuntime.h"
#include <gtest/gtest.h>
#include <harken/Harken.h>

#include <algorithm>
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

TEST(RuntimeRemoteProtocol, ExecutionObservationHasIndependentStrictFrames) {
    EventCaptureRequestV2 request;
    request.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT;
    request.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
    auto frame = MakeFrame(Message::EventStartV2, 1, request);
    ASSERT_EQ(24u, frame.payload.size());
    EventCaptureRequestV2 decoded;
    EXPECT_TRUE(DecodePayload(frame, decoded));
    frame.payload.push_back(0);
    EXPECT_FALSE(DecodePayload(frame, decoded));
    frame.payload.resize(23);
    EXPECT_FALSE(DecodePayload(frame, decoded));
    request.requestSymbols = 2;
    EXPECT_FALSE(DecodePayload(MakeFrame(Message::EventStartV2, 1, request), decoded));

    EventCaptureStatusV2 status;
    status.acceptedExecutionFactVersion = 1;
    status.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS | VN_MONITOR_PERM_RECORD;
    status.acceptedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
    status.deniedPermissionMask = VN_MONITOR_PERM_RECORD;
    status.supportedFactKinds[1] = 1ull << 48;
    status.identityUnavailableCount = 7;
    frame = MakeFrame(Message::EventStatusV2, 1, status);
    ASSERT_EQ(128u, frame.payload.size());
    EventCaptureStatusV2 result;
    ASSERT_TRUE(DecodePayload(frame, result));
    EXPECT_EQ(status.supportedFactKinds[1], result.supportedFactKinds[1]);
    EXPECT_EQ(7u, result.identityUnavailableCount);
    EventCaptureStatus legacy;
    EXPECT_FALSE(DecodePayload(frame, legacy));
    const auto oldFrame = MakeFrame(Message::EventStatus, 1, status.base);
    EXPECT_EQ(56u, oldFrame.payload.size());
    EXPECT_TRUE(DecodePayload(oldFrame, legacy));
    EXPECT_FALSE(DecodePayload(oldFrame, result));
    status.reserved0 = 1;
    EXPECT_FALSE(DecodePayload(MakeFrame(Message::EventStatusV2, 1, status), result));
    status.reserved0 = 0;
    status.deniedPermissionMask |= VN_MONITOR_PERM_OBSERVE_EVENTS;
    EXPECT_FALSE(DecodePayload(MakeFrame(Message::EventStatusV2, 1, status), result));
}

TEST(RuntimeRemote, RestrictedExecutionObservationGrantDoesNotTrustRequest) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionDescV2 desc{};
    desc.structSize = sizeof(desc);
    desc.requestedExecutionFactVersion = 1;
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT | VN_MONITOR_STREAM_STATE;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS | VN_MONITOR_PERM_OBSERVE_STATE |
        VN_MONITOR_PERM_RECORD | VN_MONITOR_PERM_CAPTURE_CONTROL | VN_MONITOR_PERM_RAW_AUDIO;
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, harken::monitor::OpenEventSession(
        reinterpret_cast<const VnMonitorSessionDesc*>(&desc), &handle, true, 123,
        VN_MONITOR_PERM_OBSERVE_EVENTS));
    VnMonitorEventSessionStatusV2 status{};
    status.structSize = sizeof(status);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEventSessionStatus(handle,
        reinterpret_cast<VnMonitorEventSessionStatus*>(&status)));
    EXPECT_EQ(VN_MONITOR_PERM_OBSERVE_EVENTS, status.acceptedPermissionMask);
    EXPECT_EQ(desc.requestedPermissionMask & ~VN_MONITOR_PERM_OBSERVE_EVENTS, status.deniedPermissionMask);
    EXPECT_EQ(VN_MONITOR_STREAM_EVENT, status.acceptedStreamKindMask);
    EXPECT_EQ(VN_MONITOR_OBSERVATION_DENIED, status.baselineAvailability);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
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
    for (const auto* address : {"0.0.0.0", "192.168.1.20", "10.0.0.20", "::", "game.local"}) {
        SCOPED_TRACE(address);
        config.bindAddress = address;
        EXPECT_EQ(VN_INVALID_ARG, Vn_RemoteStart(&config));
        EXPECT_EQ(0u, Status().tcpPort);
        EXPECT_EQ(0u, Status().discoveryPort);
        EXPECT_EQ(VN_OK, Vn_Update());
        EXPECT_EQ(VN_OK, Vn_RemoteStop());
    }
    config.bindAddress = "127.0.0.1";
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

struct ExecutionCapture {
    struct Symbol { VnMonitorRecordHeader header; VnMonitorHostObjectSymbolV1 value; };
    struct IdentityRow { VnMonitorRecordHeader header; VnMonitorIdentityFactV1 value; };
    std::uint64_t epoch = 0, nextDelivery = 1;
    harken::monitor::MonitorIdentityCutVerifier cut;
    std::vector<Symbol> symbols;
    std::vector<IdentityRow> identities;
    std::vector<VnMonitorRecordHeader> facts;
    EventCaptureStatusV2 final;
    bool drained = false;

    void Receive(const Frame& frame) {
        using namespace harken::monitor;
        if (frame.type == Message::EventStatusV2) {
            EventCaptureStatusV2 status;
            ASSERT_TRUE(DecodePayload(frame, status));
            EXPECT_EQ(VN_OK, status.base.result);
            EXPECT_EQ(epoch, status.base.sessionEpoch);
            if (status.base.state == VN_MONITOR_EVENT_SESSION_DRAINED) {
                drained = true;
                final = status;
            }
            return;
        }
        if (frame.type != Message::EventRecords) return;
        EXPECT_FALSE(drained);
        MonitorRecords records;
        ASSERT_TRUE(DecodePayload(frame, records));
        EXPECT_EQ(epoch, records.sessionEpoch);
        std::uint32_t count = 0;
        for (std::size_t offset = 0; offset < records.bytes.size(); ++count) {
            const auto available = records.bytes.size() - offset;
            ASSERT_GE(available, kPackedEnvelopeBytes);
            const auto* bytes = records.bytes.data() + offset;
            ExecutionPayloadReader length{bytes + 16};
            std::uint64_t size = 0;
            length(size);
            ASSERT_GE(size, kPackedEnvelopeBytes);
            ASSERT_LE(size, available);
            VnMonitorRecordHeader header{};
            ASSERT_TRUE(ReadIdentityHeader(bytes, size, header));
            EXPECT_EQ(epoch, header.monitorSessionEpoch);
            EXPECT_EQ(nextDelivery++, header.deliverySequence);
            const auto* payload = bytes + kPackedEnvelopeBytes;
            switch (header.recordKind) {
            case VN_MONITOR_RECORD_IDENTITY_CUT_BEGIN: {
                VnMonitorIdentityCutBeginV1 value{};
                ASSERT_TRUE(DecodeIdentityBegin(header, payload, header.payloadBytes, value));
                EXPECT_TRUE(cut.Begin(header, value));
                break;
            }
            case VN_MONITOR_RECORD_IDENTITY_FACT: {
                VnMonitorIdentityFactV1 value{};
                ASSERT_TRUE(DecodeIdentityFact(header, payload, header.payloadBytes, value));
                if (value.stateCutId != 0) EXPECT_TRUE(cut.Row(header, value));
                identities.push_back({header, value});
                break;
            }
            case VN_MONITOR_RECORD_HOST_OBJECT_SYMBOL: {
                VnMonitorHostObjectSymbolV1 value{};
                ASSERT_TRUE(DecodeHostSymbol(header, payload, header.payloadBytes, value));
                if (value.stateCutId != 0) EXPECT_TRUE(cut.Row(header, value));
                symbols.push_back({header, value});
                break;
            }
            case VN_MONITOR_RECORD_IDENTITY_CUT_END: {
                VnMonitorIdentityCutEndV1 value{};
                ASSERT_TRUE(DecodeIdentityEnd(header, payload, header.payloadBytes, value));
                EXPECT_TRUE(cut.End(header, value));
                break;
            }
            case VN_MONITOR_RECORD_EXECUTION_FACT: {
                VnMonitorExecutionFactV1 value{};
                EXPECT_TRUE(DecodeExecutionFact(header, payload, header.payloadBytes, value));
                facts.push_back(header);
                break;
            }
            case VN_MONITOR_RECORD_DROP_SUMMARY:
                ADD_FAILURE() << "Uncongested socket capture must not lose records";
                break;
            default: break;
            }
            offset += size;
        }
        EXPECT_EQ(records.recordCount, count);
    }
};

VnMonitorHostRegistrationV2 RegisterRemoteHost(VnEmitterHandle emitter, const VnMonitorHostSessionV1& host) {
    VnMonitorHostRegistrationV2 value{};
    value.structSize = sizeof(value);
    value.ownershipKind = VN_MONITOR_HOST_UNITY_EMITTER;
    value.engineInstance = host.engineInstance;
    value.sdkSession = host.sdkSession;
    value.emitter = emitter;
    value.localSequence = 1;
    value.registrationRevision = 1;
    value.nameBytes = 8;
    std::memcpy(value.name, "Original", value.nameBytes);
    EXPECT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&value));
    return value;
}

TEST(RuntimeRemote, ExecutionSocketPreservesFrozenMixedCutDeltasSymbolsAndSealedTail) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorHostSessionV1 host{};
    host.structSize = sizeof(host);
    ASSERT_EQ(VN_OK, Vn_MonitorBeginHostSession(&host));
    VnEmitterHandle original = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&original));
    auto registration = RegisterRemoteHost(original, host);
    const auto config = RemoteConfig();
    ASSERT_EQ(VN_OK, Vn_RemoteStart(&config));
    Client client;
    ASSERT_TRUE(client.Connect(Status().tcpPort));
    Identity identity;
    ASSERT_TRUE(client.Welcome(identity));
    harken::monitor::StopDrainForTests();
    EventCaptureRequestV2 request;
    request.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT | VN_MONITOR_STREAM_STATE;
    request.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS | VN_MONITOR_PERM_OBSERVE_STATE | VN_MONITOR_PERM_SYMBOLS;
    request.requestSymbols = 1;
    ASSERT_TRUE(client.Send(MakeFrame(Message::EventStartV2, 2, request)));
    Frame frame;
    ASSERT_TRUE(WaitUntil([&] { return client.Read(frame) && frame.requestId == 2; }));
    EventCaptureStatusV2 started;
    ASSERT_TRUE(DecodePayload(frame, started));
    ASSERT_EQ(VN_OK, started.base.result);
    ExecutionCapture capture;
    capture.epoch = started.base.sessionEpoch;
    VnMonitorSessionDescV2 localDesc{};
    localDesc.structSize = sizeof(localDesc);
    localDesc.requestedExecutionFactVersion = 1;
    localDesc.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT;
    localDesc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
    localDesc.consumerQueueBytes = 65536;
    VnMonitorSessionHandle local = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenEventSession(reinterpret_cast<const VnMonitorSessionDesc*>(&localDesc), &local));
    harken::monitor::StopDrainForTests();
    ++registration.registrationRevision;
    registration.nameBytes = 7;
    std::memset(registration.name, 0, sizeof(registration.name));
    std::memcpy(registration.name, "Updated", registration.nameBytes);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(original));
    VnEmitterHandle replacement = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&replacement));
    ASSERT_NE(original, replacement);
    registration.emitter = replacement;
    registration.localSequence = 2;
    registration.registrationRevision = 1;
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    Writer seal;
    seal.U64(capture.epoch);
    ASSERT_TRUE(client.Send({Message::EventSeal, 3, std::move(seal.bytes)}));
    ASSERT_TRUE(WaitUntil([&] {
        while (client.Read(frame)) {
            capture.Receive(frame);
            if (frame.requestId == 3) return true;
        }
        return false;
    }));
    EXPECT_FALSE(capture.drained);
    ASSERT_EQ(VN_OK, Vn_MonitorSealEventSession(local));
    harken::monitor::StartDrainForTests();
    ASSERT_TRUE(WaitUntil([&] {
        while (client.Read(frame)) capture.Receive(frame);
        return capture.drained;
    }));
    EXPECT_TRUE(capture.cut.Complete());
    EXPECT_EQ(VN_MONITOR_OBSERVATION_AVAILABLE, capture.final.baselineAvailability);
    EXPECT_EQ(VN_MONITOR_OBSERVATION_AVAILABLE, capture.final.symbolAvailability);
    EXPECT_EQ(0u, capture.final.base.sourceDroppedRecords);
    EXPECT_EQ(0u, capture.final.base.deliveryDroppedRecords);
    EXPECT_EQ(0u, capture.final.base.transportDroppedRecords);
    bool frozenOriginal = false, changedOriginal = false, newObject = false, baselineEmitter = false, retiredEmitter = false;
    for (const auto& symbol : capture.symbols) {
        const auto& value = symbol.value;
        if (value.field != VN_MONITOR_HOST_SYMBOL_NAME) continue;
        const std::string text(reinterpret_cast<const char*>(value.text), value.textBytes);
        frozenOriginal |= value.stateCutId != 0 && symbol.header.instanceHandle == original &&
            value.registrationRevision == 1 && text == "Original";
        changedOriginal |= value.stateCutId == 0 && symbol.header.instanceHandle == original &&
            value.registrationRevision == 2 && value.factKind == VN_MONITOR_SYMBOL_UPSERT && text == "Updated";
        newObject |= value.stateCutId == 0 && symbol.header.instanceHandle == replacement && value.hostSequence == 2;
    }
    for (const auto& row : capture.identities) {
        if (row.value.fact.subjectNamespace != VN_MONITOR_SUBJECT_EMITTER) continue;
        baselineEmitter |= row.value.stateCutId != 0 && row.header.instanceHandle == original;
        retiredEmitter |= row.value.stateCutId == 0 && row.header.instanceHandle == original &&
            row.value.fact.factKind == VN_MONITOR_FACT_IDENTITY_TOMBSTONE;
    }
    EXPECT_TRUE(frozenOriginal);
    EXPECT_TRUE(changedOriginal);
    EXPECT_TRUE(newObject);
    EXPECT_TRUE(baselineEmitter);
    EXPECT_TRUE(retiredEmitter);
    EXPECT_GE(capture.facts.size(), 2u);
    std::array<std::uint8_t, 65536> localBytes{};
    VnMonitorPollDesc poll{sizeof(poll), 0, localBytes.data(), localBytes.size()};
    VnMonitorPollResult result{};
    result.structSize = sizeof(result);
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(local, &poll, &result));
    unsigned sharedFacts = 0;
    for (std::uint64_t offset = 0; offset < result.bytesWritten;) {
        harken::monitor::ExecutionPayloadReader read{localBytes.data() + offset + 16};
        std::uint64_t bytes = 0;
        read(bytes);
        ASSERT_LE(bytes, result.bytesWritten - offset);
        VnMonitorRecordHeader header{};
        ASSERT_TRUE(harken::monitor::ReadIdentityHeader(localBytes.data() + offset, bytes, header));
        if (header.recordKind == VN_MONITOR_RECORD_EXECUTION_FACT) {
            const auto matching = std::find_if(capture.facts.begin(), capture.facts.end(), [&](const auto& remote) {
                return remote.engineMonitorInstanceId == header.engineMonitorInstanceId &&
                    remote.producerId == header.producerId && remote.sourceSequence == header.sourceSequence;
            });
            ASSERT_NE(capture.facts.end(), matching);
            EXPECT_NE(0u, header.monitorSessionEpoch);
            EXPECT_NE(capture.epoch, header.monitorSessionEpoch);
            ++sharedFacts;
        }
        offset += bytes;
    }
    EXPECT_EQ(capture.facts.size(), sharedFacts);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(local));
}

TEST(RuntimeRemote, ExecutionSocketNeverSendsSymbolsWithoutBothRequestAndPermission) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorHostSessionV1 host{};
    host.structSize = sizeof(host);
    ASSERT_EQ(VN_OK, Vn_MonitorBeginHostSession(&host));
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    auto registration = RegisterRemoteHost(emitter, host);
    const auto config = RemoteConfig();
    ASSERT_EQ(VN_OK, Vn_RemoteStart(&config));
    Client client;
    ASSERT_TRUE(client.Connect(Status().tcpPort));
    Identity identity;
    ASSERT_TRUE(client.Welcome(identity));
    std::uint32_t requestId = 2;
    for (const bool requestSymbols : {false, true}) {
        SCOPED_TRACE(requestSymbols);
        EventCaptureRequestV2 request;
        request.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT | VN_MONITOR_STREAM_STATE;
        request.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS | VN_MONITOR_PERM_OBSERVE_STATE |
            (requestSymbols ? 0 : VN_MONITOR_PERM_SYMBOLS);
        request.requestSymbols = requestSymbols ? 1u : 0u;
        const auto startId = requestId++;
        ASSERT_TRUE(client.Send(MakeFrame(Message::EventStartV2, startId, request)));
        Frame frame;
        ASSERT_TRUE(WaitUntil([&] { return client.Read(frame) && frame.requestId == startId; }));
        EventCaptureStatusV2 started;
        ASSERT_TRUE(DecodePayload(frame, started));
        ASSERT_EQ(VN_OK, started.base.result);
        EXPECT_EQ(requestSymbols ? VN_MONITOR_OBSERVATION_DENIED : VN_MONITOR_OBSERVATION_NOT_REQUESTED,
            started.symbolAvailability);
        ExecutionCapture capture;
        capture.epoch = started.base.sessionEpoch;
        ++registration.registrationRevision;
        ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
        Writer seal;
        seal.U64(capture.epoch);
        ASSERT_TRUE(client.Send({Message::EventSeal, requestId++, std::move(seal.bytes)}));
        ASSERT_TRUE(WaitUntil([&] {
            while (client.Read(frame)) capture.Receive(frame);
            return capture.drained;
        }));
        EXPECT_TRUE(capture.cut.Complete());
        EXPECT_TRUE(capture.symbols.empty());
        EXPECT_FALSE(capture.identities.empty());
        EXPECT_EQ(0u, capture.final.base.sourceDroppedRecords);
    }
}

TEST(RuntimeRemote, ExecutionObservationUsesServerWhitelistAndFreshConnectionGrant) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto config = RemoteConfig();
    ASSERT_EQ(VN_OK, Vn_RemoteStart(&config));
    Client client;
    ASSERT_TRUE(client.Connect(Status().tcpPort));
    Identity identity;
    ASSERT_TRUE(client.Welcome(identity));
    ASSERT_NE(0u, identity.capabilities & kCapabilityExecutionObservation);
    EventCaptureRequestV2 request;
    request.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT | VN_MONITOR_STREAM_STATE;
    request.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS | VN_MONITOR_PERM_OBSERVE_STATE |
        VN_MONITOR_PERM_SYMBOLS | VN_MONITOR_PERM_RAW_AUDIO | VN_MONITOR_PERM_CAPTURE_CONTROL |
        VN_MONITOR_PERM_RECORD;
    request.requestSymbols = 1;
    ASSERT_TRUE(client.Send(MakeFrame(Message::EventStartV2, 2, request)));
    Frame frame;
    ASSERT_TRUE(WaitUntil([&] { return client.Read(frame) && frame.requestId == 2; }));
    ASSERT_EQ(Message::EventStatusV2, frame.type);
    EventCaptureStatusV2 started;
    ASSERT_TRUE(DecodePayload(frame, started));
    ASSERT_EQ(VN_OK, started.base.result);
    EXPECT_EQ(1u, started.acceptedExecutionFactVersion);
    constexpr auto forbidden = VN_MONITOR_PERM_RAW_AUDIO | VN_MONITOR_PERM_CAPTURE_CONTROL |
        VN_MONITOR_PERM_RECORD;
    EXPECT_EQ(0u, started.acceptedPermissionMask & forbidden);
    EXPECT_EQ(forbidden, started.deniedPermissionMask & forbidden);
    EXPECT_EQ(VN_MONITOR_PERM_OBSERVE_EVENTS | VN_MONITOR_PERM_OBSERVE_STATE,
              started.acceptedPermissionMask & ~VN_MONITOR_PERM_SYMBOLS);
    EXPECT_EQ(request.requestedStreamKindMask, started.acceptedStreamKindMask);
    Writer seal;
    seal.U64(started.base.sessionEpoch);
    ASSERT_TRUE(client.Send({Message::EventSeal, 3, std::move(seal.bytes)}));
    EventCaptureStatusV2 stopped;
    ASSERT_TRUE(WaitUntil([&] {
        while (client.Read(frame)) {
            if (frame.type == Message::EventStatusV2 && DecodePayload(frame, stopped) &&
                stopped.base.state == VN_MONITOR_EVENT_SESSION_DRAINED) return true;
        }
        return false;
    }));
    EXPECT_EQ(started.base.sessionEpoch, stopped.base.sessionEpoch);

    client.Close();
    ASSERT_TRUE(WaitUntil([] { return Status().state == VN_REMOTE_LISTENING; }));
    Client replacement;
    ASSERT_TRUE(replacement.Connect(Status().tcpPort));
    Identity replacementIdentity;
    ASSERT_TRUE(replacement.Welcome(replacementIdentity));
    EXPECT_NE(identity.sessionId, replacementIdentity.sessionId);
    request.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT;
    request.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
    request.requestSymbols = 0;
    ASSERT_TRUE(replacement.Send(MakeFrame(Message::EventStartV2, 4, request)));
    ASSERT_TRUE(WaitUntil([&] { return replacement.Read(frame) && frame.requestId == 4; }));
    ASSERT_TRUE(DecodePayload(frame, started));
    ASSERT_EQ(VN_OK, started.base.result);
    EXPECT_NE(stopped.base.sessionEpoch, started.base.sessionEpoch);
    EXPECT_EQ(VN_MONITOR_PERM_OBSERVE_EVENTS, started.acceptedPermissionMask);
    EXPECT_EQ(VN_MONITOR_OBSERVATION_NOT_REQUESTED, started.symbolAvailability);
    EXPECT_EQ(VN_MONITOR_OBSERVATION_NOT_REQUESTED, started.baselineAvailability);
    EXPECT_EQ(0u, started.symbolsDecision);
}

TEST(RuntimeRemote, ExecutionObservationRequiresNegotiatedCapability) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto config = RemoteConfig();
    ASSERT_EQ(VN_OK, Vn_RemoteStart(&config));
    Client client;
    ASSERT_TRUE(client.Connect(Status().tcpPort));
    Hello hello;
    hello.capabilities &= ~kCapabilityExecutionObservation;
    ASSERT_TRUE(client.Send(MakeFrame(Message::Hello, 1, hello)));
    Frame frame;
    ASSERT_TRUE(WaitUntil([&] { return client.Read(frame); }, false));
    Identity identity;
    ASSERT_TRUE(DecodePayload(frame, identity));
    EXPECT_EQ(0u, identity.capabilities & kCapabilityExecutionObservation);
    EventCaptureRequestV2 request;
    request.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT;
    request.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
    ASSERT_TRUE(client.Send(MakeFrame(Message::EventStartV2, 2, request)));
    ASSERT_TRUE(WaitUntil([&] { return client.Read(frame); }));
    ASSERT_EQ(Message::EventStatusV2, frame.type);
    EventCaptureStatusV2 status;
    ASSERT_TRUE(DecodePayload(frame, status));
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED, status.base.result);
    EXPECT_EQ(0u, status.base.sessionEpoch);
    ASSERT_TRUE(client.Send({Message::EventStart, 3, {}}));
    ASSERT_TRUE(WaitUntil([&] { return client.Read(frame); }));
    ASSERT_EQ(Message::EventStatus, frame.type);
    EventCaptureStatus oldStatus;
    ASSERT_TRUE(DecodePayload(frame, oldStatus));
    EXPECT_EQ(VN_OK, oldStatus.result);
}

TEST(RuntimeRemote, ReconnectBetweenGrantSnapshotAndDispatchKeepsNewRequestQueued) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto config = RemoteConfig();
    ASSERT_EQ(VN_OK, Vn_RemoteStart(&config));
    Client original;
    ASSERT_TRUE(original.Connect(Status().tcpPort));
    Identity identity;
    ASSERT_TRUE(original.Welcome(identity));
    ASSERT_NE(0u, identity.capabilities & kCapabilityExecutionObservation);
    Client replacement;
    struct Context { Client* original; Client* replacement; bool queued = false; } context{&original, &replacement};
    SetBeforeCommandDispatchForTests([](void* raw) {
        auto& ctx = *static_cast<Context*>(raw);
        ctx.original->Close();
        ASSERT_TRUE(WaitUntil([] { return Status().state == VN_REMOTE_LISTENING; }, false));
        ASSERT_TRUE(ctx.replacement->Connect(Status().tcpPort));
        Hello hello;
        hello.capabilities &= ~kCapabilityExecutionObservation;
        ASSERT_TRUE(ctx.replacement->Send(MakeFrame(Message::Hello, 1, hello)));
        Frame frame;
        ASSERT_TRUE(WaitUntil([&] { return ctx.replacement->Read(frame); }, false));
        EventCaptureRequestV2 request;
        request.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT;
        request.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
        ASSERT_TRUE(ctx.replacement->Send(MakeFrame(Message::EventStartV2, 2, request)));
        ctx.queued = WaitUntil([] { return PendingCommandsForTests() != 0; }, false);
    }, &context);
    EXPECT_EQ(VN_OK, Vn_Update());
    ASSERT_TRUE(context.queued);
    EXPECT_EQ(1u, PendingCommandsForTests());
    Frame frame;
    ASSERT_TRUE(WaitUntil([&] { return replacement.Read(frame); }));
    ASSERT_EQ(2u, frame.requestId);
    ASSERT_EQ(Message::EventStatusV2, frame.type);
    EventCaptureStatusV2 result;
    ASSERT_TRUE(DecodePayload(frame, result));
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED, result.base.result);
    EXPECT_EQ(0u, result.base.sessionEpoch);
}

TEST(RuntimeRemote, RepeatedV2SealKeepsV2ReplyAfterImmediateDrain) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto config = RemoteConfig();
    ASSERT_EQ(VN_OK, Vn_RemoteStart(&config));
    Client client;
    ASSERT_TRUE(client.Connect(Status().tcpPort));
    Identity identity;
    ASSERT_TRUE(client.Welcome(identity));
    EventCaptureRequestV2 request;
    request.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT;
    request.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
    ASSERT_TRUE(client.Send(MakeFrame(Message::EventStartV2, 2, request)));
    Frame frame;
    ASSERT_TRUE(WaitUntil([&] { return client.Read(frame); }));
    EventCaptureStatusV2 started;
    ASSERT_TRUE(DecodePayload(frame, started));
    ASSERT_EQ(VN_OK, started.base.result);
    for (std::uint32_t id : {3u, 4u}) {
        Writer seal;
        seal.U64(started.base.sessionEpoch);
        ASSERT_TRUE(client.Send({Message::EventSeal, id, std::move(seal.bytes)}));
        ASSERT_TRUE(WaitUntil([&] { return client.Read(frame) && frame.requestId == id; }));
        ASSERT_EQ(Message::EventStatusV2, frame.type);
        EventCaptureStatusV2 result;
        ASSERT_TRUE(DecodePayload(frame, result));
        if (id == 3) {
            EXPECT_EQ(VN_OK, result.base.result);
            EXPECT_EQ(VN_MONITOR_EVENT_SESSION_DRAINED, result.base.state);
        } else EXPECT_EQ(VN_MONITOR_STALE_SESSION, result.base.result);
    }
}

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
    config.bindAddress = nullptr;
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
    EXPECT_EQ("127.0.0.1", identity.bindAddress);
    EXPECT_STREQ("127.0.0.1", running.bindAddress);
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
