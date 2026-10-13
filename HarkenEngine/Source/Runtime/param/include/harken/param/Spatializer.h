// Spatializer, logic-thread side: listener state, per-voice emitter
// mirrors (position, orientation, spatial mode, bound attenuation
// profile), the attenuation profile registry, and the per-tick evaluation
// that turns geometry into parameter contributions.
//
// Attenuation is not a special system — it is the parameter system applied
// to distance. Each tick (inside Vn_Update, before bindings evaluate and
// commits flush) the spatializer computes every spatialized voice's
// distance and azimuth from the logic-side mirrors, evaluates the bound
// profile's curves (override or built-in default) with distance as the
// input, and writes the results into the combine model's attenuation
// slots. The engine-computed pan travels the same way: all engine spatial
// contributions occupy the attenuation slot, leaving the API-direct slot
// to the caller.
//
// The mirrors duplicate the emitter positions the render contexts also
// hold; the render side keeps its copy for future render-rate consumers,
// while all acoustic math reads the logic-side mirror — the audio thread
// never computes geometry.
#ifndef HARKEN_PARAM_SPATIALIZER_H
#define HARKEN_PARAM_SPATIALIZER_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>

#include "harken/core/EngineConfig.h"
#include "harken/param/Curve.h"
#include "harken/param/ParamSystem.h"

#include <cstdint>
#include <array>
#include <vector>

namespace harken {

// The emitter table an attached voice derives its geometry from each tick.
// Forward-declared: the spatializer reads emitter poses through a reference,
// no ownership.
class EmitterTable;

// Attenuation profile table capacity, preallocated at init like the
// Variable table (kMaxVariables precedent).
inline constexpr std::uint32_t kMaxAttenuationProfiles = 64;

// The distinct targets one spatialized voice's evaluation can dirty in a
// single tick: the engine-computed pan plus the four profile-driven
// outputs (volume, lowpass cutoff, width, low-shelf gain) — the exact
// SetAttenuationSlot destinations in Evaluate(). Config-time worst-case
// publication estimates multiply this by maxVoices; keep it in step with
// the evaluation whenever a spatial contribution is added or removed.
inline constexpr std::uint32_t kSpatialDrivenTargetsPerVoice = 5;

// A registered attenuation profile in evaluated form: validated scalars
// plus prebuilt override curves (distance in, target-domain value out).
struct AttenuationProfile {
    VnAttenuationId id{};
    std::uint32_t rolloff = VN_ROLLOFF_LINEAR;
    float minDistance = 1.0f;
    float maxDistance = 2.0f;
    float airNearHz = 20000.0f;
    float airFarHz = 20000.0f;
    float proximityRadius = 0.0f;
    float proximityMaxBoostDb = 0.0f;
    float proximityFrequency = kToneLowShelfDefaultHz;
    bool coneEnabled = false;
    float coneInnerDeg = 0.0f;
    float coneOuterDeg = 0.0f;
    float coneOuterGainDb = 0.0f;
    float coneOuterLowpassHz = 20000.0f;
    bool volumeOverride = false;
    bool lowpassOverride = false;
    bool widthOverride = false;
    Curve volumeCurve;
    Curve lowpassCurve;
    Curve widthCurve;
};

class Spatializer {
public:
    explicit Spatializer(const EngineConfig& config);

    class PreparedProfileToken {
    public:
        bool IsPrepared() const noexcept { return m_prepared; }
        std::uint32_t SlotIndex() const noexcept { return m_slotIndex; }

    private:
        friend class Spatializer;

        AttenuationProfile m_profile{};
        std::uint32_t m_slotIndex = 0;
        bool m_append = false;
        bool m_prepared = false;
        bool m_retain = false;
        std::uint32_t m_census = 0;
        std::uint64_t m_generation = 0;
    };

    struct PackProfileInput {
        VnAttenuationId id{};
        const VnAttenuationDesc* descriptor = nullptr;
    };
    struct RetainedProfileKey {
        VnAttenuationId id{};
        std::uint64_t generation = 0;
    };
    using VoiceLiveness = bool (*)(void*, VnVoiceHandle) noexcept;

    class PreparedProfileSet {
    public:
        PreparedProfileSet() = default;
        ~PreparedProfileSet() noexcept;
        PreparedProfileSet(const PreparedProfileSet&) = delete;
        PreparedProfileSet& operator=(const PreparedProfileSet&) = delete;
        PreparedProfileSet(PreparedProfileSet&& other) noexcept;
        PreparedProfileSet& operator=(PreparedProfileSet&& other) noexcept;
        bool IsPrepared() const noexcept { return m_owner != nullptr; }
        VnAttenuationId IdAt(std::uint32_t slot) const noexcept;
        std::uint64_t GenerationAt(std::uint32_t slot) const noexcept;

    private:
        friend class Spatializer;
        Spatializer* m_owner = nullptr;
        std::vector<AttenuationProfile> m_profiles;
        std::array<std::uint64_t, kMaxAttenuationProfiles> m_generations{};
        std::array<std::uint8_t, kMaxAttenuationProfiles> m_occupied{};
        std::vector<PreparedProfileToken> m_retains;
        std::uint32_t m_count = 0;
    };

    VnResult PreparePackProfiles(const PackProfileInput* profiles, std::size_t count,
        std::uint64_t generation, PreparedProfileSet* out,
        const std::vector<RetainedProfileKey>& retained = {});
    void CancelPreparedSet(PreparedProfileSet* prepared) noexcept;
    void CommitPreparedSet(PreparedProfileSet* prepared) noexcept;
    void SweepEndedVoices(VoiceLiveness alive, void* context) noexcept;
    VnResult SweepRetainedProfiles(
        const std::vector<RetainedProfileKey>& retained = {}) noexcept;
    VnResult CheckRetainedProfileKeys(
        const std::vector<RetainedProfileKey>& retained) const noexcept;
    bool VisitPublishedRetainSlots(
        bool (*visit)(void* context, VnAttenuationId id,
                      std::uint64_t generation) noexcept,
        void* context) const noexcept;
    std::size_t RetainedProfileCount() const noexcept;
    VnResult SetRetainEpochForTests(std::size_t slot, std::uint32_t epoch) noexcept;
    VnResult SetVoiceRetainEpochForTests(VnVoiceHandle handle, std::uint32_t epoch) noexcept;

    // Read-only registered-profile lookup; also the internal C++ test
    // observation seam for registration semantics (no C ABI exposure).
    const AttenuationProfile* FindProfile(VnAttenuationId id,
                                         std::uint64_t generation = 0) const;
    std::uint64_t CurrentProfileGeneration(VnAttenuationId id) const noexcept;

    // Listener pose. The caller (API layer) has already rejected
    // non-finite components; this refuses a degenerate basis — zero-length
    // forward or up, or parallel forward and up — because the panning
    // right axis is their cross product.
    VnResult SetListener(VnVec3 pos, VnVec3 forward, VnVec3 up);

    // Profile registration: validates the descriptor, builds override
    // curves, and stores the evaluated form. VN_INVALID_ARG for a null or
    // mis-sized descriptor, non-finite or out-of-contract fields, or an
    // invalid override curve; VN_IN_USE for an id already registered;
    // VN_NO_ATTENUATION for a full table.
    VnResult RegisterProfile(VnAttenuationId id,
                             const VnAttenuationDesc* desc);
    std::uint32_t FreeProfileSlotAtOrdinal(std::uint32_t ordinal) const noexcept;

    // Pure registration admission: the exact refusal RegisterProfile would
    // produce for this id and descriptor — VN_INVALID_ARG on any descriptor
    // fault (override curves included), VN_IN_USE for an id already
    // registered, VN_NO_ATTENUATION for a full table — with zero side
    // effects. The registration path runs this admission first, so the two
    // cannot drift.
    VnResult CanRegisterProfile(VnAttenuationId id,
                                const VnAttenuationDesc* desc) const;

    // Hot reload: rebuilds a registered profile's evaluated form from a new
    // descriptor, in place. Validation is RegisterProfile's, complete before
    // any state change (a refusal keeps the old curves fully in force);
    // VN_UNKNOWN_ID when the id names no registered profile. Bound voices
    // pick the new curves up at the next per-tick evaluation — the parameter
    // system's ordinary dirty path; logic-side curves need no generation.
    VnResult ReplaceProfile(VnAttenuationId id,
                            const VnAttenuationDesc* desc);

    // Prepared Pack commits bind the validated, fully built profile to an
    // exact preallocated slot before the media transaction. Register tokens
    // use the caller's revision-wide append slot; replace tokens resolve the
    // current profile slot. Commit performs only one POD assignment and the
    // append census update.
    VnResult PrepareRegisterProfile(VnAttenuationId id,
                                    const VnAttenuationDesc* desc,
                                    std::uint32_t slotIndex,
                                    PreparedProfileToken* out) const;
    VnResult PrepareReplaceProfile(VnAttenuationId id,
                                   const VnAttenuationDesc* desc,
                                   PreparedProfileToken* out) const;
    void CancelPreparedProfile(PreparedProfileToken* token) noexcept;
    void CommitPreparedProfile(PreparedProfileToken* token) noexcept;

    void NoteVoiceEnded(VnVoiceHandle handle) noexcept;

    // Voice mirror lifecycle, called by the API layer. OnVoiceStarted
    // claims the reused slot's mirror (positions from the play
    // descriptor, default orientation, no profile). SetVoicePosition
    // stores for every voice — 2D voices keep their position with no
    // acoustic consequence. Handle-mismatched calls are ignored (the
    // voice ended; same advisory rule as the rest of the logic side).
    // emitterHandle (0 = none) attaches the voice to an emitter: each tick
    // the evaluation fans the emitter's pose into this voice's mirror, and
    // the per-voice position/orientation setters are refused while attached.
    void OnVoiceStarted(VnVoiceHandle handle, bool spatialized, float x,
                        float y, float z, VnEmitterHandle emitterHandle = 0);
    void SetVoicePosition(VnVoiceHandle handle, float x, float y, float z);
    void SetVoiceDistanceScale(VnVoiceHandle handle, float scale);
    void SetVoiceSpatializationMix(VnVoiceHandle handle, float mix);

    // The emitter a live voice is attached to, or 0 (none, or a stale voice
    // handle). The API layer combines this with the emitter table's liveness
    // to decide whether a per-voice spatial write is refused VN_EMITTER_ATTACHED
    // (attachment holds only while the emitter is live; a destroyed emitter
    // leaves the voice individually addressable again).
    VnEmitterHandle AttachedEmitter(VnVoiceHandle handle) const;

    // Emitter orientation for the directional cone. The vector must be
    // non-zero (VN_INVALID_ARG); it is normalized here so evaluation
    // never renormalizes per tick.
    VnResult SetVoiceOrientation(VnVoiceHandle handle, VnVec3 forward);

    // Binds a voice's mirror to a registered profile; VN_UNKNOWN_ID when
    // the id was never registered. Handle liveness was checked by the API
    // layer; a mirror mismatch is a benign no-op returning VN_OK-shaped
    // staleness handled by the caller's CheckLive.
    VnResult BindVoice(VnVoiceHandle handle, VnAttenuationId attenuationId,
                       std::uint64_t generation = 0);

    // Whether a profile id is registered. Lets the API layer validate an
    // attenuation bind staged for a not-yet-started voice with the same
    // VN_UNKNOWN_ID contract the live bind applies.
    bool HasProfile(VnAttenuationId id, std::uint64_t generation = 0) const;

    // Per-tick spatial evaluation (inside Vn_Update, before the commit
    // flush). First fans out emitters: every voice attached to a live emitter
    // takes the emitter's position and orientation (so one emitter move
    // updates all its voices); a voice whose emitter was destroyed detaches
    // and keeps its last emitter-derived state frozen. Then, for every live
    // spatialized voice, computes distance and azimuth and writes pan plus —
    // when a profile is bound — volume, lowpass, width, and near-field shelf
    // contributions into the attenuation slots. Voices whose geometry did not
    // change write identical values, which the slot writer treats as no-ops.
    void Evaluate(ParamSystem& params, const EmitterTable& emitters);

    // The distance-driven volume attenuation in dB that Evaluate() writes to
    // the volume attenuation slot for a voice at world position (px,py,pz)
    // bound to this profile — the non-cone term (override curve or rolloff).
    // Recomputes a parked voice's audibility contribution from its stored (or
    // its live emitter's) position against the current listener, without a
    // render slot. Returns 0 for id 0 or an unregistered profile. The cone
    // angular term is deliberately excluded: a parked voice's cone contribution
    // stays frozen at its park value (distance-driven return is tracked;
    // cone-angle-driven return is not).
    float AttenuationVolumeDb(VnAttenuationId attenuationId, float px, float py,
                              float pz, float distanceScale,
                              std::uint64_t generation = 0) const;

    // A portable snapshot of a voice's spatial mirror, for park/return. Return
    // re-applies it to the new render slot (OnVoiceStarted + BindVoice +
    // SetVoiceOrientation) so the returned voice keeps its geometry.
    struct SpatialVoiceState {
        bool spatialized = false;
        float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
        float fwdX = 0.0f, fwdY = 0.0f, fwdZ = -1.0f;
        float distanceScale = 1.0f;
        float spatializationMix = 1.0f;
        VnAttenuationId attenuationId{}; // 0 = no profile bound
        std::uint64_t attenuationGeneration = 0;
        VnEmitterHandle emitterHandle = 0; // 0 = none
    };
    // Captures the live voice's spatial mirror into *out. False for a stale
    // voice handle (no mirror owns it).
    bool CaptureVoice(VnVoiceHandle handle, SpatialVoiceState* out) const;

    std::uint32_t ProfileCount() const { return m_profileCount; }
    std::uint32_t ProfileCapacity() const { return kMaxAttenuationProfiles; }

private:
    struct VoiceMirror {
        VnVoiceHandle handle = 0; // 0 = slot never started a voice
        VnEmitterHandle emitterHandle = 0; // 0 = none; set = follows an emitter
        bool spatialized = false;
        bool evaluated = false; // first spatial installation may snap
        float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
        float distanceScale = 1.0f;
        float spatializationMix = 1.0f;
        // Emitter forward, unit length; the cone's axis.
        float fwdX = 0.0f, fwdY = 0.0f, fwdZ = -1.0f;
        std::int32_t profileIndex = -1; // -1 = no attenuation bound
        std::int32_t retainIndex = -1;
        std::uint32_t retainEpoch = 0;
        std::uint64_t profileGeneration = 0;
    };

    enum class RetainSlotState : std::uint8_t {
        Free = 0,
        Prepared = 1,
        Published = 2,
    };

    struct RetainSlot {
        AttenuationProfile pod{};
        VnAttenuationId id{};
        std::uint64_t generation = 0;
        std::uint32_t epoch = 1;
        std::uint32_t census = 0;
        RetainSlotState state = RetainSlotState::Free;
    };

    VnResult PrepareRetainProfile(VnAttenuationId id, PreparedProfileToken* out);
    void CancelPreparedRetain(PreparedProfileToken* token) noexcept;
    void CommitPreparedRetain(PreparedProfileToken* token) noexcept;
    const AttenuationProfile* ResolveProfile(const VoiceMirror& mirror) const noexcept;
    void ReleaseRetainedMirror(VoiceMirror& mirror) noexcept;

    VoiceMirror* MirrorFor(VnVoiceHandle handle);

    // Listener pose. rightX/Y/Z is the normalized forward x up cross
    // product, rebuilt at SetListener so evaluation is pure arithmetic.
    float m_listenerX = 0.0f, m_listenerY = 0.0f, m_listenerZ = 0.0f;
    float m_rightX = 1.0f, m_rightY = 0.0f, m_rightZ = 0.0f;

    std::vector<VoiceMirror> m_mirrors; // indexed by voice slot
    std::vector<AttenuationProfile> m_profiles; // fixed capacity
    std::array<std::uint64_t, kMaxAttenuationProfiles> m_profileGenerations{};
    std::array<std::uint8_t, kMaxAttenuationProfiles> m_profileOccupied{};
    std::uint32_t m_profileCount = 0;
    std::vector<RetainSlot> m_retainRing;
    bool m_pendingProfileSet = false;
};

} // namespace harken

#endif // HARKEN_PARAM_SPATIALIZER_H
