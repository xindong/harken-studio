#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <harken/HarkenDevelopment.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <harken/HarkenDevelopment.h>
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

static_assert(VN_REMOTE_PROTOCOL_MAJOR == harken::remote::kProtocolMajor);
static_assert(VN_REMOTE_CAPABILITY_SNAPSHOT == harken::remote::kCapabilitySnapshot &&
              VN_REMOTE_CAPABILITY_CATALOG == harken::remote::kCapabilityCatalog &&
              VN_REMOTE_CAPABILITY_CONTROL == harken::remote::kCapabilityControl &&
              VN_REMOTE_CAPABILITY_MONITOR_RECORDS == harken::remote::kCapabilityMonitorRecords &&
              VN_REMOTE_CAPABILITY_EVENT_CAPTURE == harken::remote::kCapabilityEventCapture &&
              VN_REMOTE_CAPABILITY_EXECUTION_OBSERVATION ==
                  harken::remote::kCapabilityExecutionObservation);
static_assert(VN_REMOTE_OPERATION_POST_ACTION ==
                  static_cast<std::uint32_t>(harken::remote::Operation::PostAction) &&
              VN_REMOTE_OPERATION_STOP_ALL ==
                  static_cast<std::uint32_t>(harken::remote::Operation::StopAll) &&
              VN_REMOTE_OPERATION_STOP_VOICE ==
                  static_cast<std::uint32_t>(harken::remote::Operation::StopVoice) &&
              VN_REMOTE_OPERATION_STOP_ACTION ==
                  static_cast<std::uint32_t>(harken::remote::Operation::StopAction) &&
              VN_REMOTE_OPERATION_SET_GLOBAL_VARIABLE ==
                  static_cast<std::uint32_t>(harken::remote::Operation::SetGlobalVariable) &&
              VN_REMOTE_OPERATION_SET_BUS_VOLUME ==
                  static_cast<std::uint32_t>(harken::remote::Operation::SetBusVolume) &&
              VN_REMOTE_OPERATION_SET_VOICE_VOLUME ==
                  static_cast<std::uint32_t>(harken::remote::Operation::SetVoiceVolume));
static_assert(VN_REMOTE_CATALOG_ACTION ==
                  static_cast<std::uint32_t>(harken::remote::CatalogKind::Action) &&
              VN_REMOTE_CATALOG_VARIABLE ==
                  static_cast<std::uint32_t>(harken::remote::CatalogKind::Variable) &&
              VN_REMOTE_CATALOG_BUS == static_cast<std::uint32_t>(harken::remote::CatalogKind::Bus));

namespace harken::remote {
namespace {

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void CloseSocket(Socket value) {
    if (value != kInvalidSocket)
        closesocket(value);
}
int LastError() { return WSAGetLastError(); }
bool WouldBlock(int error) { return error == WSAEWOULDBLOCK; }
bool InProgress(int error) { return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS; }
bool SetNonBlocking(Socket value) {
    u_long enabled = 1;
    return ioctlsocket(value, FIONBIO, &enabled) == 0;
}
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
void CloseSocket(Socket value) {
    if (value != kInvalidSocket)
        close(value);
}
int LastError() { return errno; }
bool WouldBlock(int error) { return error == EAGAIN || error == EWOULDBLOCK || error == EINTR; }
bool InProgress(int error) { return error == EINPROGRESS || error == EINTR; }
bool SetNonBlocking(Socket value) {
#if defined(__APPLE__)
    int enabled = 1;
    if (setsockopt(value, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0)
        return false;
#endif
    const int flags = fcntl(value, F_GETFL, 0);
    return flags >= 0 && fcntl(value, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

constexpr std::uint64_t kRequestTimeoutMs = 5000;
constexpr std::uint64_t kHeartbeatMs = 10000;
constexpr std::uint64_t kSilenceTimeoutMs = 15000;
constexpr std::size_t kMaxQueuedEvents = 4096;
constexpr unsigned kFramesPerPump = 64;

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

template <std::size_t N> void CopyText(char (&out)[N], const std::string& text) {
    const std::size_t count = std::min(text.size(), N - 1);
    std::memcpy(out, text.data(), count);
    out[count] = '\0';
}

void FillTarget(VnRemoteClientTarget& target, const Identity& identity, const std::string& address) {
    target = {};
    target.result = static_cast<VnResult>(identity.result);
    target.protocolMajor = identity.major;
    target.protocolMinor = identity.minor;
    target.processId = identity.processId;
    target.tcpPort = identity.tcpPort;
    target.sampleRate = identity.sampleRate;
    target.blockFrames = identity.blockFrames;
    target.compatible = identity.result == 0 && identity.major == kProtocolMajor ? 1u : 0u;
    target.instanceId = identity.instanceId;
    target.sessionId = identity.sessionId;
    target.capabilities = identity.capabilities;
    CopyText(target.address, address);
    CopyText(target.bindAddress, identity.bindAddress);
    CopyText(target.applicationName, identity.applicationName);
    CopyText(target.projectName, identity.projectName);
    CopyText(target.machineName, identity.machineName);
    CopyText(target.engineVersion, identity.engineVersion);
    CopyText(target.buildId, identity.buildId);
    CopyText(target.platform, identity.platform);
}

void FillCapture(VnRemoteClientCaptureStatus& out, const EventCaptureStatusV2& status, bool execution) {
    out = {};
    out.result = static_cast<VnResult>(status.base.result);
    out.state = status.base.state;
    out.sessionEpoch = status.base.sessionEpoch;
    out.startedTimestampNs = status.base.startedTimestampNs;
    out.stoppedTimestampNs = status.base.stoppedTimestampNs;
    out.sourceDroppedRecords = status.base.sourceDroppedRecords;
    out.deliveryDroppedRecords = status.base.deliveryDroppedRecords;
    out.transportDroppedRecords = status.base.transportDroppedRecords;
    out.execution = execution ? 1u : 0u;
    out.acceptedExecutionFactVersion = status.acceptedExecutionFactVersion;
    out.baselineAvailability = status.baselineAvailability;
    out.requestedPermissionMask = status.requestedPermissionMask;
    out.acceptedPermissionMask = status.acceptedPermissionMask;
    out.deniedPermissionMask = status.deniedPermissionMask;
    out.unsupportedPermissionMask = status.unsupportedPermissionMask;
    out.acceptedStreamKindMask = status.acceptedStreamKindMask;
    out.symbolsDecision = status.symbolsDecision;
    out.symbolAvailability = status.symbolAvailability;
    out.identityUnavailableCount = status.identityUnavailableCount;
    out.baselineStateCutId = status.baselineStateCutId;
    out.supportedFactKinds[0] = status.supportedFactKinds[0];
    out.supportedFactKinds[1] = status.supportedFactKinds[1];
}

bool LoopbackHost(const char* host, in_addr* out) {
    if (std::strcmp(host, "localhost") == 0) {
        out->s_addr = htonl(INADDR_LOOPBACK);
        return true;
    }
    return inet_pton(AF_INET, host, out) == 1 && (ntohl(out->s_addr) >> 24) == 127;
}

class Client {
  public:
    struct Event {
        VnRemoteClientEvent value{};
        std::vector<std::uint8_t> data;
    };

    Client(std::uint32_t flags, std::string name) : flags_(flags), name_(std::move(name)) {}
    ~Client() {
        CloseSession();
        CloseSocket(discovery_);
    }

    VnResult Discover(std::uint32_t port) {
        CloseSocket(discovery_);
        discovery_ = kInvalidSocket;
        discoveryRequestId_ = 0;
        if (port == 0 || port > 65535)
            return VN_INVALID_ARG;
        discovery_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (discovery_ == kInvalidSocket || !SetNonBlocking(discovery_))
            return DiscoveryFailed();
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(LocalOnly() ? INADDR_LOOPBACK : INADDR_ANY);
        if (bind(discovery_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0)
            return DiscoveryFailed();
        const std::uint32_t id = NextRequestId();
        if (id == 0)
            return DiscoveryFailed();
        discoveryRequestId_ = id;
        const auto bytes = EncodeFrame({Message::Discover, id, {}});
        unsigned char loop = 1;
        unsigned char ttl = LocalOnly() ? 0 : 1;
        setsockopt(discovery_, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof(loop));
        setsockopt(discovery_, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
        if (LocalOnly()) {
            in_addr loopback{};
            loopback.s_addr = htonl(INADDR_LOOPBACK);
            setsockopt(discovery_, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&loopback),
                       sizeof(loopback));
        }
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(static_cast<std::uint16_t>(port));
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bool sent = SendDatagram(bytes, target);
        inet_pton(AF_INET, kDiscoveryGroup, &target.sin_addr);
        sent = SendDatagram(bytes, target) || sent;
        return sent ? VN_OK : DiscoveryFailed();
    }

    VnResult Connect(const char* host, std::uint32_t port) {
        if (host == nullptr || port == 0 || port > 65535)
            return VN_INVALID_ARG;
        if (state_ == State::Connecting || state_ == State::Handshaking || state_ == State::Connected)
            return VN_IN_USE;
        in_addr address{};
        if (LocalOnly()) {
            if (!LoopbackHost(host, &address))
                return VN_INVALID_ARG;
        } else if (std::strcmp(host, "localhost") == 0) {
            address.s_addr = htonl(INADDR_LOOPBACK);
        } else if (inet_pton(AF_INET, host, &address) != 1) {
            return VN_INVALID_ARG;
        }
        CloseSession();
        DropSessionEvents();
        char text[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &address, text, sizeof(text));
        address_ = text;
        state_ = State::Connecting;
        deadline_ = NowMs() + kRequestTimeoutMs;
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == kInvalidSocket || !SetNonBlocking(socket_)) {
            Fail(VN_REMOTE_CLIENT_FAILURE_CONNECT, LastError());
            return VN_OK;
        }
        sockaddr_in remote{};
        remote.sin_family = AF_INET;
        remote.sin_port = htons(static_cast<std::uint16_t>(port));
        remote.sin_addr = address;
        if (connect(socket_, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) != 0) {
            const int error = LastError();
            if (!InProgress(error))
                Fail(VN_REMOTE_CLIENT_FAILURE_CONNECT, error);
        }
        return VN_OK;
    }

    void Disconnect() {
        CloseSession();
        DropSessionEvents();
        state_ = State::Idle;
    }

    VnResult Request(const VnRemoteClientRequest& request, std::uint32_t* outRequestId) {
        if (state_ != State::Connected)
            return VN_NOT_INITIALIZED;
        Frame frame;
        Message expected = Message::Error;
        Operation operation = Operation::StopAll;
        switch (request.kind) {
        case VN_REMOTE_CLIENT_REQUEST_SNAPSHOT:
            frame.type = Message::SnapshotRequest;
            expected = Message::Snapshot;
            break;
        case VN_REMOTE_CLIENT_REQUEST_CATALOG: {
            Writer writer;
            writer.U32(request.offset);
            frame = {Message::CatalogRequest, 0, std::move(writer.bytes)};
            expected = Message::Catalog;
            break;
        }
        case VN_REMOTE_CLIENT_REQUEST_COMMAND: {
            Command command;
            command.operation = static_cast<Operation>(request.operation);
            command.expectedGeneration = request.expectedGeneration;
            command.target = request.target;
            command.instanceHandle = request.instanceHandle;
            command.secondary = request.secondary;
            command.value = request.value;
            command.durationMs = request.durationMs;
            frame = MakeFrame(Message::Command, 0, command);
            expected = Message::CommandResult;
            operation = command.operation;
            break;
        }
        case VN_REMOTE_CLIENT_REQUEST_MONITOR_START: {
            MonitorRequest monitor;
            monitor.streamMask = request.streamMask;
            monitor.cadenceHz = request.cadenceHz;
            monitor.bytesPerSecond = request.bytesPerSecond;
            frame = MakeFrame(Message::MonitorStart, 0, monitor);
            expected = Message::MonitorStatus;
            break;
        }
        case VN_REMOTE_CLIENT_REQUEST_MONITOR_STOP:
            frame.type = Message::MonitorStop;
            expected = Message::MonitorStatus;
            break;
        case VN_REMOTE_CLIENT_REQUEST_EVENT_START:
            frame.type = Message::EventStart;
            expected = Message::EventStatus;
            captureExecution_ = false;
            break;
        case VN_REMOTE_CLIENT_REQUEST_EVENT_START_V2: {
            EventCaptureRequestV2 capture;
            capture.executionFactVersion = request.executionFactVersion;
            capture.requestedStreamKindMask = request.streamMask;
            capture.requestedPermissionMask = request.permissionMask;
            capture.requestSymbols = request.requestSymbols;
            capture.consumerQueueBytes = request.consumerQueueBytes;
            frame = MakeFrame(Message::EventStartV2, 0, capture);
            expected = Message::EventStatusV2;
            captureExecution_ = true;
            break;
        }
        case VN_REMOTE_CLIENT_REQUEST_EVENT_SEAL: {
            Writer writer;
            writer.U64(request.sessionEpoch);
            frame = {Message::EventSeal, 0, std::move(writer.bytes)};
            expected = captureExecution_ ? Message::EventStatusV2 : Message::EventStatus;
            break;
        }
        default:
            return VN_INVALID_ARG;
        }
        const std::uint32_t id = Send(std::move(frame), expected, operation);
        if (id == 0)
            return VN_OK;
        if (outRequestId)
            *outRequestId = id;
        return VN_OK;
    }

    VnResult Poll(VnRemoteClientEvent* out, void* buffer, std::uint64_t bufferBytes) {
        if (events_.empty())
            Pump();
        if (events_.empty())
            return VN_REMOTE_CLIENT_NO_EVENT;
        Event& front = events_.front();
        *out = front.value;
        out->structSize = sizeof(*out);
        if (front.data.size() > bufferBytes || (!front.data.empty() && buffer == nullptr))
            return VN_REMOTE_CLIENT_BUFFER_TOO_SMALL;
        if (!front.data.empty())
            std::memcpy(buffer, front.data.data(), front.data.size());
        events_.pop_front();
        return VN_OK;
    }

  private:
    enum class State { Idle, Connecting, Handshaking, Connected, Failed };

    struct Pending {
        Message expected = Message::Error;
        Operation operation = Operation::StopAll;
        std::uint64_t deadline = 0;
    };

    bool LocalOnly() const { return (flags_ & VN_REMOTE_CLIENT_LOCAL_ONLY) != 0; }

    VnResult DiscoveryFailed() {
        CloseSocket(discovery_);
        discovery_ = kInvalidSocket;
        discoveryRequestId_ = 0;
        return VN_DEVICE_ERROR;
    }

    bool SendDatagram(const std::vector<std::uint8_t>& bytes, const sockaddr_in& target) {
        return sendto(discovery_, reinterpret_cast<const char*>(bytes.data()),
                      static_cast<int>(bytes.size()), 0, reinterpret_cast<const sockaddr*>(&target),
                      sizeof(target)) == static_cast<int>(bytes.size());
    }

    std::uint32_t NextRequestId() {
        if (nextRequestId_ == std::numeric_limits<std::uint32_t>::max()) {
            Fail(VN_REMOTE_CLIENT_FAILURE_SEQUENCE_EXHAUSTED);
            return 0;
        }
        return nextRequestId_++;
    }

    Event& Emit(std::uint32_t kind, std::uint32_t requestId = 0) {
        if (events_.size() >= kMaxQueuedEvents)
            events_.pop_front();
        events_.emplace_back();
        Event& event = events_.back();
        event.value.structSize = sizeof(event.value);
        event.value.kind = kind;
        event.value.requestId = requestId;
        return event;
    }

    void DropSessionEvents() {
        events_.erase(std::remove_if(events_.begin(), events_.end(),
                                     [](const Event& event) {
                                         return event.value.kind != VN_REMOTE_CLIENT_EVENT_TARGET_FOUND;
                                     }),
                      events_.end());
    }

    void CloseSession() {
        CloseSocket(socket_);
        socket_ = kInvalidSocket;
        pending_.clear();
        receive_.clear();
        send_.clear();
    }

    void Fail(std::uint32_t failure, int osError = 0, VnResult result = VN_OK,
              const Identity* identity = nullptr) {
        if (state_ == State::Failed || state_ == State::Idle)
            return;
        CloseSession();
        state_ = State::Failed;
        Event& event = Emit(VN_REMOTE_CLIENT_EVENT_FAILED);
        event.value.failure = failure;
        event.value.osError = osError;
        event.value.result = result;
        if (identity)
            FillTarget(event.value.target, *identity, address_);
    }

    std::uint32_t Send(Frame frame, Message expected, Operation operation = Operation::StopAll) {
        const std::uint32_t id = NextRequestId();
        if (id == 0)
            return 0;
        frame.requestId = id;
        if (send_.size() > kMaxPayloadBytes) {
            Fail(VN_REMOTE_CLIENT_FAILURE_SEND);
            return 0;
        }
        const auto bytes = EncodeFrame(frame);
        if (bytes.empty()) {
            Fail(VN_REMOTE_CLIENT_FAILURE_SEND);
            return 0;
        }
        send_.insert(send_.end(), bytes.begin(), bytes.end());
        pending_[id] = {expected, operation, NowMs() + kRequestTimeoutMs};
        Flush();
        return state_ == State::Failed ? 0 : id;
    }

    bool HasPending(Message expected) const {
        return std::any_of(pending_.begin(), pending_.end(),
                           [expected](const auto& row) { return row.second.expected == expected; });
    }

    void Flush() {
        while (socket_ != kInvalidSocket && !send_.empty()) {
            const int count = send(socket_, reinterpret_cast<const char*>(send_.data()),
                                   static_cast<int>(send_.size()), kSendFlags);
            if (count > 0) {
                send_.erase(send_.begin(), send_.begin() + count);
                continue;
            }
            const int error = LastError();
            if (count < 0 && WouldBlock(error))
                return;
            Fail(VN_REMOTE_CLIENT_FAILURE_SEND, error);
            return;
        }
    }

    void Pump() {
        ReadDiscovery();
        const std::uint64_t now = NowMs();
        if (state_ == State::Connecting) {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(socket_, &writable);
            fd_set failed = writable;
            timeval zero{};
            const int ready = select(static_cast<int>(socket_ + 1), nullptr, &writable, &failed, &zero);
            if (ready > 0) {
                int error = 0;
#if defined(_WIN32)
                int length = sizeof(error);
#else
                socklen_t length = sizeof(error);
#endif
                getsockopt(socket_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length);
                if (error != 0 || FD_ISSET(socket_, &failed)) {
                    Fail(VN_REMOTE_CLIENT_FAILURE_CONNECT, error);
                    return;
                }
                state_ = State::Handshaking;
                lastReceive_ = lastPing_ = now;
                Emit(VN_REMOTE_CLIENT_EVENT_HANDSHAKING);
                Hello hello;
                hello.clientName = name_;
                Send(MakeFrame(Message::Hello, 0, hello), Message::Welcome);
            } else if (now >= deadline_) {
                Fail(VN_REMOTE_CLIENT_FAILURE_CONNECT_TIMEOUT);
                return;
            }
        }
        if (socket_ == kInvalidSocket || state_ == State::Connecting)
            return;
        Flush();
        ReadSocket();
        if (socket_ == kInvalidSocket)
            return;
        const std::uint64_t later = NowMs();
        if (state_ == State::Handshaking && later >= deadline_) {
            Fail(VN_REMOTE_CLIENT_FAILURE_CONNECT_TIMEOUT);
            return;
        }
        for (const auto& row : pending_) {
            if (later >= row.second.deadline) {
                Fail(row.second.expected == Message::CommandResult
                         ? VN_REMOTE_CLIENT_FAILURE_COMMAND_TIMEOUT
                         : VN_REMOTE_CLIENT_FAILURE_RESPONSE_TIMEOUT);
                return;
            }
        }
        if (state_ != State::Connected)
            return;
        if (later >= lastReceive_ + kSilenceTimeoutMs) {
            Fail(VN_REMOTE_CLIENT_FAILURE_SILENT);
            return;
        }
        if (later >= lastPing_ + kHeartbeatMs && !HasPending(Message::Pong)) {
            lastPing_ = later;
            Send({Message::Ping, 0, {}}, Message::Pong);
        }
    }

    void ReadSocket() {
        for (unsigned frames = 0; socket_ != kInvalidSocket && frames < kFramesPerPump;) {
            const std::size_t limit = kMaxPayloadBytes + kHeaderBytes;
            if (receive_.size() < limit) {
                std::uint8_t chunk[16384];
                const std::size_t room = std::min(sizeof(chunk), limit - receive_.size());
                const int count = recv(socket_, reinterpret_cast<char*>(chunk), static_cast<int>(room), 0);
                if (count > 0) {
                    receive_.insert(receive_.end(), chunk, chunk + count);
                } else if (count == 0) {
                    DrainFrames(frames);
                    Fail(VN_REMOTE_CLIENT_FAILURE_DISCONNECTED);
                    return;
                } else {
                    const int error = LastError();
                    if (!WouldBlock(error)) {
                        DrainFrames(frames);
                        Fail(VN_REMOTE_CLIENT_FAILURE_CONNECT, error);
                        return;
                    }
                    DrainFrames(frames);
                    return;
                }
            }
            if (!DrainFrames(frames))
                return;
        }
    }

    bool DrainFrames(unsigned& frames) {
        while (socket_ != kInvalidSocket && frames < kFramesPerPump && receive_.size() >= kHeaderBytes) {
            Header header;
            if (!DecodeHeader(receive_.data(), receive_.size(), header)) {
                Fail(VN_REMOTE_CLIENT_FAILURE_PROTOCOL);
                return false;
            }
            const std::size_t size = kHeaderBytes + header.payloadBytes;
            if (receive_.size() < size)
                return true;
            Frame frame;
            frame.type = header.type;
            frame.requestId = header.requestId;
            frame.payload.assign(receive_.begin() + kHeaderBytes, receive_.begin() + size);
            receive_.erase(receive_.begin(), receive_.begin() + size);
            lastReceive_ = NowMs();
            ++frames;
            Dispatch(frame);
        }
        return socket_ != kInvalidSocket;
    }

    void Dispatch(const Frame& frame) {
        if (frame.type == Message::Error && frame.requestId == 0 && state_ == State::Handshaking) {
            Reader reader(frame.payload);
            std::int32_t result = 0;
            if (reader.I32(result) && reader.Done() && result == VN_IN_USE)
                Fail(VN_REMOTE_CLIENT_FAILURE_IN_USE, 0, VN_IN_USE);
            else
                Fail(VN_REMOTE_CLIENT_FAILURE_PROTOCOL);
            return;
        }
        const bool streaming = frame.requestId == 0 &&
            (frame.type == Message::Snapshot || frame.type == Message::MonitorRecords ||
             frame.type == Message::MonitorStatus || frame.type == Message::EventStatus ||
             frame.type == Message::EventStatusV2 || frame.type == Message::EventRecords);
        Pending request;
        if (!streaming) {
            const auto found = pending_.find(frame.requestId);
            if (found == pending_.end() ||
                (frame.type != found->second.expected && frame.type != Message::Error)) {
                Fail(VN_REMOTE_CLIENT_FAILURE_PROTOCOL);
                return;
            }
            request = found->second;
            pending_.erase(found);
        }
        if (frame.type == Message::Error) {
            Reader reader(frame.payload);
            std::int32_t result = 0;
            if (!reader.I32(result) || !reader.Done()) {
                Fail(VN_REMOTE_CLIENT_FAILURE_PROTOCOL);
                return;
            }
            Emit(VN_REMOTE_CLIENT_EVENT_REJECTED, frame.requestId).value.result = static_cast<VnResult>(result);
            return;
        }
        if (frame.type == Message::Welcome) {
            Identity identity;
            if (!DecodePayload(frame, identity) || identity.tcpPort > 65535 || identity.instanceId == 0) {
                Fail(VN_REMOTE_CLIENT_FAILURE_PROTOCOL);
                return;
            }
            if (identity.result != 0 || identity.major != kProtocolMajor) {
                Fail(VN_REMOTE_CLIENT_FAILURE_INCOMPATIBLE, 0, static_cast<VnResult>(identity.result), &identity);
                return;
            }
            state_ = State::Connected;
            FillTarget(Emit(VN_REMOTE_CLIENT_EVENT_CONNECTED, frame.requestId).value.target, identity, address_);
            return;
        }
        if (state_ != State::Connected) {
            Fail(VN_REMOTE_CLIENT_FAILURE_PROTOCOL);
            return;
        }
        switch (frame.type) {
        case Message::Pong:
            if (!frame.payload.empty())
                Fail(VN_REMOTE_CLIENT_FAILURE_PROTOCOL);
            return;
        case Message::Catalog: {
            Catalog catalog;
            if (!DecodePayload(frame, catalog)) {
                Fail(VN_REMOTE_CLIENT_FAILURE_PROTOCOL);
                return;
            }
            Event& event = Emit(VN_REMOTE_CLIENT_EVENT_CATALOG, frame.requestId);
            event.value.result = static_cast<VnResult>(catalog.result);
            event.value.catalogGeneration = catalog.generation;
            event.value.catalogTotal = catalog.totalCount;
            event.value.catalogOffset = catalog.offset;
            event.value.catalogRows = static_cast<std::uint32_t>(catalog.entries.size());
            event.data.resize(catalog.entries.size() * sizeof(VnRemoteClientCatalogRow));
            for (std::size_t i = 0; i < catalog.entries.size(); ++i) {
                VnRemoteClientCatalogRow row{};
                row.id = catalog.entries[i].id;
                row.kind = static_cast<std::uint32_t>(catalog.entries[i].kind);
                std::memcpy(event.data.data() + i * sizeof(row), &row, sizeof(row));
            }
            event.value.dataBytes = event.data.size();
            return;
        }
        case Message::Snapshot: {
            Snapshot snapshot;
            if (!DecodePayload(frame, snapshot)) {
                Fail(VN_REMOTE_CLIENT_FAILURE_PROTOCOL);
                return;
            }
            Event& event = Emit(VN_REMOTE_CLIENT_EVENT_SNAPSHOT, frame.requestId);
            auto& out = event.value.snapshot;
            out.result = static_cast<VnResult>(snapshot.result);
            out.activeVoices = snapshot.activeVoices;
            out.blockIndex = snapshot.blockIndex;
            out.busCount = snapshot.busCount;
            out.callbackCpuUs = snapshot.callbackCpuUs;
            out.updateCpuUs = snapshot.updateCpuUs;
            out.voiceRows = static_cast<std::uint32_t>(snapshot.voices.size());
            out.commandBackpressure = snapshot.commandBackpressure;
            out.droppedNotifications = snapshot.droppedNotifications;
            out.busRows = static_cast<std::uint32_t>(snapshot.buses.size());
            event.data.resize(snapshot.voices.size() * sizeof(VnRemoteClientVoice) +
                              snapshot.buses.size() * sizeof(VnRemoteClientBus));
            std::uint8_t* at = event.data.data();
            for (const auto& voice : snapshot.voices) {
                const VnRemoteClientVoice row{voice.handle, voice.soundId, voice.state, voice.peak};
                std::memcpy(at, &row, sizeof(row));
                at += sizeof(row);
            }
            for (const auto& bus : snapshot.buses) {
                const VnRemoteClientBus row{bus.id, bus.generation, bus.peak};
                std::memcpy(at, &row, sizeof(row));
                at += sizeof(row);
            }
            event.value.dataBytes = event.data.size();
            return;
        }
        case Message::EventStatus:
        case Message::EventStatusV2: {
            const bool execution = frame.type == Message::EventStatusV2;
            EventCaptureStatusV2 status;
            const bool decoded = execution ? DecodePayload(frame, status) : DecodePayload(frame, status.base);
            if (!decoded) {
                Fail(VN_REMOTE_CLIENT_FAILURE_PROTOCOL);
                return;
            }
            FillCapture(Emit(VN_REMOTE_CLIENT_EVENT_CAPTURE_STATUS, frame.requestId).value.capture, status,
                        execution);
            return;
        }
        case Message::EventRecords:
        case Message::MonitorRecords: {
            MonitorRecords records;
            if (!DecodePayload(frame, records)) {
                Fail(VN_REMOTE_CLIENT_FAILURE_PROTOCOL);
                return;
            }
            Event& event = Emit(frame.type == Message::EventRecords ? VN_REMOTE_CLIENT_EVENT_CAPTURE_RECORDS
                                                                    : VN_REMOTE_CLIENT_EVENT_MONITOR_RECORDS,
                                frame.requestId);
            event.value.sessionEpoch = records.sessionEpoch;
            event.value.recordCount = records.recordCount;
            event.data = std::move(records.bytes);
            event.value.dataBytes = event.data.size();
            return;
        }
        case Message::MonitorStatus: {
            MonitorStatus status;
            if (!DecodePayload(frame, status)) {
                Fail(VN_REMOTE_CLIENT_FAILURE_PROTOCOL);
                return;
            }
            Event& event = Emit(VN_REMOTE_CLIENT_EVENT_MONITOR_STATUS, frame.requestId);
            event.value.result = static_cast<VnResult>(status.result);
            event.value.sessionEpoch = status.sessionEpoch;
            event.value.planGeneration = status.planGeneration;
            event.value.streamMask = status.streamMask;
            return;
        }
        case Message::CommandResult: {
            CommandResult result;
            if (!DecodePayload(frame, result) || result.operation != request.operation) {
                Fail(VN_REMOTE_CLIENT_FAILURE_PROTOCOL);
                return;
            }
            Event& event = Emit(VN_REMOTE_CLIENT_EVENT_COMMAND_RESULT, frame.requestId);
            event.value.result = static_cast<VnResult>(result.result);
            event.value.operation = static_cast<std::uint32_t>(result.operation);
            event.value.handle = result.handle;
            return;
        }
        default:
            Fail(VN_REMOTE_CLIENT_FAILURE_PROTOCOL);
            return;
        }
    }

    void ReadDiscovery() {
        for (unsigned processed = 0; discovery_ != kInvalidSocket && processed < 64; ++processed) {
            std::uint8_t bytes[4096];
            sockaddr_in sender{};
#if defined(_WIN32)
            int length = sizeof(sender);
#else
            socklen_t length = sizeof(sender);
#endif
            const int count = recvfrom(discovery_, reinterpret_cast<char*>(bytes), sizeof(bytes), 0,
                                       reinterpret_cast<sockaddr*>(&sender), &length);
            if (count <= 0)
                return;
            const bool senderLoopback = (ntohl(sender.sin_addr.s_addr) >> 24) == 127;
            if (LocalOnly() && !senderLoopback)
                continue;
            Header header;
            if (!DecodeHeader(bytes, static_cast<std::size_t>(count), header) ||
                header.type != Message::Announcement || header.requestId != discoveryRequestId_ ||
                static_cast<std::size_t>(count) != kHeaderBytes + header.payloadBytes)
                continue;
            Reader reader(bytes + kHeaderBytes, header.payloadBytes);
            Identity identity;
            if (!Decode(reader, identity) || identity.tcpPort == 0 || identity.tcpPort > 65535 ||
                identity.instanceId == 0)
                continue;
            in_addr address = sender.sin_addr;
            if (!identity.bindAddress.empty() && identity.bindAddress != "0.0.0.0" &&
                inet_pton(AF_INET, identity.bindAddress.c_str(), &address) != 1)
                continue;
            if (LocalOnly() && (ntohl(address.s_addr) >> 24) != 127)
                continue;
            char text[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &address, text, sizeof(text));
            FillTarget(Emit(VN_REMOTE_CLIENT_EVENT_TARGET_FOUND).value.target, identity, text);
        }
    }

    std::uint32_t flags_;
    std::string name_;
    Socket socket_ = kInvalidSocket;
    Socket discovery_ = kInvalidSocket;
    State state_ = State::Idle;
    std::string address_;
    std::map<std::uint32_t, Pending> pending_;
    std::vector<std::uint8_t> receive_;
    std::vector<std::uint8_t> send_;
    std::deque<Event> events_;
    std::uint32_t nextRequestId_ = 1;
    std::uint32_t discoveryRequestId_ = 0;
    std::uint64_t deadline_ = 0;
    std::uint64_t lastReceive_ = 0;
    std::uint64_t lastPing_ = 0;
    bool captureExecution_ = false;
};

std::mutex g_clientsMutex;
std::map<VnRemoteClient, std::shared_ptr<Client>> g_clients;
VnRemoteClient g_nextClient = 1;

std::shared_ptr<Client> Find(VnRemoteClient handle) {
    std::lock_guard lock(g_clientsMutex);
    const auto found = g_clients.find(handle);
    return found == g_clients.end() ? nullptr : found->second;
}

}
}

using harken::remote::Client;

extern "C" {

VnResult Vn_RemoteClientCreate(const VnRemoteClientDesc* desc, VnRemoteClient* outClient) {
    if (outClient == nullptr || desc == nullptr || desc->structSize != sizeof(*desc) ||
        (desc->flags & ~static_cast<std::uint32_t>(VN_REMOTE_CLIENT_LOCAL_ONLY)) != 0)
        return VN_INVALID_ARG;
    *outClient = 0;
    const std::string name = desc->clientName ? desc->clientName : "";
    if (name.size() > 128)
        return VN_INVALID_ARG;
    try {
#if defined(_WIN32)
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            return VN_DEVICE_ERROR;
#endif
        auto client = std::make_shared<Client>(desc->flags, name);
        std::lock_guard lock(harken::remote::g_clientsMutex);
        const VnRemoteClient handle = harken::remote::g_nextClient++;
        harken::remote::g_clients.emplace(handle, std::move(client));
        *outClient = handle;
        return VN_OK;
    } catch (...) {
        return VN_DEVICE_ERROR;
    }
}

VnResult Vn_RemoteClientDestroy(VnRemoteClient client) {
    std::shared_ptr<Client> owned;
    {
        std::lock_guard lock(harken::remote::g_clientsMutex);
        const auto found = harken::remote::g_clients.find(client);
        if (found == harken::remote::g_clients.end())
            return VN_STALE_HANDLE;
        owned = std::move(found->second);
        harken::remote::g_clients.erase(found);
    }
    owned.reset();
#if defined(_WIN32)
    WSACleanup();
#endif
    return VN_OK;
}

VnResult Vn_RemoteClientDiscover(VnRemoteClient client, uint32_t discoveryPort) {
    const auto found = harken::remote::Find(client);
    if (!found)
        return VN_STALE_HANDLE;
    try {
        return found->Discover(discoveryPort);
    } catch (...) {
        return VN_DEVICE_ERROR;
    }
}

VnResult Vn_RemoteClientConnect(VnRemoteClient client, const char* host, uint32_t tcpPort) {
    const auto found = harken::remote::Find(client);
    if (!found)
        return VN_STALE_HANDLE;
    try {
        return found->Connect(host, tcpPort);
    } catch (...) {
        return VN_DEVICE_ERROR;
    }
}

VnResult Vn_RemoteClientDisconnect(VnRemoteClient client) {
    const auto found = harken::remote::Find(client);
    if (!found)
        return VN_STALE_HANDLE;
    found->Disconnect();
    return VN_OK;
}

VnResult Vn_RemoteClientRequest(VnRemoteClient client, const VnRemoteClientRequest* request,
                                uint32_t* outRequestId) {
    if (request == nullptr || request->structSize != sizeof(*request))
        return VN_INVALID_ARG;
    if (outRequestId)
        *outRequestId = 0;
    const auto found = harken::remote::Find(client);
    if (!found)
        return VN_STALE_HANDLE;
    try {
        return found->Request(*request, outRequestId);
    } catch (...) {
        return VN_DEVICE_ERROR;
    }
}

VnResult Vn_RemoteClientPoll(VnRemoteClient client, VnRemoteClientEvent* outEvent, void* buffer,
                             uint64_t bufferBytes) {
    if (outEvent == nullptr || outEvent->structSize != sizeof(*outEvent))
        return VN_INVALID_ARG;
    const auto found = harken::remote::Find(client);
    if (!found)
        return VN_STALE_HANDLE;
    try {
        return found->Poll(outEvent, buffer, bufferBytes);
    } catch (...) {
        return VN_DEVICE_ERROR;
    }
}

}
