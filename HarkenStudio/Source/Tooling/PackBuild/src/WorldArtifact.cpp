#include "harken_studio_build/WorldArtifact.h"

#include "harken_studio_build/WorldCodec.h"
#include "harken_studio_build/PackFormat.h"

#include <array>
#include <limits>

namespace harkenstudiobuild {
namespace {

void AppendU16(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void AppendU32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    AppendU16(out, static_cast<std::uint16_t>(value));
    AppendU16(out, static_cast<std::uint16_t>(value >> 16));
}

void AppendU64(std::vector<std::uint8_t> &out, std::uint64_t value) {
    AppendU32(out, static_cast<std::uint32_t>(value));
    AppendU32(out, static_cast<std::uint32_t>(value >> 32));
}

} // namespace

bool EncodeCurrentWorldArtifactV1(const ContainerWorldInstallViewV1 &world,
                                  std::vector<std::uint8_t> &out) noexcept {
    try {
        std::vector<std::uint8_t> body;
        if (!EncodeCurrentWorldShippingBody(world, body) ||
            body.size() > kCurrentWorldArtifactMaxBodyBytes ||
            body.size() >
                std::numeric_limits<std::size_t>::max() - kCurrentWorldArtifactHeaderBytes) {
            out.clear();
            return false;
        }
        out.clear();
        out.reserve(kCurrentWorldArtifactHeaderBytes + body.size());
        const std::uint8_t magic[] = {'V', 'N', 'C', 'W', 'O', 'R', 'L', 'D'};
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

} // namespace harkenstudiobuild
