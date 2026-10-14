#ifndef HARKEN_CORE_VARIABLE_READ_CUT_H
#define HARKEN_CORE_VARIABLE_READ_CUT_H

#include <harken/ObjectId.h>

#include <cstdint>

namespace harken {

struct CommandRecord;
class VariableServiceV1;
class VoicePool;

struct VariableReadCutRef {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;

    bool Valid() const noexcept {
        return slot != UINT32_MAX && generation != 0;
    }
    bool operator==(const VariableReadCutRef&) const = default;
};

struct VariableTimelineReadUseRef {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;
    bool Valid() const noexcept { return slot != UINT32_MAX && generation != 0; }
};

enum class VariableReadCutStatus : std::uint8_t {
    Waiting,
    Applied,
    Failed,
};

enum class VariableReadCutValueStatus : std::uint8_t {
    Missing,
    Value,
    Failed,
};

enum class VariableNativeStartReceipt : std::uint8_t {
    None,
    Prepared,
    Pending,
    Installed,
    Activated,
    Failed,
};

struct VariableNativeStartIdentity {
    VariableReadCutRef cut{};
    std::uint64_t nativeHandle = 0;
    std::uint64_t world = 0;
    std::uint64_t transport = 0;
    std::uint64_t startFrame = 0;
    std::uint64_t sequence = 0;
    std::uint32_t executionOrdinalExclusive = 0;
};

struct VariablePlaybackOrder {
    std::uint64_t frame = 0;
    std::uint64_t sequence = 0;
    std::uint32_t ordinal = 0;
};

// Valid only while the matching native Arm audio reservation remains held.
class VariableTimelineInitializerProof {
public:
    VariableTimelineInitializerProof() = default;
    VariableTimelineInitializerProof(const VariableTimelineInitializerProof&) = delete;
    VariableTimelineInitializerProof& operator=(const VariableTimelineInitializerProof&) = delete;
    bool Valid() const noexcept { return row_ != nullptr; }
    void Reset() noexcept {
        row_ = nullptr;
        nativeHandle_ = 0;
        commands_ = nullptr;
        commandCount_ = 0;
        order_ = {};
    }
private:
    friend class VariableServiceV1;
    friend class VoicePool;
    void* row_ = nullptr;
    std::uint64_t nativeHandle_ = 0;
    const CommandRecord* commands_ = nullptr;
    std::uint32_t commandCount_ = 0;
    VariablePlaybackOrder order_{};
};

struct VariableNativeEffectIdentity {
    std::uint32_t type = 0;
    std::uint32_t poolIndex = UINT32_MAX;
    std::uint64_t generation = 0;
};

struct VariableToneBandValues {
    float gainDb = 0.0f;
    float frequencyHz = 0.0f;
    float q = 1.0f;
    bool valid = false;
};

struct VariableNativeStartAuthority {
    void* context = nullptr;
    bool (*installed)(void*, const VariableNativeStartIdentity&) noexcept = nullptr;
    VariableReadCutStatus (*ready)(void*, const VariableNativeStartIdentity&) noexcept = nullptr;
    bool (*finish)(void*, const VariableNativeStartIdentity&, bool) noexcept = nullptr;

    bool Valid() const noexcept {
        return installed != nullptr && ready != nullptr && finish != nullptr;
    }
};

// Synchronous logic-side evaluation only; consumers retain the token separately.
struct VariableReadCutReader {
    void* context = nullptr;
    VariableReadCutRef cut{};
    VariableReadCutValueStatus (*read)(void*, VariableReadCutRef, VnObjectId, float*) noexcept = nullptr;

    VariableReadCutValueStatus Read(VnObjectId variable, float* out) const noexcept {
        return read == nullptr ? VariableReadCutValueStatus::Failed : read(context, cut, variable, out);
    }
};

} // namespace harken

#endif // HARKEN_CORE_VARIABLE_READ_CUT_H
