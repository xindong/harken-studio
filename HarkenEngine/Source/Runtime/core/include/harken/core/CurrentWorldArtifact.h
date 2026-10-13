// DirectWorld preview envelope (magic VNCWORLD). The body bytes are the
// same EncodeCurrentWorldShippingBody payload as CanonicalDefinitionPartition,
// and the header bodyVintage is kCurrentWorldShippingBodyVintage — the
// same source as kCanonicalDefinitionPartitionBodyVintage. Studio preview
// may load this file; games load .pack. Not a third project/pack disk
// contract — do not register in schema catalog.
#ifndef HARKEN_CORE_CURRENT_WORLD_ARTIFACT_H
#define HARKEN_CORE_CURRENT_WORLD_ARTIFACT_H

#include "harken/core/CurrentWorld.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace harken {

class CurrentWorldBodyImage;

inline constexpr std::uint32_t kCurrentWorldArtifactHeaderBytes = 64;
inline constexpr std::uint64_t kCurrentWorldArtifactMaxBodyBytes =
    UINT64_C(67108864);

enum class CurrentWorldLoadStatusV1 : std::uint32_t {
    Ok = 0,
    InvalidArgument = 1,
    IoError = 2,
    TooLarge = 3,
    BadMagic = 4,
    UnsupportedEnvelope = 5,
    UnsupportedBody = 6,
    Truncated = 7,
    SizeMismatch = 8,
    ChecksumMismatch = 9,
    DecodeFailed = 10,
    SemanticInvalid = 11,
    CapabilityUnavailable = 12,
    Capacity = 13,
    Backpressure = 14,
    RuntimeRecreateRequired = 15,
    NotInitialized = 16,
};

struct CurrentWorldArtifactHeaderV1 {
    std::uint64_t bodyBytes = 0;
    std::uint64_t bodyChecksum = 0;
    std::uint64_t runtimeSemanticFingerprint = 0;
    std::uint64_t capabilityMask = 0;
};

CurrentWorldLoadStatusV1 InspectCurrentWorldArtifactHeaderV1(
    const std::uint8_t* bytes, std::size_t capturedHeaderBytes,
    std::uint64_t observedFileBytes,
    CurrentWorldArtifactHeaderV1& outHeader) noexcept;

CurrentWorldLoadStatusV1 DecodeCurrentWorldArtifactBodyV1(
    const CurrentWorldArtifactHeaderV1& header, const std::uint8_t* body,
    std::size_t bodyBytes, CurrentWorldBodyImage& outImage) noexcept;

bool EncodeCurrentWorldArtifactV1(
    const ContainerWorldInstallViewV1& world,
    std::vector<std::uint8_t>& out) noexcept;

} // namespace harken

#endif // HARKEN_CORE_CURRENT_WORLD_ARTIFACT_H
