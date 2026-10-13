#include "harken/core/CurrentWorldArtifact.h"

#include "harken/core/CurrentWorldCodec.h"
#include "harken/media/PackFormat.h"

#include <array>
#include <cstring>
#include <limits>

namespace harken {
namespace {

void AppendU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void AppendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    AppendU16(out, static_cast<std::uint16_t>(value));
    AppendU16(out, static_cast<std::uint16_t>(value >> 16));
}

void AppendU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    AppendU32(out, static_cast<std::uint32_t>(value));
    AppendU32(out, static_cast<std::uint32_t>(value >> 32));
}

std::uint16_t ReadU16(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>(bytes[0]) |
           (static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t ReadU32(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint32_t>(ReadU16(bytes)) |
           (static_cast<std::uint32_t>(ReadU16(bytes + 2)) << 16);
}

std::uint64_t ReadU64(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint64_t>(ReadU32(bytes)) |
           (static_cast<std::uint64_t>(ReadU32(bytes + 4)) << 32);
}

} // namespace

CurrentWorldLoadStatusV1 InspectCurrentWorldArtifactHeaderV1(
    const std::uint8_t* bytes, std::size_t capturedHeaderBytes,
    std::uint64_t observedFileBytes,
    CurrentWorldArtifactHeaderV1& outHeader) noexcept {
    outHeader = {};
    if (bytes == nullptr && capturedHeaderBytes != 0) {
        return CurrentWorldLoadStatusV1::InvalidArgument;
    }
    if (observedFileBytes < kCurrentWorldArtifactHeaderBytes ||
        capturedHeaderBytes < kCurrentWorldArtifactHeaderBytes) {
        return CurrentWorldLoadStatusV1::Truncated;
    }

    constexpr std::array<std::uint8_t, 8> magic = {
        'V', 'N', 'C', 'W', 'O', 'R', 'L', 'D'};
    if (std::memcmp(bytes, magic.data(), magic.size()) != 0) {
        return CurrentWorldLoadStatusV1::BadMagic;
    }
    if (ReadU16(bytes + 8) != 1 || ReadU16(bytes + 10) != 0 ||
        ReadU32(bytes + 12) != kCurrentWorldArtifactHeaderBytes ||
        ReadU64(bytes + 56) != 0) {
        return CurrentWorldLoadStatusV1::UnsupportedEnvelope;
    }
    if (ReadU32(bytes + 16) != 1 ||
        ReadU32(bytes + 20) != kCurrentWorldShippingBodyVintage) {
        return CurrentWorldLoadStatusV1::UnsupportedBody;
    }

    outHeader.bodyBytes = ReadU64(bytes + 24);
    outHeader.bodyChecksum = ReadU64(bytes + 32);
    outHeader.runtimeSemanticFingerprint = ReadU64(bytes + 40);
    outHeader.capabilityMask = ReadU64(bytes + 48);
    if (outHeader.bodyBytes > kCurrentWorldArtifactMaxBodyBytes) {
        return CurrentWorldLoadStatusV1::TooLarge;
    }

    const std::uint64_t expectedFileBytes =
        kCurrentWorldArtifactHeaderBytes + outHeader.bodyBytes;
    if (expectedFileBytes > observedFileBytes) {
        return CurrentWorldLoadStatusV1::Truncated;
    }
    if (expectedFileBytes < observedFileBytes) {
        return CurrentWorldLoadStatusV1::SizeMismatch;
    }
    return CurrentWorldLoadStatusV1::Ok;
}

CurrentWorldLoadStatusV1 DecodeCurrentWorldArtifactBodyV1(
    const CurrentWorldArtifactHeaderV1& header, const std::uint8_t* body,
    std::size_t bodyBytes, CurrentWorldBodyImage& outImage) noexcept {
    if (body == nullptr && bodyBytes != 0) {
        return CurrentWorldLoadStatusV1::InvalidArgument;
    }
    if (header.bodyBytes > kCurrentWorldArtifactMaxBodyBytes) {
        return CurrentWorldLoadStatusV1::TooLarge;
    }
    if (bodyBytes < header.bodyBytes) {
        return CurrentWorldLoadStatusV1::Truncated;
    }
    if (bodyBytes > header.bodyBytes) {
        return CurrentWorldLoadStatusV1::SizeMismatch;
    }
    if (Fnv1a64(body, bodyBytes) != header.bodyChecksum) {
        return CurrentWorldLoadStatusV1::ChecksumMismatch;
    }
    const CurrentWorldBodyDecodeResult decode = outImage.DecodeWithResult(
        body, bodyBytes, kCurrentWorldShippingBodyVintage,
        CurrentWorldDecodeMode::StrictArtifact);
    if (decode == CurrentWorldBodyDecodeResult::Capacity) {
        return CurrentWorldLoadStatusV1::Capacity;
    }
    if (decode != CurrentWorldBodyDecodeResult::Ok) {
        return CurrentWorldLoadStatusV1::DecodeFailed;
    }
    const auto& identity = outImage.ProductionView()->identity;
    if (identity.runtimeSemanticFingerprint !=
            header.runtimeSemanticFingerprint ||
        identity.capabilityMask != header.capabilityMask) {
        return CurrentWorldLoadStatusV1::SemanticInvalid;
    }
    return CurrentWorldLoadStatusV1::Ok;
}

bool EncodeCurrentWorldArtifactV1(
    const ContainerWorldInstallViewV1& world,
    std::vector<std::uint8_t>& out) noexcept {
    try {
        std::vector<std::uint8_t> body;
        if (!EncodeCurrentWorldShippingBody(world, body) ||
            body.size() > kCurrentWorldArtifactMaxBodyBytes ||
            body.size() > std::numeric_limits<std::size_t>::max() -
                              kCurrentWorldArtifactHeaderBytes) {
            out.clear();
            return false;
        }
        out.clear();
        out.reserve(kCurrentWorldArtifactHeaderBytes + body.size());
        const std::uint8_t magic[] = {'V', 'N', 'C', 'W',
                                      'O', 'R', 'L', 'D'};
        out.insert(out.end(), std::begin(magic), std::end(magic));
        AppendU16(out, 1);
        AppendU16(out, 0);
        AppendU32(out, kCurrentWorldArtifactHeaderBytes);
        AppendU32(out, 1);
        AppendU32(out, kCurrentWorldShippingBodyVintage);
        AppendU64(out, body.size());
        AppendU64(out, Fnv1a64(body.data(), body.size()));
        AppendU64(out, world.identity.runtimeSemanticFingerprint);
        AppendU64(out, world.identity.capabilityMask);
        AppendU64(out, 0);
        out.insert(out.end(), body.begin(), body.end());
        return true;
    } catch (...) {
        out.clear();
        return false;
    }
}

} // namespace harken
