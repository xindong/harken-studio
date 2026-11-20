#include "TraceCodec.h"
#include "harken/param/VariableService.h"

#include <array>
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace harken {

namespace {

constexpr std::array<std::uint32_t, kTraceConfigVintageCount>
NativeConfigVintages() noexcept {
    return {
        static_cast<std::uint32_t>(offsetof(VnConfig, onNotification)),
        static_cast<std::uint32_t>(offsetof(VnConfig, logCallback)),
        static_cast<std::uint32_t>(
            offsetof(VnConfig, delayEffectPoolCapacity)),
        static_cast<std::uint32_t>(offsetof(VnConfig, streamRingFrames)),
        static_cast<std::uint32_t>(offsetof(VnConfig, maxActions)),
        static_cast<std::uint32_t>(offsetof(VnConfig, maxTriggerDepth)),
        static_cast<std::uint32_t>(offsetof(VnConfig, mixChannelLayout)),
        static_cast<std::uint32_t>(offsetof(VnConfig, maxBindings)),
        static_cast<std::uint32_t>(
            offsetof(VnConfig, gainEffectPoolCapacity)),
        static_cast<std::uint32_t>(
            offsetof(VnConfig, limiterEffectPoolCapacity)),
        static_cast<std::uint32_t>(
            offsetof(VnConfig, compressorEffectPoolCapacity)),
        static_cast<std::uint32_t>(
            offsetof(VnConfig, maxBusTopologyOwners)),
        static_cast<std::uint32_t>(
            offsetof(VnConfig, maxTopologyPlanBytes) +
            sizeof(VnConfig::maxTopologyPlanBytes)),
        static_cast<std::uint32_t>(
            offsetof(VnConfig, reserved10) +
            sizeof(VnConfig::reserved10)),
        static_cast<std::uint32_t>(
            offsetof(VnConfig, maxPackReplacementPeakBytes) +
            sizeof(VnConfig::maxPackReplacementPeakBytes)),
        static_cast<std::uint32_t>(
            offsetof(VnConfig, reserved12) +
            sizeof(VnConfig::reserved12)),
        static_cast<std::uint32_t>(
            offsetof(VnConfig, streamRingBytes)),
        static_cast<std::uint32_t>(offsetof(VnConfig, reserved13)),
        static_cast<std::uint32_t>(sizeof(VnConfig)),
    };
}

bool FieldPresent(std::uint32_t declared, std::size_t offset,
                  std::size_t bytes) noexcept {
    return offset <= declared && bytes <= declared - offset;
}

void AppendU32(std::vector<std::uint8_t>& out,
               std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 24));
}

void AppendU8(std::vector<std::uint8_t>& out,
              std::uint8_t value) {
    out.push_back(value);
}

void AppendU16(std::vector<std::uint8_t>& out,
               std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void AppendU64(std::vector<std::uint8_t>& out,
               std::uint64_t value) {
    AppendU32(out, static_cast<std::uint32_t>(value));
    AppendU32(out, static_cast<std::uint32_t>(value >> 32));
}

void AppendF32(std::vector<std::uint8_t>& out, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    AppendU32(out, bits);
}

struct Reader {
    const std::uint8_t* bytes = nullptr;
    std::size_t size = 0;
    std::size_t offset = 0;
    bool failed = false;

    std::uint8_t U8() noexcept {
        if (failed || size - offset < 1) {
            failed = true;
            return 0;
        }
        return bytes[offset++];
    }

    std::uint16_t U16() noexcept {
        if (failed || size - offset < 2) {
            failed = true;
            return 0;
        }
        const std::uint16_t value =
            static_cast<std::uint16_t>(bytes[offset]) |
            static_cast<std::uint16_t>(bytes[offset + 1] << 8);
        offset += 2;
        return value;
    }

    std::uint32_t U32() noexcept {
        if (failed || size - offset < 4) {
            failed = true;
            return 0;
        }
        const std::uint32_t value =
            static_cast<std::uint32_t>(bytes[offset]) |
            (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
            (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
            (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
        offset += 4;
        return value;
    }

    std::uint64_t U64() noexcept {
        const std::uint64_t lo = U32();
        const std::uint64_t hi = U32();
        return lo | (hi << 32);
    }

    float F32() noexcept {
        const std::uint32_t bits = U32();
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof value);
        return value;
    }
};

struct PrefixEncodeVisitor {
    std::vector<std::uint8_t>& out;
    void U32(std::uint32_t value) { AppendU32(out, value); }
    void U64(std::uint64_t value) { AppendU64(out, value); }
};

struct PrefixDecodeVisitor {
    Reader& reader;
    void U32(std::uint32_t& value) noexcept { value = reader.U32(); }
    void U64(std::uint64_t& value) noexcept { value = reader.U64(); }
};

#define HARKEN_TRACE_CONFIG_U32_FIELDS(X)                                    \
    X(sampleRate)                                                           \
    X(blockFrames)                                                          \
    X(maxVoices)                                                            \
    X(fxSlotsPerVoice)                                                      \
    X(arenaBytes)                                                           \
    X(deviceMode)                                                           \
    X(maxBuses)                                                             \
    X(commandQueueDepth)                                                    \
    X(notificationQueueDepth)                                               \
    X(delayEffectPoolCapacity)                                              \
    X(toneEffectPoolCapacity)                                               \
    X(streamRingFrames)                                                     \
    X(maxStreams)                                                           \
    X(prebufferMs)                                                          \
    X(starvationCooldownBlocks)                                             \
    X(maxPacks)                                                             \
    X(maxActions)                                                           \
    X(maxContainers)                                                        \
    X(maxContainerChildren)                                                 \
    X(containerCursorCapacity)                                              \
    X(pendingActionCapacity)                                                \
    X(maxEmitters)                                                          \
    X(emitterVariableSlots)                                                 \
    X(maxParkedVoices)                                                      \
    X(voiceStealPolicy)                                                     \
    X(audibilityParkEnabled)                                                \
    X(randomSeed)                                                           \
    X(voiceSendSlots)                                                       \
    X(busSendSlotsPerBus)                                                   \
    X(fxSlotsPerBus)                                                        \
    X(topologyPlanPoolCapacity)                                             \
    X(maxTriggerDepth)                                                      \
    X(mixChannelLayout)                                                     \
    X(deviceChannelLayout)                                                  \
    X(renderWorkerCount)                                                    \
    X(maxBindings)                                                          \
    X(maxBindingAttachments)                                                \
    X(voiceBindingInstanceSlots)                                            \
    X(meterRingBlocks)                                                      \
    X(gainEffectPoolCapacity)                                               \
    X(limiterEffectPoolCapacity)                                            \
    X(compressorEffectPoolCapacity)                                         \
    X(dynamicRouteCapacity)                                                 \
    X(dynamicRoutePlanPoolCapacity)                                         \
    X(dynamicBusTapCapacity)                                                \
    X(dynamicVoiceTapCapacity)                                              \
    X(maxBusTopologyOwners)                                                 \
    X(maxControlCommandsPerQuantum)

} // namespace

std::uint32_t ConfigVintageFromNativeSize(
    std::uint32_t nativeSize) noexcept {
    const auto sizes = NativeConfigVintages();
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        if (sizes[i] == nativeSize) {
            return static_cast<std::uint32_t>(i + 1);
        }
    }
    return 0;
}

std::uint32_t NativeConfigSizeForVintage(
    std::uint32_t vintage) noexcept {
    if (vintage == 0 || vintage > kTraceConfigVintageCount) {
        return 0;
    }
    return NativeConfigVintages()[vintage - 1];
}

bool EncodeTraceConfig(const VnConfig& config,
                       std::vector<std::uint8_t>& out) noexcept {
    TraceSessionOrigin origin;
    std::memcpy(&origin.requested, &config,
                std::min<std::size_t>(config.structSize, sizeof(VnConfig)));
    origin.resolvedSampleRate =
        origin.requested.sampleRate != 0 ? origin.requested.sampleRate : 48000u;
    origin.resolvedDeviceLayout =
        origin.requested.deviceChannelLayout != VN_CHANNEL_LAYOUT_DEFAULT
            ? origin.requested.deviceChannelLayout
            : origin.requested.mixChannelLayout != VN_CHANNEL_LAYOUT_DEFAULT
                  ? origin.requested.mixChannelLayout
                  : VN_CHANNEL_LAYOUT_STEREO;
    return EncodeTraceConfig(origin, out);
}

bool EncodeTraceConfig(const TraceSessionOrigin& origin,
                       std::vector<std::uint8_t>& out) noexcept {
    VnConfig config{};
    std::memcpy(&config, &origin.requested,
                std::min<std::size_t>(origin.requested.structSize, sizeof(VnConfig)));
    const std::uint32_t vintage =
        ConfigVintageFromNativeSize(config.structSize);
    if (vintage == 0 || origin.resolvedSampleRate == 0 ||
        origin.resolvedDeviceLayout == VN_CHANNEL_LAYOUT_DEFAULT ||
        (vintage >= 14 &&
         (config.pendingActionCapacity != 0 || config.reserved10 != 0)) ||
        (vintage >= 15 && config.reserved11 != 0) ||
        (vintage >= 16 && config.reserved12 != 0)) {
        return false;
    }
    if (vintage < 14) config.pendingActionCapacity = 0;
    if (vintage <= 16) {
        if (vintage < 5 || !config.streamRingFrames) config.streamRingFrames = 16384;
        config.streamRingBytes = 0;
    } else if (vintage == 17) {
        config.streamRingBytes = config.streamRingFrames;
        config.streamRingFrames = 0;
    } else if (config.reserved13 || config.reserved14 ||
               (config.streamRingFrames && config.streamRingBytes)) {
        return false;
    }
    try {
        out.clear();
        out.reserve(kTraceConfigWireBytes);
        AppendU32(out, kTraceConfigSchemaVersion);
        AppendU32(out, kTraceConfigVintageCount);
        AppendU32(out, origin.resolvedSampleRate);
        AppendU32(out, origin.resolvedDeviceLayout);
        // config is a zero-initialized copy bounded by the caller's native size.
        // Its stream fields above already express the original capacity units.
#define HARKEN_ENCODE_CONFIG_FIELD(field) AppendU32(out, static_cast<std::uint32_t>(config.field));
        HARKEN_TRACE_CONFIG_U32_FIELDS(HARKEN_ENCODE_CONFIG_FIELD)
#undef HARKEN_ENCODE_CONFIG_FIELD
        std::uint32_t parkBits = 0;
        if (FieldPresent(config.structSize,
                         offsetof(VnConfig, parkThresholdDb),
                         sizeof(VnConfig::parkThresholdDb))) {
            static_assert(sizeof(parkBits) == sizeof(config.parkThresholdDb));
            std::memcpy(&parkBits, &config.parkThresholdDb, sizeof parkBits);
        }
        AppendU32(out, parkBits);
        const std::uint64_t topologyBytes =
            FieldPresent(config.structSize,
                         offsetof(VnConfig, maxTopologyPlanBytes),
                         sizeof(VnConfig::maxTopologyPlanBytes))
                ? config.maxTopologyPlanBytes
                : 0;
        AppendU64(out, topologyBytes);
#define HARKEN_ENCODE_TAIL_U32(field) \
        AppendU32(out, FieldPresent(config.structSize, offsetof(VnConfig, field), sizeof(config.field)) ? config.field : 0u)
#define HARKEN_ENCODE_TAIL_U64(field) \
        AppendU64(out, FieldPresent(config.structSize, offsetof(VnConfig, field), sizeof(config.field)) ? config.field : 0u)
        HARKEN_ENCODE_TAIL_U32(maxActionFirings);
        HARKEN_ENCODE_TAIL_U32(maxCatalogEntries);
        HARKEN_ENCODE_TAIL_U64(maxCatalogBytes);
        HARKEN_ENCODE_TAIL_U64(maxResidentDefinitionBytes);
        HARKEN_ENCODE_TAIL_U64(maxCompositeIndexBytes);
        HARKEN_ENCODE_TAIL_U64(maxPackDecodeStagingBytes);
        HARKEN_ENCODE_TAIL_U64(maxPackResourceBytes);
        HARKEN_ENCODE_TAIL_U64(maxPackReplacementPeakBytes);
        HARKEN_ENCODE_TAIL_U32(maxActionCooldownRows);
#undef HARKEN_ENCODE_TAIL_U32
#undef HARKEN_ENCODE_TAIL_U64
        AppendU32(out, config.streamRingBytes);
        return out.size() == kTraceConfigWireBytes;
    } catch (...) {
        out.clear();
        return false;
    }
}

bool DecodeTraceConfig(const std::uint8_t* bytes, std::size_t byteCount,
                       VnConfig* out,
                       std::uint32_t* semanticVintage,
                       std::uint32_t* resolvedSampleRate,
                       std::uint32_t* resolvedDeviceLayout) noexcept {
    if (bytes == nullptr || out == nullptr || semanticVintage == nullptr ||
        byteCount < 4 * sizeof(std::uint32_t)) {
        return false;
    }
    Reader reader{bytes, byteCount};
    const std::uint32_t schema = reader.U32();
    const std::uint32_t vintage = reader.U32();
    const std::uint32_t resolvedRate = reader.U32();
    const std::uint32_t resolvedLayout = reader.U32();
    if (schema != kTraceConfigSchemaVersion || vintage == 0 ||
        vintage > kTraceConfigVintageCount || resolvedRate == 0 ||
        resolvedLayout == VN_CHANNEL_LAYOUT_DEFAULT) {
        return false;
    }
    constexpr std::uint32_t tailSizes[] = {
        kTraceConfigWireBytesV14, kTraceConfigWireBytesV15, kTraceConfigWireBytesV16,
        kTraceConfigWireBytesV17, kTraceConfigWireBytesV18, kTraceConfigWireBytes};
    static_assert(sizeof(tailSizes) / sizeof(tailSizes[0]) == kTraceConfigVintageCount - 13);
    const auto expected = vintage <= 13 ? kTraceConfigWireBytesV13 : tailSizes[vintage - 14];
    if (byteCount != expected) return false;
    VnConfig decoded{};
    decoded.structSize = sizeof(VnConfig);
#define HARKEN_DECODE_CONFIG_FIELD(field) decoded.field = reader.U32();
    HARKEN_TRACE_CONFIG_U32_FIELDS(HARKEN_DECODE_CONFIG_FIELD)
#undef HARKEN_DECODE_CONFIG_FIELD
    const std::uint32_t parkBits = reader.U32();
    std::memcpy(&decoded.parkThresholdDb, &parkBits, sizeof parkBits);
    decoded.maxTopologyPlanBytes = reader.U64();
    if (vintage >= 14) {
        decoded.maxActionFirings = reader.U32();
    }
    if (vintage >= 15) {
        decoded.maxCatalogEntries = reader.U32();
        decoded.maxCatalogBytes = reader.U64();
        decoded.maxResidentDefinitionBytes = reader.U64();
        decoded.maxCompositeIndexBytes = reader.U64();
        decoded.maxPackDecodeStagingBytes = reader.U64();
        decoded.maxPackResourceBytes = reader.U64();
        decoded.maxPackReplacementPeakBytes = reader.U64();
    }
    if (vintage >= 16) {
        decoded.maxActionCooldownRows = reader.U32();
    }
    if (vintage == 17) {
        (void)reader.U64();
    }
    if (vintage <= 16) {
        if (vintage < 5 || !decoded.streamRingFrames) decoded.streamRingFrames = 16384;
    } else if (vintage <= 18) {
        decoded.streamRingBytes = decoded.streamRingFrames;
        decoded.streamRingFrames = 0;
    } else {
        decoded.streamRingBytes = reader.U32();
        if (decoded.streamRingFrames && decoded.streamRingBytes) return false;
    }
    const std::uint32_t historicalPendingCapacity =
        decoded.pendingActionCapacity;
    decoded.pendingActionCapacity = 0;
    if (vintage >= 14 && historicalPendingCapacity != 0) {
        return false;
    }
    if (reader.failed || reader.offset != reader.size) {
        return false;
    }
    decoded.onNotification = nullptr;
    decoded.notificationUserData = nullptr;
    decoded.logCallback = nullptr;
    decoded.logUserData = nullptr;
    *out = decoded;
    *semanticVintage = vintage;
    if (resolvedSampleRate != nullptr) {
        *resolvedSampleRate = resolvedRate;
    }
    if (resolvedDeviceLayout != nullptr) {
        *resolvedDeviceLayout = resolvedLayout;
    }
    return true;
}

bool EncodeTraceCurrentWorldBody(
    const ContainerWorldInstallViewV1& world,
    std::vector<std::uint8_t>& out) noexcept {
    if (world.sourceBodyVintage == kCurrentWorldBodyVintageV20)
        return EncodeCurrentWorldBodyV20(world, out);
    if (world.sourceBodyVintage == kCurrentWorldBodyVintageV19)
        return EncodeCurrentWorldBodyV19(world, out);
    return EncodeCurrentWorldShippingBodyRawAttempt(world, out);
}

} // namespace harken
