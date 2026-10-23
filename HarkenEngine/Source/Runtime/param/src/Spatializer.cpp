#include "harken/param/Spatializer.h"

#include "harken/param/AttenuationDefaults.h"
#include "harken/core/LogicAllocCheckpoint.h"
#include "harken/param/EmitterTable.h"

#include <cassert>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace harken {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kSpatialVolumeRampMs = 10;

enum class OverrideCurveDomain {
    VolumeDb,
    LowpassHz,
    Width,
};

bool SameCurve(const Curve& a, const Curve& b) noexcept {
    if (a.pointCount != b.pointCount) {
        return false;
    }
    for (std::uint32_t i = 0; i < a.pointCount; ++i) {
        if (a.points[i].x != b.points[i].x || a.points[i].y != b.points[i].y) {
            return false;
        }
        if (i + 1 < a.pointCount) {
            const auto& x = a.handles[i];
            const auto& y = b.handles[i];
            if (x.h1x != y.h1x || x.h1y != y.h1y ||
                x.h2x != y.h2x || x.h2y != y.h2y) {
                return false;
            }
        }
    }
    return true;
}

bool SameProfile(const AttenuationProfile& a, const AttenuationProfile& b) noexcept {
    return a.id == b.id && a.rolloff == b.rolloff &&
        a.minDistance == b.minDistance && a.maxDistance == b.maxDistance &&
        a.airNearHz == b.airNearHz && a.airFarHz == b.airFarHz &&
        a.proximityRadius == b.proximityRadius &&
        a.proximityMaxBoostDb == b.proximityMaxBoostDb &&
        a.proximityFrequency == b.proximityFrequency &&
        a.coneEnabled == b.coneEnabled && a.coneInnerDeg == b.coneInnerDeg &&
        a.coneOuterDeg == b.coneOuterDeg && a.coneOuterGainDb == b.coneOuterGainDb &&
        a.coneOuterLowpassHz == b.coneOuterLowpassHz &&
        a.volumeOverride == b.volumeOverride && a.lowpassOverride == b.lowpassOverride &&
        a.widthOverride == b.widthOverride && SameCurve(a.volumeCurve, b.volumeCurve) &&
        SameCurve(a.lowpassCurve, b.lowpassCurve) && SameCurve(a.widthCurve, b.widthCurve);
}

bool RetainedKeyLess(const Spatializer::RetainedProfileKey& a,
                     const Spatializer::RetainedProfileKey& b) noexcept {
    return a.id < b.id || (a.id == b.id && a.generation < b.generation);
}

VnResult ValidateRetainedKeys(
    const std::vector<Spatializer::RetainedProfileKey>& keys,
    std::size_t ringSize) noexcept {
    if (keys.size() > ringSize && keys.size() - ringSize > kMaxAttenuationProfiles) {
        return VN_PACK_RESIDENT_CAPACITY;
    }
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i].id == VnObjectId{} || keys[i].generation == 0 ||
            (i != 0 && !RetainedKeyLess(keys[i - 1], keys[i]))) {
            return VN_INVALID_ARG;
        }
    }
    return VN_OK;
}

bool RetainsProfile(const std::vector<Spatializer::RetainedProfileKey>& keys,
                    VnAttenuationId id, std::uint64_t generation) noexcept {
    return std::binary_search(keys.begin(), keys.end(),
                             Spatializer::RetainedProfileKey{id, generation},
                             RetainedKeyLess);
}

std::size_t RetainCapacity(const EngineConfig& config) {
    const std::size_t plans = config.topologyPlanPoolCapacity == 0
        ? 4 : config.topologyPlanPoolCapacity;
    if (plans > std::numeric_limits<std::size_t>::max() / kMaxAttenuationProfiles) {
        throw std::length_error("attenuation retain capacity");
    }
    detail::LogicAllocCheckpoint(detail::LogicAllocSite::None);
    return plans * kMaxAttenuationProfiles;
}

std::uint32_t FloatBits(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float bit width drift");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool CanonicalHandleTail(const VnAttenuationCurve& curve) {
    if (curve.pointCount == 0 ||
        curve.pointCount > VN_ATTENUATION_MAX_CURVE_POINTS) {
        return false;
    }
    for (std::uint32_t i = curve.pointCount - 1;
         i < VN_ATTENUATION_MAX_CURVE_POINTS - 1; ++i) {
        const VnCurveHandlePair& handle = curve.handles[i];
        if (FloatBits(handle.h1x) != 0u || FloatBits(handle.h1y) != 0u ||
            FloatBits(handle.h2x) != 0u || FloatBits(handle.h2y) != 0u) {
            return false;
        }
    }
    return true;
}

bool InAuthorizedDomain(float value, OverrideCurveDomain domain) {
    if (!std::isfinite(value)) {
        return false;
    }
    switch (domain) {
    case OverrideCurveDomain::VolumeDb:
        return value >= kAttenuationFloorDb && value <= 0.0f;
    case OverrideCurveDomain::LowpassHz:
        return value > 0.0f;
    case OverrideCurveDomain::Width:
        return value >= 0.0f && value <= 1.0f;
    }
    return false;
}

float DbToAmplitude(float valueDb) {
    return static_cast<float>(
        std::pow(10.0, static_cast<double>(valueDb) / 20.0));
}

float AmplitudeToDb(float amplitude) {
    return static_cast<float>(
        20.0 * std::log10(static_cast<double>(amplitude)));
}

// The spatializer mirrors are keyed by the internal render-slot handle (the
// API layer translates the public ledger handle before reaching here), so the
// low 32 bits index the render slot and the full handle is verified against
// the stored mirror to reject a reused slot's stale handle.
std::uint32_t SlotOf(VnVoiceHandle handle) {
    return static_cast<std::uint32_t>(handle);
}

bool FiniteVec(const VnVec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Length in the double domain: float component squares of finite but
// large values (near FLT_MAX) overflow to infinity, which would slip
// past a "> 0" gate and normalize into a near-zero axis. Doubles hold
// the squares exactly enough; callers still reject a non-finite result.
double Length3(double x, double y, double z) {
    return std::sqrt(x * x + y * y + z * z);
}

float EffectiveDistance(double pairDistance, float distanceScale,
                         float emitterDistanceScale) {
    const double scale = static_cast<double>(distanceScale) * emitterDistanceScale;
    return static_cast<float>(std::min(pairDistance / scale,
                                      static_cast<double>(std::numeric_limits<float>::max())));
}

// Builds an internal curve from one public override block. The fixed-capacity
// ABI tail and authored value domain are boundary contracts; Curve::SetPoints
// remains the geometry validator.
bool BuildOverrideCurve(const VnAttenuationCurve& in,
                        OverrideCurveDomain domain, Curve* out) {
    if (in.pointCount == 0 || in.pointCount > kMaxCurvePoints) {
        return false;
    }
    if (!CanonicalHandleTail(in)) {
        return false;
    }
    CurvePoint points[kMaxCurvePoints];
    CurveHandlePair handles[kMaxCurvePoints - 1];
    for (std::uint32_t i = 0; i < in.pointCount; ++i) {
        points[i].x = in.points[i].x;
        points[i].y = in.points[i].y;
        if (!InAuthorizedDomain(points[i].y, domain)) {
            return false;
        }
    }
    for (std::uint32_t i = 0; i + 1 < in.pointCount; ++i) {
        handles[i].h1x = in.handles[i].h1x;
        handles[i].h1y = in.handles[i].h1y;
        handles[i].h2x = in.handles[i].h2x;
        handles[i].h2y = in.handles[i].h2y;
        if (!InAuthorizedDomain(handles[i].h1y, domain) ||
            !InAuthorizedDomain(handles[i].h2y, domain)) {
            return false;
        }
    }
    if (domain == OverrideCurveDomain::VolumeDb) {
        for (std::uint32_t i = 0; i < in.pointCount; ++i) {
            points[i].y = DbToAmplitude(points[i].y);
        }
        for (std::uint32_t i = 0; i + 1 < in.pointCount; ++i) {
            handles[i].h1y = DbToAmplitude(handles[i].h1y);
            handles[i].h2y = DbToAmplitude(handles[i].h2y);
        }
    }
    return out->SetPoints(points, handles, in.pointCount);
}

// The whole profile validation + build, shared by registration and hot
// reload so a replace validates exactly as a register does. Fills *out only
// on success; a refusal touches nothing.
VnResult BuildProfileFromDesc(VnAttenuationId id,
                              const VnAttenuationDesc* desc,
                              AttenuationProfile* out) {
    constexpr std::uint32_t kLegacyDescBytes =
        static_cast<std::uint32_t>(offsetof(VnAttenuationDesc,
                                            proximityFrequency));
    if (desc == nullptr ||
        (desc->structSize != sizeof(VnAttenuationDesc) &&
         desc->structSize != kLegacyDescBytes)) {
        return VN_INVALID_ARG;
    }
    // A full-size descriptor carries the proximity corner, but a zero value is
    // the unset sentinel, not an authored 0 Hz: a caller that grows to the new
    // struct size and zero-initializes (never touching the field) must keep the
    // legacy 250 Hz fallback rather than be refused. Only a non-zero corner is
    // treated as authored and validated. The structSize guard short-circuits
    // ahead of the field read so a legacy payload never reads past its bytes.
    // The presence half of the test is "the caller's struct extends past this
    // field", not "the caller's struct is the newest one": tested against
    // sizeof, an append would silently drop an authored corner back to the
    // legacy fallback for callers compiled at today's full size.
    const bool hasAuthoredFrequency =
        desc->structSize >= offsetof(VnAttenuationDesc, proximityFrequency) +
                                sizeof(VnAttenuationDesc::proximityFrequency) &&
        desc->proximityFrequency != 0.0f;
    // Enable flags are a strict 0/1 contract (the play descriptor's
    // spatialized precedent): values above 1 are reserved for future
    // header vintages and refused rather than guessed at.
    if (desc->coneEnabled > 1u || desc->volumeCurve.enabled > 1u ||
        desc->lowpassCurve.enabled > 1u || desc->widthCurve.enabled > 1u) {
        return VN_INVALID_ARG;
    }
    if (!std::isfinite(desc->minDistance) ||
        !std::isfinite(desc->maxDistance) ||
        !std::isfinite(desc->airAbsorptionNearHz) ||
        !std::isfinite(desc->airAbsorptionFarHz) ||
        !std::isfinite(desc->proximityRadius) ||
        !std::isfinite(desc->proximityMaxBoostDb) ||
        (hasAuthoredFrequency &&
         !std::isfinite(desc->proximityFrequency))) {
        return VN_INVALID_ARG;
    }
    // minDistance anchors the inverse-square and logarithmic models (both
    // divide by it) and must leave a non-empty span to maxDistance. The
    // air-absorption endpoints live in the log-frequency domain, so both
    // must be positive.
    if (!(desc->minDistance > 0.0f) ||
        !(desc->maxDistance > desc->minDistance) ||
        !(desc->airAbsorptionNearHz > 0.0f) ||
        !(desc->airAbsorptionFarHz > 0.0f) ||
        desc->proximityRadius < 0.0f ||
        (hasAuthoredFrequency &&
         !(desc->proximityFrequency > 0.0f))) {
        return VN_INVALID_ARG;
    }
    if (desc->rolloff != VN_ROLLOFF_LINEAR &&
        desc->rolloff != VN_ROLLOFF_INVERSE_SQUARE &&
        desc->rolloff != VN_ROLLOFF_LOGARITHMIC) {
        return VN_INVALID_ARG;
    }
    const bool coneEnabled = desc->coneEnabled != 0;
    if (coneEnabled) {
        if (!std::isfinite(desc->coneInnerDeg) ||
            !std::isfinite(desc->coneOuterDeg) ||
            !std::isfinite(desc->coneOuterGainDb) ||
            !std::isfinite(desc->coneOuterLowpassHz)) {
            return VN_INVALID_ARG;
        }
        if (desc->coneInnerDeg < 0.0f ||
            desc->coneOuterDeg < desc->coneInnerDeg ||
            desc->coneOuterDeg > 180.0f ||
            !(desc->coneOuterLowpassHz > 0.0f)) {
            return VN_INVALID_ARG;
        }
    }

    AttenuationProfile built;
    built.id = id;
    built.rolloff = desc->rolloff;
    built.minDistance = desc->minDistance;
    built.maxDistance = desc->maxDistance;
    built.airNearHz = desc->airAbsorptionNearHz;
    built.airFarHz = desc->airAbsorptionFarHz;
    built.proximityRadius = desc->proximityRadius;
    built.proximityMaxBoostDb = desc->proximityMaxBoostDb;
    built.proximityFrequency = hasAuthoredFrequency
                                   ? desc->proximityFrequency
                                   : kToneLowShelfDefaultHz;
    built.coneEnabled = coneEnabled;
    built.coneInnerDeg = desc->coneInnerDeg;
    built.coneOuterDeg = desc->coneOuterDeg;
    built.coneOuterGainDb = desc->coneOuterGainDb;
    built.coneOuterLowpassHz = desc->coneOuterLowpassHz;
    built.volumeOverride = desc->volumeCurve.enabled != 0;
    built.lowpassOverride = desc->lowpassCurve.enabled != 0;
    built.widthOverride = desc->widthCurve.enabled != 0;
    if (built.volumeOverride &&
        !BuildOverrideCurve(desc->volumeCurve, OverrideCurveDomain::VolumeDb,
                            &built.volumeCurve)) {
        return VN_INVALID_ARG;
    }
    if (built.lowpassOverride &&
        !BuildOverrideCurve(desc->lowpassCurve,
                            OverrideCurveDomain::LowpassHz,
                            &built.lowpassCurve)) {
        return VN_INVALID_ARG;
    }
    if (built.widthOverride &&
        !BuildOverrideCurve(desc->widthCurve, OverrideCurveDomain::Width,
                            &built.widthCurve)) {
        return VN_INVALID_ARG;
    }
    *out = built;
    return VN_OK;
}

} // namespace

Spatializer::Spatializer(const EngineConfig& config)
    : Spatializer(config, config.voiceCapacity.Render()) {}

Spatializer::Spatializer(const EngineConfig& config, std::uint32_t initialVoiceRows) {
    if (!m_mirrors.Reserve(config.voiceCapacity.Render()) ||
        !m_profiles.Reserve(kMaxAttenuationProfiles) ||
        !m_retainRing.Reserve(RetainCapacity(config)) ||
        !PrepareVoiceStorage(initialVoiceRows)) throw std::bad_alloc{};
}

VnResult Spatializer::SetListener(VnVec3 pos, VnVec3 forward, VnVec3 up) {
    // The panning basis is right = normalize(forward x up); a zero-length
    // or parallel pair leaves no defined right axis, which is a caller
    // contract violation rather than a representable pose. The cross
    // product and its length run in double: finite float components near
    // FLT_MAX overflow float products to infinity, and a non-finite
    // length is refused rather than divided by.
    const double cx = static_cast<double>(forward.y) * up.z -
                      static_cast<double>(forward.z) * up.y;
    const double cy = static_cast<double>(forward.z) * up.x -
                      static_cast<double>(forward.x) * up.z;
    const double cz = static_cast<double>(forward.x) * up.y -
                      static_cast<double>(forward.y) * up.x;
    const double len = std::sqrt(cx * cx + cy * cy + cz * cz);
    if (!std::isfinite(len) || !(len > 0.0)) {
        return VN_INVALID_ARG;
    }
    m_listenerX = pos.x;
    m_listenerY = pos.y;
    m_listenerZ = pos.z;
    m_rightX = static_cast<float>(cx / len);
    m_rightY = static_cast<float>(cy / len);
    m_rightZ = static_cast<float>(cz / len);
    return VN_OK;
}

VnResult Spatializer::SetListenerDistanceReference(VnEmitterHandle handle,
                                                    const EmitterTable& emitters) {
    if (handle != 0) {
        if (!emitters.IsPublicLive(handle)) return VN_STALE_HANDLE;
        if (emitters.Context(handle).spatialEmitter == 0)
            return VN_CAPABILITY_UNAVAILABLE;
    }
    m_distanceReference = handle;
    return VN_OK;
}

void Spatializer::NoteEmitterDestroyed(VnEmitterHandle handle) noexcept {
    if (m_distanceReference == handle) m_distanceReference = 0;
}

double Spatializer::DistanceToReference(const EmitterTable& emitters,
                                        float px, float py, float pz,
                                        double listenerDistance) const {
    SpatialSourceState reference;
    if (m_distanceReference == 0 || !emitters.Pose(m_distanceReference, &reference))
        return listenerDistance;
    return Length3(static_cast<double>(px) - reference.posX,
                   static_cast<double>(py) - reference.posY,
                   static_cast<double>(pz) - reference.posZ);
}

VnResult Spatializer::CanRegisterProfile(VnAttenuationId id,
                                         const VnAttenuationDesc* desc) const {
    AttenuationProfile built;
    const VnResult validated = BuildProfileFromDesc(id, desc, &built);
    if (validated != VN_OK) {
        return validated;
    }
    if (FindProfile(id) != nullptr) {
        // Same rule as PCM registration: a live id never silently
        // replaces — the explicit replace verb is the one way to swap
        // content under a registered id.
        return VN_IN_USE;
    }
    if (m_profileCount >= kMaxAttenuationProfiles) {
        return VN_NO_ATTENUATION;
    }
    return VN_OK;
}

VnResult Spatializer::RegisterProfile(VnAttenuationId id,
                                      const VnAttenuationDesc* desc) {
    const VnResult admitted = CanRegisterProfile(id, desc);
    if (admitted != VN_OK) {
        return admitted;
    }
    if (!m_profiles.ConstructThrough(m_profiles.Capacity())) return VN_BACKPRESSURE;
    // Rebuild the evaluated form: the admission proved the same build
    // succeeds, so this cannot refuse. The build is pure, which keeps the
    // admission/commit split free of shared scratch state.
    AttenuationProfile built;
    if (BuildProfileFromDesc(id, desc, &built) != VN_OK) {
        return VN_INVALID_ARG; // unreachable: the admission ran this build
    }
    for (std::size_t index = 0; index < m_profiles.size(); ++index) {
        if (m_profileOccupied[index] == 0) {
            m_profiles[index] = built;
            m_profileOccupied[index] = 1;
            ++m_profileCount;
            return VN_OK;
        }
    }
    assert(false);
    return VN_OK;
}

VnResult Spatializer::ReplaceProfile(VnAttenuationId id,
                                     const VnAttenuationDesc* desc) {
    // Full validation before the lookup so any refusal — including one for
    // an id that would have been found — leaves the old curves untouched.
    AttenuationProfile built;
    const VnResult validated = BuildProfileFromDesc(id, desc, &built);
    if (validated != VN_OK) {
        return validated;
    }
    for (std::uint32_t i = 0; i < m_profiles.size(); ++i) {
        if (m_profileOccupied[i] != 0 && m_profiles[i].id == id) {
            if (m_profileGenerations[i] != 0) {
                return VN_IN_USE;
            }
            // Overwrite in place: bound voices reference the profile by
            // index, so the next per-tick evaluation reads the new curves
            // with no rebind.
            m_profiles[i] = built;
            return VN_OK;
        }
    }
    return VN_UNKNOWN_ID; // replace never implicitly registers
}

std::uint32_t Spatializer::FreeProfileSlotAtOrdinal(std::uint32_t ordinal) const noexcept {
    for (std::uint32_t slot = 0; slot < m_profileOccupied.size(); ++slot) {
        if (m_profileOccupied[slot] == 0) {
            if (ordinal == 0) {
                return slot;
            }
            --ordinal;
        }
    }
    return kMaxAttenuationProfiles;
}

VnResult Spatializer::PrepareRegisterProfile(
    VnAttenuationId id, const VnAttenuationDesc* desc,
    std::uint32_t slotIndex, PreparedProfileToken* out) const {
    if (out == nullptr || out->m_prepared) {
        return VN_INVALID_ARG;
    }
    AttenuationProfile built;
    const VnResult valid = BuildProfileFromDesc(id, desc, &built);
    if (valid != VN_OK) {
        return valid;
    }
    if (FindProfile(id) != nullptr) {
        return VN_IN_USE;
    }
    if (slotIndex >= kMaxAttenuationProfiles || m_profileOccupied[slotIndex] != 0) {
        return VN_NO_ATTENUATION;
    }
    if (!m_profiles.ConstructThrough(m_profiles.Capacity())) return VN_BACKPRESSURE;
    out->m_profile = built;
    out->m_slotIndex = slotIndex;
    out->m_append = true;
    out->m_prepared = true;
    return VN_OK;
}

VnResult Spatializer::PrepareReplaceProfile(
    VnAttenuationId id, const VnAttenuationDesc* desc,
    PreparedProfileToken* out) const {
    if (out == nullptr || out->m_prepared) {
        return VN_INVALID_ARG;
    }
    AttenuationProfile built;
    const VnResult valid = BuildProfileFromDesc(id, desc, &built);
    if (valid != VN_OK) {
        return valid;
    }
    for (std::uint32_t i = 0; i < m_profiles.size(); ++i) {
        if (m_profileOccupied[i] != 0 && m_profiles[i].id == id) {
            if (m_profileGenerations[i] != 0) {
                return VN_IN_USE;
            }
            out->m_profile = built;
            out->m_slotIndex = i;
            out->m_append = false;
            out->m_prepared = true;
            return VN_OK;
        }
    }
    return VN_UNKNOWN_ID;
}

void Spatializer::CancelPreparedProfile(
    PreparedProfileToken* token) noexcept {
    if (token != nullptr) {
        *token = PreparedProfileToken{};
    }
}

void Spatializer::CommitPreparedProfile(
    PreparedProfileToken* token) noexcept {
    assert(token != nullptr && token->m_prepared);
    assert(token->m_slotIndex < m_profiles.size());
    if (token->m_append) {
        assert(m_profileOccupied[token->m_slotIndex] == 0);
    } else {
        assert(m_profileOccupied[token->m_slotIndex] != 0);
    }
    m_profiles[token->m_slotIndex] = token->m_profile;
    m_profileOccupied[token->m_slotIndex] = 1;
    if (token->m_append) {
        ++m_profileCount;
    }
    *token = PreparedProfileToken{};
}

void Spatializer::OnVoiceStarted(VnVoiceHandle handle, bool spatialized,
                                 float x, float y, float z,
                                 VnEmitterHandle emitterHandle) {
    const std::uint32_t slot = SlotOf(handle);
    if (slot >= m_mirrors.size()) {
        return;
    }
    VoiceMirror& mirror = m_mirrors[slot];
    if (mirror.handle != 0) {
        NoteVoiceEnded(mirror.handle);
    }
    mirror = VoiceMirror{};
    mirror.handle = handle;
    mirror.emitterHandle = emitterHandle;
    mirror.spatialized = spatialized;
    // The play-descriptor position seeds the mirror; an attached voice's pose
    // is overwritten from its emitter on the first Evaluate.
    mirror.posX = x;
    mirror.posY = y;
    mirror.posZ = z;
}

VnEmitterHandle Spatializer::AttachedEmitter(VnVoiceHandle handle) const {
    const std::uint32_t slot = SlotOf(handle);
    if (handle == 0 || slot >= m_mirrors.size() ||
        m_mirrors[slot].handle != handle) {
        return 0;
    }
    return m_mirrors[slot].emitterHandle;
}

void Spatializer::SetVoicePosition(VnVoiceHandle handle, float x, float y,
                                   float z) {
    VoiceMirror* mirror = MirrorFor(handle);
    if (mirror == nullptr) {
        return;
    }
    mirror->posX = x;
    mirror->posY = y;
    mirror->posZ = z;
}

void Spatializer::RestoreVoiceSource(VnVoiceHandle handle,
                                     const SpatialSourceState& source) {
    VoiceMirror* mirror = MirrorFor(handle);
    if (mirror != nullptr) {
        mirror->posX = source.posX;
        mirror->posY = source.posY;
        mirror->posZ = source.posZ;
        mirror->fwdX = source.fwdX;
        mirror->fwdY = source.fwdY;
        mirror->fwdZ = source.fwdZ;
        mirror->emitterDistanceScale = source.emitterDistanceScale;
    }
}

void Spatializer::SetVoiceDistanceScale(VnVoiceHandle handle, float scale) {
    VoiceMirror* mirror = MirrorFor(handle);
    if (mirror != nullptr) {
        mirror->distanceScale = scale;
    }
}

void Spatializer::SetVoiceSpatializationMix(VnVoiceHandle handle, float mix) {
    VoiceMirror* mirror = MirrorFor(handle);
    if (mirror != nullptr) {
        mirror->spatializationMix = mix;
    }
}

VnResult Spatializer::SetVoiceOrientation(VnVoiceHandle handle,
                                          VnVec3 forward) {
    if (!FiniteVec(forward)) {
        return VN_INVALID_ARG;
    }
    // Zero length defines no cone axis; a non-finite length (component
    // magnitudes whose squares overflow even the double sum) has no
    // representable direction either. Both are argument errors.
    const double len = Length3(forward.x, forward.y, forward.z);
    if (!std::isfinite(len) || !(len > 0.0)) {
        return VN_INVALID_ARG;
    }
    VoiceMirror* mirror = MirrorFor(handle);
    if (mirror == nullptr) {
        return VN_OK; // ended in flight; the caller's CheckLive gated it
    }
    mirror->fwdX = static_cast<float>(forward.x / len);
    mirror->fwdY = static_cast<float>(forward.y / len);
    mirror->fwdZ = static_cast<float>(forward.z / len);
    return VN_OK;
}

VnResult Spatializer::BindVoice(VnVoiceHandle handle,
                                VnAttenuationId attenuationId,
                                std::uint64_t generation) {
    const AttenuationProfile* profile = FindProfile(attenuationId, generation);
    if (profile == nullptr) {
        return VN_UNKNOWN_ID;
    }
    VoiceMirror* mirror = MirrorFor(handle);
    if (mirror == nullptr) {
        return VN_OK; // ended in flight; the caller's CheckLive gated it
    }
    ReleaseRetainedMirror(*mirror);
    mirror->profileIndex = -1;
    for (std::uint32_t i = 0; i < m_profiles.size(); ++i) {
        if (&m_profiles[i] == profile) {
            mirror->profileIndex = static_cast<std::int32_t>(i);
            mirror->profileGeneration = m_profileGenerations[i];
            return VN_OK;
        }
    }
    for (std::uint32_t i = 0; i < m_retainRing.size(); ++i) {
        auto& slot = m_retainRing[i];
        if (&slot.pod == profile) {
            mirror->retainIndex = static_cast<std::int32_t>(i);
            mirror->retainEpoch = slot.epoch;
            mirror->profileGeneration = slot.generation;
            ++slot.census;
            return VN_OK;
        }
    }
    return VN_UNKNOWN_ID;
}

void Spatializer::Evaluate(ParamSystem& params, const EmitterTable& emitters) {
    for (VoiceMirror& mirror : m_mirrors) {
        if (mirror.handle != 0) {
            (void)EvaluateVoice(mirror.handle, params, emitters);
        }
    }
}

VnResult Spatializer::EvaluateVoice(VnVoiceHandle handle, ParamSystem& params,
                                    const EmitterTable& emitters, std::uint32_t* targetMask) {
    if (targetMask != nullptr)
        *targetMask = 0;
    VoiceMirror* found = MirrorFor(handle);
    if (found == nullptr)
        return VN_INVALID_ARG;
    VoiceMirror& mirror = *found;
    // Emitter fan-out: an attached voice takes its emitter's pose each
    // tick. A destroyed emitter no longer resolves, so the voice detaches
    // — it keeps its last emitter-derived pose (frozen) and becomes
    // individually addressable again. Runs before the geometry below so
    // the distance/azimuth this tick reflect the emitter's current pose.
    if (mirror.emitterHandle != 0) {
        SpatialSourceState source;
        if (emitters.Pose(mirror.emitterHandle, &source)) {
            RestoreVoiceSource(handle, source);
        } else {
            mirror.emitterHandle = 0; // detached; last pose stays frozen
        }
    }
    if (!mirror.spatialized) {
        return VN_OK;
    }
    const bool initialEvaluation = !mirror.evaluated;
    const double dx = static_cast<double>(mirror.posX) - m_listenerX;
    const double dy = static_cast<double>(mirror.posY) - m_listenerY;
    const double dz = static_cast<double>(mirror.posZ) - m_listenerZ;
    const double pairDistance = Length3(dx, dy, dz);
    const float effectiveDistance = EffectiveDistance(
        DistanceToReference(emitters, mirror.posX, mirror.posY, mirror.posZ, pairDistance),
        mirror.distanceScale, mirror.emitterDistanceScale);

    // Azimuth pan: the projection of the emitter direction onto the
    // listener's right axis. A coincident emitter has no direction
    // and pans center; front and back project identically (stereo
    // output does not distinguish them).
    float pan = 0.0f;
    if (pairDistance > 0.0f) {
        pan = static_cast<float>((dx * m_rightX + dy * m_rightY + dz * m_rightZ) / pairDistance);
        pan = (pan < -1.0f) ? -1.0f : (pan > 1.0f) ? 1.0f : pan;
    }
    params.SetAttenuationSlot(mirror.handle, kTargetVoicePan, pan);
    if (targetMask != nullptr)
        *targetMask = 1u << kTargetVoicePan;

    const AttenuationProfile* resolved = ResolveProfile(mirror);
    if (resolved == nullptr) {
        if (mirror.profileIndex >= 0 || mirror.retainIndex >= 0) {
            return VN_UNKNOWN_ID;
        }
        // No profile bound: azimuth pan is geometry, but distance
        // attenuation is policy — none applies implicitly.
        // Direction-only onset must not consume the first profile snap:
        // a following raw Bind still takes effect at the first Update.
        mirror.evaluated = mirror.evaluated || targetMask == nullptr;
        return VN_OK;
    }
    const AttenuationProfile& profile = *resolved;

    float volumeDb = profile.volumeOverride
                         ? AmplitudeToDb(profile.volumeCurve.Eval(effectiveDistance))
                         : RolloffGainToDb(RolloffGain(profile.rolloff, effectiveDistance,
                                                       profile.minDistance, profile.maxDistance));
    float lowpassHz =
        profile.lowpassOverride
            ? profile.lowpassCurve.Eval(effectiveDistance)
            : AirAbsorptionHz(effectiveDistance, profile.minDistance, profile.maxDistance,
                              profile.airNearHz, profile.airFarHz);
    const float width = profile.widthOverride ? profile.widthCurve.Eval(effectiveDistance) : 0.0f;
    const float shelfDb =
        NearFieldShelfDb(effectiveDistance, profile.proximityRadius, profile.proximityMaxBoostDb);

    if (profile.coneEnabled) {
        // Angle between the emitter's forward axis and the direction
        // toward the listener; a coincident listener reads on-axis.
        float thetaDeg = 0.0f;
        if (pairDistance > 0.0f) {
            double cosTheta =
                (-dx * mirror.fwdX - dy * mirror.fwdY - dz * mirror.fwdZ) / pairDistance;
            cosTheta = (cosTheta < -1.0f) ? -1.0f : (cosTheta > 1.0f) ? 1.0f : cosTheta;
            thetaDeg = static_cast<float>(std::acos(static_cast<double>(cosTheta)) * 180.0 / kPi);
        }
        volumeDb += ConeGainDb(thetaDeg, profile.coneInnerDeg, profile.coneOuterDeg,
                               profile.coneOuterGainDb);
        const float coneHz = ConeLowpassHz(thetaDeg, profile.coneInnerDeg, profile.coneOuterDeg,
                                           profile.coneOuterLowpassHz);
        lowpassHz = (coneHz < lowpassHz) ? coneHz : lowpassHz;
    }

    const std::uint32_t volumeRampMs = initialEvaluation ? 0u : kSpatialVolumeRampMs;
    params.SetAttenuationSlot(mirror.handle, kTargetVoiceVolume, volumeDb, volumeRampMs);
    params.SetAttenuationSlot(mirror.handle, kTargetVoiceLowpassCutoff, lowpassHz);
    params.SetAttenuationSlot(mirror.handle, kTargetVoiceWidth, width);
    params.ApplyProximityFrequency(mirror.handle, profile.proximityFrequency);
    params.SetAttenuationSlot(mirror.handle, kTargetVoiceLowShelfGainDb, shelfDb);
    mirror.evaluated = true;
    if (targetMask != nullptr) {
        *targetMask |= (1u << kTargetVoiceVolume) | (1u << kTargetVoiceLowpassCutoff) |
                       (1u << kTargetVoiceWidth) | (1u << kTargetVoiceLowShelfGainDb) |
                       (1u << kTargetVoiceLowShelfFreqHz);
    }
    return VN_OK;
}

bool Spatializer::HasProfile(VnAttenuationId id, std::uint64_t generation) const {
    return FindProfile(id, generation) != nullptr;
}

Spatializer::VoiceMirror* Spatializer::MirrorFor(VnVoiceHandle handle) {
    const std::uint32_t slot = SlotOf(handle);
    if (handle == 0 || slot >= m_mirrors.size() ||
        m_mirrors[slot].handle != handle) {
        return nullptr;
    }
    return &m_mirrors[slot];
}

const AttenuationProfile* Spatializer::FindProfile(
    VnAttenuationId id, std::uint64_t generation) const {
    for (std::uint32_t i = 0; i < m_profiles.size(); ++i) {
        if (m_profileOccupied[i] != 0 && m_profiles[i].id == id &&
            (generation == 0 || m_profileGenerations[i] == generation)) {
            return &m_profiles[i];
        }
    }
    if (generation != 0) {
        for (const auto& slot : m_retainRing) {
            if (slot.state == RetainSlotState::Published &&
                slot.id == id && slot.generation == generation) {
                return &slot.pod;
            }
        }
    }
    return nullptr;
}

std::uint64_t Spatializer::CurrentProfileGeneration(VnAttenuationId id) const noexcept {
    const AttenuationProfile* profile = FindProfile(id, 0);
    if (profile == nullptr || id == VnObjectId{}) {
        return 0;
    }
    if (profile >= m_profiles.data() &&
        profile < m_profiles.data() + m_profiles.size()) {
        const auto index = static_cast<std::uint32_t>(profile - m_profiles.data());
        return m_profileGenerations[index];
    }
    for (const auto& slot : m_retainRing) {
        if (&slot.pod == profile) {
            return slot.generation;
        }
    }
    return 0;
}

float Spatializer::AttenuationVolumeDb(const EmitterTable& emitters,
                                       VnAttenuationId attenuationId, float px,
                                       float py, float pz,
                                       float distanceScale,
                                       std::uint64_t generation,
                                       float emitterDistanceScale) const {
    const AttenuationProfile* profile = FindProfile(attenuationId, generation);
    if (profile == nullptr) {
        return 0.0f;
    }
    const double dx = static_cast<double>(px) - m_listenerX;
    const double dy = static_cast<double>(py) - m_listenerY;
    const double dz = static_cast<double>(pz) - m_listenerZ;
    const double pairDistance = Length3(dx, dy, dz);
    const float effectiveDistance =
        EffectiveDistance(DistanceToReference(emitters, px, py, pz, pairDistance),
                          distanceScale, emitterDistanceScale);
    // Matches Evaluate()'s volume-slot term (minus the cone angular addend).
    return profile->volumeOverride
               ? AmplitudeToDb(profile->volumeCurve.Eval(effectiveDistance))
               : RolloffGainToDb(RolloffGain(profile->rolloff,
                                             effectiveDistance,
                                             profile->minDistance,
                                             profile->maxDistance));
}

bool Spatializer::CaptureVoice(VnVoiceHandle handle,
                               SpatialVoiceState* out) const {
    const std::uint32_t slot = SlotOf(handle);
    if (handle == 0 || slot >= m_mirrors.size() ||
        m_mirrors[slot].handle != handle) {
        return false;
    }
    const VoiceMirror& mirror = m_mirrors[slot];
    out->spatialized = mirror.spatialized;
    out->posX = mirror.posX;
    out->posY = mirror.posY;
    out->posZ = mirror.posZ;
    out->fwdX = mirror.fwdX;
    out->fwdY = mirror.fwdY;
    out->fwdZ = mirror.fwdZ;
    out->distanceScale = mirror.distanceScale;
    out->emitterDistanceScale = mirror.emitterDistanceScale;
    out->spatializationMix = mirror.spatializationMix;
    const auto* profile = ResolveProfile(mirror);
    out->attenuationId = profile != nullptr ? profile->id : VnAttenuationId{};
    out->attenuationGeneration = profile != nullptr ? mirror.profileGeneration : 0;
    out->emitterHandle = mirror.emitterHandle;
    return true;
}

const AttenuationProfile* Spatializer::ResolveProfile(
    const VoiceMirror& mirror) const noexcept {
    if (mirror.retainIndex >= 0) {
        const std::size_t index = static_cast<std::size_t>(mirror.retainIndex);
        if (index >= m_retainRing.size()) {
            return nullptr;
        }
        const RetainSlot& slot = m_retainRing[index];
        if (slot.state != RetainSlotState::Published ||
            slot.epoch != mirror.retainEpoch ||
            slot.generation != mirror.profileGeneration) {
            return nullptr;
        }
        return &slot.pod;
    }
    if (mirror.profileIndex < 0) {
        return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(mirror.profileIndex);
    if (index >= m_profiles.size() || m_profileOccupied[index] == 0 ||
        m_profileGenerations[index] != mirror.profileGeneration) {
        return nullptr;
    }
    return &m_profiles[index];
}

VnResult Spatializer::PrepareRetainProfile(VnAttenuationId id,
                                           PreparedProfileToken* out) {
    if (out == nullptr || out->m_prepared) {
        return VN_INVALID_ARG;
    }
    const AttenuationProfile* live = FindProfile(id);
    if (live == nullptr) {
        return VN_UNKNOWN_ID;
    }
    const auto liveIndex = static_cast<std::uint32_t>(live - m_profiles.data());
    const auto generation = m_profileGenerations[liveIndex];
    if (generation == 0) {
        return VN_INVALID_ARG;
    }
    std::uint32_t census = 0;
    for (const VoiceMirror& mirror : m_mirrors) {
        if (mirror.handle != 0 && mirror.retainIndex < 0 &&
            mirror.profileIndex == static_cast<std::int32_t>(liveIndex) &&
            mirror.profileGeneration == generation) {
            ++census;
        }
    }
    std::size_t free = m_retainRing.size();
    for (std::size_t i = 0; i < m_retainRing.size(); ++i) {
        if (m_retainRing[i].state == RetainSlotState::Free &&
            m_retainRing[i].epoch != UINT32_MAX) {
            free = i;
            break;
        }
    }
    if (free == m_retainRing.size() && free < m_retainRing.Capacity()) {
        if (!m_retainRing.ConstructThrough(free + 1u)) return VN_BACKPRESSURE;
    }
    if (free == m_retainRing.size()) {
        return VN_PACK_RESIDENT_CAPACITY;
    }
    if (m_prepareRetainedStorage != nullptr &&
        !m_prepareRetainedStorage(m_retainedStorageContext)) return VN_BACKPRESSURE;
    RetainSlot& slot = m_retainRing[free];
    slot.pod = *live;
    slot.id = id;
    slot.generation = generation;
    slot.census = census;
    slot.state = RetainSlotState::Prepared;
    out->m_slotIndex = static_cast<std::uint32_t>(free);
    out->m_prepared = true;
    out->m_retain = true;
    out->m_census = census;
    out->m_generation = slot.generation;
    out->m_profile = *live;
    return VN_OK;
}

void Spatializer::CancelPreparedRetain(PreparedProfileToken* token) noexcept {
    if (token == nullptr || !token->m_prepared || !token->m_retain) {
        return;
    }
    if (token->m_slotIndex < m_retainRing.size()) {
        RetainSlot& slot = m_retainRing[token->m_slotIndex];
        if (slot.state == RetainSlotState::Prepared) {
            slot.state = RetainSlotState::Free;
            slot.census = 0;
        }
    }
    *token = PreparedProfileToken{};
}

void Spatializer::CommitPreparedRetain(PreparedProfileToken* token) noexcept {
    if (token == nullptr || !token->m_prepared || !token->m_retain ||
        token->m_slotIndex >= m_retainRing.size()) {
        return;
    }
    RetainSlot& slot = m_retainRing[token->m_slotIndex];
    slot.state = RetainSlotState::Published;
    for (VoiceMirror& mirror : m_mirrors) {
        if (mirror.handle == 0 || mirror.profileIndex < 0 || mirror.retainIndex >= 0) {
            continue;
        }
        const std::size_t liveIndex =
            static_cast<std::size_t>(mirror.profileIndex);
        if (liveIndex >= m_profiles.size() || m_profiles[liveIndex].id != slot.id ||
            mirror.profileGeneration != slot.generation) {
            continue;
        }
        mirror.retainIndex = static_cast<std::int32_t>(token->m_slotIndex);
        mirror.retainEpoch = slot.epoch;
        mirror.profileGeneration = slot.generation;
        mirror.profileIndex = -1;
    }
    *token = PreparedProfileToken{};
}

void Spatializer::NoteVoiceEnded(VnVoiceHandle handle) noexcept {
    VoiceMirror* mirror = MirrorFor(handle);
    if (mirror == nullptr) {
        return;
    }
    ReleaseRetainedMirror(*mirror);
    *mirror = VoiceMirror{};
}

void Spatializer::ReleaseRetainedMirror(VoiceMirror& mirror) noexcept {
    if (mirror.retainIndex >= 0) {
        const std::size_t index = static_cast<std::size_t>(mirror.retainIndex);
        if (index < m_retainRing.size()) {
            RetainSlot& slot = m_retainRing[index];
            if (slot.state == RetainSlotState::Published &&
                slot.epoch == mirror.retainEpoch && slot.generation == mirror.profileGeneration) {
                if (slot.census > 0) {
                    --slot.census;
                }
            }
        }
    }
    mirror.retainIndex = -1;
    mirror.retainEpoch = 0;
}

Spatializer::PreparedProfileSet::~PreparedProfileSet() noexcept {
    if (m_owner != nullptr) {
        m_owner->CancelPreparedSet(this);
    }
}

Spatializer::PreparedProfileSet::PreparedProfileSet(PreparedProfileSet&& other) noexcept {
    *this = std::move(other);
}

Spatializer::PreparedProfileSet& Spatializer::PreparedProfileSet::operator=(
    PreparedProfileSet&& other) noexcept {
    if (this != &other) {
        if (m_owner != nullptr) {
            m_owner->CancelPreparedSet(this);
        }
        m_owner = std::exchange(other.m_owner, nullptr);
        m_profiles = std::move(other.m_profiles);
        m_generations = other.m_generations;
        m_occupied = other.m_occupied;
        m_retains = std::move(other.m_retains);
        m_count = other.m_count;
    }
    return *this;
}

VnAttenuationId Spatializer::PreparedProfileSet::IdAt(std::uint32_t slot) const noexcept {
    return slot < m_profiles.size() && m_generations[slot] != 0 ? m_profiles[slot].id : VnAttenuationId{};
}

std::uint64_t Spatializer::PreparedProfileSet::GenerationAt(std::uint32_t slot) const noexcept {
    return slot < m_generations.size() ? m_generations[slot] : 0;
}

VnResult Spatializer::PreparePackProfiles(const PackProfileInput* profiles,
    std::size_t count, std::uint64_t generation, PreparedProfileSet* out,
    const std::vector<RetainedProfileKey>& retained) {
    if (out == nullptr) {
        return VN_INVALID_ARG;
    }
    if (out->IsPrepared() || m_pendingProfileSet) {
        return VN_IN_USE;
    }
    if (count != 0 && (profiles == nullptr || generation == 0)) {
        return VN_INVALID_ARG;
    }
    const auto keysValid = ValidateRetainedKeys(retained, m_retainRing.Capacity());
    if (keysValid != VN_OK) {
        return keysValid;
    }
    if (count > kMaxAttenuationProfiles) {
        return VN_PACK_RESIDENT_CAPACITY;
    }
    if (count != 0 && !m_profiles.ConstructThrough(m_profiles.Capacity())) return VN_BACKPRESSURE;
    try {
        PreparedProfileSet prepared;
        prepared.m_owner = this;
        m_pendingProfileSet = true;
        detail::LogicAllocCheckpoint(detail::LogicAllocSite::None);
        prepared.m_profiles.assign(m_profiles.begin(), m_profiles.end());
        prepared.m_generations = m_profileGenerations;
        prepared.m_occupied = m_profileOccupied;
        detail::LogicAllocCheckpoint(detail::LogicAllocSite::None);
        prepared.m_retains.reserve(kMaxAttenuationProfiles);
        for (std::uint32_t slot = 0; slot < m_profiles.size(); ++slot) {
            if (m_profileGenerations[slot] != 0) {
                prepared.m_profiles[slot] = {};
                prepared.m_generations[slot] = 0;
                prepared.m_occupied[slot] = 0;
            }
        }
        std::array<AttenuationProfile, kMaxAttenuationProfiles> built{};
        std::array<std::uint32_t, kMaxAttenuationProfiles> assigned{};
        assigned.fill(UINT32_MAX);
        for (std::size_t i = 0; i < count; ++i) {
            if (profiles[i].id == VnObjectId{}) {
                return VN_BAD_PACK;
            }
            const auto valid = BuildProfileFromDesc(profiles[i].id, profiles[i].descriptor, &built[i]);
            if (valid != VN_OK) {
                return valid;
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (profiles[i].id == profiles[j].id) {
                    return VN_BAD_PACK;
                }
            }
            const auto* old = FindProfile(profiles[i].id);
            if (old == nullptr) {
                continue;
            }
            const auto slot = static_cast<std::uint32_t>(old - m_profiles.data());
            if (m_profileGenerations[slot] == 0) {
                return VN_IN_USE;
            }
            const bool unchanged = SameProfile(*old, built[i]);
            if (!unchanged && generation <= m_profileGenerations[slot]) {
                return VN_REVISION_CONFLICT;
            }
            assigned[i] = slot;
            prepared.m_profiles[slot] = built[i];
            prepared.m_occupied[slot] = 1;
            prepared.m_generations[slot] = unchanged ? m_profileGenerations[slot] : generation;
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (assigned[i] != UINT32_MAX) {
                continue;
            }
            for (const auto& retained : m_retainRing) {
                if (retained.state != RetainSlotState::Free && retained.id == profiles[i].id &&
                    generation <= retained.generation) {
                    return VN_REVISION_CONFLICT;
                }
            }
            const auto slot = std::find(prepared.m_occupied.begin(), prepared.m_occupied.end(), 0);
            if (slot == prepared.m_occupied.end()) {
                return VN_PACK_RESIDENT_CAPACITY;
            }
            const auto index = static_cast<std::size_t>(slot - prepared.m_occupied.begin());
            prepared.m_profiles[index] = built[i];
            *slot = 1;
            prepared.m_generations[index] = generation;
        }
        for (std::uint32_t slot = 0; slot < m_profiles.size(); ++slot) {
            if (prepared.m_occupied[slot] != 0) {
                ++prepared.m_count;
            }
            if (m_profileGenerations[slot] == 0 ||
                (prepared.m_profiles[slot].id == m_profiles[slot].id &&
                 prepared.m_generations[slot] == m_profileGenerations[slot])) {
                continue;
            }
            bool needed = RetainsProfile(retained, m_profiles[slot].id,
                                         m_profileGenerations[slot]);
            for (const auto& mirror : m_mirrors) {
                needed = needed || (mirror.handle != 0 && mirror.retainIndex < 0 &&
                    mirror.profileIndex == static_cast<std::int32_t>(slot) &&
                    mirror.profileGeneration == m_profileGenerations[slot]);
            }
            if (needed) {
                prepared.m_retains.emplace_back();
                const auto result = PrepareRetainProfile(m_profiles[slot].id, &prepared.m_retains.back());
                if (result != VN_OK) {
                    return result;
                }
            }
        }
        *out = std::move(prepared);
        return VN_OK;
    } catch (const std::bad_alloc&) {
        return VN_BACKPRESSURE;
    } catch (const std::length_error&) {
        return VN_PACK_RESIDENT_CAPACITY;
    }
}

void Spatializer::CancelPreparedSet(PreparedProfileSet* prepared) noexcept {
    if (prepared == nullptr || prepared->m_owner != this) {
        return;
    }
    for (auto& token : prepared->m_retains) {
        CancelPreparedRetain(&token);
    }
    m_pendingProfileSet = false;
    prepared->m_owner = nullptr;
}

void Spatializer::CommitPreparedSet(PreparedProfileSet* prepared) noexcept {
    const bool valid = prepared != nullptr && prepared->m_owner == this &&
        prepared->m_profiles.size() == m_profiles.size();
    assert(valid);
    if (!valid) {
        return;
    }
    for (auto& token : prepared->m_retains) {
        CommitPreparedRetain(&token);
    }
    std::copy(prepared->m_profiles.begin(), prepared->m_profiles.end(), m_profiles.begin());
    m_profileGenerations = prepared->m_generations;
    m_profileOccupied = prepared->m_occupied;
    m_profileCount = prepared->m_count;
    m_pendingProfileSet = false;
    prepared->m_owner = nullptr;
}

VnResult Spatializer::CheckRetainedProfileKeys(
    const std::vector<RetainedProfileKey>& retained) const noexcept {
    return ValidateRetainedKeys(retained, m_retainRing.Capacity());
}

bool Spatializer::VisitPublishedRetainSlots(
    bool (*visit)(void* context, VnAttenuationId id, std::uint64_t generation) noexcept,
    void* context) const noexcept {
    if (visit == nullptr) {
        return false;
    }
    for (const RetainSlot& slot : m_retainRing) {
        if (slot.state != RetainSlotState::Published || slot.id == VnObjectId{} ||
            slot.generation == 0) {
            continue;
        }
        if (!visit(context, slot.id, slot.generation)) {
            return false;
        }
    }
    return true;
}

void Spatializer::SweepEndedVoices(VoiceLiveness alive, void* context) noexcept {
    if (m_pendingProfileSet || alive == nullptr) {
        return;
    }
    for (const auto& mirror : m_mirrors) {
        if (mirror.handle != 0 && !alive(context, mirror.handle)) {
            NoteVoiceEnded(mirror.handle);
        }
    }
}

VnResult Spatializer::SweepRetainedProfiles(
    const std::vector<RetainedProfileKey>& retained) noexcept {
    if (m_pendingProfileSet) {
        return VN_IN_USE;
    }
    const auto keysValid = ValidateRetainedKeys(retained, m_retainRing.Capacity());
    if (keysValid != VN_OK) {
        return keysValid;
    }
    for (auto& slot : m_retainRing) {
        if (slot.state == RetainSlotState::Published && slot.census == 0 &&
            !RetainsProfile(retained, slot.id, slot.generation)) {
            slot.state = RetainSlotState::Free;
            if (slot.epoch != UINT32_MAX) {
                ++slot.epoch;
            }
        }
    }
    return VN_OK;
}

std::size_t Spatializer::RetainedProfileCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(m_retainRing.begin(), m_retainRing.end(),
        [](const auto& slot) { return slot.state == RetainSlotState::Published; }));
}

VnResult Spatializer::SetVoiceRetainEpochForTests(VnVoiceHandle handle,
                                                std::uint32_t epoch) noexcept {
    auto* mirror = MirrorFor(handle);
    if (mirror == nullptr || mirror->retainIndex < 0 || epoch == 0) {
        return VN_INVALID_ARG;
    }
    mirror->retainEpoch = epoch;
    return VN_OK;
}

VnResult Spatializer::SetRetainEpochForTests(std::size_t index, std::uint32_t epoch) noexcept {
    if (index >= m_retainRing.Capacity() || epoch == 0) {
        return VN_INVALID_ARG;
    }
    if (!m_retainRing.ConstructThrough(index + 1u)) return VN_BACKPRESSURE;
    if (m_retainRing[index].state != RetainSlotState::Free) return VN_INVALID_ARG;
    m_retainRing[index].epoch = epoch;
    return VN_OK;
}

} // namespace harken
