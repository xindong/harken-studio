#ifndef HARKEN_RUNTIME_REMOTE_PROTOCOL_H
#define HARKEN_RUNTIME_REMOTE_PROTOCOL_H

#include <harken/ObjectId.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace harken::remote {

static_assert(sizeof(float) == sizeof(std::uint32_t) && std::numeric_limits<float>::is_iec559);

inline constexpr std::uint32_t kMagic = 0x4d524e56u;
inline constexpr std::uint32_t kProtocolMajor = 2;
inline constexpr std::uint32_t kProtocolMinor = 0;
inline constexpr std::uint32_t kHeaderBytes = 16;
inline constexpr std::uint32_t kMaxPayloadBytes = 262144;
inline constexpr std::uint16_t kDiscoveryPort = 37840;
inline constexpr const char *kDiscoveryGroup = "239.255.37.84";
inline constexpr std::uint64_t kCapabilitySnapshot = 1;
inline constexpr std::uint64_t kCapabilityCatalog = 2;
inline constexpr std::uint64_t kCapabilityControl = 4;
inline constexpr std::uint64_t kCapabilityMonitorRecords = 8;
inline constexpr std::uint64_t kCapabilityEventCapture = 16;
inline constexpr std::uint64_t kCapabilityExecutionObservation = 32;
inline constexpr std::uint64_t kCapabilities = 63;

enum class Message : std::uint16_t {
    Hello = 1,
    Welcome = 2,
    Error = 3,
    Ping = 4,
    Pong = 5,
    SnapshotRequest = 10,
    Snapshot = 11,
    CatalogRequest = 12,
    Catalog = 13,
    Command = 20,
    CommandResult = 21,
    MonitorStart = 30,
    MonitorStop = 31,
    MonitorRecords = 32,
    MonitorStatus = 33,
    EventStart = 34,
    EventSeal = 35,
    EventStatus = 36,
    EventRecords = 37,
    EventStartV2 = 38,
    EventStatusV2 = 39,
    Goodbye = 40,
    Discover = 100,
    Announcement = 101
};

enum class Operation : std::uint32_t {
    PostAction = 1,
    PostActionByName = 2,
    StopAll = 3,
    StopVoice = 4,
    StopAction = 5,
    SetGlobalVariable = 6,
    SetBusVolume = 7,
    SetVoiceVolume = 8
};

enum class CatalogKind : std::uint32_t { Action = 1, Variable = 2, Bus = 3 };

struct Header {
    std::uint32_t payloadBytes = 0;
    Message type = Message::Error;
    std::uint16_t flags = 0;
    std::uint32_t requestId = 0;
};

struct Frame {
    Message type = Message::Error;
    std::uint32_t requestId = 0;
    std::vector<std::uint8_t> payload;
};

class Writer {
  public:
    std::vector<std::uint8_t> bytes;
    void U16(std::uint16_t value) {
        U8(value);
        U8(value >> 8);
    }
    void U32(std::uint32_t value) {
        U16(value);
        U16(value >> 16);
    }
    void U64(std::uint64_t value) {
        U32(static_cast<std::uint32_t>(value));
        U32(static_cast<std::uint32_t>(value >> 32));
    }
    void I32(std::int32_t value) { U32(static_cast<std::uint32_t>(value)); }
    void ObjectId(VnObjectId value) {
        const auto encoded = harken::ObjectIdBytes(value);
        Raw(encoded.data(), encoded.size());
    }
    void F32(float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, 4);
        U32(bits);
    }
    void Raw(const void *data, std::size_t size) {
        if (size != 0) {
            const auto *first = static_cast<const std::uint8_t *>(data);
            bytes.insert(bytes.end(), first, first + size);
        }
    }
    void String(const std::string &value) {
        U32(static_cast<std::uint32_t>(value.size()));
        Raw(value.data(), value.size());
    }

  private:
    void U8(std::uint32_t value) { bytes.push_back(static_cast<std::uint8_t>(value)); }
};

class Reader {
  public:
    Reader(const void *data, std::size_t size)
        : data_(static_cast<const std::uint8_t *>(data)), size_(size) {}
    explicit Reader(const std::vector<std::uint8_t> &value) : Reader(value.data(), value.size()) {}
    bool U16(std::uint16_t &value) {
        if (Remaining() < 2)
            return false;
        value = static_cast<std::uint16_t>(data_[offset_] |
                                           (static_cast<std::uint16_t>(data_[offset_ + 1]) << 8));
        offset_ += 2;
        return true;
    }
    bool U32(std::uint32_t &value) {
        std::uint16_t low = 0, high = 0;
        if (!U16(low) || !U16(high))
            return false;
        value = low | (static_cast<std::uint32_t>(high) << 16);
        return true;
    }
    bool U64(std::uint64_t &value) {
        std::uint32_t low = 0, high = 0;
        if (!U32(low) || !U32(high))
            return false;
        value = low | (static_cast<std::uint64_t>(high) << 32);
        return true;
    }
    bool I32(std::int32_t &value) {
        std::uint32_t bits = 0;
        if (!U32(bits))
            return false;
        std::memcpy(&value, &bits, 4);
        return true;
    }
    bool ObjectId(VnObjectId &value) {
        if (Remaining() < 16) return false;
        value = harken::ObjectIdFromBytes(
            std::span<const std::uint8_t, 16>(data_ + offset_, 16));
        offset_ += 16;
        return true;
    }
    bool F32(float &value) {
        std::uint32_t bits = 0;
        if (!U32(bits))
            return false;
        std::memcpy(&value, &bits, 4);
        return true;
    }
    bool String(std::string &value, std::uint32_t maximum = 4096) {
        std::uint32_t count = 0;
        if (!U32(count) || count > maximum || Remaining() < count)
            return false;
        value.assign(reinterpret_cast<const char *>(data_ + offset_), count);
        offset_ += count;
        return value.find('\0') == std::string::npos;
    }
    bool Raw(std::vector<std::uint8_t> &value, std::uint32_t count) {
        if (Remaining() < count)
            return false;
        value.assign(data_ + offset_, data_ + offset_ + count);
        offset_ += count;
        return true;
    }
    std::size_t Remaining() const { return size_ - offset_; }
    bool Done() const { return offset_ == size_; }

  private:
    const std::uint8_t *data_;
    std::size_t size_;
    std::size_t offset_ = 0;
};

inline bool DecodeHeader(const void *bytes, std::size_t size, Header &header) {
    if (size < kHeaderBytes)
        return false;
    Reader reader(bytes, kHeaderBytes);
    std::uint32_t magic = 0;
    std::uint16_t type = 0;
    if (!reader.U32(magic) || !reader.U32(header.payloadBytes) || !reader.U16(type) ||
        !reader.U16(header.flags) || !reader.U32(header.requestId))
        return false;
    header.type = static_cast<Message>(type);
    return magic == kMagic && header.payloadBytes <= kMaxPayloadBytes && header.flags == 0;
}

inline std::vector<std::uint8_t> EncodeFrame(const Frame &frame) {
    if (frame.payload.size() > kMaxPayloadBytes)
        return {};
    Writer writer;
    writer.U32(kMagic);
    writer.U32(static_cast<std::uint32_t>(frame.payload.size()));
    writer.U16(static_cast<std::uint16_t>(frame.type));
    writer.U16(0);
    writer.U32(frame.requestId);
    writer.Raw(frame.payload.data(), frame.payload.size());
    return writer.bytes;
}

struct Hello {
    std::uint32_t major = kProtocolMajor;
    std::uint32_t minor = kProtocolMinor;
    std::uint64_t capabilities = kCapabilities;
    std::string clientName;
};

struct Identity {
    std::int32_t result = 0;
    std::uint32_t major = kProtocolMajor;
    std::uint32_t minor = kProtocolMinor;
    std::uint64_t capabilities = kCapabilities;
    std::uint64_t instanceId = 0;
    std::uint64_t sessionId = 0;
    std::uint32_t processId = 0;
    std::uint32_t tcpPort = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t blockFrames = 0;
    std::string applicationName;
    std::string projectName;
    std::string engineVersion;
    std::string buildId;
    std::string machineName;
    std::string bindAddress;
    std::string platform;
};

struct Voice {
    std::uint64_t handle = 0;
    VnObjectId soundId{};
    std::uint32_t state = 0;
    float peak = 0;
};

struct Bus {
    VnObjectId id{};
    std::uint32_t generation = 0;
    float peak = 0;
};

struct Snapshot {
    std::int32_t result = 0;
    std::uint64_t blockIndex = 0;
    std::uint32_t activeVoices = 0;
    std::uint32_t busCount = 0;
    std::uint32_t callbackCpuUs = 0;
    std::uint32_t updateCpuUs = 0;
    std::uint64_t commandBackpressure = 0;
    std::uint64_t droppedNotifications = 0;
    std::vector<Voice> voices;
    std::vector<Bus> buses;
};

struct CatalogEntry {
    CatalogKind kind = CatalogKind::Action;
    VnObjectId id{};
    std::string name;
};

struct Catalog {
    std::int32_t result = 0;
    std::uint64_t generation = 0;
    std::uint32_t totalCount = 0;
    std::uint32_t offset = 0;
    std::vector<CatalogEntry> entries;
};

struct Command {
    Operation operation = Operation::StopAll;
    std::uint64_t expectedGeneration = 0;
    VnObjectId target{};
    std::uint64_t instanceHandle = 0;
    std::uint64_t secondary = 0;
    float value = 0;
    std::uint32_t durationMs = 0;
    std::string name;
};

struct CommandResult {
    std::int32_t result = 0;
    Operation operation = Operation::StopAll;
    std::uint64_t handle = 0;
};

struct MonitorRequest {
    std::uint32_t streamMask = 4;
    std::uint32_t cadenceHz = 20;
    std::uint32_t bytesPerSecond = 262144;
};

struct MonitorStatus {
    std::int32_t result = 0;
    std::uint64_t sessionEpoch = 0;
    std::uint64_t planGeneration = 0;
    std::uint32_t streamMask = 0;
};

struct MonitorRecords {
    std::uint64_t sessionEpoch = 0;
    std::uint32_t recordCount = 0;
    std::vector<std::uint8_t> bytes;
};

struct EventCaptureStatus {
    std::int32_t result = 0;
    std::uint32_t state = 0;
    std::uint64_t sessionEpoch = 0;
    std::uint64_t startedTimestampNs = 0;
    std::uint64_t stoppedTimestampNs = 0;
    std::uint64_t sourceDroppedRecords = 0;
    std::uint64_t deliveryDroppedRecords = 0;
    std::uint64_t transportDroppedRecords = 0;
};

struct EventCaptureRequestV2 {
    std::uint32_t executionFactVersion = 1;
    std::uint32_t requestedStreamKindMask = 0;
    std::uint32_t requestedPermissionMask = 0;
    std::uint32_t requestSymbols = 0;
    std::uint64_t consumerQueueBytes = 0;
};

struct EventCaptureStatusV2 {
    EventCaptureStatus base{};
    std::uint32_t acceptedExecutionFactVersion = 0;
    std::uint32_t baselineAvailability = 0;
    std::uint64_t supportedFactKinds[2]{};
    std::uint32_t requestedPermissionMask = 0;
    std::uint32_t acceptedPermissionMask = 0;
    std::uint32_t deniedPermissionMask = 0;
    std::uint32_t unsupportedPermissionMask = 0;
    std::uint32_t acceptedStreamKindMask = 0;
    std::uint32_t symbolsDecision = 0;
    std::uint32_t symbolAvailability = 0;
    std::uint32_t reserved0 = 0;
    std::uint64_t identityUnavailableCount = 0;
    std::uint64_t baselineStateCutId = 0;
};

inline void Encode(Writer &w, const Hello &v) {
    w.U32(v.major);
    w.U32(v.minor);
    w.U64(v.capabilities);
    w.String(v.clientName);
}
inline bool Decode(Reader &r, Hello &v) {
    return r.U32(v.major) && r.U32(v.minor) && r.U64(v.capabilities) &&
           r.String(v.clientName, 128) && r.Done();
}
inline void Encode(Writer &w, const Identity &v) {
    w.I32(v.result);
    w.U32(v.major);
    w.U32(v.minor);
    w.U64(v.capabilities);
    w.U64(v.instanceId);
    w.U64(v.sessionId);
    w.U32(v.processId);
    w.U32(v.tcpPort);
    w.U32(v.sampleRate);
    w.U32(v.blockFrames);
    w.String(v.applicationName);
    w.String(v.projectName);
    w.String(v.engineVersion);
    w.String(v.buildId);
    w.String(v.machineName);
    w.String(v.bindAddress);
    w.String(v.platform);
}
inline bool Decode(Reader &r, Identity &v) {
    return r.I32(v.result) && r.U32(v.major) && r.U32(v.minor) && r.U64(v.capabilities) &&
           r.U64(v.instanceId) && r.U64(v.sessionId) && r.U32(v.processId) && r.U32(v.tcpPort) &&
           r.U32(v.sampleRate) && r.U32(v.blockFrames) && r.String(v.applicationName, 128) &&
           r.String(v.projectName, 128) && r.String(v.engineVersion, 32) &&
           r.String(v.buildId, 128) && r.String(v.machineName, 128) &&
           r.String(v.bindAddress, 64) && r.String(v.platform, 32) && r.Done();
}
inline void Encode(Writer &w, const Snapshot &v) {
    w.I32(v.result);
    w.U64(v.blockIndex);
    w.U32(v.activeVoices);
    w.U32(v.busCount);
    w.U32(v.callbackCpuUs);
    w.U32(v.updateCpuUs);
    w.U64(v.commandBackpressure);
    w.U64(v.droppedNotifications);
    w.U32(static_cast<std::uint32_t>(v.voices.size()));
    for (const auto &row : v.voices) {
        w.U64(row.handle);
        w.ObjectId(row.soundId);
        w.U32(row.state);
        w.F32(row.peak);
    }
    w.U32(static_cast<std::uint32_t>(v.buses.size()));
    for (const auto &row : v.buses) {
        w.ObjectId(row.id);
        w.U32(row.generation);
        w.F32(row.peak);
    }
}
inline bool Decode(Reader &r, Snapshot &v) {
    std::uint32_t count = 0;
    if (!r.I32(v.result) || !r.U64(v.blockIndex) || !r.U32(v.activeVoices) || !r.U32(v.busCount) ||
        !r.U32(v.callbackCpuUs) || !r.U32(v.updateCpuUs) || !r.U64(v.commandBackpressure) ||
        !r.U64(v.droppedNotifications) || !r.U32(count) || count > 4096 ||
        count > r.Remaining() / 32)
        return false;
    v.voices.resize(count);
    for (auto &row : v.voices)
        if (!r.U64(row.handle) || !r.ObjectId(row.soundId) || !r.U32(row.state) || !r.F32(row.peak))
            return false;
    if (!r.U32(count) || count > 4096 || count > r.Remaining() / 24)
        return false;
    v.buses.resize(count);
    for (auto &row : v.buses)
        if (!r.ObjectId(row.id) || !r.U32(row.generation) || !r.F32(row.peak))
            return false;
    return r.Done();
}
inline void Encode(Writer &w, const Catalog &v) {
    w.I32(v.result);
    w.U64(v.generation);
    w.U32(v.totalCount);
    w.U32(v.offset);
    w.U32(static_cast<std::uint32_t>(v.entries.size()));
    for (const auto &row : v.entries) {
        w.U32(static_cast<std::uint32_t>(row.kind));
        w.ObjectId(row.id);
        w.String(row.name);
    }
}
inline bool Decode(Reader &r, Catalog &v) {
    std::uint32_t count = 0;
    if (!r.I32(v.result) || !r.U64(v.generation) || !r.U32(v.totalCount) || !r.U32(v.offset) ||
        !r.U32(count) || count > 16384 || count > r.Remaining() / 24)
        return false;
    v.entries.resize(count);
    for (auto &row : v.entries) {
        std::uint32_t kind = 0;
        if (!r.U32(kind) || kind < 1 || kind > 3 || !r.ObjectId(row.id) || !r.String(row.name, 65535))
            return false;
        row.kind = static_cast<CatalogKind>(kind);
    }
    return r.Done();
}
inline void Encode(Writer &w, const Command &v) {
    w.U32(static_cast<std::uint32_t>(v.operation));
    w.U64(v.expectedGeneration);
    w.ObjectId(v.target);
    w.U64(v.instanceHandle);
    w.U64(v.secondary);
    w.F32(v.value);
    w.U32(v.durationMs);
    w.String(v.name);
}
inline bool Decode(Reader &r, Command &v) {
    std::uint32_t op = 0;
    if (!r.U32(op) || !r.U64(v.expectedGeneration) || !r.ObjectId(v.target) || !r.U64(v.instanceHandle) || !r.U64(v.secondary) ||
        !r.F32(v.value) || !r.U32(v.durationMs) || !r.String(v.name) || !r.Done())
        return false;
    v.operation = static_cast<Operation>(op);
    return true;
}
inline void Encode(Writer &w, const CommandResult &v) {
    w.I32(v.result);
    w.U32(static_cast<std::uint32_t>(v.operation));
    w.U64(v.handle);
}
inline bool Decode(Reader &r, CommandResult &v) {
    std::uint32_t op = 0;
    if (!r.I32(v.result) || !r.U32(op) || !r.U64(v.handle) || !r.Done())
        return false;
    v.operation = static_cast<Operation>(op);
    return true;
}
inline void Encode(Writer &w, const MonitorRequest &v) {
    w.U32(v.streamMask);
    w.U32(v.cadenceHz);
    w.U32(v.bytesPerSecond);
}
inline bool Decode(Reader &r, MonitorRequest &v) {
    return r.U32(v.streamMask) && r.U32(v.cadenceHz) && r.U32(v.bytesPerSecond) && r.Done();
}
inline void Encode(Writer &w, const MonitorStatus &v) {
    w.I32(v.result);
    w.U64(v.sessionEpoch);
    w.U64(v.planGeneration);
    w.U32(v.streamMask);
}
inline bool Decode(Reader &r, MonitorStatus &v) {
    return r.I32(v.result) && r.U64(v.sessionEpoch) && r.U64(v.planGeneration) &&
           r.U32(v.streamMask) && r.Done();
}
inline void Encode(Writer &w, const MonitorRecords &v) {
    w.U64(v.sessionEpoch);
    w.U32(static_cast<std::uint32_t>(v.bytes.size()));
    w.U32(v.recordCount);
    w.Raw(v.bytes.data(), v.bytes.size());
}
inline bool Decode(Reader &r, MonitorRecords &v) {
    std::uint32_t count = 0;
    return r.U64(v.sessionEpoch) && r.U32(count) && count <= kMaxPayloadBytes - 16 &&
           r.U32(v.recordCount) && r.Raw(v.bytes, count) && r.Done();
}

inline void Encode(Writer &w, const EventCaptureStatus &v) {
    w.I32(v.result);
    w.U32(v.state);
    w.U64(v.sessionEpoch);
    w.U64(v.startedTimestampNs);
    w.U64(v.stoppedTimestampNs);
    w.U64(v.sourceDroppedRecords);
    w.U64(v.deliveryDroppedRecords);
    w.U64(v.transportDroppedRecords);
}
inline bool DecodeEventCaptureStatusPrefix(Reader &r, EventCaptureStatus &v) {
    return r.I32(v.result) && r.U32(v.state) && v.state <= 2 &&
        r.U64(v.sessionEpoch) && r.U64(v.startedTimestampNs) && r.U64(v.stoppedTimestampNs) &&
        r.U64(v.sourceDroppedRecords) && r.U64(v.deliveryDroppedRecords) &&
        r.U64(v.transportDroppedRecords);
}
inline bool Decode(Reader &r, EventCaptureStatus &v) {
    return DecodeEventCaptureStatusPrefix(r, v) && r.Done();
}

inline void Encode(Writer &w, const EventCaptureRequestV2 &v) {
    w.U32(v.executionFactVersion);
    w.U32(v.requestedStreamKindMask);
    w.U32(v.requestedPermissionMask);
    w.U32(v.requestSymbols);
    w.U64(v.consumerQueueBytes);
}
inline bool Decode(Reader &r, EventCaptureRequestV2 &v) {
    return r.U32(v.executionFactVersion) && r.U32(v.requestedStreamKindMask) &&
        r.U32(v.requestedPermissionMask) && r.U32(v.requestSymbols) && v.requestSymbols <= 1 &&
        r.U64(v.consumerQueueBytes) && r.Done();
}
inline void Encode(Writer &w, const EventCaptureStatusV2 &v) {
    Encode(w, v.base);
    w.U32(v.acceptedExecutionFactVersion);
    w.U32(v.baselineAvailability);
    for (auto kinds : v.supportedFactKinds) w.U64(kinds);
    w.U32(v.requestedPermissionMask);
    w.U32(v.acceptedPermissionMask);
    w.U32(v.deniedPermissionMask);
    w.U32(v.unsupportedPermissionMask);
    w.U32(v.acceptedStreamKindMask);
    w.U32(v.symbolsDecision);
    w.U32(v.symbolAvailability);
    w.U32(v.reserved0);
    w.U64(v.identityUnavailableCount);
    w.U64(v.baselineStateCutId);
}
inline bool Decode(Reader &r, EventCaptureStatusV2 &v) {
    return DecodeEventCaptureStatusPrefix(r, v.base) &&
        r.U32(v.acceptedExecutionFactVersion) && v.acceptedExecutionFactVersion <= 2 &&
        r.U32(v.baselineAvailability) && v.baselineAvailability <= 5 &&
        r.U64(v.supportedFactKinds[0]) && r.U64(v.supportedFactKinds[1]) &&
        r.U32(v.requestedPermissionMask) && r.U32(v.acceptedPermissionMask) &&
        r.U32(v.deniedPermissionMask) && r.U32(v.unsupportedPermissionMask) &&
        r.U32(v.acceptedStreamKindMask) && r.U32(v.symbolsDecision) && v.symbolsDecision <= 3 &&
        r.U32(v.symbolAvailability) && v.symbolAvailability <= 5 &&
        r.U32(v.reserved0) && v.reserved0 == 0 &&
        r.U64(v.identityUnavailableCount) && r.U64(v.baselineStateCutId) && r.Done() &&
        (v.acceptedPermissionMask & v.deniedPermissionMask) == 0 &&
        (v.acceptedPermissionMask & v.unsupportedPermissionMask) == 0 &&
        (v.deniedPermissionMask & v.unsupportedPermissionMask) == 0 &&
        (v.acceptedPermissionMask | v.deniedPermissionMask | v.unsupportedPermissionMask) ==
            v.requestedPermissionMask;
}

template <class T> Frame MakeFrame(Message type, std::uint32_t requestId, const T &value) {
    Writer writer;
    Encode(writer, value);
    return {type, requestId, std::move(writer.bytes)};
}
template <class T> bool DecodePayload(const Frame &frame, T &value) {
    Reader reader(frame.payload);
    return Decode(reader, value);
}

}

#endif
