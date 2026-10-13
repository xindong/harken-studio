#ifndef HARKEN_MEDIA_CANONICAL_ATTENUATION_FINGERPRINT_H
#define HARKEN_MEDIA_CANONICAL_ATTENUATION_FINGERPRINT_H

#include <harken/BusDefinitionFingerprintValue.h>
#include <harken/media/PackLogicImage.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace harken {

inline std::uint64_t CanonicalAttenuationDefinitionRevisionV1(
    const PackLogicImage::Attenuation& attenuation) noexcept {
    const auto finite = [](float value) noexcept {
        return std::isfinite(value);
    };
    const auto& d = attenuation.desc;
    const auto curveFinite = [&](const VnAttenuationCurve& c) noexcept {
        if (c.enabled > 1 || c.pointCount > VN_ATTENUATION_MAX_CURVE_POINTS) return false;
        if (c.enabled == 0 && c.pointCount != 0) return false;
        for (std::uint32_t i = 0; i < c.pointCount; ++i) {
            if (!finite(c.points[i].x) || !finite(c.points[i].y)) return false;
        }
        for (std::uint32_t i = 0; i + 1 < c.pointCount; ++i) {
            if (!finite(c.handles[i].h1x) || !finite(c.handles[i].h1y) ||
                !finite(c.handles[i].h2x) || !finite(c.handles[i].h2y)) return false;
        }
        for (std::uint32_t i = c.pointCount; i < VN_ATTENUATION_MAX_CURVE_POINTS; ++i) {
            if (busfingerprint::CanonicalFloatBits(c.points[i].x) != 0 ||
                busfingerprint::CanonicalFloatBits(c.points[i].y) != 0) return false;
        }
        const std::uint32_t usedHandles = c.pointCount > 0 ? c.pointCount - 1 : 0;
        for (std::uint32_t i = usedHandles; i < VN_ATTENUATION_MAX_CURVE_POINTS - 1; ++i) {
            if (busfingerprint::CanonicalFloatBits(c.handles[i].h1x) != 0 ||
                busfingerprint::CanonicalFloatBits(c.handles[i].h1y) != 0 ||
                busfingerprint::CanonicalFloatBits(c.handles[i].h2x) != 0 ||
                busfingerprint::CanonicalFloatBits(c.handles[i].h2y) != 0) return false;
        }
        return true;
    };
    if (attenuation.id == VnObjectId{} || d.structSize != sizeof(VnAttenuationDesc) ||
        d.coneEnabled > 1 || d.reserved0 != 0 ||
        !finite(d.minDistance) || !finite(d.maxDistance) ||
        !finite(d.airAbsorptionNearHz) || !finite(d.airAbsorptionFarHz) ||
        !finite(d.proximityRadius) || !finite(d.proximityMaxBoostDb) ||
        !finite(d.coneInnerDeg) || !finite(d.coneOuterDeg) ||
        !finite(d.coneOuterGainDb) || !finite(d.coneOuterLowpassHz) ||
        !finite(d.proximityFrequency) || !curveFinite(d.volumeCurve) ||
        !curveFinite(d.lowpassCurve) || !curveFinite(d.widthCurve)) return 0;
    std::uint64_t hash = busfingerprint::kOffsetBasis;
    const auto byte = [&](std::uint8_t value) { hash = busfingerprint::Step(hash, value); };
    const auto u32 = [&](std::uint32_t value) { busfingerprint::AppendU32(byte, value); };
    const auto u64 = [&](std::uint64_t value) { busfingerprint::AppendU64(byte, value); };
    const auto f32 = [&](float value) { u32(busfingerprint::CanonicalFloatBits(value)); };
    const auto curve = [&](const VnAttenuationCurve& value) {
        u32(value.enabled); u32(value.pointCount);
        for (std::uint32_t i = 0; i < VN_ATTENUATION_MAX_CURVE_POINTS; ++i) {
            f32(value.points[i].x); f32(value.points[i].y);
        }
        for (std::uint32_t i = 0; i < VN_ATTENUATION_MAX_CURVE_POINTS - 1; ++i) {
            f32(value.handles[i].h1x); f32(value.handles[i].h1y);
            f32(value.handles[i].h2x); f32(value.handles[i].h2y);
        }
    };
    constexpr char tag[] = "harken.attenuation.definition";
    u32(static_cast<std::uint32_t>(sizeof(tag) - 1));
    for (std::size_t i = 0; i + 1 < sizeof(tag); ++i) byte(static_cast<std::uint8_t>(tag[i]));
    u32(1); u64(attenuation.id.high); u64(attenuation.id.low); u32(d.structSize); u32(d.rolloff);
    f32(d.minDistance); f32(d.maxDistance); f32(d.airAbsorptionNearHz);
    f32(d.airAbsorptionFarHz); f32(d.proximityRadius); f32(d.proximityMaxBoostDb);
    u32(d.coneEnabled); f32(d.coneInnerDeg); f32(d.coneOuterDeg);
    f32(d.coneOuterGainDb); f32(d.coneOuterLowpassHz); u32(d.reserved0);
    curve(d.volumeCurve); curve(d.lowpassCurve); curve(d.widthCurve); f32(d.proximityFrequency);
    return busfingerprint::Finalize(hash);
}

}

#endif
