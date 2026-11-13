// Architecture-independent codecs for the VTRC container. Public C structs
// are process ABI objects; they must never be copied into a persistent trace.
// This module is the only boundary that translates between those native
// structs and the fixed-width little-endian trace schema.
#ifndef HARKEN_API_TRACE_CODEC_H
#define HARKEN_API_TRACE_CODEC_H

#include <harken/Harken.h>

#include "TraceFormat.h"
#include "harken/core/CurrentWorldCodec.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace harken {

inline bool IsTransientPackTraceRefusal(VnResult result) noexcept {
    return result == VN_BACKPRESSURE || result == VN_PACK_RESIDENT_CAPACITY ||
        result == VN_NO_PACK || result == VN_NO_BUS || result == VN_DEVICE_ERROR;
}

struct TraceSessionOrigin {
    VnConfig requested{};
    std::uint32_t resolvedSampleRate = 0;
    std::uint32_t resolvedDeviceLayout = VN_CHANNEL_LAYOUT_DEFAULT;
};

// Converts between the native structSize vocabulary of the current build and
// the stable semantic vintage carried by VTRC. Zero means "not a known
// vintage". Native byte sizes may differ between 32-bit and 64-bit builds.
std::uint32_t ConfigVintageFromNativeSize(
    std::uint32_t nativeSize) noexcept;
std::uint32_t NativeConfigSizeForVintage(
    std::uint32_t vintage) noexcept;

// Encodes the declared native config into the fixed v4 schema. Fields absent
// from an older caller vintage become zero, matching Vn_Init's defaulting
// contract. The four process-local pointer fields are never inspected or
// serialized. Decode always produces a current-native VnConfig with null
// pointers; semanticVintage reports the caller vocabulary recorded on wire.
bool EncodeTraceConfig(const VnConfig& config,
                       std::vector<std::uint8_t>& out) noexcept;
bool EncodeTraceConfig(const TraceSessionOrigin& origin,
                       std::vector<std::uint8_t>& out) noexcept;
bool DecodeTraceConfig(const std::uint8_t* bytes, std::size_t byteCount,
                       VnConfig* out,
                       std::uint32_t* semanticVintage,
                       std::uint32_t* resolvedSampleRate = nullptr,
                       std::uint32_t* resolvedDeviceLayout = nullptr) noexcept;

bool EncodeTraceCurrentWorldBody(
    const ContainerWorldInstallViewV1& world,
    std::vector<std::uint8_t>& out) noexcept;

using TraceCurrentWorldImage = CurrentWorldBodyImage;

} // namespace harken

#endif // HARKEN_API_TRACE_CODEC_H
