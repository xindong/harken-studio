// Internal engine object behind the C API. This header is not installed; the
// test harness and the future host include it directly to reach the null
// backend's pump (the public API stays narrow — spec section 16 drives blocks
// with a test clock, not with public entry points).
#ifndef HARKEN_API_ENGINE_H
#define HARKEN_API_ENGINE_H

#include <harken/ObjectId.h>
#include "DeviceBackend.h"
#include "EngineFrontierOwnerLedger.h"
#include "EnginePendingSelectionBatch.h"
#include "EngineNativeVoiceSide.h"
#include "EngineQueueScheduleStorage.h"
#include "PlayableRuntime.h"
#include "ProjectGenerationTransaction.h"
#include "TraceCodec.h"
#include "PackLogicStore.h"
#include "TraceRecorder.h"
#include "MonitorEmitterRegistry.h"
#include "harken/render/OriginalSourceSubmission.h"
#include "PreparedSingleCurrentOriginBundle.h"

#include "harken/core/ChannelLayout.h"
#include "harken/core/ActionFiringTable.h"
#include "harken/core/ActionSeekTable.h"
#include "harken/core/StreamSeekHandoffTable.h"
#include "harken/core/ActionDeferredRecordTable.h"
#include "harken/core/ActionCooldownTable.h"
#include "harken/core/CapturedTimelineVoiceInstall.h"
#include "harken/core/AuthoredStopFence.h"
#include "harken/core/CommandQueue.h"
#include "harken/core/CpuClock.h"
#include "harken/core/CurrentSelection.h"
#include "harken/core/EngineConfig.h"
#include "harken/core/MeterBlockRing.h"
#include "harken/core/NotificationQueue.h"
#include "harken/core/LogRing.h"
#include "harken/core/LogicAllocCheckpoint.h"
#include "harken/core/KnownCompletion.h"
#include "harken/core/Random.h"
#include "harken/core/SnapshotBuffer.h"
#include "harken/core/SourceCursorMirror.h"
#include "harken/core/DeferredIntentPool.h"
#include "harken/core/OccurrenceStatePool.h"
#include "harken/core/PackComposition.h"
#include "harken/core/CompositeCompletionOwnerPool.h"
#include "harken/core/PlayableInstanceTable.h"
#include "harken/core/ContainerPlaybackTable.h"
#include "harken/core/QueueRuntime.h"
#include "harken/core/VariableControlRuntime.h"
#include "harken/core/VariableControlGrid.h"
#include "harken/core/PlayablePropertyCut.h"
#include "harken/core/ProgramGenerationRegistry.h"
#include "harken/core/ScheduledTransactionPool.h"
#include "harken/core/StableSlotStorage.h"
#include "harken/core/RuntimeWorkIndex.h"
#include "harken/core/DecisionTable.h"
#include "harken/core/ArmGroupTable.h"
#include "harken/core/TimelineArmTable.h"
#include "harken/core/TimelineExecutor.h"
#include "harken/core/ImmediateActionLane.h"
#include "harken/core/OriginalImmediateLaneConditionalProjection.h"
#include "harken/core/PrivateSelectedQueueFutureCalendar.h"
#include "harken/core/QueueContinuationLane.h"
#include "harken/core/PrivateOriginalQueueGroupClockProof.h"
#include "harken/core/TimelinePlanner.h"
#include "harken/core/TransitionInstancePool.h"
#include "harken/core/TransportEpochOwner.h"
#include "harken/core/VariableSliceTable.h"
#include "harken/core/VoiceLedger.h"
#include "harken/core/VoiceManager.h"
#include "harken/media/PcmRegistry.h"
#include "harken/media/PackTable.h"
#include "harken/media/StreamManager.h"
#include "harken/param/BindingTable.h"
#include "harken/param/BusTargetStore.h"
#include "harken/param/EffectParamTargetStore.h"
#include "harken/param/EmitterTable.h"
#include "harken/param/MeterProbe.h"
#include "harken/param/ParamSystem.h"
#include "harken/param/ParamTargetTable.h"
#include "harken/param/PropertyPublisher.h"
#include "harken/param/Spatializer.h"
#include "harken/param/VariableService.h"
#include "harken/render/BuiltinEffects.h"
#include "harken/render/BusTree.h"
#include "harken/render/DynamicRoute.h"
#include "harken/render/EffectPool.h"
#include "harken/render/MixerLoop.h"
#include "harken/render/NullBackend.h"
#include "harken/render/TopologyPlan.h"
#include "harken/render/VoicePool.h"
#include "harken/render/VoiceTimelineSink.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "harken/core/MonitorRequest.h"

namespace harken {

// Entry-neutral read-only authority used by Runtime convergence tests and the
// future execution adapters. It deliberately carries existing world views by
// reference; it does not own or mutate selection, playback, or ABI state.
enum class PlayableExecutionAuthorityKind : std::uint8_t {
    CurrentWorld = 0,
    FrozenWorld = 1,
    Pack = 2,
};

enum class PlayableExecutionResolveStatus : std::uint8_t {
    Ok = 0,
    InvalidRequest = 1,
    AuthorityUnavailable = 2,
    UnknownPlayable = 3,
    FingerprintMismatch = 4,
    UnsupportedPlayable = 5,
};

struct PlayableExecutionRequest {
    PlayableRefV1 root{};
};

struct PlayableExecutionCanonicalResult {
    PlayableExecutionResolveStatus status =
        PlayableExecutionResolveStatus::InvalidRequest;
    PlayableRefV1 root{};
    PlayableKindV1 kind = PlayableKindV1::Sound;
    VnObjectId stableId{};
    std::uint64_t programSemanticFingerprint = 0;
    std::uint64_t authorityIdentity = 0;
    ProgramSourceIdentityV1 source{};
    std::uint32_t initialClosureCapabilities = 0;
};

class PlayableExecutionAuthority {
public:
    static PlayableExecutionAuthority Current(
        const PlayableWorldViewV1& world) noexcept {
        return {PlayableExecutionAuthorityKind::CurrentWorld, &world, nullptr};
    }
    static PlayableExecutionAuthority Frozen(
        const PlayableWorldViewV1& world) noexcept {
        return {PlayableExecutionAuthorityKind::FrozenWorld, &world, nullptr};
    }
    static PlayableExecutionAuthority Pack(
        const PackPlaybackView& world) noexcept {
        return {PlayableExecutionAuthorityKind::Pack, nullptr, &world};
    }

    PlayableExecutionAuthorityKind Kind() const noexcept { return kind_; }
    std::uint64_t Identity() const noexcept {
        return world_ != nullptr ? world_->generation
             : pack_ != nullptr ? pack_->CompositeGeneration()
                                : 0;
    }

    PlayableExecutionCanonicalResult Resolve(
        const PlayableExecutionRequest& request) const noexcept {
        PlayableExecutionCanonicalResult result{};
        result.root = request.root;
        result.kind = request.root.kind;
        result.stableId = request.root.stableId;
        result.programSemanticFingerprint =
            request.root.expectedProgramSemanticFingerprint;
        result.authorityIdentity = Identity();
        if (request.root.stableId == VnObjectId{} ||
            request.root.expectedProgramSemanticFingerprint == 0) {
            result.status = PlayableExecutionResolveStatus::InvalidRequest;
            return result;
        }
        if (world_ == nullptr && pack_ == nullptr) {
            result.status = PlayableExecutionResolveStatus::AuthorityUnavailable;
            return result;
        }

        const auto contentKind = [&]() noexcept
            -> playableproperties::ContentPropertyKindV1 {
            switch (request.root.kind) {
            case PlayableKindV1::Sound:
                return playableproperties::ContentPropertyKindV1::Sound;
            case PlayableKindV1::Container:
                return playableproperties::ContentPropertyKindV1::Container;
            case PlayableKindV1::Track:
                return playableproperties::ContentPropertyKindV1::Track;
            case PlayableKindV1::Piece:
                return playableproperties::ContentPropertyKindV1::Piece;
            }
            return playableproperties::ContentPropertyKindV1::Layer;
        };
        if (request.root.kind != PlayableKindV1::Sound &&
            request.root.kind != PlayableKindV1::Container &&
            request.root.kind != PlayableKindV1::Track &&
            request.root.kind != PlayableKindV1::Piece) {
            result.status = PlayableExecutionResolveStatus::UnsupportedPlayable;
            return result;
        }

        const auto sameRef = [](const PlayableRefV1& a,
                                const PlayableRefV1& b) noexcept {
            return a.kind == b.kind && a.stableId == b.stableId &&
                   a.expectedProgramSemanticFingerprint ==
                       b.expectedProgramSemanticFingerprint &&
                   a.contentRevision == b.contentRevision &&
                   a.routeStableId == b.routeStableId &&
                   a.compatibilityFingerprint == b.compatibilityFingerprint;
        };
        result.source = world_ != nullptr ? world_->ProgramSource()
            : pack_->ResolveProgramSource(contentKind(), request.root.stableId);
        result.initialClosureCapabilities = world_ != nullptr
            ? world_->InitialClosureCapabilities()
            : pack_->InitialClosureCapabilities(request.root.kind, request.root.stableId);
        const auto property = [&]() noexcept
            -> const PlayablePropertyProgramViewV1* {
            if (world_ != nullptr) {
                for (std::uint32_t i = 0; i < world_->propertyCount; ++i) {
                    const auto& candidate = world_->properties[i];
                    if (candidate.contentPropertyKind == contentKind() &&
                        candidate.stableId == request.root.stableId) {
                        return &candidate;
                    }
                }
                return nullptr;
            }
            return pack_->ResolveProperty(contentKind(), request.root.stableId);
        }();
        if (request.root.kind == PlayableKindV1::Sound ||
            request.root.kind == PlayableKindV1::Container) {
            if (property == nullptr) {
                result.status = PlayableExecutionResolveStatus::UnknownPlayable;
                return result;
            }
            PlayableRefV1 canonicalRef{};
            canonicalRef.kind = request.root.kind;
            canonicalRef.stableId = property->stableId;
            canonicalRef.expectedProgramSemanticFingerprint =
                property->programSemanticFingerprint;
            canonicalRef.contentRevision = property->contentRevision;
            canonicalRef.routeStableId = property->routeStableId;
            canonicalRef.compatibilityFingerprint =
                property->compatibilityFingerprint;
            if (!sameRef(canonicalRef, request.root)) {
                result.status = PlayableExecutionResolveStatus::FingerprintMismatch;
                return result;
            }
            result.root.kind = request.root.kind;
            result.root.stableId = property->stableId;
            result.root.expectedProgramSemanticFingerprint =
                property->programSemanticFingerprint;
            result.root.contentRevision = property->contentRevision;
            result.root.routeStableId = property->routeStableId;
            result.root.compatibilityFingerprint =
                property->compatibilityFingerprint;
            result.status = PlayableExecutionResolveStatus::Ok;
            return result;
        }

        const PlayableRefV1* canonical = nullptr;
        if (request.root.kind == PlayableKindV1::Track) {
            if (world_ != nullptr) {
                for (std::uint32_t i = 0; i < world_->trackScheduleCount; ++i) {
                    const auto& row = world_->trackSchedules[i];
                    if (row.rootDefinition.stableId == request.root.stableId) {
                        canonical = &row.rootDefinition;
                        break;
                    }
                }
            } else if (const auto row =
                           pack_->ResolveTrack(request.root.stableId, 0)) {
                canonical = &row->rootDefinition;
            }
        } else {
            if (world_ != nullptr) {
                for (std::uint32_t i = 0; i < world_->pieceScheduleCount; ++i) {
                    const auto& row = world_->pieceSchedules[i];
                    if (row.rootDefinition.stableId == request.root.stableId) {
                        canonical = &row.rootDefinition;
                        break;
                    }
                }
            } else if (const auto row =
                           pack_->ResolvePiece(request.root.stableId, 0)) {
                canonical = &row->rootDefinition;
            }
        }
        if (canonical == nullptr) {
            result.status = PlayableExecutionResolveStatus::UnknownPlayable;
            return result;
        }
        if (!sameRef(*canonical, request.root)) {
            result.status = PlayableExecutionResolveStatus::FingerprintMismatch;
            return result;
        }
        result.root = *canonical;
        result.status = PlayableExecutionResolveStatus::Ok;
        return result;
    }

private:
    PlayableExecutionAuthority(PlayableExecutionAuthorityKind kind,
                               const PlayableWorldViewV1* world,
                               const PackPlaybackView* pack) noexcept
        : kind_(kind), world_(world), pack_(pack) {}

    PlayableExecutionAuthorityKind kind_ =
        PlayableExecutionAuthorityKind::CurrentWorld;
    const PlayableWorldViewV1* world_ = nullptr;
    const PackPlaybackView* pack_ = nullptr;
};

// Adapter-facing entry point. Keeping this symbol in the API translation unit
// makes the convergence seam explicit without exposing a public C ABI.
PlayableExecutionCanonicalResult ResolvePlayableExecutionEntry(
    const PlayableExecutionAuthority& authority,
    const PlayableExecutionRequest& request) noexcept;

// Logic-side mirror of one voice->bus send slot: whether it is active, the
// dense target bus index, and the last-set level. It mirrors the audio-thread
// send state so a park can capture a voice's sends, a pending start can stage
// them, and a return can replay them. Bus destruction removes tombstoned
// targets from every live, pending, and parked mirror before replay. Levels
// replay as a snap (a returned voice starts fresh at its virtual offset).
// Logic thread only.
struct VoiceSendConfig {
    bool active = false;
    std::uint32_t targetBusIndex = 0;
    float levelDb = 0.0f;
    std::uint32_t targetBusGeneration = 0;
};

// Staging for a voice whose start was deferred — by a steal (the pool was
// full when the raw voice start was requested, so a victim was told to stop and this voice
// waits for the freed slot), by a resolved container onset delay (the play
// is fully resolved but its start is due at a later frame), or by a staged
// install that waits for its caller's explicit commit. The record holds
// everything needed to issue the compiled play once its gates open, plus any
// per-voice state the caller set while the voice was pending — all applied at
// start so the deferred voice behaves as if it had started at its intended
// moment. One record per ledger slot; `active` marks the slots currently
// pending. Logic thread only.
struct PendingStartRecord {
    bool active = false;
    VnVoiceHandle publicHandle = 0;
    // Frame-clock gate for an onset-deferred start: the start is not issued
    // before the logic frame clock reaches this frame (0 = no time gate, the
    // steal-deferred case). Set from the resolved container path's summed
    // effective delay, tick-quantized by the same ms-to-frames arithmetic as
    // delayed Action Steps.
    std::uint64_t dueFrame = 0;
    // A staged install holds the record out of the deferred-start service
    // entirely: the caller is still installing per-voice state, and only its
    // explicit commit may publish the start. Cleared by that commit; a
    // rejected commit restores it, so a refusal leaves the install exactly as
    // it was and the deferred pass never starts a voice behind the caller's
    // back.
    bool stagedInstall = false;

    // Compiled play inputs.
    bool isStream = false;
    PCMView view;                       // PCM source (isStream == false)
    VnSoundId soundId{};              // registered PCM or stream Sound id
    std::uint64_t loopStartFrame = 0;
    std::uint64_t loopEndFrame = 0;
    std::uint32_t loopCount = 1;
    float baseVolumeDb = 0.0f;
    float outputBusVolumeDb = 0.0f;
    double ratio = 1.0;
    // Selected once from source and mix facts at play acceptance. Deferred
    // start, parking, and return inherit it; pitch updates never reselect it.
    ResampleProfile resampleProfile =
        ResampleProfile::LegacyCompatibility;
    std::uint32_t busIndex = 0;
    std::uint32_t busGeneration = 0;
    bool spatialized = false;
    SpatialSourceState source;
    std::uint32_t priority = 128;
    VnEmitterHandle emitterHandle = 0;  // emitter to attach at deferred start
    VnSoundId originPlayable{};       // Sound id named by a raw start or an Action
                                        // Step; the key matched by a stop Step
    bool heldPcmRef = false;            // a use-count reference held while pending
    // Stream source (isStream == true): the registration pinned against
    // unregistration while pending, released at start or cancel.
    std::uint32_t streamFileIndex = 0;
    StreamManager::StreamFacts streamFacts{};
    bool heldStreamRef = false;
    // A stream-slot reservation (Engine::pendingStreamReservations) is held for
    // this pending stream start until it claims a slot or is cancelled.
    bool heldStreamReservation = false;

    // Per-voice state staged while pending, replayed at start.
    bool hasVolume = false;
    float volumeDb = 0.0f;
    bool hasPitch = false;
    double pitchRatio = 1.0;
    bool hasPosition = false;
    float setPosX = 0.0f;
    float setPosY = 0.0f;
    float setPosZ = 0.0f;
    bool hasAttenuation = false;
    VnAttenuationId attenuationId{};
    std::uint64_t attenuationGeneration = 0;
    bool hasDistanceScale = false;
    float distanceScale = 1.0f;
    bool hasSpatializationMix = false;
    float spatializationMix = 1.0f;
    bool hasPan = false;
    float pan = 0.0f;
    bool hasWidth = false;
    float width = 0.0f;
    bool hasLowpass = false;
    float lowpassCutoffHz = 0.0f;
    float lowpassQ = kToneDefaultQ;
    std::uint32_t lowpassRampMs = 0;
    bool hasHighpass = false;
    float highpassCutoffHz = 0.0f;
    float highpassQ = kToneDefaultQ;
    std::uint32_t highpassRampMs = 0;
    struct StagedEffect {
        EffectObjectIdentity identity{};
        bool set = false;
        bool ownsInstance = false;
        std::uint32_t type = 0;      // VnEffectType
        std::uint32_t poolIndex = 0; // acquired+configured instance
        std::uint64_t poolGeneration = 0;
        // Committed parameter values in canonical per-type index order, mirrored
        // as they are set so a directly-parked deferred voice can reconstruct the
        // effect on return without reading the (recycled) instance state.
        std::array<float, kMaxEffectParamCount> params{};
    };
    std::array<StagedEffect, kMaxFxSlotsPerVoice> effects{};
    std::array<EffectParamTargetStore::PreparedSeedToken,
               kMaxFxSlotsPerVoice>
        preparedSeedTokens{};
    VnVoiceHandle preparedSeedRenderHandle = 0;
    bool effectSeedsPrepared = false;
    // Per-slot bypass target, staged independent of the effect install so it
    // persists across a clear and applies to a slot bypassed while empty —
    // matching the live/parked per-slot bypass. Applied at the deferred start
    // before the effect installs (an empty slot settles directly to the target;
    // a later install keeps the settled mix, so no wet block leaks).
    std::array<bool, kMaxFxSlotsPerVoice> effectBypass{};
    bool hasTonePreset = false;
    VoiceTonePreset tonePreset{};
    // Per-voice sends staged while pending, applied at the deferred start (each
    // active slot re-issues a set-voice-send at start). A voice receives
    // nothing, so sends never create a cycle and need no re-validation here.
    std::array<VoiceSendConfig, kMaxVoiceSendSlots> voiceSends{};

    // The resolved attachment set PINNED at play acceptance:
    // identities, never curve copies, so a Replace between acceptance and onset is live and the
    // first audible block uses the new curve. Attaching or detaching
    // after acceptance does not alter this set. At onset the pinned
    // entries instantiate on the new voice and their resolved initial
    // targets publish in the SAME admitted batch as the start command.
    struct PinnedBinding {
        VnObjectId bindingId{};
        VnObjectId attachmentObjectId{};
    };
    std::array<PinnedBinding, kMaxVoiceBindingInstanceSlots> pinnedBindings{};
    std::uint32_t pinnedBindingCount = 0;
    std::vector<ParamSystem::PreparedBindingToken> preparedBindings;
    std::array<VnBindingId, kPackV2VoiceRecipeMaxBindings>
        recipeBindingIds{};
    std::uint32_t recipeBindingCount = 0;
    // Worst-case publication demand reserved at acceptance for the pinned
    // set's unique targets (section 10.4 moved to the earliest failure
    // point): the onset's instance binds consume it (release-then-regrow,
    // net zero), a cancel releases it, and a direct park transfers it to
    // the ParkRecord's hold — so the onset itself can no longer fail the
    // capacity gate.
    std::uint32_t pinnedReservedTargets = 0;

    void Reset() { *this = PendingStartRecord{}; }
};

// The logic-side facts needed to reconstruct a voice's Play — retained per
// Render slot at start for PCM audibility return and M9 stream pressure
// waiting. Cleared when a slot's voice ends. Logic thread only.
struct VoicePlayFacts {
    bool valid = false;
    VnVoiceHandle renderHandle = 0;
    VnSoundId originPlayable{};   // the Sound id the play named
    VnSoundId leaf{};             // registered PCM or stream Sound id
    bool isStream = false;
    PCMView view;                   // PCM source (isStream == false)
    std::uint32_t streamFileIndex = 0;
    std::uint32_t busIndex = 0;
    std::uint32_t busGeneration = 0;
    std::uint64_t loopStartFrame = 0;
    std::uint64_t loopEndFrame = 0;
    std::uint32_t loopCount = 0;    // immutable authored pass count
    bool hasExitMarker = false;
    std::uint64_t exitMarkerSourceFrame = 0;
    float baseVolumeDb = 0.0f;
    float outputBusVolumeDb = 0.0f;
    double basePitchRatio = 1.0;
    // Immutable processing contract retained across Render slot lifetimes.
    ResampleProfile resampleProfile =
        ResampleProfile::LegacyCompatibility;
    std::uint32_t sourceSampleRate = 0;
    std::uint64_t sourceFrameCount = 0; // for virtual one-shot end
    std::uint32_t priority = 128;       // steal priority, for return-time steal
    bool hasTonePreset = false;
    VoiceTonePreset tonePreset{};
    std::array<VnBindingId, kPackV2VoiceRecipeMaxBindings>
        recipeBindingIds{};
    std::uint32_t recipeBindingCount = 0;
};

// Logic-owned observation data. Extended paths retain their backing across slot reuse.
struct EnginePlaybackPathStorage {
    std::uint64_t voiceInstance = 0;
    VnObjectId soundId{};
    VnObjectId outputBusId{};
    std::uint32_t pathRowCount = 0;
    PlaybackPathRow firstPathRow{};
    std::unique_ptr<PlaybackPathRow[]> additionalPathRows;
    std::uint32_t sendCount = 0;
    TimelineVoiceSendInstall sends[kTimelineMaxVoiceSends]{};

    bool PreparePath(std::size_t count) noexcept {
        if (memguard::IsAudioTagged() || count > kPlaybackPathRowCapacity) return false;
        if (count <= 1 || additionalPathRows != nullptr) return true;
        try {
            detail::LogicAllocCheckpoint(detail::LogicAllocSite::PlaybackPathStorage);
            additionalPathRows = std::make_unique<PlaybackPathRow[]>(kPlaybackPathRowCapacity - 1u);
        } catch (...) {
            return false;
        }
        return true;
    }

    void AssignPath(std::span<const PlaybackPathRow> rows) noexcept {
        assert(rows.size() <= kPlaybackPathRowCapacity &&
               (rows.size() <= 1 || additionalPathRows != nullptr));
        pathRowCount = static_cast<std::uint32_t>(rows.size());
        firstPathRow = rows.empty() ? PlaybackPathRow{} : rows.front();
        if (rows.size() > 1)
            std::copy(rows.begin() + 1, rows.end(), additionalPathRows.get());
    }

    PlaybackPathObservation Capture() const noexcept {
        PlaybackPathObservation observation{};
        observation.voiceInstance = voiceInstance;
        observation.soundId = soundId;
        observation.outputBusId = outputBusId;
        observation.path.rowCount = pathRowCount;
        if (pathRowCount != 0) observation.path.rows[0] = firstPathRow;
        if (pathRowCount > 1)
            std::copy_n(additionalPathRows.get(), pathRowCount - 1u, observation.path.rows + 1);
        observation.sendCount = sendCount;
        std::copy_n(sends, sendCount, observation.sends);
        return observation;
    }
};

// A parked voice's full replay + timeline state, indexed by ledger slot (like
// pendingStarts). Holds the play facts, the captured spatial + parameter state
// re-applied on return, and the frozen non-spatial audibility contribution.
// PCM parking advances virtualCursor. Stream waiting keeps an exact feed
// frontier in streamResumeFrame; a professional park token additionally owns
// the audible resampler state. Logic thread only.
// One voice-effect slot's configuration captured at park and re-staged on
// return: the type, its committed parameters, and its bypass. Re-staging
// acquires a fresh pool instance (clean DSP state - the spec's "fresh start at
// the virtual offset"), so only the configuration, never the internal filter/
// delay memory, is carried across a park.
struct CapturedEffect {
    EffectObjectIdentity identity{};
    std::uint32_t slot = 0;
    std::uint32_t type = kEffectTypeNone;
    std::array<float, kMaxEffectParamCount> params{};
};

struct ParkRecord {
    enum class State : std::uint8_t {
        PcmResumeReady = 0,
        StreamAckPending = 1,
        StreamResumeReady = 2
    };

    bool active = false;
    State state = State::PcmResumeReady;
    VoicePlayFacts facts;
    std::uint64_t streamResumeFrame = 0; // exact producer feed frontier for a
                                         // professional token; legacy restart
                                         // checkpoint otherwise
    double virtualCursor = 0.0;         // source frames, fractional accumulate
    std::uint64_t lastAdvanceFrame = 0; // service frame of the last virtual
                                        // timeline advance
    double resolvedPitchCents = 0.0;    // pitch cents at park EXCLUDING the
                                        // definition-backed instance term
                                        // (base + API-direct + raw-binding
                                        // contributions); adjusted by
                                        // API-direct deltas while parked
    double resolvedPitchRatio = 1.0;    // wire-clamped ratio of the cents
                                        // above PLUS the live instance pitch
                                        // term — recomputed each tick so the
                                        // virtual cursor advances at the
                                        // CURRENT resolved pitch (section 5.6)
    std::uint32_t loopsRemaining = 0;   // finite-loop passes left (0 = n/a)
    float frozenNonSpatialDb = 0.0f;    // metricAtPark - attenuationAtPark -
                                        // instance volume term at park (the
                                        // frozen non-instance remainder)
    // Live definition-backed contribution terms, re-evaluated every tick
    // from bindingInstances against the current definition revisions and
    // current scoped Variable values (section 5.6): the volume term feeds
    // the parked audibility metric, the pitch term the virtual cursor.
    float instanceVolumeDb = 0.0f;
    double instancePitchCents = 0.0;
    VnEmitterHandle stateEmitter = 0;
    Spatializer::SpatialVoiceState spatial;
    std::vector<ParamSystem::ApiDirectState> apiDirect;
    // Raw (definition-less) curve-copy bindings — the M1 engine-internal
    // wiring, unchanged. The public system's instances are NOT here.
    std::vector<ParamSystem::BindingState> bindings;
    // Binding instance IDENTITIES (bindingId + attachment identity),
    // never curve copies — the section-5.6 amendment of the M3 frozen-
    // copy shape. Return re-applies them by identity, so a replaced
    // curve is live on the returned voice's first block.
    std::vector<ParamSystem::InstanceState> bindingInstances;
    std::vector<ParamSystem::PreparedBindingToken> preparedBindings;
    // Section-10.4 worst-case demand HELD while parked: the instance
    // entries leave the parameter table at park (their identities live
    // above), but a parked logical voice still owns its instances, so
    // their unique-target demand stays reserved here — released when the
    // parked voice ends, consumed by the return's re-binds (which grow
    // back into exactly this room, so a return's instance re-application
    // cannot fail the capacity gate). A detach while parked leaves the
    // hold conservatively high until return or end.
    std::uint32_t instanceReservedTargets = 0;
    std::vector<CapturedEffect> effects; // voice-effect config re-staged on return
    // Fresh effect instances pre-acquired before a return publishes a steal
    // claim. They remain logic-owned until the exact claimed Render slot is
    // available, then transfer to the returned voice's command batch.
    std::array<EffectInstanceRef, kMaxFxSlotsPerVoice> returnEffectRefs{};
    std::array<EffectParamTargetStore::PreparedSeedToken,
               kMaxFxSlotsPerVoice>
        returnSeedTokens{};
    std::uint32_t returnEffectCount = 0;
    VnVoiceHandle returnPreparedRenderHandle = 0;
    bool returnEffectsPrepared = false;
    std::array<VnBindingId, kPackV2VoiceRecipeMaxBindings>
        recipeBindingIds{};
    std::uint32_t recipeBindingCount = 0;
    // The render handle the voice held at park. Effect-parameter binding apply-
    // instances (VN_TARGET_SCOPE_VOICE_EFFECT_PARAM) keep their pool slots across
    // the park as a held reservation, still keyed by this stale handle; the
    // return retargets them to the fresh render slot by it, and a park-end
    // reclaims them by it. Set at every park; a harmless no-op for a voice that
    // held no effect-parameter binding (no instance rows match it).
    VnVoiceHandle parkedRenderHandle = 0;
    // Per-slot bypass target, captured independent of the effects list so it
    // persists across a park exactly as the live per-slot bypass does — a slot
    // bypassed while empty stays bypassed when an effect is later installed into
    // it (whether that install is live, staged, or on a parked voice).
    std::array<bool, kMaxFxSlotsPerVoice> effectBypass{};
    // Per-voice sends captured at park and re-applied on return (each active
    // slot re-issues a set-voice-send). Bus destruction clears captured slots
    // that target its tombstoned cascade before a return can replay them.
    std::array<VoiceSendConfig, kMaxVoiceSendSlots> voiceSends{};
    bool heldPcmRef = false;            // a PCM use-count ref held while parked
    bool heldStreamRef = false;
    std::uint64_t streamParkRequestSequence = 0;
    StreamParkTokenRef streamParkToken{};
};

enum class EngineTimelineOccurrencePhase : std::uint8_t {
    Unprepared = 0,
    AwaitingArmAck = 1,
    ReadyToFile = 2,
    AwaitingReserveRetry = 3,
    Filed = 4,
    Started = 5,
    Stopping = 6,
    Cancelling = 7,
    Terminal = 9,
};

enum class EnginePlayableStartPhase : std::uint8_t {
    Free = 0,
    AwaitingArmAck = 2,
    PendingImmediateEntry = 3,
    Filed = 5,
    Started = 6,
    Cancelling = 7,
};

struct EngineOrdinaryRetiringLeaf {
    bool occupied = false;
    std::uint32_t nextResource = UINT32_MAX;
    TimelineArmToken arm{};
    VnVoiceHandle render = 0;
    VoiceReleaseObserver releaseObserver{};
    OccurrenceToken occurrence{};
    KnownCompletionPredicateV1 completion{};
    std::uint64_t exitSeenAt = 0;
    bool exitSeen = false;
};

struct EngineOrdinaryRetiringSource {
    PlayableInstanceHandle root = 0;
    PlayableInstanceHandle subtree = 0;
    std::uint32_t firstLeafResource = UINT32_MAX;
    std::uint32_t leafCount = 0;
    OccurrenceToken parentOccurrence{};
    CompositeCompletionOwnerToken compositeOwner{};
    PlayableTerminalKind terminalKind = PlayableTerminalKind::NaturalCompletion;
    bool ownsScheduleResources = false;
    bool committed = false;
    bool resourcesReleased = false;
};

struct EngineQueueProgramBankBody : EngineQueueScheduleColumns {
    static constexpr std::size_t kProgramCapacity = 2u * kMaxKnownScheduleLeaves;
    static constexpr std::size_t kEventsPerProgram = 2u;
    static constexpr std::size_t kEventCapacity = kEventsPerProgram * kProgramCapacity;
    std::array<TimelineProgramView, 2> aggregateRuntimes{};
    std::array<ProgramView, 2> aggregateSchedules{};
    std::array<CurveLUT, 2> curves{};
    std::array<const CurveLUT*, 2> originalCurves{};
    std::array<ArmGroupToken, 2> groups{};
    TransitionInstanceToken transition{};
    PlayableInstanceHandle supersededRoot = 0;
    std::uint64_t supersededEpoch = 0;
    std::uint32_t programCount = 0;
    std::uint32_t eventCount = 0;
    std::uint32_t curveCount = 0;
    std::uint32_t aggregateCount = 0;
    bool occupied = false;
    bool published = false;
    bool heldByOwner = false;
};

// Lifetime issuance and return belong to the serialized logic producer.
// Bank storage stays at its original fixed Engine address for every loan.
struct EngineQueueProgramBank : EngineQueueProgramBankBody {
    QueueContinuationLane continuationLane;
    EngineQueueScheduleStorage::Loan scheduleStorageLoan;
    TimelineProgramReadFence readFence;
    class OriginalReservation {
    public:
        OriginalReservation() noexcept = default;
        ~OriginalReservation() { Release(); }
        OriginalReservation(const OriginalReservation&) = delete;
        OriginalReservation& operator=(const OriginalReservation&) = delete;
        OriginalReservation(OriginalReservation&& other) noexcept { Take(other); }
        OriginalReservation& operator=(OriginalReservation&& other) noexcept {
            if (this != &other) { Release(); Take(other); }
            return *this;
        }
        bool Valid() const noexcept { return owner_ != nullptr && owner_->Matches(*this); }
        std::uint64_t Generation() const noexcept { return Valid() ? generation_ : 0; }
        void Release() noexcept {
            if (owner_ != nullptr) {
                const bool valid = owner_->Matches(*this);
                assert(valid);
                if (valid) owner_->originalReservationMarker_ = 0;
            }
            owner_ = nullptr; generation_ = 0; marker_ = 0;
        }
    private:
        friend struct EngineQueueProgramBank;
        void Take(OriginalReservation& other) noexcept {
            owner_ = other.owner_; generation_ = other.generation_; marker_ = other.marker_;
            other.owner_ = nullptr; other.generation_ = 0; other.marker_ = 0;
        }
        EngineQueueProgramBank* owner_ = nullptr;
        std::uint64_t generation_ = 0;
        std::uint64_t marker_ = 0;
    };

    class OriginalLifetimePin {
    public:
        OriginalLifetimePin() noexcept = default;
        ~OriginalLifetimePin() { Release(); }
        OriginalLifetimePin(const OriginalLifetimePin&) = delete;
        OriginalLifetimePin& operator=(const OriginalLifetimePin&) = delete;
        OriginalLifetimePin(OriginalLifetimePin&& other) noexcept { Take(other); }
        OriginalLifetimePin& operator=(OriginalLifetimePin&& other) noexcept {
            if (this != &other) { Release(); Take(other); }
            return *this;
        }
        bool Valid() const noexcept { return owner_ != nullptr && owner_->Matches(*this); }
        std::uint64_t Generation() const noexcept { return Valid() ? generation_ : 0; }
        bool Borrow(OriginalLifetimePin* out) const noexcept {
            return Valid() && owner_->IssuePin(generation_, out);
        }
        void Release() noexcept {
            if (owner_ != nullptr) {
                const bool valid = owner_->Matches(*this);
                assert(valid);
                if (valid) --owner_->originalLifetimePinCount_;
            }
            owner_ = nullptr; generation_ = 0; marker_ = 0;
        }
    private:
        friend struct EngineQueueProgramBank;
        void Take(OriginalLifetimePin& other) noexcept {
            owner_ = other.owner_; generation_ = other.generation_; marker_ = other.marker_;
            other.owner_ = nullptr; other.generation_ = 0; other.marker_ = 0;
        }
        EngineQueueProgramBank* owner_ = nullptr;
        std::uint64_t generation_ = 0;
        std::uint64_t marker_ = 0;
    };

    EngineQueueProgramBank() noexcept = default;
    ~EngineQueueProgramBank() {
        assert(originalReservationMarker_ == 0 && originalLifetimePinCount_ == 0);
    }
    EngineQueueProgramBank(const EngineQueueProgramBank&) = delete;
    EngineQueueProgramBank& operator=(const EngineQueueProgramBank&) = delete;
    EngineQueueProgramBank(EngineQueueProgramBank&&) = delete;
    EngineQueueProgramBank& operator=(EngineQueueProgramBank&&) = delete;

    bool TryReserve(OriginalReservation* out = nullptr) noexcept {
        if (occupied || HasOriginalLifetimeOwners() || generation_ == UINT64_MAX ||
            (out != nullptr && out->owner_ != nullptr)) return false;
        ++generation_;
        occupied = true;
        if (out != nullptr) {
            originalReservationMarker_ = generation_;
            out->owner_ = this; out->generation_ = generation_; out->marker_ = generation_;
        }
        return true;
    }

    bool IssueLifetimePin(const OriginalReservation& original, OriginalLifetimePin* out) noexcept {
        return Matches(original) && IssuePin(original.generation_, out);
    }

    bool HasOriginalLifetimeOwners() const noexcept {
        return originalReservationMarker_ != 0 || originalLifetimePinCount_ != 0;
    }
    std::uint64_t Generation() const noexcept { return generation_; }

    bool HasOriginalOwnersOtherThan(const OriginalReservation& reservation,
        const OriginalLifetimePin& baseline) const noexcept {
        return !Matches(reservation) || !Matches(baseline) ||
            originalLifetimePinCount_ != 1u;
    }
    bool TryResetOriginalStorage(OriginalReservation& reservation,
        OriginalLifetimePin& baseline) noexcept {
        if (HasOriginalOwnersOtherThan(reservation, baseline)) return false;
        // The serialized producer has already passed the actual reader/fence guards.
        baseline.Release();
        reservation.Release();
        const bool reset = TryResetStorage();
        assert(reset);
        return reset;
    }

    // Callers still own the actual program/observer/read-fence qualification.
    bool TryResetStorage() noexcept {
        if (HasOriginalLifetimeOwners() || continuationLane.Active()) return false;
        for (const auto token : members) if (token.Valid()) return false;
        scheduleStorageLoan.Reset();
        programCount = 0; eventCount = 0; curveCount = 0; aggregateCount = 0;
        groups = {}; transition = {};
        supersededRoot = 0; supersededEpoch = 0;
        readFence.ResetAfterSafeRetirement();
        occupied = false; published = false; heldByOwner = false;
        return true;
    }

    bool TryCancelAlias(EngineQueueProgramBank*& alias) noexcept {
        if (alias != this) return false;
        if (!published && !TryResetStorage()) return false;
        alias = nullptr;
        return true;
    }

private:
    friend struct EngineQueueProgramBankTestAccess;
    bool Matches(const OriginalReservation& original) const noexcept {
        return original.owner_ == this && occupied && original.generation_ == generation_ &&
            original.marker_ != 0 && original.marker_ == originalReservationMarker_;
    }
    bool Matches(const OriginalLifetimePin& original) const noexcept {
        return original.owner_ == this && occupied && original.generation_ == generation_ &&
            original.marker_ != 0 && original.marker_ <= lastLifetimePinMarker_ &&
            originalLifetimePinCount_ != 0;
    }
    bool IssuePin(std::uint64_t generation, OriginalLifetimePin* out) noexcept {
        if (!occupied || generation != generation_ || out == nullptr || out->owner_ != nullptr ||
            originalLifetimePinCount_ == UINT32_MAX || lastLifetimePinMarker_ == UINT64_MAX)
            return false;
        ++originalLifetimePinCount_;
        out->owner_ = this; out->generation_ = generation_;
        out->marker_ = ++lastLifetimePinMarker_;
        return true;
    }
    std::uint64_t generation_ = 0;
    std::uint64_t originalReservationMarker_ = 0;
    std::uint64_t lastLifetimePinMarker_ = 0;
    std::uint32_t originalLifetimePinCount_ = 0;
};

struct EngineQueueProgramRange {
    std::uint32_t first = 0;
    std::uint32_t count = 0;

    std::span<ProgramView> Resolve(EngineQueueProgramBank* bank) const noexcept {
        if (bank == nullptr || !bank->occupied || bank->programCount > bank->schedules.size() ||
            first > bank->programCount || count > bank->programCount - first) return {};
        return std::span{bank->schedules}.subspan(first, count);
    }
};

struct EngineMonitorSelection {
    VoiceLedger::MonitorSelection observation{};
    TimelineArmToken arm{};
};

struct VariableControlMemberCompletion {
    PlayableInstanceHandle leafInstance = 0;
    std::uint64_t leafEpoch = 0;
    SourceTerminalToken source{};
    bool resourcesRetired = false;
    SourceTerminalPayload completedSource{};
    std::uint64_t sourceCompletionSequence = 0;
    std::uint64_t sourceReleaseSequence = 0;
    VnVoiceHandle sourceRender = 0;
    TimelineArmToken sourceStartArm{};
    std::uint64_t sourceStartedBlockSequence = 0;
};

struct EnginePlayableServiceToken {
    std::uint32_t slot = UINT32_MAX;
    std::uint64_t generation = 0;
    bool Valid() const noexcept { return slot != UINT32_MAX && generation != 0; }
    bool operator==(const EnginePlayableServiceToken&) const noexcept = default;
};

struct EngineInitialQueueTimingMember {
    EnginePlayableServiceToken service{};
    EngineQueueProgramBank* successorBank = nullptr;
    UnplacedBatchCredit credit{};
    std::uint64_t oldEntryFrame = 0;
    std::uint64_t newEntryFrame = 0;
    std::uint64_t newCompletionFrame = 0;
    std::uint64_t newZeroWorkCompletionFrame = 0;
    std::uint64_t oldSuccessorZeroWorkCompletionFrame = 0;
    std::uint64_t newSuccessorZeroWorkCompletionFrame = 0;
    bool successorHasZeroWorkCompletion = false;
    std::uint64_t newTransitionEndFrame = 0;
    TransitionInstanceToken privateTimingTransition{};
    std::uint64_t oldTransitionAudibleFrame = 0;
    std::uint64_t oldTransitionLockBlock = 0;
    std::uint64_t newTransitionAudibleFrame = 0;
    std::uint64_t newTransitionLockBlock = 0;
    std::array<std::uint64_t, 3> newGroupFrames{};
    std::uint32_t groupCount = 0;
    std::uint32_t transactionCount = 0;
    std::size_t firstProgram = 0;
    std::size_t firstGroup = 0;
    std::uint32_t currentLeafCount = 0;
    std::uint32_t successorLeafCount = 0;
    KnownCompletionPredicateV1 currentCompletion{};
    KnownCompletionPredicateV1 successorCompletion{};
    bool currentCompletionBound = false;
    bool successorCompletionBound = false;
    QueueInstanceToken queue{};
    ConditionalQueueVersionToken conditional{};
    DecisionToken decision{};
    OccurrenceToken currentOccurrence{};
    std::uint64_t conditionalLogicalStartFrame = 0;
    std::uint64_t conditionalLogicalCompletionFrame = 0;
};

struct EngineInitialQueueTimingEvent {
    const EventTemplate* frozen = nullptr;
    const TimelineEventPayload* payload = nullptr;
    EnginePlayableServiceToken holder{};
    std::size_t programIndex = 0;
    std::uint64_t relativeFrame = 0;
    std::uint64_t newExecuteFrame = 0;
    std::uint64_t newEnvelopeStartFrame = 0;
    std::uint64_t newEnvelopeEndFrame = 0;
};

struct EngineInitialQueueStartMirror {
    std::uint32_t startEventOffset = UINT32_MAX;
    std::uint32_t successorProgramIndex = UINT32_MAX;
    std::uint8_t successorMirrorPlane = 0;
    std::uint32_t successorBankAggregate = UINT32_MAX;
    std::uint64_t oldPhysicalStartFrame = 0;
    std::uint64_t oldCopiedAnchorFrame = 0;
    std::uint64_t newCopiedAnchorFrame = 0;
    std::uint64_t oldAggregateAnchorFrame = 0;
    std::uint64_t newAggregateAnchorFrame = 0;
    std::uint64_t oldBankAggregateAnchorFrame = 0;
    std::uint64_t newBankAggregateAnchorFrame = 0;
    KnownCompletionPredicateV1 oldPhysicalCompletion{};
    KnownCompletionPredicateV1 newPhysicalCompletion{};
    bool physicalCompletionBound = false;
    bool retainedOriginalSuccessor = false;
};

struct EngineInitialQueueTimingProgram {
    const ProgramView* frozen = nullptr;
    std::uint64_t newEntryFrame = 0;
    std::uint64_t newArmStartFrame = 0;
    std::uint64_t newArmDeadline = 0;
    TimelineArmToken arm{};
    std::size_t firstEvent = 0;
    std::uint32_t eventCount = 0;
    EngineInitialQueueStartMirror startMirror{};
};

struct EngineFrontierQueueCurrentOwnerContext;
struct EngineFrontierQueueInitialDecisionContext;

struct EngineInitialQueueEntryDecisionOwner {
    ActionFiringHandle root = 0;
    EnginePlayableServiceToken service{};
    PlayableInstanceHandle owner = 0;
    std::uint64_t ownerEpoch = 0;
    std::uint64_t invocationGeneration = 0;
    PreparedDecisionReservation reservation{};
    EngineFrontierQueueCurrentOwnerContext* borrowedCurrent = nullptr;
    DecisionToken committed{};
    bool transferred = false;
};

struct EnginePlayablePartitionToken {
    std::uint32_t slot = UINT32_MAX;
    std::uint64_t generation = 0;
    bool Valid() const noexcept { return slot != UINT32_MAX && generation != 0; }
};

struct EnginePlayableServiceReference {
    EnginePlayableServiceToken token{};
    PlayableInstanceHandle owner = 0;
    std::uint64_t epoch = 0;
    std::uint64_t invocationGeneration = 0;
    bool Valid() const noexcept {
        return token.Valid() && owner != 0 && epoch != 0 && invocationGeneration != 0;
    }
};

struct EnginePlayableMemberReference {
    EnginePlayableServiceReference service{};
    std::uint64_t partitionGeneration = 0;
    std::uint32_t memberOffset = UINT32_MAX;
    PlayableInstanceHandle leaf = 0;
    std::uint64_t leafEpoch = 0;
    std::uint32_t authoredChildOrdinal = 0;
};

// Installed relations retain the target generation; owner facts belong to that service.
struct EnginePlayableMemberRelation {
    EnginePlayableServiceToken service{};
    PlayableInstanceHandle leaf = 0;
    std::uint64_t leafEpoch = 0;
    std::uint32_t logicalOffset = 0;
    std::uint32_t memberOffset = UINT32_MAX;
    std::uint32_t authoredChildOrdinal = 0;
    std::uint32_t next = UINT32_MAX;
};

struct EngineMemberProgramToken {
    std::uint32_t slot = UINT32_MAX;
    std::uint64_t generation = 0;
    bool Valid() const noexcept { return slot != UINT32_MAX && generation != 0; }
    bool operator==(const EngineMemberProgramToken&) const noexcept = default;
};

struct EngineMemberProgramRecord {
    std::array<TimelineEventPayload, 2> payloads{};
    std::array<EventTemplate, 2> events{};
    TimelineProgramView runtime{};
    ProgramView schedule{};
    EnginePlayableMemberReference member{};
    EnginePlayableMemberToken memberToken{};
    TimelineProgramReadFence readFence;
    std::uint64_t generation = 0;
    bool directoryOwned = false;
    bool occupied = false;
};

struct EngineControlProgramRecord {
    TimelineEventPayload payload{};
    EventTemplate event{};
    TimelineProgramView runtime{};
    ProgramView schedule{};
    EnginePlayableServiceToken service{};
    bool reserved = false;
};

struct EnginePlayableMemberRecord {
    CapturedTimelineVoiceInstall voice{};
    PlayableRefV1 leaf{};
    TimelineArmToken arm{};
    EngineMonitorSelection monitorSelection{};
    VnVoiceHandle render = 0;
    VoiceReleaseObserver release{};
    VariableControlMemberCompletion completion{};
    OccurrenceToken occurrence{};
    KnownCompletionPredicateV1 nativeCompletion{};
    std::uint64_t startFrame = 0;
    std::uint32_t postSourceTailFrames = 0;
    EnginePlayableServiceToken service{};
    std::uint64_t partitionGeneration = 0;
    std::uint64_t generation = 0;
    std::uint32_t physicalSlot = UINT32_MAX;
    std::uint32_t programOwners = 0;
    std::uint8_t dynamic = 0;
    bool primaryOwned = false;
    bool retirePrimary = false;
    bool occupied = false;
};

struct EngineQueueDeferredSourceObservation {
    EnginePlayableMemberReference member{};
    TimelineArmToken arm{};
    TimelineArmObservation observation{};
    EnginePlayableServiceReference service{};
};

namespace detail { struct EnginePlayablePartitionTestAccess; struct EngineOriginalServiceStorageTestAccess; }

class EnginePlayablePartitionPool {
    struct Slot;
public:
    class ReservationInput {
    public:
        ReservationInput() noexcept = default;
        ReservationInput(const ReservationInput&) noexcept = default;
        ReservationInput(ReservationInput&& other) noexcept : ReservationInput(other) {}
        ReservationInput& operator=(const ReservationInput& other) noexcept;
        ReservationInput& operator=(ReservationInput&& other) noexcept { return *this = other; }
        bool Reset() noexcept;
        bool Valid() const noexcept { return pool_ != nullptr && token_.Valid() && owner_.Valid(); }
        EnginePlayablePartitionPool* Pool() const noexcept { return pool_; }
        EnginePlayablePartitionToken Token() const noexcept { return token_; }
        EnginePlayableServiceToken Owner() const noexcept { return owner_; }
    private:
        friend class EnginePlayablePartitionPool;
        EnginePlayablePartitionPool* pool_ = nullptr;
        EnginePlayablePartitionToken token_{};
        EnginePlayableServiceToken owner_{};
    };
    class OriginalStoragePin {
    public:
        OriginalStoragePin() noexcept = default;
        ~OriginalStoragePin() { Return(); }
        OriginalStoragePin(const OriginalStoragePin&) = delete;
        OriginalStoragePin& operator=(const OriginalStoragePin&) = delete;
        OriginalStoragePin(OriginalStoragePin&& other) noexcept { *this = std::move(other); }
        OriginalStoragePin& operator=(OriginalStoragePin&& other) noexcept;
        bool Valid() const noexcept;
        void Return() noexcept;
    private:
        friend class EnginePlayablePartitionPool;
        EnginePlayablePartitionPool* pool_ = nullptr;
        Slot* slot_ = nullptr;
        EnginePlayablePartitionToken token_{};
        EnginePlayableServiceToken owner_{};
        std::uint64_t marker_ = 0;
        bool baseline_ = false;
    };
    class PreparedCommitProof {
    public:
        PreparedCommitProof() noexcept = default;
        ~PreparedCommitProof() { Release(); }
        PreparedCommitProof(const PreparedCommitProof&) = delete;
        PreparedCommitProof& operator=(const PreparedCommitProof&) = delete;
        PreparedCommitProof(PreparedCommitProof&& other) noexcept { *this = std::move(other); }
        PreparedCommitProof& operator=(PreparedCommitProof&& other) noexcept;
        bool Valid() const noexcept;
        bool Associated(const ReservationInput& original) const noexcept;
        void Release() noexcept;
    private:
        friend class EnginePlayablePartitionPool;
        EnginePlayablePartitionPool* pool_ = nullptr;
        Slot* slot_ = nullptr;
        EnginePlayablePartitionToken token_{};
        EnginePlayableServiceToken owner_{};
    };

    explicit EnginePlayablePartitionPool(std::uint32_t capacity) : slots_(capacity) {}
    ~EnginePlayablePartitionPool() {
        for (const auto& slot : slots_) assert(slot.originalStorageLoans == 0);
    }
    EnginePlayablePartitionPool(const EnginePlayablePartitionPool&) = delete;
    EnginePlayablePartitionPool& operator=(const EnginePlayablePartitionPool&) = delete;
    EnginePlayablePartitionPool(EnginePlayablePartitionPool&&) = delete;
    EnginePlayablePartitionPool& operator=(EnginePlayablePartitionPool&&) = delete;

    EnginePlayablePartitionToken Reserve(EnginePlayableServiceToken owner,
        ReservationInput* output = nullptr, OriginalStoragePin* original = nullptr) noexcept {
        if (!owner.Valid() || (output != nullptr && output->Valid()) ||
            (original != nullptr && (original->pool_ != nullptr ||
                                    nextOriginalStorageMarker_ == UINT64_MAX))) return {};
        for (const auto& slot : slots_) {
            if (slot.occupied && slot.owner == owner) return {};
        }
        for (std::size_t offset = 0; offset < slots_.size(); ++offset) {
            const auto index = (next_ + offset) % slots_.size();
            auto& slot = slots_[index];
            if (slot.occupied || slot.quarantined) continue;
            slot.owner = owner;
            slot.occupied = true;
            slot.committed = false;
            slot.published = false;
            slot.retiring = false;
            slot.retirementSequence = 0;
            slot.commitHeld = false;
            slot.preparingCommitOwned = true;
            slot.originalOwner = owner;
            assert(slot.originalStorageLoans == 0);
            slot.originalStorageMarker = original == nullptr ? 0 : ++nextOriginalStorageMarker_;
            slot.originalStorageLoans = original == nullptr ? 0 : 1;
            next_ = (index + 1u) % slots_.size();
            const EnginePlayablePartitionToken token{static_cast<std::uint32_t>(index), slot.generation};
            if (output != nullptr) {
                output->pool_ = this;
                output->token_ = token;
                output->owner_ = owner;
            }
            if (original != nullptr) {
                original->pool_ = this;
                original->slot_ = &slot;
                original->token_ = token;
                original->owner_ = owner;
                original->marker_ = slot.originalStorageMarker;
                original->baseline_ = true;
            }
            return token;
        }
        return {};
    }

    bool BorrowOriginalStorage(const OriginalStoragePin& original,
        OriginalStoragePin* output) noexcept {
        if (output == nullptr || output->pool_ != nullptr || original.pool_ != this ||
            !original.Valid() || original.slot_->originalStorageLoans == UINT32_MAX) return false;
        ++original.slot_->originalStorageLoans;
        output->pool_ = this;
        output->slot_ = original.slot_;
        output->token_ = original.token_;
        output->owner_ = original.owner_;
        output->marker_ = original.marker_;
        output->baseline_ = false;
        return true;
    }

    bool OriginalStorageIssuerExhausted() const noexcept { return nextOriginalStorageMarker_ == UINT64_MAX; }

    bool OriginalStorageReclaimReady(const OriginalStoragePin& own) const noexcept {
        return own.pool_ == this && own.baseline_ && own.Valid() &&
            own.slot_->originalStorageLoans == 1;
    }

    bool OriginalStorageMatches(const OriginalStoragePin& actual,
        EnginePlayablePartitionToken token, EnginePlayableServiceToken owner) const noexcept {
        return actual.pool_ == this && actual.Valid() && actual.token_.slot == token.slot &&
            actual.token_.generation == token.generation && actual.owner_ == owner;
    }

    bool Owns(EnginePlayablePartitionToken token,
              EnginePlayableServiceToken owner) const noexcept {
        return token.Valid() && token.slot < slots_.size() &&
            slots_[token.slot].occupied && slots_[token.slot].generation == token.generation &&
            slots_[token.slot].owner == owner;
    }

    EnginePlayableServiceToken CaptureOwner(std::uint32_t partitionSlot) const noexcept {
        return partitionSlot < slots_.size() && slots_[partitionSlot].occupied
            ? slots_[partitionSlot].owner : EnginePlayableServiceToken{};
    }

    bool Commit(EnginePlayablePartitionToken token,
                EnginePlayableServiceToken owner) noexcept {
        if (!Owns(token, owner) || slots_[token.slot].retiring || slots_[token.slot].commitHeld) return false;
        slots_[token.slot].committed = true;
        slots_[token.slot].preparingCommitOwned = false;
        return true;
    }

    bool CommitProofHeld(EnginePlayablePartitionToken token,
                         EnginePlayableServiceToken owner) const noexcept {
        return Owns(token, owner) && slots_[token.slot].commitHeld;
    }

    bool ProvePreparedCommit(const ReservationInput& original, PreparedCommitProof* output) noexcept {
        if (output == nullptr || output->Valid() || original.pool_ != this ||
            !Owns(original.token_, original.owner_)) return false;
        auto& slot = slots_[original.token_.slot];
        if (slot.quarantined || slot.committed || slot.published || slot.retiring ||
            slot.retirementSequence != 0 || slot.commitHeld || !slot.preparingCommitOwned ||
            slot.originalOwner != original.owner_) return false;
        slot.commitHeld = true;
        output->pool_ = this;
        output->slot_ = &slot;
        output->token_ = original.token_;
        output->owner_ = original.owner_;
        return true;
    }

    static void ConsumePreparedCommit(PreparedCommitProof&& proof) noexcept {
        assert(proof.Valid());
        proof.slot_->committed = true;
        proof.slot_->preparingCommitOwned = false;
        proof.Release();
    }

    bool Cancel(EnginePlayablePartitionToken token,
                EnginePlayableServiceToken owner) noexcept {
        if (!Owns(token, owner) || slots_[token.slot].committed || slots_[token.slot].commitHeld ||
            slots_[token.slot].originalStorageLoans != 0) return false;
        Recycle(token.slot);
        return true;
    }

    bool RollbackCommit(EnginePlayablePartitionToken token,
                        EnginePlayableServiceToken owner) noexcept {
        if (!Owns(token, owner) || slots_[token.slot].published || slots_[token.slot].commitHeld) return false;
        slots_[token.slot].committed = false;
        return true;
    }

    bool Publish(EnginePlayablePartitionToken token,
                 EnginePlayableServiceToken owner) noexcept {
        if (!Owns(token, owner) || !slots_[token.slot].committed || slots_[token.slot].commitHeld) return false;
        slots_[token.slot].published = true;
        return true;
    }

    bool Published(EnginePlayablePartitionToken token,
                   EnginePlayableServiceToken owner) const noexcept {
        return Owns(token, owner) && slots_[token.slot].published;
    }

    bool PrivateCommitted(EnginePlayablePartitionToken token,
                          EnginePlayableServiceToken owner) const noexcept {
        return Owns(token, owner) && slots_[token.slot].committed &&
            !slots_[token.slot].published && !slots_[token.slot].retiring &&
            !slots_[token.slot].commitHeld && !slots_[token.slot].preparingCommitOwned;
    }

    bool BeginRetirement(EnginePlayablePartitionToken token,
                         EnginePlayableServiceToken owner,
                         std::uint64_t completedSequence) noexcept {
        if (!Owns(token, owner) || !slots_[token.slot].committed || slots_[token.slot].commitHeld) return false;
        auto& slot = slots_[token.slot];
        if (!slot.retiring) {
            slot.retiring = true;
            slot.retirementSequence = completedSequence;
        }
        return true;
    }

    bool CanRelease(EnginePlayablePartitionToken token,
                    EnginePlayableServiceToken owner,
                    std::uint64_t completedSequence) const noexcept {
        return Owns(token, owner) && !slots_[token.slot].commitHeld && (!slots_[token.slot].committed ||
            (slots_[token.slot].retiring &&
             completedSequence > slots_[token.slot].retirementSequence));
    }

    bool DeferRetirement(EnginePlayablePartitionToken token,
                         EnginePlayableServiceToken owner,
                         std::uint64_t completedSequence) noexcept {
        if (!Owns(token, owner) || !slots_[token.slot].retiring || slots_[token.slot].commitHeld) return false;
        slots_[token.slot].retirementSequence = completedSequence;
        return true;
    }

    bool Release(EnginePlayablePartitionToken token,
                 EnginePlayableServiceToken owner,
                 std::uint64_t completedSequence) noexcept {
        if (!CanRelease(token, owner, completedSequence) ||
            slots_[token.slot].originalStorageLoans != 0) return false;
        Recycle(token.slot);
        return true;
    }

    std::uint32_t OccupiedCount() const noexcept {
        return static_cast<std::uint32_t>(std::count_if(slots_.begin(), slots_.end(),
            [](const Slot& slot) { return slot.occupied; }));
    }

    std::uint32_t Capacity() const noexcept { return static_cast<std::uint32_t>(slots_.size()); }
    std::size_t StorageBytes() const noexcept { return slots_.capacity() * sizeof(Slot); }

private:
    friend struct detail::EnginePlayablePartitionTestAccess;
    struct Slot {
        EnginePlayableServiceToken owner{};
        EnginePlayableServiceToken originalOwner{};
        std::uint64_t generation = 1;
        std::uint64_t retirementSequence = 0;
        bool occupied = false;
        bool committed = false;
        bool published = false;
        bool retiring = false;
        bool quarantined = false;
        bool commitHeld = false;
        bool preparingCommitOwned = false;
        std::uint64_t originalStorageMarker = 0;
        std::uint32_t originalStorageLoans = 0;
    };
    void Recycle(std::uint32_t index) noexcept {
        auto& slot = slots_[index];
        assert(slot.originalStorageLoans == 0);
        slot.originalStorageMarker = 0;
        slot.owner = {};
        slot.occupied = false;
        slot.committed = false;
        slot.published = false;
        slot.retiring = false;
        slot.retirementSequence = 0;
        slot.originalOwner = {};
        slot.commitHeld = false;
        slot.preparingCommitOwned = false;
        if (slot.generation == UINT64_MAX) slot.quarantined = true;
        else ++slot.generation;
    }
    std::vector<Slot> slots_;
    std::size_t next_ = 0;
    std::uint64_t nextOriginalStorageMarker_ = 0;
};

inline EnginePlayablePartitionPool::ReservationInput&
EnginePlayablePartitionPool::ReservationInput::operator=(const ReservationInput& other) noexcept {
    if (this == &other || (pool_ != nullptr && pool_->CommitProofHeld(token_, owner_))) return *this;
    pool_ = other.pool_;
    token_ = other.token_;
    owner_ = other.owner_;
    return *this;
}

inline bool EnginePlayablePartitionPool::ReservationInput::Reset() noexcept {
    if (pool_ != nullptr && pool_->CommitProofHeld(token_, owner_)) return false;
    pool_ = nullptr;
    token_ = {};
    owner_ = {};
    return true;
}

inline EnginePlayablePartitionPool::PreparedCommitProof&
EnginePlayablePartitionPool::PreparedCommitProof::operator=(PreparedCommitProof&& other) noexcept {
    if (this == &other) return *this;
    Release();
    pool_ = other.pool_;
    slot_ = other.slot_;
    token_ = other.token_;
    owner_ = other.owner_;
    other.pool_ = nullptr;
    other.slot_ = nullptr;
    other.token_ = {};
    other.owner_ = {};
    return *this;
}

inline bool EnginePlayablePartitionPool::PreparedCommitProof::Valid() const noexcept {
    return pool_ != nullptr && slot_ != nullptr && slot_->commitHeld && pool_->Owns(token_, owner_);
}

inline bool EnginePlayablePartitionPool::PreparedCommitProof::Associated(
    const ReservationInput& original) const noexcept {
    return Valid() && pool_ == original.Pool() && token_.slot == original.Token().slot &&
        token_.generation == original.Token().generation && owner_ == original.Owner();
}

inline void EnginePlayablePartitionPool::PreparedCommitProof::Release() noexcept {
    if (slot_ != nullptr) slot_->commitHeld = false;
    pool_ = nullptr;
    slot_ = nullptr;
    token_ = {};
    owner_ = {};
}

inline EnginePlayablePartitionPool::OriginalStoragePin&
EnginePlayablePartitionPool::OriginalStoragePin::operator=(OriginalStoragePin&& other) noexcept {
    if (this == &other) return *this;
    Return();
    pool_ = other.pool_;
    slot_ = other.slot_;
    token_ = other.token_;
    owner_ = other.owner_;
    marker_ = other.marker_;
    baseline_ = other.baseline_;
    other.pool_ = nullptr;
    other.slot_ = nullptr;
    other.token_ = {};
    other.owner_ = {};
    other.marker_ = 0;
    other.baseline_ = false;
    return *this;
}

inline bool EnginePlayablePartitionPool::OriginalStoragePin::Valid() const noexcept {
    return pool_ != nullptr && slot_ != nullptr && marker_ != 0 &&
        pool_->Owns(token_, owner_) && slot_ == &pool_->slots_[token_.slot] &&
        slot_->originalOwner == owner_ && slot_->originalStorageMarker == marker_ &&
        slot_->originalStorageLoans != 0;
}

inline void EnginePlayablePartitionPool::OriginalStoragePin::Return() noexcept {
    if (pool_ != nullptr) {
        assert(Valid());
        if (Valid()) --slot_->originalStorageLoans;
    }
    pool_ = nullptr;
    slot_ = nullptr;
    token_ = {};
    owner_ = {};
    marker_ = 0;
    baseline_ = false;
}

class EngineOriginalPackIdentityLease {
public:
    EngineOriginalPackIdentityLease() noexcept = default;
    EngineOriginalPackIdentityLease(const EngineOriginalPackIdentityLease&) = delete;
    EngineOriginalPackIdentityLease& operator=(const EngineOriginalPackIdentityLease&) = delete;
    EngineOriginalPackIdentityLease(EngineOriginalPackIdentityLease&& other) noexcept
        : receiver_(std::exchange(other.receiver_, nullptr)), native_(std::move(other.native_)) {}
    EngineOriginalPackIdentityLease& operator=(EngineOriginalPackIdentityLease&& other) noexcept {
        if (this != &other) {
            native_ = std::move(other.native_);
            receiver_ = std::exchange(other.receiver_, nullptr);
        }
        return *this;
    }
    bool Valid() const noexcept { return receiver_ != nullptr && native_.Valid(); }
    PackComposition::OriginalLeasePhase Phase() const noexcept { return native_.Phase(); }
    PackPartitionLease OriginalLease() const noexcept { return native_.OriginalLease(); }
    std::size_t OriginalRowIndex() const noexcept { return native_.Valid() ? native_.index_ : SIZE_MAX; }
private:
    friend struct Engine;
    Engine* receiver_ = nullptr;
    PackComposition::OriginalLeaseEpisode native_;
};

struct EngineActionPlayStorage;

struct EnginePlayableStartRow {
    MonitorRequestContext playbackRequest{};
    MonitorEmitterOrigin emitterOrigin{};
    VnObjectId projectId{};
    std::uint32_t slot = UINT32_MAX;
    std::uint64_t generation = 1;
    std::uint32_t serviceCount = 0;
    EnginePlayableServiceToken rootService{};
    bool quarantined = false;
    VnEmitterHandle invocationEmitter = 0;
    ActionFiringHandle actionFiring = 0;
    std::uint32_t actionExecutionOrdinal = UINT32_MAX;
    VnResult actionTerminalResult = VN_OK;
    ActionTerminalReason actionTerminalReason = ActionTerminalReason::None;
    PlayableRetirementObserver pendingStopRetirement{};
    std::uint64_t worldGeneration = 0;
    PackPartitionLease packLease{};
    PackPlaybackView packPlaybackView{};
    EngineOriginalPackIdentityLease originalPackLease;
    EngineActionPlayStorage* preparingStorage = nullptr;
    std::uint64_t preparingStorageGeneration = 0;
    PlayableInstanceHandle variableControlOuterRootInstance = 0;
    std::uint64_t variableControlOuterRootEpoch = 0;
};

struct EnginePlainInitialRetryRetirement {
    ImmediateLaneHandle lane{};
    EnginePlayableServiceReference root{};
    EnginePlayableServiceReference resource{};
    ActionFiringHandle cohort = 0;
    TimelineArmToken arm{};
    OccurrenceToken occurrence{};
    SourceTerminalToken source{};
    CompletionOwnerRef owner{};
    PlayableProgramKey program{};
};

struct EngineVariableTreeStorage {
    struct Instance {
        PlayableInstanceHandle handle = 0;
        std::uint64_t epoch = 0;
    };
    struct Controller {
        VariableControlInstanceToken instance{};
        EnginePlayableServiceReference service{};
        DecisionToken decision{};
        std::uint32_t selectedOrdinal = 0;
        std::uint32_t continuationOrdinal = 0;
        bool continuous = false;
    };
    std::vector<Instance> leaves;
    std::vector<PlayableInstanceHandle> nestedInstances;
    std::vector<Controller> controllers;
    std::vector<Instance> containers;
    std::vector<SelectionPathNodeV1> selectionPath;

    std::size_t StorageBytes() const noexcept {
        return leaves.capacity() * sizeof(Instance) + nestedInstances.capacity() * sizeof(PlayableInstanceHandle) +
            controllers.capacity() * sizeof(Controller) + containers.capacity() * sizeof(Instance) +
            selectionPath.capacity() * sizeof(SelectionPathNodeV1);
    }
};

struct EngineVariableMemberWeights {
    struct ChildWeight {
        std::uint32_t ordinal = 0;
        WeightSegmentV1 current{};
        WeightSegmentV1 pending{};
    };
    using Values = std::vector<WeightSegmentV1>;
    using ChildValues = std::vector<ChildWeight>;
    std::shared_ptr<Values> storage;
    std::shared_ptr<ChildValues> childStorage;

    std::size_t size() const noexcept { return storage ? storage->size() : 0; }
    bool empty() const noexcept { return size() == 0; }
    WeightSegmentV1& operator[](std::size_t index) noexcept { return (*storage)[index]; }
    const WeightSegmentV1& operator[](std::size_t index) const noexcept { return (*storage)[index]; }
    std::span<ChildWeight> Children() noexcept {
        return childStorage ? std::span<ChildWeight>{*childStorage} : std::span<ChildWeight>{};
    }
    std::span<const ChildWeight> Children() const noexcept {
        return childStorage ? std::span<const ChildWeight>{*childStorage} : std::span<const ChildWeight>{};
    }
    std::size_t MemberStorageBytes() const noexcept {
        return storage ? sizeof(Values) + storage->capacity() * sizeof(WeightSegmentV1) : 0;
    }
    std::size_t ChildStorageBytes() const noexcept {
        return childStorage ? sizeof(ChildValues) + childStorage->capacity() * sizeof(ChildWeight) : 0;
    }
    std::size_t StorageBytes() const noexcept { return MemberStorageBytes() + ChildStorageBytes(); }
};

struct EngineVariableInitialArms {
    using Values = std::vector<TimelineArmToken>;
    std::shared_ptr<Values> storage;
    std::uint32_t continuousOffset = 0;

    std::size_t size() const noexcept { return storage ? storage->size() : 0; }
    std::span<TimelineArmToken> ValuesView() noexcept {
        return storage ? std::span<TimelineArmToken>{*storage} : std::span<TimelineArmToken>{};
    }
    std::span<const TimelineArmToken> ValuesView() const noexcept {
        return storage ? std::span<const TimelineArmToken>{*storage} : std::span<const TimelineArmToken>{};
    }
    auto Discrete() noexcept { return ValuesView().first(continuousOffset); }
    auto Discrete() const noexcept { return ValuesView().first(continuousOffset); }
    auto Continuous() noexcept { return ValuesView().subspan(continuousOffset); }
    auto Continuous() const noexcept { return ValuesView().subspan(continuousOffset); }
    std::size_t StorageBytes() const noexcept {
        return storage ? sizeof(Values) + storage->capacity() * sizeof(TimelineArmToken) : 0;
    }
};

// Views never allocate or acquire ownership. Native owners bind their own backing.
template<class T, std::size_t N>
class EngineServiceArrayView {
public:
    using Array = std::array<T, N>;
    EngineServiceArrayView() noexcept = default;
    EngineServiceArrayView(const EngineServiceArrayView&) = delete;
    EngineServiceArrayView(EngineServiceArrayView&&) = delete;
    EngineServiceArrayView& operator=(const Array& values) noexcept {
        assert(values_ != nullptr);
        *values_ = values;
        return *this;
    }
    EngineServiceArrayView& operator=(const EngineServiceArrayView& other) noexcept {
        if (other.values_ == nullptr) Clear();
        else *this = *other.values_;
        return *this;
    }
    std::size_t size() const noexcept { return values_ == nullptr ? 0u : N; }
    static constexpr std::size_t Capacity() noexcept { return N; }
    T* data() noexcept { return values_ == nullptr ? nullptr : values_->data(); }
    const T* data() const noexcept { return values_ == nullptr ? nullptr : values_->data(); }
    T* begin() noexcept { return data(); }
    const T* begin() const noexcept { return data(); }
    T* end() noexcept { return values_ == nullptr ? nullptr : data() + N; }
    const T* end() const noexcept { return values_ == nullptr ? nullptr : data() + N; }
    T& operator[](std::size_t index) noexcept {
        assert(values_ != nullptr && index < N);
        return (*values_)[index];
    }
    const T& operator[](std::size_t index) const noexcept {
        assert(values_ != nullptr && index < N);
        return (*values_)[index];
    }
    void Clear() noexcept { if (values_ != nullptr) values_->fill(T{}); }
    void fill(const T& value) noexcept {
        assert(values_ != nullptr);
        values_->fill(value);
    }
    const Array& Snapshot() const noexcept {
        static const Array empty{};
        return values_ == nullptr ? empty : *values_;
    }
    operator const Array&() const noexcept { return Snapshot(); }
private:
    friend struct EnginePlayableServiceRow;
    friend struct EngineOwnedQueueFutureFiling;
    void Bind(Array& values) noexcept { values_ = &values; }
    Array* values_ = nullptr;
};

// Future filing data is separate from the playable service's Current owners.
// A retained copy is scheduling data, never a native publication/apply right.
struct EngineQueueFutureFilingLayout {
    EngineMonitorSelection queueMonitorSelection{};
    DecisionToken queueDecision{};
    ConditionalQueueVersionToken queueConditional{};
    std::uint8_t queueSuccessorProgramPlane = 1;
    EngineQueueProgramBank* queueSuccessorProgramBank = nullptr;
    EngineQueueProgramRange queueSuccessorPrograms{};
    std::uint32_t queueSuccessorScheduleBase = 0;
    std::uint32_t queueSuccessorScheduleLeafCount = 0;
    KnownCompletionPredicateV1 queueSuccessorCompletion{};
    CompositeCompletionOwnerToken queueSuccessorCompositeOwner{};
    bool queueSuccessorHasZeroWorkCompletion = false;
    std::uint64_t queueSuccessorZeroWorkCompletionFrame = 0;
    OccurrenceToken queueSuccessorOccurrence{};
    TimelineArmToken queueSuccessorArm{};
    ArmGroupToken queueArmGroup{};
    TransitionInstanceToken queueTransition{};
    std::uint64_t queueTransitionEndFrame = 0;
    EngineServiceArrayView<TimelineArmToken, 2u * kMaxKnownScheduleLeaves>
        queueTransitionGroupArms{};
    std::uint32_t queueTransitionGroupArmCount = 0;
    std::uint32_t queueSourceControlArmCount = 0;
    ProgramView queueSourceSchedule{};
    ProgramView queueSuccessorSchedule{};
    bool queueSameProgramKey = false;
    bool queueDecisionCommitted = false;
};

struct EngineOwnedQueueFutureFiling : EngineQueueFutureFilingLayout {
    EngineOwnedQueueFutureFiling() noexcept {
        queueTransitionGroupArms.Bind(arms_);
    }
    explicit EngineOwnedQueueFutureFiling(const EngineQueueFutureFilingLayout& other) noexcept
        : EngineOwnedQueueFutureFiling() {
        EngineQueueFutureFilingLayout::operator=(other);
    }
    EngineOwnedQueueFutureFiling(const EngineOwnedQueueFutureFiling& other) noexcept
        : EngineOwnedQueueFutureFiling(static_cast<const EngineQueueFutureFilingLayout&>(other)) {}
    EngineOwnedQueueFutureFiling(EngineOwnedQueueFutureFiling&& other) noexcept
        : EngineOwnedQueueFutureFiling(static_cast<const EngineQueueFutureFilingLayout&>(other)) {}
    EngineOwnedQueueFutureFiling& operator=(const EngineQueueFutureFilingLayout& other) noexcept {
        EngineQueueFutureFilingLayout::operator=(other);
        return *this;
    }
    EngineOwnedQueueFutureFiling& operator=(const EngineOwnedQueueFutureFiling& other) noexcept {
        return *this = static_cast<const EngineQueueFutureFilingLayout&>(other);
    }
    EngineOwnedQueueFutureFiling& operator=(EngineOwnedQueueFutureFiling&& other) noexcept {
        return *this = static_cast<const EngineQueueFutureFilingLayout&>(other);
    }
private:
    std::array<TimelineArmToken, 2u * kMaxKnownScheduleLeaves> arms_{};
};

struct EngineDiscreteRetargetStorage {
    std::vector<TimelineArmToken> groupArms;
    std::vector<TimelineEventPayload> payloads;
    std::vector<TimelineEventPayload> enteringPayloads;
    std::vector<TimelineProgramView> runtimePrograms;
    std::vector<TimelineProgramView> enteringRuntimePrograms;
    std::vector<EventTemplate> events;
    std::vector<EventTemplate> enteringEvents;
    std::vector<ProgramView> schedules;
    std::vector<ProgramView> enteringSchedules;
};

struct EngineQueueRuntimeViews {
    std::span<TimelineEventPayload> queueSourcePayloads{};
    std::span<EventTemplate> queueSourceEvents{};
    TimelineProgramView queueSourceRuntimeProgram{};
    std::span<TimelineProgramView> queueSourceRuntimePrograms{};
    std::span<ProgramView> queueSourceSchedules{};
    TimelineProgramView queueSuccessorRuntimeProgram{};
    // The service owns one frozen target, shared by its preparing frontier.
    CapturedTimelineVoiceInstall canonicalInitialQueueTargetVoice{};
};

struct EngineVariableServicePayload {
    struct VariableControlRetargetMember {
        EnginePlayableMemberReference reference{};
        WeightSegmentV1 pendingWeight{};
        TimelineArmToken arm{};
    };
    BankVersionToken variableControlDiscreteMissedBank{};
    PlayableInstanceHandle variableControlDiscreteMissedSource = 0;
    std::uint32_t variableControlDiscreteMissedOrdinal = 0;
    DecisionToken variableControlDiscreteRetargetDecision{};
    TransitionInstanceToken variableControlDiscreteRetargetTransition{};
    LogicCommitSequence variableControlDiscreteRetargetTicket = 0;
    BankVersionToken variableControlDiscreteRetargetBank{};
    TimelineArmToken variableControlDiscreteRetargetArm{};
    TimelineArmToken variableControlDiscreteRetargetEnteringArm{};
    std::uint32_t variableControlDiscreteRetargetGroupMemberCount = 0;
    std::uint32_t variableControlDiscreteRetargetSourceMemberCount = 0;
    std::uint32_t variableControlDiscreteRetargetTargetMemberCount = 0;
    std::uint32_t variableControlDiscreteRetargetDesiredChildOrdinal = 0;
    PlayableInstanceHandle variableControlDiscreteRetargetIncomingChild = 0;
    std::uint64_t variableControlDiscreteRetargetIncomingChildEpoch = 0;
    std::uint32_t variableControlDiscreteRetargetIncomingLeafCount = 0;
    std::uint32_t variableControlDiscreteRetargetIncomingNestedCount = 0;
    std::uint32_t variableControlDiscreteRetargetIncomingNestedVariableControlCount = 0;
    std::uint32_t variableControlDiscreteRetargetIncomingContainerCount = 0;
    PlayableInstanceHandle variableControlDiscreteRetargetIncomingQueueSuccessorInstance = 0;
    std::uint64_t variableControlDiscreteRetargetIncomingQueueSuccessorEpoch = 0;
    std::uint32_t variableControlDiscreteRetargetIncomingQueueSuccessorOrdinal = 0;
    PlayableInstanceHandle variableControlDiscreteRetargetIncomingQueueOwnerInstance = 0;
    std::uint64_t variableControlDiscreteRetargetIncomingQueueOwnerEpoch = 0;
    std::uint32_t variableControlDiscreteRetargetIncomingQueueOwnerIndex = UINT32_MAX;
    std::uint32_t variableControlDiscreteRetargetIncomingSelectionCount = 0;
    ArmGroupToken variableControlDiscreteRetargetArmGroup{};
    PreparedGroupToken variableControlDiscreteRetargetPages{};
    std::uint64_t variableControlDiscreteRetargetFrame = 0;
    std::uint64_t variableControlDiscreteRetargetEndFrame = 0;
    std::uint64_t variableControlDiscreteRetargetZeroWorkCompletionFrame = 0;
    std::uint64_t variableControlDiscretePendingDesiredOrdinal = 0;
    std::uint64_t variableControlDiscretePendingDesiredFrame = 0;
    BankVersionToken variableControlDiscreteDeferredSupersedeBank{};
    std::uint64_t variableControlNextEvaluationFrame = 0;
    DecisionToken variableControlRetargetDecision{};
    LogicCommitSequence variableControlRetargetTicket = 0;
    BankVersionToken variableControlRetargetBank{};
    ArmGroupToken variableControlRetargetArmGroup{};
    PreparedGroupToken variableControlRetargetPages{};
    std::uint64_t variableControlRetargetFrame = 0;
    std::uint64_t variableControlRetargetEnvelopeEndFrame = 0;
    std::uint64_t variableControlRetargetZeroWorkCompletionFrame = 0;
    EngineServiceArrayView<std::uint32_t, 64> variableControlRetargetLogicalOrdinals{};
    EngineServiceArrayView<std::uint64_t, 64> variableControlRetargetLogicalCompletionFrames{};
    EngineServiceArrayView<bool, 64> variableControlRetargetLogicalCompleted{};
    std::uint32_t variableControlRetargetLogicalCount = 0;
    std::vector<VariableControlRetargetMember> variableControlRetargetMembers;
    EngineVariableMemberWeights variableControlMemberWeights;
    BankVersionToken variableControlDeferredSupersedeBank{};
};

struct EngineQueueServicePayload {
    VnObjectId queueContainerStableId{};
    std::uint64_t queueProgramSemanticFingerprint = 0;
    VnObjectId queueParentContainerStableId{};
    std::uint64_t queueParentProgramSemanticFingerprint = 0;
    std::uint64_t initialQueueEntryAttemptGeneration = 0;
    OccurrenceToken queueCurrentOccurrence{};
    KnownCompletionPredicateV1 queueCurrentCompletion{};
    CompositeCompletionOwnerToken queueCurrentCompositeOwner{};
    std::uint8_t queueCurrentProgramPlane = 0;
    EngineQueueProgramBank* queueCurrentProgramBank = nullptr;
    EngineQueueProgramRange queueCurrentPrograms{};
    std::uint32_t queueCurrentScheduleBase = 0;
    std::uint32_t queueCurrentScheduleLeafCount = 0;
    EngineServiceArrayView<std::uint16_t, kMaxKnownScheduleLeaves> queueInitialCurrentMemberIndices{};
    EngineServiceArrayView<TimelineArmToken, kMaxKnownScheduleLeaves> queueInitialCurrentDelayedArms{};
    std::uint32_t queueInitialCurrentMemberLeafCount = 0;
    std::uint16_t queueInitialCurrentDelayedCount = 0;
    CompletionOwnerRef queueSuccessorOwner{};
    OccurrenceToken queueRetiringOccurrence{};
    OccurrenceToken queueAbortedSuccessorOccurrence{};
    CompletionOwnerRef queueRetiringOwner{};
    TimelineArmToken queueSourceArm{};
    ArmGroupToken queueInitialLaneArmGroup{};
    TimelineArmToken queueArrivalArm{};
    TimelineArmToken queueArrivalSourceArm{};
    std::uint32_t queueArrivalSourceControlArmCount = 0;
    std::uint32_t queueArrivalTransitionGroupArmCount = 0;
    TransitionInstanceToken queueArrivalTransition{};
    ArmGroupToken queueArrivalArmGroup{};
    std::uint32_t queueArrivalScheduleBase = 0;
    std::uint32_t queueArrivalScheduleLeafCount = 0;
    PreparedGroupToken queuePreparedPages{};
    UnplacedBatchCredit queueUnplacedCredit{};
    UnplacedBatchCredit queueUnplacedDemandCredit{};
    UnplacedBatchCreditRequest queueUnplacedDemand{};
    std::array<std::uint64_t, 3> queueUnplacedGroupFrames{};
    std::uint32_t queueUnplacedGroupCount = 0;
    std::uint32_t queueUnplacedTransactionCount = 0;
    EngineFrontierQueueCurrentOwnerContext* privateInitialQueueCurrent = nullptr;
    DecisionToken canonicalInitialQueueDecision{};
    VnVoiceHandle queueSuccessorRenderHandle = 0;
    LogicCommitSequence queueTicket = 0;
    LogicCommitSequence queueSuccessorTicket = 0;
    std::uint64_t queueCompletionFrame = 0;
    std::uint32_t queueCurrentTailFrames = 0;
    std::uint32_t queueSuccessorTailFrames = 0;
    VoiceReleaseObserver queueArrivalCurrentRelease{};
};

struct EngineServiceFeatureArrays {
    EngineQueueRuntimeViews queueRuntimeViews;
    EngineDiscreteRetargetStorage discreteRetarget;
    EngineQueueFutureFilingLayout queueFiling{};
    struct UnpublishedVariableGroupCancellation {
        EnginePlayableServiceToken service{};
        EnginePlayablePartitionToken partition{};
        VariableControlInstanceToken instance{};
        VariableControlSide side = VariableControlSide::Discrete;
        DecisionToken decision{};
        ArmGroupToken group{};
        PlayableInstanceHandle rootInstance = 0;
        PlayableInstanceHandle serviceRootInstance = 0;
        PlayableInstanceHandle ownerInstance = 0;
        std::uint64_t ownerEpoch = 0;
        std::uint64_t invocationGeneration = 0;
        std::uint64_t world = 0;
        ActionFiringHandle firing = 0;
        std::uint32_t memberCount = 0;
    };
    PlayableRefV1 variableControlPropertyRoot{};
    Plan03GroupSliceIdentity initialLaneDiscreteGroup{};
    Plan03GroupSliceIdentity initialLaneContinuousGroup{};
    UnpublishedVariableGroupCancellation variableControlDiscreteGroupCancellation{};
    UnpublishedVariableGroupCancellation variableControlContinuousGroupCancellation{};
    EngineVariableTreeStorage variableControlRetargetIncoming;
    Plan03GroupSliceIdentity variableControlDiscreteRetargetSlice{};
    Plan03GroupSliceIdentity variableControlRetargetSlice{};
    Plan03GroupSliceIdentity variableControlDiscreteDeferredSupersedeSlice{};
    Plan03GroupSliceIdentity variableControlDeferredSupersedeSlice{};
    QueueCheckpointKey queueParentCheckpointKey{};
    QueueCheckpointKey queueCheckpointKey{};
    RandomQueueSeedContext queueParentRandomSeed{};
    RandomQueueSeedContext queueRandomSeed{};
    EngineQueueDeferredSourceObservation queueDeferredSource{};
    PlayableRefV1 queueSuccessorLeaf{};
    DecisionReserveRequest canonicalInitialQueueRequest{};
    std::array<SelectionDeltaRefV1, kSelectionPathNodeCapacity> selectionDeltas{};
    std::array<std::uint32_t, kSelectionPathNodeCapacity> selectedChildOrdinals{};
    std::array<PlayableInstanceHandle, kMaxKnownScheduleLeaves + 64u> variableControlChildInstances{};
    std::array<PlayableKind, kMaxKnownScheduleLeaves + 64u> variableControlChildKinds{};
    std::array<std::uint64_t, kMaxKnownScheduleLeaves + 64u> variableControlChildEpochs{};
    std::array<PlayableInstanceHandle, kMaxKnownScheduleLeaves> variableControlLeafInstances{};
    std::array<std::uint64_t, kMaxKnownScheduleLeaves> variableControlLeafEpochs{};
    std::array<std::uint32_t, 64> variableControlContinuousLogicalOrdinals{};
    std::array<std::uint64_t, 64> variableControlContinuousLogicalCompletionFrames{};
    std::array<bool, 64> variableControlContinuousLogicalCompleted{};
    std::array<std::uint32_t, 64> variableControlRetargetLogicalOrdinals{};
    std::array<std::uint64_t, 64> variableControlRetargetLogicalCompletionFrames{};
    std::array<bool, 64> variableControlRetargetLogicalCompleted{};
    std::array<std::uint16_t, kMaxKnownScheduleLeaves> queueInitialCurrentMemberIndices{};
    std::array<TimelineArmToken, kMaxKnownScheduleLeaves> queueInitialCurrentDelayedArms{};
    std::array<TimelineArmToken, 2u * kMaxKnownScheduleLeaves> queueTransitionGroupArms{};
    EffectiveQueueState queueParentInitialState{};
    EffectiveQueueState queueParentResetInitialState{};
    EffectiveQueueState queueNextState{};
    std::array<PlayableInstanceHandle, kSelectionPathNodeCapacity> containerInstances{};
    std::array<SelectionPathNodeV1, kSelectionPathNodeCapacity> selectionPath{};
    CurveLUT transitionSourceFadeCurve{};
    CurveLUT transitionTargetFadeCurve{};
    PlayableWorldViewV1 variableControlWorld{};
    PlayableWorldViewV1 queueWorld{};
    PlayableWorldViewV1 containerLoopWorld{};
    TimelineProgramView variableControlDiscreteRetargetRuntimeProgram{};
    TimelineProgramView variableControlDiscreteRetargetEnteringRuntimeProgram{};
    ProgramView variableControlDiscreteRetargetSchedule{};
    ProgramView variableControlDiscreteRetargetEnteringSchedule{};
    EngineVariableServicePayload variablePayload{};
    EngineQueueServicePayload queuePayload{};
};

struct EnginePlayableServiceRow {
    using UnpublishedVariableGroupCancellation =
        EngineServiceFeatureArrays::UnpublishedVariableGroupCancellation;
    // Keep common lifecycle observations together, ahead of container-specific state.
    EnginePlayableServiceToken identity{};
    EnginePlayablePartitionToken partition{};
    PlayableInstanceHandle ownerInstance = 0;
    PlayableInstanceHandle rootInstance = 0;
    TimelineArmToken arm{};
    VnVoiceHandle renderHandle = 0;
    std::uint64_t lastAppliedBlockSequence = 0;
    PlayableRefV1 root{};
    PlayableRefV1 leaf{};
    VariableControlInstanceToken variableControlInstance{};
    std::uint32_t scheduleStorageBase = 0;
    std::uint32_t scheduleLeafCount = 0;
    std::vector<EngineMemberProgramToken> memberPrograms;
    std::vector<EnginePlayableMemberToken> members;
    std::uint32_t memberRelationHead = UINT32_MAX;
    std::uint32_t memberRelationCount = 0;
    std::uint32_t childLiveCount = 0;
    std::uint32_t childAudibleRetiringCount = 0;
    std::uint32_t pendingFutureCount = 0;
    EnginePlayableStartPhase phase = EnginePlayableStartPhase::Free;
    PlayableControlLifecycle controlLifecycle = PlayableControlLifecycle::Preparing;
    PlayableControlReason controlReason = PlayableControlReason::None;
    std::uint8_t pendingCancellationIntent = 0;
    bool partitionRetiring = false;
    bool pendingImmediateEntry = false;
    bool pendingImmediateRetry = false;
    bool zeroWorkMarker = false;
    bool hasZeroWorkCompletion = false;
    bool variableControlDeferredCancellation = false;
    bool queueMode = false;
    bool queueTimingPending = false;
    bool ordinaryLifecycleOwned = false;
    bool selectionCommitted = false;
    bool terminalObserved = false;
    FrontierEntryReceiptRef frontierEntry{};
    FrontierEntryReceiptRef frontierLifetimeEntry{};
    DecisionFuturePin frontierLifetimePin{};
    EnginePlayableServiceReference frontierChildService{};
    ArmGroupToken frontierCoordinatorGroup{};
    AcceptedQueueSignalAdvanceV1 frontierAcceptedSignal{};
    TimelineArmReaderPin frontierArmPin{};
    EnginePlayableServiceReference parentService{};
    std::uint32_t parentChildOrdinal = 0;
    std::uint64_t ownerEpoch = 0;
    bool quarantined = false;
    EnginePlayableStartRow* invocation = nullptr;
    std::uint64_t invocationGeneration = 0;
    std::uint32_t privateVariableControlRollbackSlot = UINT32_MAX;
    std::uint64_t privateVariableControlRollbackGeneration = 0;
    std::uint32_t publishedDeferredStoppingSlot = UINT32_MAX;
    std::uint64_t publishedDeferredStoppingGeneration = 0;
    std::uint32_t privateOrdinaryDeferredRollbackSlot = UINT32_MAX;
    std::uint64_t privateOrdinaryDeferredRollbackGeneration = 0;
    PlayableInstanceHandle activeLeafInstance = 0;
    bool actionPrepared = false;
    LogicCommitSequence ticket = 0;
    CommittedTransactionRef ordinaryStartTransaction{};
    bool ordinaryStartPageRemoved = false;
    bool ordinaryStartCancelFault = false;
    bool playableNotificationDone = false;
    bool ordinaryStartCancelProof = false;
    EnginePlayableServiceToken ordinaryStartProofService{};
    EnginePlayablePartitionToken ordinaryStartProofPartition{};
    std::uint64_t ordinaryStartProofInvocationGeneration = 0;
    TimelineArmToken ordinaryStartProofArm{};
    TimelineTransactionProgramRef ordinaryStartProofProgram{};
    std::uint64_t startFrame = 0;
    ImmediateLaneTimeDomain pendingImmediateTimeDomain =
        ImmediateLaneTimeDomain::RelativeToAnchor;
    std::uint64_t pendingImmediateFixedEntryFrame = 0;
    ImmediateLaneHandle immediateLane{};
    EnginePlainInitialRetryRetirement plainInitialRetryRetirement{};
    struct PrivateImmediateEntryReceiver {
        ImmediateLaneHandle lane{};
        ActionFiringHandle root = 0;
        std::uint32_t ordinal = UINT32_MAX;
        FrontierEntryReceiptRef entry{};
        EnginePlayableServiceToken service{};
        EnginePlayablePartitionToken partition{};
        std::uint64_t invocationGeneration = 0;
        std::vector<ImmediateLanePrivateEntryResultCell> results;
        ImmediateLanePrivateEntryResultView shape{};
        ImmediateLanePrivateEntryResultView terminal{};
        bool stopAccepted = false;
        bool projectionReceived = false;
        bool commonPinsRetired = false;
        bool Valid() const noexcept {
            return lane.Valid() && root != 0 && ordinal != UINT32_MAX && entry.Valid() &&
                service.Valid() && partition.Valid() && invocationGeneration != 0;
        }
    } privateImmediateEntry{};
    bool pendingImmediateRetryStopIssued = false;
private:
    friend struct Engine;
    bool cohortLinked_ = false;
    ActionFiringHandle pendingImmediateCohortRoot_ = 0;
    std::uint32_t cohortPrevious_ = UINT32_MAX;
    std::uint32_t cohortNext_ = UINT32_MAX;
public:
    ActionFiringHandle PendingImmediateCohortRoot() const noexcept {
        return pendingImmediateCohortRoot_;
    }
    std::uint64_t zeroWorkCompletionFrame = 0;
    EngineServiceArrayView<SelectionDeltaRefV1, kSelectionPathNodeCapacity> selectionDeltas{};
    EngineServiceArrayView<std::uint32_t, kSelectionPathNodeCapacity> selectedChildOrdinals{};
    std::uint32_t selectionCount = 0;
    std::uint32_t selectionDeltaCount = 0;
    SelectionSeedProbeCopyV1 selectionSeedProbe{};
    std::uint64_t variableControlAcceptedEpoch = 0;
    DecisionToken variableControlDiscreteDecision{};
    LogicCommitSequence variableControlDiscreteTicket = 0;
    BankVersionToken variableControlDiscreteBank{};
    ArmGroupToken variableControlDiscreteArmGroup{};
    PreparedGroupToken variableControlDiscretePages{};
    bool variableControlDiscreteDecisionCommitted = false;
    bool variableControlDiscreteCleanupStarted = false;
    DecisionToken variableControlContinuousDecision{};
    LogicCommitSequence variableControlContinuousTicket = 0;
    BankVersionToken variableControlContinuousBank{};
    ArmGroupToken variableControlContinuousArmGroup{};
    PreparedGroupToken variableControlContinuousPages{};
    bool variableControlContinuousDecisionCommitted = false;
    bool variableControlContinuousCleanupStarted = false;
    std::uint32_t variableControlDiscreteMemberCount = 0;
    std::uint32_t variableControlInitialDiscreteMemberCount = 0;
    std::uint32_t variableControlDiscreteCurrentChildOrdinal = 0;
    EngineVariableInitialArms variableControlInitialArms;
    // One child instance per published VariableControl member.  The root
    // remains the owner of the VariableControl decision state; these handles
    // own the playable voice/schedule lifetime and are retired with the tree.
    EngineServiceArrayView<PlayableInstanceHandle, kMaxKnownScheduleLeaves + 64u> variableControlChildInstances{};
    EngineServiceArrayView<PlayableKind, kMaxKnownScheduleLeaves + 64u> variableControlChildKinds{};
    EngineServiceArrayView<std::uint64_t, kMaxKnownScheduleLeaves + 64u> variableControlChildEpochs{};
    // Nested child handles remain indexed after the top-level row members so
    // lifecycle cleanup and diagnostics can enumerate the full subtree.
    std::uint32_t variableControlNestedChildCount = 0;
    bool variableControlIndependentFrontier = false;
    // Internal owners for nested VariableControl containers. These tokens are
    // intentionally private to the start row and are released with its tree.
    std::vector<EngineVariableTreeStorage::Controller> variableControlControllers;
    std::uint32_t variableControlNestedVariableControlCount = 0;
    std::vector<EngineVariableTreeStorage::Instance> variableControlContainers;
    std::uint32_t variableControlNestedContainerCount = 0;
    EngineServiceArrayView<PlayableInstanceHandle, kMaxKnownScheduleLeaves> variableControlLeafInstances{};
    EngineServiceArrayView<std::uint64_t, kMaxKnownScheduleLeaves> variableControlLeafEpochs{};
    // The nested Queue owns its cursor and occurrences on the exact
    // Container instance that represents the Queue program. This is not
    // necessarily the first nested Container in a deep VariableControl path.
    std::uint32_t variableControlNestedQueueOwnerIndex = UINT32_MAX;
    PlayableInstanceHandle variableControlNestedQueueOwnerInstance = 0;
    std::uint64_t variableControlNestedQueueOwnerEpoch = 0;
    PlayableInstanceHandle variableControlNestedQueueSuccessorInstance = 0;
    std::uint64_t variableControlNestedQueueSuccessorEpoch = 0;
    std::uint32_t variableControlNestedQueueSuccessorOrdinal = 0;
    std::uint32_t variableControlChildCount = 0;
    std::uint32_t variableControlContinuousMemberOffset = 0;
    std::uint32_t variableControlDiscreteTransitionOffset = 0;
    std::uint32_t variableControlContinuousMemberCount = 0;
    std::uint32_t variableControlInitialContinuousMemberCount = 0;
    bool variableControlWorldUsesCurrentAuthority = false;
    VnEmitterHandle variableControlEmitter = 0;
    std::uint64_t variableControlDiscreteNextEvaluationFrame = 0;
    std::uint64_t variableControlDiscreteZeroWorkCompletionFrame = 0;
    bool variableControlDiscreteHasZeroWorkCompletion = false;
    std::uint64_t variableControlContinuousZeroWorkCompletionFrame = 0;
    bool variableControlContinuousHasZeroWorkCompletion = false;
    EngineServiceArrayView<std::uint32_t, 64> variableControlContinuousLogicalOrdinals{};
    EngineServiceArrayView<std::uint64_t, 64> variableControlContinuousLogicalCompletionFrames{};
    EngineServiceArrayView<bool, 64> variableControlContinuousLogicalCompleted{};
    std::uint32_t variableControlContinuousLogicalCount = 0;
    std::uint32_t variableControlChildWeightCount = 0;
    bool variableControlDiscreteRetargetHasZeroWorkCompletion = false;
    bool variableControlDiscreteRetargetPrepared = false;
    bool variableControlDiscreteRetargetDecisionCommitted = false;
    bool variableControlDiscreteRetargetApplied = false;
    bool variableControlDiscreteRetargetCleanupStarted = false;
    bool variableControlDiscreteRetargetSourceExited = false;
    bool variableControlDiscreteRetargetEntering = false;
    bool variableControlDiscreteRetargetReplacement = false;
    bool variableControlDiscreteRetargetSuperseded = false;
    bool variableControlDiscreteDeferredSupersede = false;
    bool variableControlDiscreteSilent = false;
    bool variableControlRetargetHasZeroWorkCompletion = false;
    using VariableControlRetargetMember = EngineVariableServicePayload::VariableControlRetargetMember;
    // Delayed members can have a different envelope onset from their controlling child.
    bool variableControlRetargetPrepared = false;
    bool variableControlRetargetDecisionCommitted = false;
    bool variableControlRetargetApplied = false;
    bool variableControlRetargetCleanupStarted = false;
    bool variableControlRetargetExitsIssued = false;
    bool variableControlRetargetMembershipFinalized = false;
    bool variableControlMembershipExitObserved = false;
    bool variableControlRetargetSuperseded = false;
    bool variableControlDeferredSupersede = false;
    Plan03GroupSliceIdentity variableControlDeferredCancellationSlice{};
    std::uint32_t variableControlDeferredCancellationReasonBit = 0;
    MonitorControlCause pendingControlCause{};
    PlayableInstanceHandle pendingControlTarget = 0;
    std::uint64_t pendingControlTargetEpoch = 0;
    std::uint32_t pendingCancellationFadeMs = 0;
    bool pendingRegisteredStopCapture = false;
    bool queueWorldUsesCurrentAuthority = false;
    bool queueResumePosition = false;
    QueueInstanceToken queueInstance{};
    QueueInstanceToken queueParentInstance{};
    bool queueParentRandomSeedValid = false;
    bool queueParentResumePosition = false;
    bool queueNestedParentPending = false;
    bool queueRandomSeedValid = false;
    DecisionToken initialQueueEntryDecision{};
    DecisionToken queueSuccessorDecision{};
    DecisionToken queueArrivalDecision{};
    bool queueCompositeMirrorsCleared = false;
    bool queueCurrentProgramBorrowed = false;

    void ClearQueueSourcePrograms() noexcept {
        if (!HasFeatureArrays()) return;
        auto& views = QueueRuntimeViews();
        views.queueSourcePayloads = {};
        views.queueSourceEvents = {};
        views.queueSourceRuntimePrograms = {};
        views.queueSourceSchedules = {};
        views.queueSourceRuntimeProgram = {};
        QueueFiling().queueSourceSchedule = {};
    }
    bool queueArrivalUsesSuccessorStorage = false;
    bool queueTimingFinalized = false;
    bool queueFirstContinuationLane = false;
    std::uint64_t queueInitialCohortEntryFrame = 0;
    bool queueContinuationPrepared = false;
    bool queueArrivalCleanupStarted = false;
    bool queueArrivalDecisionCancelled = false;
    bool queueSuccessorArmTombstoned = false;
    bool queueNaturalEnd = false;
    bool queueSignalOnlyNoOverlap = false;
    bool queueDeferredNeedsFullPreparation = false;
    PlayableExecutionAuthorityKind containerLoopAuthorityKind =
        PlayableExecutionAuthorityKind::FrozenWorld;
    OccurrenceToken containerLoopOccurrence{};
    CompositeCompletionOwnerToken containerLoopCompositeOwner{};
    bool containerLoopOwnsScheduleResources = false;
    bool ordinaryFinalSourceTransferred = false;
    EngineServiceArrayView<PlayableInstanceHandle, kSelectionPathNodeCapacity> containerInstances{};
    std::uint32_t containerInstanceCount = 0;
    EngineServiceArrayView<SelectionPathNodeV1, kSelectionPathNodeCapacity> selectionPath{};
    std::uint32_t containerLoopIteration = 0;
    bool queueFinalSourceTombstoned = false;
    bool queueAbortRequested = false;
    bool queueCleanupStarted = false;
    bool usesCapturedVoice = false;
    bool usesCapturedScheduleVoices = false;
    CapturedTimelineVoiceInstall capturedVoice{};
    std::vector<CapturedTimelineVoiceInstall> capturedScheduleVoices{};
    TimelineEventPayload payload{};
    TimelineProgramView runtimeProgram{};
    EventTemplate event{};
    ProgramView schedule{};
    VoiceReleaseObserver releaseObserver{};
    EngineMonitorSelection monitorSelection{};
    std::uint32_t prepareCount = 0;

    bool InvocationValid() const noexcept {
        return invocation != nullptr && invocationGeneration != 0 &&
            invocation->serviceCount != 0 &&
            invocation->generation == invocationGeneration;
    }
    EnginePlayableStartRow& Invocation() noexcept {
        assert(InvocationValid());
        return *invocation;
    }
    const EnginePlayableStartRow& Invocation() const noexcept {
        assert(InvocationValid());
        return *invocation;
    }

    EngineVariableServicePayload& VariablePayload() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variablePayload;
    }
    const EngineVariableServicePayload& VariablePayload() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variablePayload;
    }
    EngineQueueServicePayload& QueuePayload() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queuePayload;
    }
    const EngineQueueServicePayload& QueuePayload() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queuePayload;
    }
    bool HasFeatureArrays() const noexcept { return featureArrays_ != nullptr; }
    EngineQueueRuntimeViews& QueueRuntimeViews() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueRuntimeViews;
    }
    const EngineQueueRuntimeViews& QueueRuntimeViews() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueRuntimeViews;
    }
    EngineQueueFutureFilingLayout& QueueFiling() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueFiling;
    }
    const EngineQueueFutureFilingLayout& QueueFiling() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueFiling;
    }
    PlayableRefV1& VariableControlPropertyRoot() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlPropertyRoot;
    }
    const PlayableRefV1& VariableControlPropertyRoot() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlPropertyRoot;
    }
    Plan03GroupSliceIdentity& InitialLaneDiscreteGroup() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->initialLaneDiscreteGroup;
    }
    const Plan03GroupSliceIdentity& InitialLaneDiscreteGroup() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->initialLaneDiscreteGroup;
    }
    Plan03GroupSliceIdentity& InitialLaneContinuousGroup() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->initialLaneContinuousGroup;
    }
    const Plan03GroupSliceIdentity& InitialLaneContinuousGroup() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->initialLaneContinuousGroup;
    }
    UnpublishedVariableGroupCancellation& VariableControlDiscreteGroupCancellation() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlDiscreteGroupCancellation;
    }
    const UnpublishedVariableGroupCancellation& VariableControlDiscreteGroupCancellation() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlDiscreteGroupCancellation;
    }
    UnpublishedVariableGroupCancellation& VariableControlContinuousGroupCancellation() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlContinuousGroupCancellation;
    }
    const UnpublishedVariableGroupCancellation& VariableControlContinuousGroupCancellation() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlContinuousGroupCancellation;
    }
    EngineVariableTreeStorage& VariableControlRetargetIncoming() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlRetargetIncoming;
    }
    const EngineVariableTreeStorage& VariableControlRetargetIncoming() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlRetargetIncoming;
    }
    Plan03GroupSliceIdentity& VariableControlDiscreteRetargetSlice() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlDiscreteRetargetSlice;
    }
    const Plan03GroupSliceIdentity& VariableControlDiscreteRetargetSlice() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlDiscreteRetargetSlice;
    }
    Plan03GroupSliceIdentity& VariableControlRetargetSlice() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlRetargetSlice;
    }
    const Plan03GroupSliceIdentity& VariableControlRetargetSlice() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlRetargetSlice;
    }
    Plan03GroupSliceIdentity& VariableControlDiscreteDeferredSupersedeSlice() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlDiscreteDeferredSupersedeSlice;
    }
    const Plan03GroupSliceIdentity& VariableControlDiscreteDeferredSupersedeSlice() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlDiscreteDeferredSupersedeSlice;
    }
    Plan03GroupSliceIdentity& VariableControlDeferredSupersedeSlice() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlDeferredSupersedeSlice;
    }
    const Plan03GroupSliceIdentity& VariableControlDeferredSupersedeSlice() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlDeferredSupersedeSlice;
    }
    QueueCheckpointKey& QueueParentCheckpointKey() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueParentCheckpointKey;
    }
    const QueueCheckpointKey& QueueParentCheckpointKey() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueParentCheckpointKey;
    }
    harken::QueueCheckpointKey& QueueCheckpointKey() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueCheckpointKey;
    }
    const harken::QueueCheckpointKey& QueueCheckpointKey() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueCheckpointKey;
    }
    RandomQueueSeedContext& QueueParentRandomSeed() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueParentRandomSeed;
    }
    const RandomQueueSeedContext& QueueParentRandomSeed() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueParentRandomSeed;
    }
    RandomQueueSeedContext& QueueRandomSeed() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueRandomSeed;
    }
    const RandomQueueSeedContext& QueueRandomSeed() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueRandomSeed;
    }
    EngineQueueDeferredSourceObservation& QueueDeferredSource() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueDeferredSource;
    }
    const EngineQueueDeferredSourceObservation& QueueDeferredSource() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueDeferredSource;
    }
    PlayableRefV1& QueueSuccessorLeaf() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueSuccessorLeaf;
    }
    const PlayableRefV1& QueueSuccessorLeaf() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueSuccessorLeaf;
    }
    DecisionReserveRequest& CanonicalInitialQueueRequest() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->canonicalInitialQueueRequest;
    }
    const DecisionReserveRequest& CanonicalInitialQueueRequest() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->canonicalInitialQueueRequest;
    }
    CurveLUT& TransitionSourceFadeCurve() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->transitionSourceFadeCurve;
    }
    CurveLUT& TransitionTargetFadeCurve() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->transitionTargetFadeCurve;
    }
    EffectiveQueueState& QueueParentInitialState() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueParentInitialState;
    }
    const EffectiveQueueState& QueueParentInitialState() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueParentInitialState;
    }
    EffectiveQueueState& QueueParentResetInitialState() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueParentResetInitialState;
    }
    const EffectiveQueueState& QueueParentResetInitialState() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueParentResetInitialState;
    }
    EffectiveQueueState& QueueNextState() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueNextState;
    }
    const EffectiveQueueState& QueueNextState() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueNextState;
    }
    PlayableWorldViewV1& VariableControlWorld() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlWorld;
    }
    const PlayableWorldViewV1& VariableControlWorld() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlWorld;
    }
    PlayableWorldViewV1& QueueWorld() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueWorld;
    }
    const PlayableWorldViewV1& QueueWorld() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->queueWorld;
    }
    PlayableWorldViewV1& ContainerLoopWorld() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->containerLoopWorld;
    }
    const PlayableWorldViewV1& ContainerLoopWorld() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->containerLoopWorld;
    }
    TimelineProgramView& VariableControlDiscreteRetargetRuntimeProgram() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlDiscreteRetargetRuntimeProgram;
    }
    const TimelineProgramView& VariableControlDiscreteRetargetRuntimeProgram() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlDiscreteRetargetRuntimeProgram;
    }
    TimelineProgramView& VariableControlDiscreteRetargetEnteringRuntimeProgram() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlDiscreteRetargetEnteringRuntimeProgram;
    }
    const TimelineProgramView& VariableControlDiscreteRetargetEnteringRuntimeProgram() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlDiscreteRetargetEnteringRuntimeProgram;
    }
    ProgramView& VariableControlDiscreteRetargetSchedule() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlDiscreteRetargetSchedule;
    }
    const ProgramView& VariableControlDiscreteRetargetSchedule() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlDiscreteRetargetSchedule;
    }
    ProgramView& VariableControlDiscreteRetargetEnteringSchedule() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlDiscreteRetargetEnteringSchedule;
    }
    const ProgramView& VariableControlDiscreteRetargetEnteringSchedule() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->variableControlDiscreteRetargetEnteringSchedule;
    }
    EngineDiscreteRetargetStorage& DiscreteRetargetStorage() noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->discreteRetarget;
    }
    const EngineDiscreteRetargetStorage& DiscreteRetargetStorage() const noexcept {
        assert(featureArrays_ != nullptr);
        return featureArrays_->discreteRetarget;
    }
    std::size_t FeatureArrayStorageBytes() const noexcept {
        return featureArrays_ == nullptr ? 0u : sizeof(EngineServiceFeatureArrays);
    }
    bool PrepareFeatureArrays() noexcept {
        if (featureArrays_ != nullptr) return true;
        if (memguard::IsAudioTagged()) return false;
        try {
            detail::LogicAllocCheckpoint(detail::LogicAllocSite::PlayableServiceFeatures);
            featureArrays_ = std::make_unique<EngineServiceFeatureArrays>();
        } catch (...) { return false; }
        BindFeatureArrays();
        return true;
    }
    void ClearFeatureArrays() noexcept {
        selectionDeltas.Clear();
        selectedChildOrdinals.Clear();
        variableControlChildInstances.Clear();
        variableControlChildKinds.Clear();
        variableControlChildEpochs.Clear();
        variableControlLeafInstances.Clear();
        variableControlLeafEpochs.Clear();
        variableControlContinuousLogicalOrdinals.Clear();
        variableControlContinuousLogicalCompletionFrames.Clear();
        variableControlContinuousLogicalCompleted.Clear();
        containerInstances.Clear();
        selectionPath.Clear();
        if (featureArrays_ != nullptr) {
            featureArrays_->queueFiling = EngineQueueFutureFilingLayout{};
            featureArrays_->variableControlPropertyRoot = {};
            featureArrays_->initialLaneDiscreteGroup = {};
            featureArrays_->initialLaneContinuousGroup = {};
            featureArrays_->variableControlDiscreteGroupCancellation = {};
            featureArrays_->variableControlContinuousGroupCancellation = {};
            featureArrays_->variableControlRetargetIncoming = {};
            featureArrays_->variableControlDiscreteRetargetSlice = {};
            featureArrays_->variableControlRetargetSlice = {};
            featureArrays_->variableControlDiscreteDeferredSupersedeSlice = {};
            featureArrays_->variableControlDeferredSupersedeSlice = {};
            featureArrays_->queueParentCheckpointKey = {};
            featureArrays_->queueCheckpointKey = {};
            featureArrays_->queueParentRandomSeed = {};
            featureArrays_->queueRandomSeed = {};
            featureArrays_->queueDeferredSource = {};
            featureArrays_->queueSuccessorLeaf = {};
            featureArrays_->canonicalInitialQueueRequest = {};
            featureArrays_->transitionSourceFadeCurve = {};
            featureArrays_->transitionTargetFadeCurve = {};
            featureArrays_->queueParentInitialState = {};
            featureArrays_->queueParentResetInitialState = {};
            featureArrays_->queueNextState = {};
            featureArrays_->variableControlWorld = {};
            featureArrays_->queueWorld = {};
            featureArrays_->containerLoopWorld = {};
            featureArrays_->variableControlDiscreteRetargetRuntimeProgram = {};
            featureArrays_->variableControlDiscreteRetargetEnteringRuntimeProgram = {};
            featureArrays_->variableControlDiscreteRetargetSchedule = {};
            featureArrays_->variableControlDiscreteRetargetEnteringSchedule = {};
            featureArrays_->variablePayload = {};
            featureArrays_->queuePayload = {};
        }
    }
    void BindFeatureArrays() noexcept {
        if (featureArrays_ == nullptr) return;
        selectionDeltas.Bind(featureArrays_->selectionDeltas);
        selectedChildOrdinals.Bind(featureArrays_->selectedChildOrdinals);
        variableControlChildInstances.Bind(featureArrays_->variableControlChildInstances);
        variableControlChildKinds.Bind(featureArrays_->variableControlChildKinds);
        variableControlChildEpochs.Bind(featureArrays_->variableControlChildEpochs);
        variableControlLeafInstances.Bind(featureArrays_->variableControlLeafInstances);
        variableControlLeafEpochs.Bind(featureArrays_->variableControlLeafEpochs);
        variableControlContinuousLogicalOrdinals.Bind(featureArrays_->variableControlContinuousLogicalOrdinals);
        variableControlContinuousLogicalCompletionFrames.Bind(featureArrays_->variableControlContinuousLogicalCompletionFrames);
        variableControlContinuousLogicalCompleted.Bind(featureArrays_->variableControlContinuousLogicalCompleted);
        VariablePayload().variableControlRetargetLogicalOrdinals.Bind(featureArrays_->variableControlRetargetLogicalOrdinals);
        VariablePayload().variableControlRetargetLogicalCompletionFrames.Bind(featureArrays_->variableControlRetargetLogicalCompletionFrames);
        VariablePayload().variableControlRetargetLogicalCompleted.Bind(featureArrays_->variableControlRetargetLogicalCompleted);
        QueuePayload().queueInitialCurrentMemberIndices.Bind(featureArrays_->queueInitialCurrentMemberIndices);
        QueuePayload().queueInitialCurrentDelayedArms.Bind(featureArrays_->queueInitialCurrentDelayedArms);
        QueueFiling().queueTransitionGroupArms.Bind(featureArrays_->queueTransitionGroupArms);
        containerInstances.Bind(featureArrays_->containerInstances);
        selectionPath.Bind(featureArrays_->selectionPath);
    }

    void Reset() noexcept {
        const auto serviceIdentity = identity;
        const bool exhausted = quarantined;
        assert(!cohortLinked_);
        assert(memberRelationHead == UINT32_MAX && memberRelationCount == 0);
        assert(std::none_of(memberPrograms.begin(), memberPrograms.end(),
            [](const auto& token) noexcept { return token.Valid(); }));
        auto programDirectory = std::move(memberPrograms);
        assert(std::none_of(members.begin(), members.end(),
            [](const auto& token) noexcept { return token.Valid(); }));
        auto memberDirectory = std::move(members);
        ClearFeatureArrays();
        if (featureArrays_ != nullptr) {
            featureArrays_->discreteRetarget = {};
            featureArrays_->queueRuntimeViews = {};
        }
        auto featureArrays = std::move(featureArrays_);
        // Reinitialize the stable row in place; a full temporary overflows the
        // producer stack when nested preparation rolls back at capacity.
        std::destroy_at(this);
        std::construct_at(this);
        identity = serviceIdentity;
        quarantined = exhausted;
        memberPrograms = std::move(programDirectory);
        members = std::move(memberDirectory);
        featureArrays_ = std::move(featureArrays);
        BindFeatureArrays();
    }
private:
    std::unique_ptr<EngineServiceFeatureArrays> featureArrays_;
};

struct EngineTimelineOccurrence {
    ProgramView schedule{};
    std::vector<EventTemplate> events;
    CapturedTimelineVoiceInstall voice;
    PlayableInstanceHandle ownerInstance = 0;
    OccurrenceToken occurrenceToken{};
    PlayableProgramKey completionProgram{};
    CompletionPredictability completionPredictability =
        CompletionPredictability::SignalOnly;
    KnownCompletionPredicateV1 completionPredicate{};
    TimelineArmToken arm{};
    EngineTimelineOccurrencePhase phase =
        EngineTimelineOccurrencePhase::Unprepared;
    std::uint32_t prepareCount = 0;
    bool forceReserveRetryForTest = false;
};

enum class TimelinePhysicalAdmissionFault : std::uint8_t {
    None = 0,
    DirectVirtual = 1,
    Parked = 2,
    PendingSteal = 3,
};

struct EngineTimelineProgramSet {
    VoiceLedger::BackingPreparation ledgerBacking;
    VoiceLedger::MonitorSelectionBacking monitorBacking;
    TimelineProgramSet programs;
    std::vector<EngineTimelineOccurrence> occurrences;
    ProgramGeneration generation = 0;
    std::uint64_t supersededAtAudioSequence = 0;
};

struct Engine;

struct EngineQueueContinuationRecord {
    PreparedDecisionReservation coordinatorReservation{};
    DecisionToken coordinator{};
    ArmGroupToken coordinatorArmGroup{};
    bool coordinatorArmGroupPrepared = false;
    bool abortRetireCommittedCoordinator = false;
    Plan03GroupSliceIdentity primaryGroup{};
    Plan03GroupSliceIdentity prerequisiteMember{};
    Plan03GroupSliceIdentity conditionalMember{};
    Plan03GroupSliceIdentity targetMember{};
    TransitionInstanceToken sourceTransitionBinding{};
    Plan03GroupReaderPin prerequisiteMemberPin{};
    FrontierActivationDescriptor descriptor{};
    EnginePlayableServiceToken service{};
    QueueInstanceToken queue{};
    ConditionalQueueVersionToken version{};
    DecisionToken memberDecision{};
    DecisionToken initialDecision{};
    PlayableInstanceHandle ownerInstance = 0;
    std::uint64_t ownerEpoch = 0;
    ActionFiringHandle root = 0;
    std::uint64_t attemptGeneration = 0;
    std::uint64_t generation = 0;
    std::uint32_t next = UINT32_MAX;
    bool failed = false;
    bool occupied = false;
};

struct EngineQueueFutureRecord {
    PreparedDecisionReservation coordinatorReservation{};
    DecisionToken coordinator{};
    ArmGroupToken coordinatorArmGroup{};
    bool coordinatorArmGroupPrepared = false;
    Plan03GroupSliceIdentity primaryGroup{};
    Plan03GroupSliceIdentity prerequisiteMember{};
    Plan03GroupSliceIdentity sourceMember{};
    Plan03GroupSliceIdentity targetMember{};
    Plan03GroupReaderPin prerequisiteMemberPin{};
    DecisionFuturePin parentPin{};
    FrontierEntryReceiptRef parentEntry{};
    FrontierActivationDescriptor descriptor{};
    EnginePlayableServiceToken service{};
    QueueInstanceToken queue{};
    DecisionToken memberDecision{};
    DecisionToken initialDecision{};
    TransitionInstanceToken sourceTransitionBinding{};
    FrontierEntryReceiptRef bindingArmPinEntry{};
    std::uint8_t exitedFutureBindings = 0;
    PlayableInstanceHandle ownerInstance = 0;
    std::uint64_t ownerEpoch = 0;
    ActionFiringHandle root = 0;
    std::uint64_t attemptGeneration = 0;
    std::uint64_t generation = 0;
    std::uint32_t next = UINT32_MAX;
    bool occupied = false;
};

// Synchronous aliases to the actual record; this view owns no lifetime or publication rights.
struct EngineQueueLaneRecordView {
    explicit EngineQueueLaneRecordView(EngineQueueContinuationRecord& record) noexcept
        : EngineQueueLaneRecordView(record, record.conditionalMember) { continuation = &record; }
    explicit EngineQueueLaneRecordView(EngineQueueFutureRecord& record) noexcept
        : EngineQueueLaneRecordView(record, record.sourceMember) { future = &record; }
    PreparedDecisionReservation& coordinatorReservation;
    DecisionToken& coordinator;
    ArmGroupToken& coordinatorArmGroup;
    bool& coordinatorArmGroupPrepared;
    Plan03GroupSliceIdentity& primaryGroup;
    Plan03GroupSliceIdentity& prerequisiteMember;
    Plan03GroupSliceIdentity& conditionalMember;
    Plan03GroupSliceIdentity& targetMember;
    TransitionInstanceToken& sourceTransitionBinding;
    Plan03GroupReaderPin& prerequisiteMemberPin;
    FrontierActivationDescriptor& descriptor;
    EnginePlayableServiceToken& service;
    QueueInstanceToken& queue;
    DecisionToken& memberDecision;
    DecisionToken& initialDecision;
    PlayableInstanceHandle& ownerInstance;
    std::uint64_t& ownerEpoch;
    ActionFiringHandle& root;
    std::uint64_t& attemptGeneration;
    std::uint64_t& generation;
    std::uint32_t& next;
    bool& occupied;
    EngineQueueContinuationRecord* continuation = nullptr;
    EngineQueueFutureRecord* future = nullptr;
private:
    template<class Record>
    EngineQueueLaneRecordView(Record& record, Plan03GroupSliceIdentity& source) noexcept
        : coordinatorReservation(record.coordinatorReservation), coordinator(record.coordinator),
          coordinatorArmGroup(record.coordinatorArmGroup), coordinatorArmGroupPrepared(record.coordinatorArmGroupPrepared),
          primaryGroup(record.primaryGroup), prerequisiteMember(record.prerequisiteMember), conditionalMember(source),
          targetMember(record.targetMember), sourceTransitionBinding(record.sourceTransitionBinding),
          prerequisiteMemberPin(record.prerequisiteMemberPin), descriptor(record.descriptor), service(record.service),
          queue(record.queue), memberDecision(record.memberDecision), initialDecision(record.initialDecision),
          ownerInstance(record.ownerInstance), ownerEpoch(record.ownerEpoch), root(record.root),
          attemptGeneration(record.attemptGeneration), generation(record.generation), next(record.next), occupied(record.occupied) {}
};

enum class InitialQueuePrivateCleanupPhase : std::uint8_t {
    None, Prove, Members, LanePins, RootReaders, Bindings, Coordinators,
    StopQueued, VoiceReceipt, Activations, Finished
};

struct EnginePrivateInitialQueueVoice {
    VnVoiceHandle render = 0;
    PlayableInstanceHandle rootInstance = 0;
    PlayableInstanceHandle leafInstance = 0;
    TimelineArmToken startArm{};
    VoiceReleaseObserver release{};
    SourceTerminalToken source{};
    EnginePlayableServiceToken service{};
    std::uint64_t serviceEpoch = 0;
    std::uint64_t capturedAudioSequence = 0;
    std::uint64_t stopQueuedAudioSequence = 0;
    bool stopQueued = false;
    bool armCancelRequested = false;
    bool rootReaderReleased = false;
    bool rootArmComplete = false;
};

struct EngineFrontierAttemptToken {
    std::uint32_t slot = UINT32_MAX;
    std::uint64_t generation = 0;
    bool Valid() const noexcept { return slot != UINT32_MAX && generation != 0; }
};

struct EngineTimelineVoiceInstallIdentity {
    VnVoiceHandle renderHandle = 0;
    PlayableInstanceHandle rootInstance = 0;
    TimelineSemanticTarget target{};
    VoiceReleaseObserver releaseObserver{};
    std::uint64_t onsetFrame = 0;
    TimelineArmToken startArm{};
    std::uint32_t busIndex = 0;
    std::uint32_t busGeneration = 0;
    std::array<VoiceSendConfig, kMaxVoiceSendSlots> voiceSends{};
};

struct EnginePlainImmediateSource {
    EnginePlayableServiceToken service{};
    EnginePlayablePartitionToken partition{};
    std::uint64_t invocationGeneration = 0;
    PlayableInstanceHandle owner = 0;
    std::uint64_t ownerEpoch = 0;
    ProgramView* schedule = nullptr;
    const OriginalSourceSubmissionRegistry::Record* submission = nullptr;
    EngineTimelineVoiceInstallIdentity installation{};
    ImmediateLaneEvent original{};
    TimelineArmReaderPin bindingReader{};
    std::uint64_t chosenOnset = 0;
    bool bound = false;
    bool armPublished = false;
    bool referencesReturned = false;
};

struct EnginePlainImmediateStopRef {
    std::uint32_t slot = UINT32_MAX;
    std::uint64_t generation = 0;
};

// Same-call native coordination receipt for two distinct prepared roots.
// It grants no Current, Source, group lifetime or catalog mutation capability.
class InitialSoundQueueCohortReceipt {
public:
    InitialSoundQueueCohortReceipt() noexcept = default;
    InitialSoundQueueCohortReceipt(const InitialSoundQueueCohortReceipt&) = delete;
    InitialSoundQueueCohortReceipt& operator=(const InitialSoundQueueCohortReceipt&) = delete;
private:
    friend struct Engine;
    struct Participant {
        EnginePlayableServiceToken service{};
        EnginePlayablePartitionToken partition{};
        std::uint64_t invocationGeneration = 0;
        FrontierOwnerIdentity owner{};
    } sound_{}, queue_{};
    std::array<OriginalQueueFutureGroupRegistration*, 2> primaries_{};
    const Engine* receiver_ = nullptr;
    ActionFiringHandle root_ = 0;
    std::uint64_t generation_ = 0;
};

// An enduring factory certificate, never a copied Current or late lookup.
// This fixed record owns no second standing native loan. Its original Lane
// baseline and every real foreign Lane loan protect this exact attempt storage.
class OriginalPreparedInitialSingleQueueOrigin;
class OriginalInitialC0FactoryRecord {
public:
    OriginalInitialC0FactoryRecord() noexcept = default;
    OriginalInitialC0FactoryRecord(const OriginalInitialC0FactoryRecord&) = delete;
    OriginalInitialC0FactoryRecord& operator=(const OriginalInitialC0FactoryRecord&) = delete;
    OriginalInitialC0FactoryRecord(OriginalInitialC0FactoryRecord&&) = delete;
    OriginalInitialC0FactoryRecord& operator=(OriginalInitialC0FactoryRecord&&) = delete;
private:
    friend struct Engine;
    friend class OriginalPreparedInitialSingleQueueOrigin;
    enum class Phase : std::uint8_t { Empty, Staging, Prepared, Bound };
    struct BoundGeometry {
        FrontierEntryReceiptRef entry{};
        std::uint64_t anchor = 0, initial = 0, source = 0, ack = 0;
    } binding_{};
    Phase phase_ = Phase::Empty;
    const Engine* receiver_ = nullptr;
    ActionFiringHandle root_ = 0;
    std::uint64_t attemptGeneration_ = 0, marker_ = 0;
    ImmediateLaneHandle lane_{};
    std::array<OriginalQueueFutureGroupRegistration*, 4> originals_{};
    std::array<Plan03GroupSliceIdentity, 4> groups_{};
    std::array<Plan03GroupSliceInput, 4> reservedInputs_{};
    std::uint64_t provisionalAnchor_ = 0;
    QueueInstanceToken queue_{};
    FrontierQueueInitialIdentity initial_{};
    FrontierQueueConditionalIdentity conditional_{};
    LogicCommitSequence issueSequence_ = 0;
    WorldGeneration world_ = 0;
    TransportEpoch transport_ = 0;
    FrontierOwnerIdentity admissionOwner_{}, queueOwner_{};
    EnginePlayableServiceToken service_{};
    EnginePlayablePartitionToken partition_{};
    std::uint64_t invocationGeneration_ = 0;
    PlayableInstanceHandle queueRoot_ = 0;
    PreparedSingleCurrentOriginBundle* current_ = nullptr;
    EngineQueueProgramBank* bank_ = nullptr;
    std::uint64_t bankGeneration_ = 0;
    TimelineArmToken sourceArm_{};
    VnVoiceHandle sourceIdentity_ = 0;
    CompletionOwnerRef completionOwner_{};
    KnownCompletionPredicateV1 predicate_{};
    DecisionToken currentDecision_{};
    OccurrenceToken currentOccurrence_{};
    TimelineTransactionProgramRef sourceRuntimeProgram_{};
    ProgramGeneration sourceProgramGeneration_ = 0;
    TimelineSemanticTarget sourceTarget_{};
    ImmediateLaneTimeDomain sourceDomain_ = ImmediateLaneTimeDomain::RelativeToAnchor;
    std::uint64_t sourceOffset_ = 0, sourceFixedBase_ = 0;
    void ClearCertificateAfterNativeDisposition() noexcept {
        phase_ = Phase::Empty; receiver_ = nullptr; root_ = 0;
        attemptGeneration_ = 0; marker_ = 0; lane_ = {};
        originals_ = {}; groups_ = {}; reservedInputs_ = {}; provisionalAnchor_ = 0;
        queue_ = {}; initial_ = {}; conditional_ = {}; issueSequence_ = 0; world_ = 0; transport_ = 0;
        admissionOwner_ = {}; queueOwner_ = {}; service_ = {}; partition_ = {};
        invocationGeneration_ = 0; queueRoot_ = 0; current_ = nullptr;
        bank_ = nullptr; bankGeneration_ = 0; sourceArm_ = {}; sourceIdentity_ = 0;
        completionOwner_ = {}; predicate_ = {}; currentDecision_ = {}; currentOccurrence_ = {};
        sourceRuntimeProgram_ = {}; sourceProgramGeneration_ = 0; sourceTarget_ = {};
        sourceDomain_ = ImmediateLaneTimeDomain::RelativeToAnchor;
        sourceOffset_ = 0; sourceFixedBase_ = 0; binding_ = {};
    }
};

// Serialized logic-producer structural loan. Borrow cannot bind a clock,
// withdraw C1, select a future, read PCM or mutate the retained Current.
class OriginalPreparedInitialSingleQueueOrigin {
public:
    OriginalPreparedInitialSingleQueueOrigin() noexcept = default;
    OriginalPreparedInitialSingleQueueOrigin(const OriginalPreparedInitialSingleQueueOrigin&) = delete;
    OriginalPreparedInitialSingleQueueOrigin& operator=(const OriginalPreparedInitialSingleQueueOrigin&) = delete;
    OriginalPreparedInitialSingleQueueOrigin(OriginalPreparedInitialSingleQueueOrigin&& other) noexcept { Take(other); }
    OriginalPreparedInitialSingleQueueOrigin& operator=(OriginalPreparedInitialSingleQueueOrigin&& other) noexcept {
        if (this != &other) { Return(); Take(other); }
        return *this;
    }
    ~OriginalPreparedInitialSingleQueueOrigin() { Return(); }
    bool Valid() const noexcept {
        return record_ != nullptr && receiver_ != nullptr && marker_ != 0 &&
            record_->receiver_ == receiver_ && record_->marker_ == marker_ &&
            record_->root_ == root_ && record_->attemptGeneration_ == generation_ &&
            record_->lane_.slot == laneIdentity_.slot && record_->lane_.generation == laneIdentity_.generation &&
            laneReceiver_ != nullptr && laneReceiver_->IsOriginalAttemptLifetimePinFor(lane_, laneIdentity_, root_) &&
            pool_ != nullptr && groups_[0].Valid() && groups_[1].Valid() &&
            groups_[0].Identity() == record_->groups_[0] && groups_[1].Identity() == record_->groups_[1] &&
            queueReceiver_ != nullptr && queueReceiver_->IsOriginalLifetimePinFor(parent_, record_->queue_) &&
            bank_.Valid() && bank_.Generation() == record_->bankGeneration_ &&
            current_.Valid() && current_.HasOriginalPlayableRetention();
    }
    bool Borrow(OriginalPreparedInitialSingleQueueOrigin* out) const noexcept {
        if (out == nullptr || out == this || out->record_ != nullptr || !Valid()) return false;
        OriginalPreparedInitialSingleQueueOrigin loan;
        if (!lane_.Borrow(&loan.lane_) || !groups_[0].Borrow(&loan.groups_[0]) ||
            !groups_[1].Borrow(&loan.groups_[1]) || !parent_.Borrow(&loan.parent_) ||
            !bank_.Borrow(&loan.bank_) || !current_.Borrow(&loan.current_)) return false;
        loan.CopyCertificate(*this);
        *out = std::move(loan);
        return true;
    }
    bool ObserveInstalledCurrent() const noexcept { return Valid() && current_.ObserveInstalled(); }
    bool CaptureInitialBinding(FrontierEntryReceiptRef* entry, std::array<std::uint64_t, 4>* frames) const noexcept {
        if (entry == nullptr || frames == nullptr || !Valid() ||
            record_->phase_ != OriginalInitialC0FactoryRecord::Phase::Bound) return false;
        *entry = record_->binding_.entry;
        *frames = {record_->binding_.anchor, record_->binding_.initial, record_->binding_.source, record_->binding_.ack};
        return true;
    }
    void Return() noexcept {
        // Drop all pointer/certificate access before returning the final native
        // Lane loan which permits the actual fixed attempt to be retired/reused.
        record_ = nullptr; receiver_ = nullptr; marker_ = 0; root_ = 0; generation_ = 0;
        laneIdentity_ = {}; laneReceiver_ = nullptr; pool_ = nullptr; queueReceiver_ = nullptr;
        current_.Release(); bank_.Release(); parent_.Release();
        groups_[1].Release(); groups_[0].Release(); lane_.Return();
    }
private:
    friend struct Engine;
    void CopyCertificate(const OriginalPreparedInitialSingleQueueOrigin& other) noexcept {
        record_ = other.record_; receiver_ = other.receiver_; marker_ = other.marker_;
        root_ = other.root_; generation_ = other.generation_; laneIdentity_ = other.laneIdentity_;
        laneReceiver_ = other.laneReceiver_; pool_ = other.pool_; queueReceiver_ = other.queueReceiver_;
    }
    void Take(OriginalPreparedInitialSingleQueueOrigin& other) noexcept {
        CopyCertificate(other);
        lane_ = std::move(other.lane_); groups_ = std::move(other.groups_);
        parent_ = std::move(other.parent_); bank_ = std::move(other.bank_); current_ = std::move(other.current_);
        other.record_ = nullptr; other.receiver_ = nullptr; other.marker_ = 0;
        other.root_ = 0; other.generation_ = 0; other.laneIdentity_ = {};
        other.laneReceiver_ = nullptr; other.pool_ = nullptr; other.queueReceiver_ = nullptr;
    }
    const OriginalInitialC0FactoryRecord* record_ = nullptr;
    const Engine* receiver_ = nullptr;
    ImmediateActionLane* laneReceiver_ = nullptr;
    ScheduledTransactionPool* pool_ = nullptr;
    QueueInstanceTable* queueReceiver_ = nullptr;
    std::uint64_t marker_ = 0, root_ = 0, generation_ = 0;
    ImmediateLaneHandle laneIdentity_{};
    OriginalImmediateLaneAttemptLifetimePin lane_{};
    std::array<OriginalQueueFutureGroupLifetimePin, 2> groups_{};
    QueueInstanceTable::OriginalLifetimePin parent_{};
    EngineQueueProgramBank::OriginalLifetimePin bank_{};
    PreparedSingleCurrentOriginBundle current_{};
};

struct EngineInitialSelectionProducer {
    EngineFrontierAttemptToken token{};
    EnginePlayableServiceReference service{};
    SelectionPendingBatchIdentityV1 identity{};
    ActionFiringHandle firing = 0;
    std::uint64_t executeFrame = 0;
    std::uint32_t executionOrdinal = 0;
    bool variableControl = false;
    bool returned = false;
};

struct EngineSelectionPublicationCell {
    EnginePendingSelectionBatch* owner = nullptr;
    EngineActionPlayStorage* cutStorage = nullptr;
    VariableReadCutRef cut{};
};

struct EngineImmediateEntryStorageDemand {
    std::uint32_t selectionProducers = 0;
    std::uint32_t plainSources = 0;
    std::uint32_t plainStops = 0;
    std::uint32_t privateVoices = 0;
    bool queueCoordinators = false;
};

struct EngineImmediateQueueCoordinators {
    PreparedDecisionReservation initialQueueCoordinatorReservation{};
    DecisionToken initialQueueCoordinator{};
    ArmGroupToken initialQueueCoordinatorArmGroup{};
    bool initialQueueCoordinatorArmGroupPrepared = false;
    Plan03GroupSliceIdentity initialQueuePrimaryGroup{};
    FrontierActivationDescriptor initialQueueDescriptor{};
    PreparedDecisionReservation queueContinuationCoordinatorReservation{};
    DecisionToken queueContinuationCoordinator{};
    ArmGroupToken queueContinuationCoordinatorArmGroup{};
    bool queueContinuationCoordinatorArmGroupPrepared = false;
    Plan03GroupSliceIdentity queueContinuationPrimaryGroup{};
    FrontierActivationDescriptor queueContinuationDescriptor{};
};

struct EngineImmediateEntryAttempt {
    ImmediateLaneHandle lane{};
    // Only the same native Reserve can fill this fixed baseline. It is not
    // an audio admission gate and is consumed only by genuine final Retire.
    OriginalImmediateLaneAttemptLifetimePin originalLaneLifetime;
    ActionFiringHandle originalLaneFactoryRoot = 0;
    std::uint64_t originalLaneFactoryAttemptGeneration = 0;
    ImmediateLaneDemand originalLaneDemand{};
    std::unique_ptr<OriginalInitialC0FactoryRecord> originalC0Factory;
    ImmediateEntryAttemptGate gate{};

    const EngineImmediateQueueCoordinators& QueueCoordinators() const noexcept {
        static constexpr EngineImmediateQueueCoordinators empty{};
        return queueCoordinators_ == nullptr ? empty : *queueCoordinators_;
    }
    EngineImmediateQueueCoordinators& PreparedQueueCoordinators() noexcept {
        assert(queueCoordinators_ != nullptr);
        return *queueCoordinators_;
    }
    void ClearQueuePrimaryGroups() noexcept {
        if (queueCoordinators_ == nullptr) return;
        queueCoordinators_->initialQueuePrimaryGroup = {};
        queueCoordinators_->queueContinuationPrimaryGroup = {};
    }
    bool HasQueueCoordinators() const noexcept { return queueCoordinators_ != nullptr; }
    EnginePlayableServiceToken queueContinuationService{};
    QueueInstanceToken queueContinuationQueue{};
    ConditionalQueueVersionToken queueContinuationVersion{};
    DecisionToken queueContinuationMemberDecision{};
    bool queueContinuationFailed = false;
    ActionTerminalReason laneFailureReason = ActionTerminalReason::None;
    std::uint32_t queueContinuationHead = UINT32_MAX;
    std::uint32_t queueContinuationCount = 0;
    std::uint32_t queueFutureHead = UINT32_MAX;
    std::uint32_t queueFutureCount = 0;
    std::uint32_t producerSlot = UINT32_MAX;
    std::uint64_t producerGeneration = 0;
    EngineFrontierAttemptToken privateVcProducerHead{};
    std::uint32_t privateVcProducerCount = 0;
    bool privateVcPoolPublished = false;
    // These cells locate the unique batch retained by its ordinary/VC holder.
    // Their identity survives Lane retirement until every original owner exits.
    std::vector<EngineInitialSelectionProducer> selectionProducers;
    std::vector<PendingSelectionBatchCommitV1> selectionCommitInputs;
    std::vector<EngineSelectionPublicationCell> selectionPublicationCells;
    ActionFiringHandle selectionCensusRoot = 0;
    std::uint64_t selectionCensusGeneration = 0;
    std::uint32_t selectionProducerCount = 0;
    std::uint64_t generation = 0;
    ActionFiringHandle root = 0;
    EnginePlayableServiceToken initialService{};
    std::uint64_t initialInvocationGeneration = 0;
    InitialSoundQueueCohortReceipt soundQueueCohort{};
    std::uint64_t initialEntryFrame = 0;
    std::uint64_t entryQuantum = 0;
    std::uint64_t lastQuantum = 0;
    std::uint32_t entryEventCount = 0;
    PreparedTransactionBatch entryTransactions{};
    PreparedTransactionBatch futureTransactions{};
    bool producerAbortPending = false;
    bool producerAbortFullyPrivate = false;
    bool producerAbortRetiredSources = false;
    bool producerAbortPartialArmPublished = false;
    bool partialInitialActivationRemoved = false;
    bool producerAbortVoicesCaptured = false;
    bool producerAbortPrivateTailReady = false;
    bool producerAbortMembersMarked = false;
    bool producerAbortClosing = false;
    bool producerAbortPendingVariableStop = false;
    bool producerExposureReleased = false;
    bool producerFailureCapacity = false;
    bool producerAbortMirrorPreActivation = false;
    bool producerAbortMirrorProved = false;
    bool producerAbortMirrorHoldersReleased = false;
    bool producerAbortMirrorGroupsCancelled = false;
    InitialQueuePrivateCleanupPhase privateCleanupPhase =
        InitialQueuePrivateCleanupPhase::None;
    bool privateInitialBindingsReleased = false;
    bool privateContinuationBindingsReleased = false;
    bool privateInitialCoordinatorRolledBack = false;
    bool privateContinuationCoordinatorRolledBack = false;
    bool privateInitialActivationCancelled = false;
    bool privateContinuationActivationCancelled = false;
    std::vector<EnginePlainImmediateSource> plainSources;
    std::vector<EnginePlainImmediateStopRef> plainStops;
    PrivateReservedPlainLayoutCommitProof plainLayout{};
    std::uint32_t plainSourceCount = 0;
    std::uint32_t plainStopCount = 0;
    std::uint64_t plainAnchor = 0;
    std::uint64_t plainFirstDue = 0;
    bool plainSelected = false;
    bool plainGeometryChosen = false;
    bool plainLayoutConsumed = false;
    bool plainArmsPinned = false;
    bool plainPublished = false;
    bool plainAbortPending = false;
    MonitorControlCause plainCancellationCause{};
    std::uint8_t plainCancellationIntent = 0;
    std::uint32_t plainCancellationFadeMs = 0;
    bool plainReferencesReturned = false;
    std::vector<EnginePrivateInitialQueueVoice> privateVoices;
    std::uint32_t privateVoiceCount = 0;
    bool active = false;
private:
    friend struct Engine;
    // Prepared before Lane reservation; retain both records at stable addresses
    // through cancellation, receipt readers and reuse of this attempt.
    std::unique_ptr<EngineImmediateQueueCoordinators> queueCoordinators_;
};
bool ResumePendingPlayableCancellation(Engine&, EnginePlayableServiceRow&) noexcept;

struct PlayableStorageLayout {
    explicit PlayableStorageLayout(const EngineConfig& config)
        : knownLeaves(config.maxKnownScheduleLeaves),
          liveRows(config.maxVariableControlLiveRows),
          memberStride(CheckedStride(2u, knownLeaves, 1u, liveRows)),
          pairedStride(CheckedStride(2u, knownLeaves)),
          currentEventStride(std::max(
              CheckedStride(2u, knownLeaves),
              CheckedStride(1u, knownLeaves, 1u, liveRows))),
          initialProgramStride(
              CheckedStride(1u, knownLeaves, 1u, liveRows)),
          endpointStride(knownLeaves),
          continuousStride(liveRows),
          memberCount(CheckedCount(config.maxPlayableInstances,
                                   memberStride)),
          pairedCount(CheckedCount(config.maxPlayableInstances,
                                   pairedStride)),
          currentEventCount(CheckedCount(config.maxPlayableInstances,
                                         currentEventStride)),
          initialProgramCount(CheckedCount(config.maxPlayableInstances,
                                           initialProgramStride)),
          endpointCount(CheckedCount(config.maxPlayableInstances,
                                     endpointStride)),
          continuousCount(CheckedCount(config.maxPlayableInstances,
                                       continuousStride)) {}

    std::size_t MemberBase(std::size_t rowIndex) const noexcept {
        return rowIndex * memberStride;
    }
    std::size_t PairedBase(std::size_t rowIndex) const noexcept {
        return rowIndex * pairedStride;
    }
    std::size_t CurrentEventBase(std::size_t rowIndex) const noexcept {
        return rowIndex * currentEventStride;
    }
    std::size_t InitialProgramBase(std::size_t rowIndex) const noexcept {
        return rowIndex * initialProgramStride;
    }
    std::size_t EndpointBase(std::size_t rowIndex) const noexcept {
        return rowIndex * endpointStride;
    }
    std::size_t ContinuousBase(std::size_t rowIndex) const noexcept {
        return rowIndex * continuousStride;
    }
    std::size_t PackedDiscreteTransitionOffset(
        std::size_t memberOffset) const noexcept {
        return knownLeaves +
            (memberOffset - (knownLeaves + liveRows));
    }

    const std::uint32_t knownLeaves;
    const std::uint32_t liveRows;
    const std::uint32_t memberStride;
    const std::uint32_t pairedStride;
    const std::uint32_t currentEventStride;
    const std::uint32_t initialProgramStride;
    const std::uint32_t endpointStride;
    const std::uint32_t continuousStride;
    const std::size_t memberCount;
    const std::size_t pairedCount;
    const std::size_t currentEventCount;
    const std::size_t initialProgramCount;
    const std::size_t endpointCount;
    const std::size_t continuousCount;

private:
    static std::uint32_t CheckedStride(
        std::uint32_t firstScale, std::uint32_t first,
        std::uint32_t secondScale = 0,
        std::uint32_t second = 0) {
        const std::uint64_t value =
            static_cast<std::uint64_t>(firstScale) * first +
            static_cast<std::uint64_t>(secondScale) * second;
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            throw std::bad_alloc{};
        }
        return static_cast<std::uint32_t>(value);
    }

    static std::size_t CheckedCount(std::uint32_t rows,
                                    std::uint32_t stride) {
        if (stride != 0 &&
            rows > std::numeric_limits<std::size_t>::max() / stride) {
            throw std::bad_alloc{};
        }
        return static_cast<std::size_t>(rows) * stride;
    }
};

class TimelineServiceCoordinator {
public:
    using AfterFreshPrepareHook = void (*)(void*);

    explicit TimelineServiceCoordinator(Engine& engine) : engine_(engine) {}

    void Service();
    void SetAfterFreshPrepareHookForTests(AfterFreshPrepareHook hook,
                                          void* context) {
        afterFreshPrepareHook_.store(hook, std::memory_order_release);
        afterFreshPrepareHookContext_.store(context, std::memory_order_release);
    }
    std::uint32_t MaterializationHighWater() const {
        return materializationHighWater_;
    }
    std::uint64_t MaterializationsPacedByBudget() const {
        return materializationsPacedByBudget_;
    }
    void SetDeferredMaterializationBudgetForTests(std::uint32_t budget) {
        deferredMaterializationBudgetForTests_ = budget;
    }

private:
    Engine& engine_;
    std::atomic<AfterFreshPrepareHook> afterFreshPrepareHook_{nullptr};
    std::atomic<void*> afterFreshPrepareHookContext_{nullptr};
    std::uint64_t budgetQuantumStamp_ = UINT64_MAX;
    std::uint64_t fixedPlayQuotaQuantum_ = UINT64_MAX;
    bool fixedPlayBeforeDeadlineUsed_ = false;
    bool fixedPlayExpiredUsed_ = false;
    std::uint32_t deferredMaterializationBudgetForTests_ = 0;
    std::uint32_t materializationsUsed_ = 0;
    std::uint32_t materializationHighWater_ = 0;
    std::uint64_t materializationsPacedByBudget_ = 0;
    bool budgetPacedForStamp_ = false;
};

struct Engine;

bool MakeVariableParamCommand(const Engine& engine,
                              const ParamCommit& commit,
                              CommandRecord* out);
bool MakeVariableEffectParamCommand(
    const Engine& engine, EffectParamTargetStore::Scope scope,
    std::uint64_t objectId, std::uint32_t slot,
    std::uint32_t paramIndex, float value, std::uint32_t effectType,
    CommandRecord* out, std::uint32_t transitionMs = 0,
    VariableToneBandValues* outToneBand = nullptr);
float VariableBusWireValue(const Engine& engine, std::uint32_t busIndex,
                           float resolvedDb, std::uint32_t targetId);

struct EngineActionFiringPlanRow {
    ActionProgramHandleV1 action{};
    ActionFiringOwnerStamp owner{};
    EngineOriginalPackIdentityLease originalPackLease;
    bool ownerLeaseHeld = false;
    std::uint32_t parentIndex = UINT32_MAX;
    std::uint32_t depth = 0;
    PreparedActionFiring prepared{};
    ActionFiringHandle handle = 0;
};

struct EngineImmediateLaneDemandQuantum {
    ImmediateLaneTimeDomain domain = ImmediateLaneTimeDomain::RelativeToAnchor;
    std::uint64_t quantum = 0;
    bool operator==(const EngineImmediateLaneDemandQuantum&) const = default;
};

struct EngineActionStepPlanRow {
    std::array<float, 64> initialVcSelectionValues{};
    BankVersionToken initialVcSelectionBank{};
    std::uint64_t initialVcSelectionWorldGeneration = 0;
    std::uint64_t initialVcSelectionFingerprint = 0;
    std::uint64_t initialVcSelectionOverlayFrame = 0;
    LogicCommitSequence initialVcSelectionSequence = 0;
    std::uint32_t initialVcSelectionExecutionOrdinal = 0;
    std::uint32_t initialVcSelectionValueCount = 0;
    bool initialVcSelectionCached = false;
    bool initialVcSelectionPackAuthority = false;
    const ActionStepProgramViewV1* step = nullptr;
    const ContainerProgramViewV3* container = nullptr;
    std::uint32_t firingIndex = UINT32_MAX;
    std::uint32_t childFiringIndex = UINT32_MAX;
    std::uint32_t executionOrdinal = 0;
    std::uint32_t depth = 0;
    std::uint32_t fadeFrames = 0;
    std::uint64_t quantizeBaseFrame = 0;
    std::uint64_t deferredFrame = 0;
    std::uint64_t capturedFadeStartFrame = 0;
    ActionCooldownToken cooldownToken{};
    bool deferred = false;
    bool stopHasFarLeaves = false;
    bool cooldownSuppressed = false;
    bool quantizeSuppressed = false;
};

struct EngineActionDeferredScratch {
    std::uint32_t plannedStepIndex = UINT32_MAX;
    PlayableInstanceHandle rootPlayable = 0;
    std::uint64_t rootScheduleEpoch = 0;
    std::uint64_t silenceFrame = 0;
    std::uint64_t fadeStartFrame = 0;
    ActionDeferredRecordToken recordToken{};
    DeferredIntentToken poolToken{};
    ActionDeferredRecordTable::PreparedPairCommitProof pairCommitProof;
    PackPartitionLease targetPackLease{};
    EngineOriginalPackIdentityLease originalPackLease;
    bool ownershipHeld = false;
};

// Only a successful native firing factory commit receives this acquired duty.
class EngineOriginalActionFiringPackHolder {
private:
    friend struct Engine;
    EngineOriginalPackIdentityLease original_;
    ActionFiringHandle firing_ = 0;
    ActionFiringHandle root_ = 0;
    ActionFiringOwnerStamp owner_{};
    bool returned_ = false;
};

// This holder receives the same acquired duty only at actual pair consumption.
// Record snapshots and target leases remain copyable, non-owning identities.
class EngineOriginalDeferredPackHolder {
private:
    friend struct Engine;
    EngineOriginalPackIdentityLease original_;
    ActionDeferredRecordToken record_{};
    DeferredIntentToken pool_{};
    PackPartitionLease lease_{};
    bool transferred_ = false;
    ActionFiringHandle transferredActionFiring_ = 0;
    bool returned_ = false;
    bool heldCancellationOwned_ = false;
    bool claimFinalizationPending_ = false;
};


struct EnginePreparedAuthoredStop {
    std::uint32_t operationSlot = UINT32_MAX;
    std::uint32_t firstResource = UINT32_MAX;
    std::uint32_t resourceCount = 0;
    bool ownsOperation = false;
};

struct EngineAuthoredStopRoot {
    EnginePreparedAuthoredStop retainedPreparation{};
    std::uint64_t generation = 0;
    std::optional<AuthoredStopOperation> operation;
    DelayedPlayableCancellation cancellation{};
    MonitorControlCause controlCause{};
    std::uint64_t fadeStartFrame = 0;
    std::uint32_t firstResource = UINT32_MAX;
    std::uint32_t resourceCount = 0;
    bool committed = false;
    bool applied = false;
    bool cancellationPending = false;
    bool detachObserverPending = false;
};

struct EngineAuthoredStopResource {
    std::uint64_t generation = 0;
    std::uint32_t operationSlot = UINT32_MAX;
    std::uint32_t nextResource = UINT32_MAX;
    struct VoiceIdentity {
        VnVoiceHandle renderHandle = 0;
        TimelineSemanticTarget target{};
        VoiceReleaseObserver releaseObserver{};
    } installation;
    TimelineArmToken arm{};
    std::array<TimelineEventPayload, 2> payloads{};
    std::array<ScheduledEventInput, 2> events{};
    TimelineProgramView runtime{};
    std::optional<AuthoredStopFenceDescriptor> fence;
    PreparedTransactionBatch transactions{};
    CommandQueue::OriginalBatchRange originalControlHeader{};
    // The same native factory owns this baseline; it is not a Catalog grant.
    // Declared after the range so it also returns first in member destruction.
    CommandQueue::OriginalControlOriginPin originalControlOrigin{};
    EnginePlayableServiceToken originalPlainService{};
    EnginePlayablePartitionToken originalPlainPartition{};
    std::uint64_t originalPlainInvocation = 0;
    PlayableInstanceHandle originalPlainOwner = 0;
    std::uint64_t originalPlainOwnerEpoch = 0;
    TimelineArmToken originalPlainSource{};
    const OriginalSourceSubmissionRegistry::Record* originalPlainSubmission = nullptr;
    ImmediateLaneHandle originalPlainLane{};
    ActionFiringHandle originalPlainCohort = 0;
    std::uint32_t originalPlainAttemptSlot = UINT32_MAX;
    std::uint64_t originalPlainAttemptGeneration = 0;
    std::uint32_t originalPlainEventCapacity = 0;
    QueueInstanceToken nativeQueueReplacement{};
    std::uint64_t nativeReplacementDomain = 0;
    OriginalImmediateLaneAttemptLifetimePin nativeStopLaneOwner;
    std::uint32_t nativeStopOwnerSlot = UINT32_MAX;
    std::uint64_t nativeStopOwnerGeneration = 0;
    TimelineArmTable::PreparedCommitProof nativeControlCommit;
    LogicCommitSequence nativeStopSequence = 0;
    bool nativeStopPublished = false;
    bool nativeStopLayoutPrepared = false, nativeStopPinsInstalled = false, nativeStopPlaced = false;
    bool originalPlainSelected = false;
    bool originalPlainBound = false;
    bool originalPlainArmPublished = false;
    bool originalPlainReferencesReturned = false;
    std::uint64_t dispatchFrame = 0;
    std::uint64_t retirementAudioSequence = 0;
    bool prepared = false;
    bool committed = false;
    bool retiring = false;
    bool retirementObserved = false;

    // Serialized producer cleanup only. Real header disposition precedes own
    // Return; foreign origins keep Reset busy and preserve the native scope.
    // Header completion does not assert Scheduled Stop or physical Source End.
    bool ReturnOriginalControlHeaderAfterDisposition() noexcept {
        const auto state = originalControlHeader.State();
        if (state == CommandQueue::OriginalRangeState::Prepared ||
            state == CommandQueue::OriginalRangeState::Published) return false;
        originalControlOrigin.Release();
        return originalControlHeader.ResetAfterDisposition();
    }
};

enum class EngineDecisionOwnershipPhase : std::uint8_t {
    Retiring,
    ActiveArrival,
};

struct EngineDecisionRetirement {
    DecisionToken decision{};
    ArmGroupToken group{};
    TransitionInstanceToken transition{};
    std::uint64_t retirementAudioSequence = 0;
    PlayableInstanceHandle rootInstance = 0;
    std::uint64_t rootScheduleEpoch = 0;
    EngineQueueProgramBank* programBank = nullptr;
    bool cancelled = false;
    bool canonicalQueue = false;
    EngineDecisionOwnershipPhase phase = EngineDecisionOwnershipPhase::Retiring;
    VoiceReleaseObserver currentRelease{};
};

static_assert(sizeof(EngineDecisionRetirement) <= 80u);
static_assert(kMaxDecisionRowsHard <= SIZE_MAX / sizeof(EngineDecisionRetirement));

class EngineOriginalQueueSourcePreparation {
public:
    EngineOriginalQueueSourcePreparation() noexcept = default;
    EngineOriginalQueueSourcePreparation(const EngineOriginalQueueSourcePreparation&) = delete;
    EngineOriginalQueueSourcePreparation& operator=(const EngineOriginalQueueSourcePreparation&) = delete;
    EngineOriginalQueueSourcePreparation(EngineOriginalQueueSourcePreparation&&) = delete;
    EngineOriginalQueueSourcePreparation& operator=(EngineOriginalQueueSourcePreparation&&) = delete;
    const PreparedSingleCurrentOriginBundle::SourcePin& OriginalSource() const noexcept { return source_; }
    const PreparedSingleCurrentOriginBundle::BodyPin& OriginalBody() const noexcept { return body_; }
    TimelineArmToken OriginalArm() const noexcept { return arm_; }
    const VoiceLedger::OriginalCompositeCompletionMutationOwner& OriginalCompositeLedger() const noexcept {
        return compositeLedgerOriginal_;
    }
    const VoiceLedger::PreparedCommitInput& OriginalCompositeLedgerInput() const noexcept {
        return compositeLedgerInput_;
    }
    VnVoiceHandle OriginalSourceIdentity() const noexcept { return sourceIdentity_; }
private:
    friend struct Engine;
    Engine* receiver_ = nullptr;
    TimelineArmToken arm_{};
    VnVoiceHandle sourceIdentity_ = 0;
    VoiceReleaseObserver physicalRelease_{};
    PreparedSingleCurrentOriginBundle::SourcePin source_;
    PreparedSingleCurrentOriginBundle::BodyPin body_;
    bool returned_ = false;
    VoiceLedger::PreparedCommitInput compositeLedgerInput_{};
    VoiceLedger::PreparedCommitProof compositeLedgerCommit_;
    VoiceLedger::OriginalCompositeCompletionMutationOwner compositeLedgerOriginal_;
    void ReturnAfterJoinedTerm() noexcept {
        compositeLedgerCommit_.Release();
        compositeLedgerOriginal_.Release();
        Return();
    }
    void Return() noexcept {
        body_.Release();
        source_.Release();
        returned_ = true;
    }
};

struct EnginePreparedTimelineVoiceStart {
    EngineMonitorSelection monitorSelection{};
    static constexpr std::uint32_t kMaxCommandCount =
        3u + kTimelineMaxVoiceSends + kTimelineMaxVoiceEffects + 2u +
        kMaxVoiceBindingInstanceSlots + kSpatialDrivenTargetsPerVoice;

    std::uint32_t ledgerSlot = UINT32_MAX;
    VnVoiceHandle renderHandle = 0;
    SourceTerminalToken sourceTerminal{};
    VoiceReleaseObserver releaseObserver{};
    TimelineArmToken arm{};
    EnginePreparedAuthoredStop inheritedStop{};
    VariableTimelineReadUseRef initialVariableReadUse{};
    TimelineArmTable::PreparedCommitInput armPreparation{};
    VoiceLedger::PreparedCommitInput ledgerPreparation{};
    VoicePool::PreparedCommitInput voicePreparation{};
    EngineOriginalQueueSourcePreparation* originalQueueSource = nullptr;
    bool inheritedStopCommitted = false;
    std::array<EffectInstanceRef, kTimelineMaxVoiceEffects> effects{};
    std::array<EffectObjectIdentity, kTimelineMaxVoiceEffects> effectIdentities{};
    std::uint32_t effectCount = 0;
    std::array<CommandRecord, kMaxCommandCount> commands{};
    std::uint32_t commandCount = 0;
    std::uint32_t postSourceTailFrames = 0;
    CompletionPredictability completionPredictability =
        CompletionPredictability::SignalOnly;
    KnownCompletionPredicateV1 completionPredicate{};
    OccurrenceToken progressOccurrence{};
    bool completionProgressBound = false;
    bool prepared = false;
    // Internal ownership bookkeeping makes partial cancellation resumable.
    bool voiceOwnershipHeld = false;
    bool ledgerOwnershipHeld = false;
    bool armOwnershipHeld = false;
    std::array<bool, kTimelineMaxVoiceEffects> effectOwnershipHeld{};
};

struct EngineActionSeekPreparation {
    bool streamed = false;
    std::uint32_t streamFileIndex = 0;
    std::uint64_t streamContentRevision = 0;
    std::uint64_t expectedOldRingToken = 0;
    std::uint64_t sourceFrameCount = 0;
    std::uint32_t sourceSampleRate = 0;
};

struct EnginePreparedRoutingMigrationV1 {
    enum class Store : std::uint8_t { ActionCurrent, ActionFuture, Replacement, Timeline };
    struct Row {
        Store store = Store::ActionCurrent;
        std::uint32_t outer = 0;
        std::uint32_t index = 0;
        std::uint64_t storageGeneration = 0;
        EnginePlayableServiceRow* service = nullptr;
        EnginePlayableServiceToken serviceIdentity{};
        PlayableInstanceHandle serviceOwner = 0;
        std::uint64_t serviceEpoch = 0;
        bool changed = false;
        EnginePreparedTimelineVoiceStart* address = nullptr;
        VnVoiceHandle handle = 0;
        std::uint32_t ledgerSlot = UINT32_MAX;
        TimelineArmToken arm{};
        std::uint32_t commandCount = 0;
        VoicePool::PreparedRoutingV1 before{};
        VoicePool::PreparedRoutingV1 after{};
        EngineTimelineVoiceInstallIdentity identityBefore{};
        EngineTimelineVoiceInstallIdentity identityAfter{};
        std::vector<std::uint8_t> expectedCommands;
        std::vector<CommandRecord> nextCommands;
    };
    Engine* owner = nullptr;
    std::weak_ptr<ProjectGenerationReservationDomainV1> domain;
    std::uint64_t domainEpoch = 0;
    std::uint64_t graphRevision = 0;
    std::vector<Row> rows;
};

struct EngineImmediateRetryCleanup {
    EnginePlayableServiceReference service{};
    ActionFiringHandle cohort = 0;
    OccurrenceToken occurrence{};
    CompletionOwnerRef owner{};
    PlayableProgramKey program{};
    SourceTerminalToken source{};
    VoiceReleaseObserver release{};
    VnVoiceHandle render = 0;
    std::uint32_t ledgerSlot = UINT32_MAX;
    bool active = false;
    bool mutationBound = false;
    bool newPinGranted = false;
    bool ownerWithdrawn = false;
    bool sourceReleased = false;
};

struct EngineTimelineStartScratch {
    StableRowStorage<EnginePreparedTimelineVoiceStart> starts;
    // Both complete leaf sides plus one coordinator program per frontier slice.
    StableRowStorage<PreparedTransactionBatch> transactions;
    StableRowStorage<CommandRecord> extraCommands;
    StableRowStorage<ImmediateLaneEvent> queueEvents;
    bool occupied = false;
    EngineImmediateRetryCleanup retryCleanup{};
};

inline constexpr std::size_t kMaxActionSelectionOwnerTokens =
    kSelectionPathNodeCapacity;
inline constexpr std::size_t kMaxActionDecisionOwnerTokens = 2u;
inline constexpr std::size_t kMaxActionOccurrenceOwnerTokens =
    2u * (1u + kMaxKnownScheduleLeaves);
inline constexpr std::size_t kMaxActionCompositeOwnerTokens = 2u;
inline constexpr std::size_t kMaxActionVoiceOwnerTokens =
    2u * kMaxKnownScheduleLeaves;
inline constexpr std::size_t kMaxActionQueueInstanceOwnerTokens = 2u;
inline constexpr std::size_t kMaxActionVariableControlOwnerTokens =
    kMaxKnownScheduleLeaves + 64u;
inline constexpr std::size_t kMaxActionPlayableTreeOwnerTokens =
    kMaxActionSelectionOwnerTokens + kMaxActionDecisionOwnerTokens +
    kMaxActionOccurrenceOwnerTokens + kMaxActionCompositeOwnerTokens +
    kMaxActionVoiceOwnerTokens + kMaxActionQueueInstanceOwnerTokens +
    (2u * kMaxActionVariableControlOwnerTokens) + 1u;
inline constexpr std::size_t kActionFinalOwnerFaultTokenCount = 1u;

static_assert(
    kMaxActionPlayableTreeOwnerTokens + kActionFinalOwnerFaultTokenCount <=
    PlayableTreeExecutor::kOwnerTokenCapacity,
    "PlayableTreeExecutor owner capacity must cover the largest Action tree");

enum class EngineFrontierAttemptPhase : std::uint8_t {
    Free, Preparing, PrivateCommitted, Published, PublishedStopping,
    PublishedGroupReclaim, CancelPending, Quarantined,
    PendingVariableChildren, PendingVariableChildrenSourcesCommitting,
    PendingVariableChildrenSourcesPrivateCommitted, PendingVariableChildrenCancelPending
};

// Keep allocator-owning objects alive across per-invocation metadata resets.
struct EngineFrontierServiceOwnerContext {
    Engine* engine = nullptr;
    EnginePlayableServiceReference service{};
};

enum class EngineInitialQueueDecisionProgress : std::uint8_t {
    Prepared, PrivateOpen, Retiring, Disposed, Transferred
};

struct EngineFrontierQueueInitialDecisionContext {
    EngineFrontierQueueCurrentOwnerContext* current = nullptr;
    EngineFrontierAttemptToken parent{};
    std::uint32_t queueIndex = UINT32_MAX;
};

// Stable native factory output, separate from scratch/row reuse. A context
// alias must still match its original full Composite token and native owner.
class EngineOriginalOccurrencePreparation {
public:
    EngineOriginalOccurrencePreparation() noexcept = default;
    EngineOriginalOccurrencePreparation(const EngineOriginalOccurrencePreparation&) = delete;
    EngineOriginalOccurrencePreparation& operator=(const EngineOriginalOccurrencePreparation&) = delete;
    EngineOriginalOccurrencePreparation(EngineOriginalOccurrencePreparation&&) noexcept = default;
    EngineOriginalOccurrencePreparation& operator=(EngineOriginalOccurrencePreparation&&) noexcept = default;
    const OccurrenceStatePool::OriginalStoragePin& OriginalStorage() const noexcept { return original_; }
    OccurrenceToken OriginalToken() const noexcept { return token_; }
private:
    friend struct Engine;
    Engine* receiver_ = nullptr;
    OccurrenceToken token_{};
    OccurrenceStatePool::OriginalStoragePin original_;
};

class EngineOriginalCompositeCompletionPreparation {
public:
    EngineOriginalCompositeCompletionPreparation() noexcept = default;
    EngineOriginalCompositeCompletionPreparation(const EngineOriginalCompositeCompletionPreparation&) = delete;
    EngineOriginalCompositeCompletionPreparation& operator=(const EngineOriginalCompositeCompletionPreparation&) = delete;
    EngineOriginalCompositeCompletionPreparation(EngineOriginalCompositeCompletionPreparation&&) noexcept = default;
    EngineOriginalCompositeCompletionPreparation& operator=(EngineOriginalCompositeCompletionPreparation&&) noexcept = default;
    const CompositeCompletionOwnerPool::OriginalLifetimePin& OriginalOwner() const noexcept { return original_; }
    CompositeCompletionOwnerToken OriginalToken() const noexcept { return token_; }
private:
    friend struct Engine;
    Engine* receiver_ = nullptr;
    CompositeCompletionOwnerToken token_{};
    CompositeCompletionOwnerPool::OriginalLifetimePin original_;
};

struct EngineFrontierQueueCurrentOwnerContext {
    Engine* engine = nullptr;
    EnginePlayableServiceReference service{};
    EngineOriginalCompositeCompletionPreparation* originalComposite = nullptr;
    PreparedDecisionReservation decision{};
    DecisionTable::PreparedTicketCommitProof originalSingleDecisionCommitProof{};
    bool decisionCommitted = false;
    bool canonicalInitial = false;
    bool initialDecisionRegistered = false;
    bool initialReservationRefused = false;
    DecisionToken canonicalDecision{};
    DecisionReserveRequest canonicalRequest{};
    LogicCommitSequence canonicalTicket = 0;
    EngineInitialQueueDecisionProgress canonicalProgress = EngineInitialQueueDecisionProgress::Prepared;
    EngineFrontierQueueInitialDecisionContext initialDecisionContext{};
    EnginePlayablePartitionToken canonicalPartition{};
    ActionFiringHandle canonicalRoot = 0;
    EffectiveQueueState canonicalCurrent{};
    EffectiveQueueState canonicalNext{};
    PlayableProgramKey canonicalSourceProgram{};
    PlayableProgramKey canonicalTargetProgram{};
    TimelineSemanticTarget canonicalSourceTarget{};
    VnVoiceHandle canonicalSourceRender = 0;
    std::uint64_t canonicalSourceStart = 0;
    std::uint64_t canonicalCompletion = 0;
    std::uint64_t canonicalSourcePhase = 0;
    std::uint64_t canonicalTargetEntry = 0;
    std::uint32_t canonicalRuleOrdinal = UINT32_MAX;
    TransitionFadeV1 canonicalSourceFade{};
    TransitionFadeV1 canonicalTargetFade{};
    ArmGroupToken canonicalResourceGroup{};
    TransitionInstanceToken canonicalResourceTransition{};
    EngineQueueProgramBank* canonicalResourceBank = nullptr;
    bool canonicalResourceExited = false;
    bool canonicalGroupExited = false;
    bool canonicalTransitionExited = false;
    bool canonicalResourcesAdopted = false;
    bool canonicalDecisionExited = false;
    bool sourceCompletionExited = false;
    OccurrenceToken occurrence{};
    bool occurrencePrepared = false;
    CompletionOwnerRef owner{};
    bool ownerPinned = false;
    std::array<EnginePreparedTimelineVoiceStart*, kMaxKnownScheduleLeaves> voices{};
    std::array<OccurrenceToken, kMaxKnownScheduleLeaves> leafOccurrences{};
    std::array<bool, kMaxKnownScheduleLeaves> leafPrepared{};
    CompositeCompletionOwnerToken composite{};
    bool compositePrepared = false;
    std::uint32_t leafCount = 0;
    std::uint32_t mutationCount = 0;
};

struct EngineFrontierQueueBindingOwnerContext {
    EngineFrontierQueueCurrentOwnerContext* current = nullptr;
};

struct EngineFrontierQueueEntryDecisionOwnerContext {
    Engine* engine = nullptr;
    EngineFrontierAttemptToken parent{};
    std::uint32_t queueIndex = UINT32_MAX;
    ActionFiringHandle root = 0;
    std::uint64_t attemptGeneration = 0;
    EngineInitialQueueEntryDecisionOwner owner{};
    bool registered = false;
};

struct EngineQueueInstanceOwnerContext {
    Engine* engine = nullptr;
    EnginePlayableServiceReference service{};
    PreparedQueueInstanceV1 prepared{};
    bool committed = false;
};

struct EngineFrontierQueueOwnerContext : EngineQueueInstanceOwnerContext {
    const ContainerProgramViewV3* program = nullptr;
    EffectiveQueueState initial{};
    EffectiveQueueState next{};
    bool hasSuccessor = false;
    EngineFrontierQueueCurrentOwnerContext current{};
    EngineFrontierQueueBindingOwnerContext binding{};
    EngineFrontierQueueEntryDecisionOwnerContext entryDecision{};
};

// Audible retarget leaves belong to PreparedTimelineStarts. Only a zero-work
// current needs its own decision and occurrence in the subtree preparation.
struct EngineRetargetQueueOwnerContext : EngineQueueInstanceOwnerContext {
    struct Current {
        PreparedDecisionReservation decision{};
        OccurrenceToken occurrence{};
        bool active = false;
        bool decisionCommitted = false;
        bool occurrencePrepared = false;
    } current;
};

// Directory growth never moves a record or its embedded callback identities.
class EngineQueueRoleDirectory {
public:
    EngineQueueRoleDirectory() noexcept = default;
    EngineQueueRoleDirectory(const EngineQueueRoleDirectory&) = delete;
    EngineQueueRoleDirectory& operator=(const EngineQueueRoleDirectory&) = delete;
    EngineQueueRoleDirectory(EngineQueueRoleDirectory&& other) noexcept { records_.swap(other.records_); }
    EngineQueueRoleDirectory& operator=(EngineQueueRoleDirectory&& other) noexcept {
        if (this != &other) {
            EngineQueueRoleDirectory incoming(std::move(other));
            records_.swap(incoming.records_);
        }
        return *this;
    }

    template<class AllowAllocation>
    EngineFrontierQueueOwnerContext* Acquire(std::uint32_t index, std::uint32_t limit,
        bool allowGrowth, AllowAllocation&& allowAllocation) noexcept {
        if (index >= limit) return nullptr;
        if (index < records_.size()) return records_[index].get();
        if (!allowGrowth || index != records_.size()) return nullptr;
        try {
            if (records_.size() == records_.capacity()) {
                if (!allowAllocation()) return nullptr;
                const auto next = std::min<std::size_t>(limit,
                    records_.empty() ? 1u : records_.size() * 2u);
                records_.reserve(next);
            }
            if (!allowAllocation()) return nullptr;
            std::unique_ptr<EngineFrontierQueueOwnerContext> record(
                new (std::nothrow) EngineFrontierQueueOwnerContext{});
            if (record == nullptr) return nullptr;
            auto* result = record.get();
            records_.push_back(std::move(record));
            return result;
        } catch (const std::bad_alloc&) {
            return nullptr;
        } catch (const std::length_error&) {
            return nullptr;
        }
    }

    template<class AllowAllocation>
    bool Construct(std::uint32_t count, AllowAllocation&& allowAllocation) noexcept {
        if (!Empty()) return false;
        if (count == 0) return true;
        try {
            if (!allowAllocation()) return false;
            records_.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
                if (Acquire(i, count, true, allowAllocation) == nullptr) return false;
            return true;
        } catch (const std::bad_alloc&) {
            return false;
        } catch (const std::length_error&) {
            return false;
        }
    }
    void ClearAfterSafeReset() noexcept {
        for (auto& record : records_) {
            std::destroy_at(record.get());
            std::construct_at(record.get());
        }
    }
    void ReleaseAfterSafeReset() noexcept { EngineQueueRoleDirectory empty; records_.swap(empty.records_); }
    bool Empty() const noexcept { return records_.empty() && records_.capacity() == 0; }
    std::size_t StorageBytes() const noexcept {
        return records_.capacity() * sizeof(std::unique_ptr<EngineFrontierQueueOwnerContext>) +
            records_.size() * sizeof(EngineFrontierQueueOwnerContext);
    }
    std::size_t size() const noexcept { return records_.size(); }
    std::size_t capacity() const noexcept { return records_.capacity(); }
    EngineFrontierQueueOwnerContext& operator[](std::size_t index) noexcept { return *records_[index]; }
    const EngineFrontierQueueOwnerContext& operator[](std::size_t index) const noexcept { return *records_[index]; }
    std::uint32_t IndexOf(const EngineFrontierQueueOwnerContext& context) const noexcept {
        for (std::uint32_t i = 0; i < records_.size(); ++i)
            if (records_[i].get() == &context) return i;
        return UINT32_MAX;
    }
private:
    std::vector<std::unique_ptr<EngineFrontierQueueOwnerContext>> records_;
};

struct EngineQueueFutureAttemptOwnerContext {
    Engine* engine = nullptr;
    EngineFrontierAttemptToken parent{};
    EngineFrontierAttemptToken child{};
    bool committed = false;
    bool rootLinkDeferred = false;
    const PlayableInstanceTable::PreparedParentLinksProof* originalParentLinks = nullptr;
};

struct EngineFrontierWorkItem {
    PlayableRefV1 authored{};
    PlayableInstanceHandle parent = 0;
    std::uint64_t parentEpoch = 0;
    PlayableInstanceHandle instance = 0;
    std::uint64_t instanceEpoch = 0;
    EnginePlayableServiceReference service{};
    std::uint32_t parentIndex = UINT32_MAX;
    std::uint32_t depth = 0;
    std::uint32_t selectedOrdinal = 0;
    std::uint32_t occurrenceOrdinal = 0;
    BankVersionToken controlBank{};
    float controlWeight = 1.0f;
    bool continuousMember = false;
};

struct EngineFrontierExpansionScratch {
    explicit EngineFrontierExpansionScratch(std::uint32_t capacity) : nodes(capacity) {}
    std::vector<EngineFrontierWorkItem> nodes;
    std::uint32_t count = 0;
    bool occupied = false;
};

struct EngineInitialFrontierSide {
    EnginePlayableServiceReference service{};
    DecisionToken decision{};
    ArmGroupToken armGroup{};
    Plan03GroupSliceIdentity group{};
};

struct EngineInitialFrontierArmPin {
    TimelineArmToken arm{};
    FrontierEntryReceiptRef entry{};
    TimelineArmReaderPin pin{};
};

struct EnginePreparedDecisionOwnerContext {
    DecisionTable* table = nullptr;
    PreparedDecisionReservation* reservation = nullptr;
    DecisionToken* output = nullptr;
    LogicCommitSequence* committedTicket = nullptr;
    bool committed = false;
    DecisionTable::PreparedTicketCommitProof ticketCommitProof{};
    bool ticketCommitProofRequired = false;
};

struct EnginePreparedScheduleLeafStart {
    TimelineVoiceInstallDesc voice{};
    PlayableRefV1 leaf{};
    std::uint64_t startOffsetFrames = 0;
    std::uint64_t endOffsetFrames = UINT64_MAX;
    std::uint32_t occurrenceOrdinal = 0;
    std::uint32_t frontierPolicyIndex = UINT32_MAX;
    WeightSegmentV1 variableWeight{};
    bool hasVariableWeight = false;
};

// Issued only by the actual service preparation; loans retain its producer storage.
class OriginalPendingVariableProducerRetention {
public:
    class Pin {
    public:
        Pin() noexcept = default;
        Pin(const Pin&) = delete;
        Pin& operator=(const Pin&) = delete;
        Pin(Pin&& other) noexcept { *this = std::move(other); }
        Pin& operator=(Pin&& other) noexcept {
            if (this != &other) {
                Return();
                original_ = std::exchange(other.original_, nullptr);
                marker_ = std::exchange(other.marker_, 0);
            }
            return *this;
        }
        ~Pin() { Return(); }
        bool Valid() const noexcept {
            return original_ != nullptr && marker_ != 0 &&
                original_->marker_ == marker_ && original_->engine_ != nullptr &&
                original_->loanCount_ != 0;
        }
        void Return() noexcept {
            if (original_ == nullptr) return;
            const bool matches = Valid();
            assert(matches);
            if (!matches) return;
            --original_->loanCount_;
            original_ = nullptr;
            marker_ = 0;
        }
    private:
        friend struct Engine;
        OriginalPendingVariableProducerRetention* original_ = nullptr;
        std::uint64_t marker_ = 0;
    };
    OriginalPendingVariableProducerRetention() noexcept = default;
    OriginalPendingVariableProducerRetention(const OriginalPendingVariableProducerRetention&) = delete;
    OriginalPendingVariableProducerRetention& operator=(const OriginalPendingVariableProducerRetention&) = delete;
    OriginalPendingVariableProducerRetention(OriginalPendingVariableProducerRetention&& other) noexcept {
        assert(other.engine_ == nullptr && other.loanCount_ == 0);
    }
    bool ReclaimReady() const noexcept { return loanCount_ == 0; }
    bool Empty() const noexcept { return engine_ == nullptr; }
    bool ForgetAfterOwnLoansReturned() noexcept {
        if (!ReclaimReady()) return false;
        engine_ = nullptr;
        storage_ = nullptr;
        storageGeneration_ = 0;
        service_ = nullptr;
        invocation_ = nullptr;
        reference_ = {};
        marker_ = 0;
        return true;
    }
private:
    friend struct Engine;
    Engine* engine_ = nullptr;
    EngineActionPlayStorage* storage_ = nullptr;
    std::uint64_t storageGeneration_ = 0;
    EnginePlayableServiceRow* service_ = nullptr;
    EnginePlayableStartRow* invocation_ = nullptr;
    EnginePlayableServiceReference reference_{};
    std::uint64_t marker_ = 0;
    std::uint32_t loanCount_ = 0;
};

// A same-Reserve association for disposal, without a Pack or Current loan.
class OriginalPreparingInvocationStorage {
public:
    OriginalPreparingInvocationStorage() noexcept = default;
    OriginalPreparingInvocationStorage(const OriginalPreparingInvocationStorage&) = delete;
    OriginalPreparingInvocationStorage& operator=(const OriginalPreparingInvocationStorage&) = delete;
    OriginalPreparingInvocationStorage(OriginalPreparingInvocationStorage&&) noexcept = default;
    OriginalPreparingInvocationStorage& operator=(OriginalPreparingInvocationStorage&&) noexcept = default;
    void ClearObservedWait() noexcept { blocked_ = {}; }
    void ForgetAfterScratchExit() noexcept {
        engine_ = nullptr;
        attempt_ = nullptr;
        attemptGeneration_ = 0;
        invocation_ = nullptr;
        invocationGeneration_ = 0;
        ended_ = false;
        blocked_ = {};
    }
private:
    friend struct Engine;
    Engine* engine_ = nullptr;
    EngineActionPlayStorage* attempt_ = nullptr;
    std::uint64_t attemptGeneration_ = 0;
    EnginePlayableStartRow* invocation_ = nullptr;
    std::uint64_t invocationGeneration_ = 0;
    bool ended_ = false;
    EnginePlayableServiceReference blocked_{};
};

// Same current-world target branch receipt, without a World or Current loan.
class OriginalWorldNoPackPreparation {
public:
    OriginalWorldNoPackPreparation() noexcept = default;
    OriginalWorldNoPackPreparation(const OriginalWorldNoPackPreparation&) = delete;
    OriginalWorldNoPackPreparation& operator=(const OriginalWorldNoPackPreparation&) = delete;
    OriginalWorldNoPackPreparation(OriginalWorldNoPackPreparation&&) noexcept = default;
    OriginalWorldNoPackPreparation& operator=(OriginalWorldNoPackPreparation&&) noexcept = default;
    void ForgetAfterScratchExit() noexcept {
        receiver_ = nullptr;
        storage_ = nullptr;
        storageGeneration_ = 0;
        worldGeneration_ = 0;
        project_ = {};
        worldFingerprint_ = 0;
        target_ = {};
    }
private:
    friend struct Engine;
    Engine* receiver_ = nullptr;
    EngineActionPlayStorage* storage_ = nullptr;
    std::uint64_t storageGeneration_ = 0;
    std::uint64_t worldGeneration_ = 0;
    VnObjectId project_{};
    std::uint64_t worldFingerprint_ = 0;
    ActionObjectRefV1 target_{};
};

struct EngineActionPlayStorage {
    struct InitialSelectionContext {
        SelectionPendingBatchIdentityV1 identity{};
        EngineSelectionPreparation preparation{};
        std::uint64_t attemptGeneration = 0;
        std::uint64_t executeFrame = 0;
        std::array<std::uint32_t, kSelectionPathNodeCapacity> materializedOrdinals{};
        std::array<SelectionPathNodeV1, kSelectionPathNodeCapacity> prefix{};
        std::uint32_t prefixCount = 0;
        bool prefixSealed = false;

        static bool EventBegin(std::uint32_t executionOrdinal, std::uint32_t* out) noexcept {
            constexpr auto capacity = kSelectionPathNodeCapacity;
            if (out == nullptr || executionOrdinal > (UINT32_MAX - (capacity - 1u)) / capacity)
                return false;
            *out = executionOrdinal * capacity;
            return true;
        }
        bool BoundFor(std::uint64_t generation) const noexcept {
            return generation != 0 && attemptGeneration == generation && identity.ticket != 0;
        }
        bool SealPrefix(std::uint64_t generation, const SelectionPathNodeV1* path,
            std::uint32_t count) noexcept {
            if (!BoundFor(generation) || prefixSealed || count > prefix.size() ||
                (count != 0 && path == nullptr)) return false;
            if (count != 0) std::copy_n(path, count, prefix.begin());
            prefixCount = count;
            prefixSealed = true;
            return true;
        }
        SelectionMemoryStatusV1 IssueBatch(SelectionMemoryTableV1& table,
            EnginePendingSelectionBatch& owner, std::uint64_t generation,
            PreparedSelectionPlayV1* const* prepared, std::uint32_t count) const noexcept {
            if (!BoundFor(generation) || preparation.mode != EngineSelectionPreparationMode::PendingInitial ||
                preparation.replacementOwner != nullptr || count > materializedOrdinals.size())
                return SelectionMemoryStatusV1::InvalidInput;
            return owner.Issue(table, identity, prepared, count, executeFrame, materializedOrdinals.data());
        }
    } initialSelection;
    EnginePendingSelectionBatch initialSelectionBatch;
    struct InitialSelectionCensusAssociation {
        EngineImmediateEntryAttempt* attempt = nullptr;
        ActionFiringHandle root = 0;
        std::uint64_t generation = 0;
        std::uint32_t index = UINT32_MAX;
        bool Empty() const noexcept { return attempt == nullptr && root == 0 && generation == 0 && index == UINT32_MAX; }
    } initialSelectionCensus;
    explicit EngineActionPlayStorage(std::uint32_t nodeCapacity =
        kMaxKnownScheduleLeaves + 64u + kSelectionPathNodeCapacity + 1u,
        std::uint32_t memberCapacity = 0,
        std::uint32_t laneMetadataCapacity = kMaxKnownScheduleLeaves,
        const EngineFrontierOwnerLedger::Limits* ownerLimits = nullptr)
          : variableControlRollbackEdges(memberCapacity),
            pendingInitialVcRetirement(memberCapacity), pendingInitialVcRows(memberCapacity),
            retiredInitialVcArms(memberCapacity),
            privateInitialVcLaneCapacity(laneMetadataCapacity),
            frontierServices(nodeCapacity), queueRoleCapacity(nodeCapacity),
            queueFutureAttempts(nodeCapacity), queueFutureOwners(nodeCapacity),
            frontierPolicies(nodeCapacity), initialControlSlices(2u * nodeCapacity),
            initialFrontierSides(2u * nodeCapacity), initialFrontierParticipants(2u * nodeCapacity),
            nestedContinuousDecisions(nodeCapacity), nestedContinuousDecisionOwners(nodeCapacity),
            playableTreeExecutor(std::in_place, nodeCapacity) {
        if (ownerLimits != nullptr) {
            variableControlChildren.resize(nodeCapacity);
            variableControlChildKinds.resize(nodeCapacity);
            variableControlLeafChildren.resize(nodeCapacity);
            variableControlContainerRows.resize(nodeCapacity);
            variableControlNestedParentIndices.resize(nodeCapacity);
            variableControlNestedPathOrdinals.resize(nodeCapacity);
            variableControlNestedModes.resize(nodeCapacity);
            ownerLedger.Initialize(*ownerLimits);
        }
    }
    enum class VariableControlRollbackStage : std::uint8_t {
        None,
        Captured,
        ParentReleased,
    };
    struct VariableControlRollbackEdge {
        std::size_t logicalSlot = 0;
        EnginePlayableMemberReference reference{};
        EnginePlayablePartitionToken childPartition{};
        bool detached = false;
    };
    struct OrdinaryDeferredRollbackOwner {
        DeferredIntentToken poolToken{};
        ActionDeferredRecordToken recordToken{};
        ActionFiringHandle firing = 0;
        PackPartitionLease targetLease{};
        EnginePlayableServiceToken service{};
        EnginePlayablePartitionToken partition{};
        std::uint64_t invocationGeneration = 0;
        std::uint64_t startFrame = 0;
        bool active = false;
        bool allowFinalization = false;
        bool rowReleased = false;
        bool recordOwnsLease = false;
        bool leaseReleased = false;
        bool poolReleased = false;
        bool recordFinalized = false;
        bool firingCountDischarged = false;
        bool fault = false;
    } ordinaryDeferredRollback{};
    struct DeferredContainerOwner {
        enum class LeaseDisposition : std::uint8_t {
            RecordOwns,
            RowOwns,
            Released,
        };
        DeferredIntentToken poolToken{};
        ActionDeferredRecordToken recordToken{};
        ActionFiringHandle firing = 0;
        std::uint32_t stepIndex = 0;
        PackPartitionLease targetLease{};
        EnginePlayableServiceToken service{};
        EnginePlayablePartitionToken partition{};
        std::uint64_t invocationGeneration = 0;
        std::uint64_t attemptGeneration = 0;
        std::uint64_t earliestFrame = 0;
        std::uint64_t stoppingBlockerFrame = 0;
        FrontierEntryReceiptRef stoppingEntry{};
        Plan03GroupSliceIdentity stoppingMember{};
        Plan03GroupSliceIdentity stoppingPrimary{};
        Plan03UnpublishedRetirementToken unpublishedRetirement{};
        LeaseDisposition lease = LeaseDisposition::RecordOwns;
        bool active = false;
        bool fault = false;
        bool rollbackRequested = false;
        bool retryRequested = false;
        bool retryApplied = false;
        bool allowFinalization = false;
        bool resourcesReleased = false;
        bool leaseReleased = false;
        bool poolReleased = false;
        bool recordFinalized = false;
        bool firingCountDischarged = false;
        bool unpublishedWitnessed = false;
        bool unpublishedBindingsReleased = false;
        bool unpublishedActivationReconciled = false;
        bool unpublishedGroupsRetired = false;
        bool unpublishedFrontierEventRemoved = false;
        bool unpublishedDecisionTransferred = false;
        bool publishedTreeTransferred = false;
        bool publishedLedgerTransferred = false;
        bool stoppingBlockerHeld = false;
    } deferredContainerOwner{};
    OriginalPreparingInvocationStorage originalInvocationStorage;
    OriginalPendingVariableProducerRetention originalPendingVariableProducer;
    OriginalWorldNoPackPreparation originalWorldNoPackPreparation;
    EngineOriginalPackIdentityLease originalPackLease;
    std::uint64_t originalPackLeaseGeneration = 0;
    ActionDeferredRecordToken originalDeferredPackRecord{};
    EngineFrontierAttemptPhase phase = EngineFrontierAttemptPhase::Free;
    std::uint64_t generation = 0;
    VariableServiceV1* initialVariableService = nullptr;
    VariableReadCutRef initialVariableCut{};
    EngineFrontierAttemptToken rollbackSuccessor{};
    VariableControlRollbackStage variableControlRollbackStage = VariableControlRollbackStage::None;
    bool variableControlRollbackSnapshotCaptured = false;
    std::uint32_t variableControlRollbackEdgeCount = 0;
    EnginePlayableServiceReference variableControlRollbackParent{};
    EnginePlayablePartitionToken variableControlRollbackParentPartition{};
    std::vector<VariableControlRollbackEdge> variableControlRollbackEdges;
    std::vector<TimelineArmTable::PendingImmediateRetirementInput>
        pendingInitialVcRetirement;
    std::vector<TimelineArmTable::Row*> pendingInitialVcRows;
    TimelineArmTable::UnpublishedPendingRetirementProof initialVcRetirementProof{};
    FrontierEntryReceiptRef initialVcRetirementEntry{};
    Plan03GroupSliceIdentity initialVcRetirementPrimary{};
    bool initialVcRetirementDecisionsOpen = false;
    bool initialVcUnboundRetirementConsumed = false;
    std::vector<TimelineArmToken> retiredInitialVcArms;
    std::uint32_t privateInitialVcLaneCapacity = 0;
    std::vector<ImmediateLaneMemberManifest> privateInitialVcLaneMembers;
    std::vector<ImmediateLaneAcceptedPrivateGroup> privateInitialVcLaneGroups;
    struct PrivateInitialVcArmPreparation {
        TimelineArmTable::PreparedCommitInput input{};
        EnginePlayableServiceReference queue{};
        EnginePlayablePartitionToken partition{};
        std::uint64_t rootGeneration = 0;
        std::uint32_t logicalOrdinal = UINT32_MAX;
    };
    std::vector<PrivateInitialVcArmPreparation> privateInitialVcArmPreparations;
    std::uint32_t retiredInitialVcArmCount = 0;
    std::uint64_t retiredInitialVcAttemptGeneration = 0;
    std::uint64_t initialVcExactCommandAttemptGeneration = 0;
    std::uint32_t initialVcExactCommandCount = 0;
    EngineFrontierOwnerLedger ownerLedger;
    EngineFrontierOwnerBatch ownerBatch;
    std::vector<EngineFrontierServiceOwnerContext> frontierServices;
    std::uint32_t queueRoleCapacity = 0;
    EngineQueueRoleDirectory frontierQueues;
    std::vector<EngineFrontierAttemptToken> queueFutureAttempts;
    std::vector<EngineQueueFutureAttemptOwnerContext> queueFutureOwners;
    std::vector<EngineFrontierWorkItem> frontierPolicies;
    std::vector<CommittedPlan03GroupSlice> initialControlSlices;
    std::vector<EngineInitialFrontierSide> initialFrontierSides;
    std::vector<DecisionParticipantRef> initialFrontierParticipants;
    std::vector<PreparedDecisionReservation> nestedContinuousDecisions;
    std::vector<EnginePreparedDecisionOwnerContext> nestedContinuousDecisionOwners;
    std::vector<PreparedPlayableInstance> variableControlChildren;
    std::vector<PlayableKind> variableControlChildKinds;
    std::vector<PreparedPlayableInstance> variableControlLeafChildren;
    std::vector<PreparedContainerPlaybackRow> variableControlContainerRows;
    std::vector<std::uint16_t> variableControlNestedParentIndices;
    std::vector<std::uint32_t> variableControlNestedPathOrdinals;
    std::vector<std::uint8_t> variableControlNestedModes;
    // Destroy the executor before the cap vectors it borrows.
    std::optional<PlayableTreeExecutor> playableTreeExecutor;
};

struct EngineActionPlayScratch {
    // All page/lane groups are covered by this scratch's single exposure owner.
    static constexpr std::uint32_t kFrontierExposureOwnerCapacity = 1;
    explicit EngineActionPlayScratch(EngineActionPlayStorage& persistent) noexcept
        : storage(persistent), playableTreeExecutor(*persistent.playableTreeExecutor),
          variableControlChildren(persistent.variableControlChildren),
          variableControlChildKinds(persistent.variableControlChildKinds),
          variableControlLeafChildren(persistent.variableControlLeafChildren),
          variableControlContainerRows(persistent.variableControlContainerRows),
          variableControlNestedParentIndices(persistent.variableControlNestedParentIndices),
          variableControlNestedPathOrdinals(persistent.variableControlNestedPathOrdinals),
          variableControlNestedModes(persistent.variableControlNestedModes),
          variableControlInitialCommittedSlices(persistent.initialControlSlices) {
        privateInitialVcPageCohort.laneMemberManifests = persistent.privateInitialVcLaneMembers;
    }
    EngineActionPlayStorage& storage;
    MonitorControlCause cancellationCause{};
    bool cancellationCauseAccepted = false;
    std::uint32_t frontierServiceCount = 0;
    std::uint32_t frontierQueueCount = 0;
    std::uint32_t queueFutureAttemptCount = 0;
    std::uint32_t frontierPolicyCount = 0;
    bool frontierSelectedQueue = false;
    std::uint32_t initialFrontierSideCount = 0;
    EnginePlayableServiceReference frontierInitialChild{};
    EnginePlayableServiceReference frontierResourceService{};

    struct PreparedAuthorityOwnerContext {
        Engine* engine = nullptr;
        std::uint64_t worldGeneration = 0;
        PackPartitionLease packLease{};
        bool pack = false;
        bool frozen = false;
        EnginePlayableServiceRow* service = nullptr;
        EngineFrontierOwnerLedger* dependencies = nullptr;
    } authorityOwnerContext{};
    bool authorityOwnerRegistered = false;
    bool frontierOwnerBatchRegistered = false;
    bool frontierOwnerLedgerComplete = false;
    struct PrivateInitialVcPageCohort {
        enum class Backing : std::uint8_t { None, Page, ImmediateLane };
        Backing backing = Backing::None;
        std::uint64_t cohortRoot = 0;
        std::array<Plan03GroupSliceIdentity, 2> laneSides{};
        Plan03GroupSliceIdentity lanePrimary{};
        std::array<PreparedPrivateImmediateLaneGroup, 3> laneOriginals{};
        std::array<PrivateImmediateLaneGroupTransferProof, 3> laneTransferProofs{};
        std::span<ImmediateLaneMemberManifest> laneMemberManifests{};
        std::uint32_t laneMemberCount = 0;
        std::uint32_t laneOwnedGroupCount = 0;
        bool laneAcceptedOriginals = false;
        std::uint32_t laneArmPreparationCount = 0;
        ImmediateLaneHandle originalLane{};
        ImmediateLaneDemand originalLaneDemand{};
        bool laneTransferRequired = false;
        bool laneMembersDeclared = false;
        std::uint32_t laneFirstMemberOrdinal = UINT32_MAX;
        std::uint32_t laneEntryOrdinal = UINT32_MAX;
        FrontierEntryReceiptRef laneEntry{};
        EngineFrontierAttemptToken rootProducerNext{};
        std::uint64_t rootProducerGeneration = 0;
        EnginePlayablePartitionToken rootProducerPartition{};
        std::uint64_t rootProducerInvocationGeneration = 0;
        bool rootActivationDischarged = false;
        bool rootSideDecisionsInvalidated = false;
        bool rootExposureReleased = false;
        bool pageTransferRequired = false;
        PreparedPageGroupTransferScope pageTransferScope{};
        std::array<PreparedPageGroupTransferProof, 3> pageTransferProofs{};
        std::array<Plan03GroupSliceIdentity, 3> pageProspectiveIdentities{};
        std::array<Plan03GroupSliceIdentity, 2> pageConsumedSideIdentities{};
        void ReleasePageTransferProofs() noexcept {
            pageBatchTransferProof.Release();
            for (auto& proof : pageTransferProofs) proof.Release();
        }
        bool PageTransferHeld(const PreparedPlan03GroupSlice& primary) const noexcept {
            if (pageBatchTransferProof.Valid()) return true;
            if (primary.Held()) return true;
            for (const auto& original : sides) if (original.Held()) return true;
            for (const auto& proof : pageTransferProofs) if (proof.Valid()) return true;
            return false;
        }
        bool pageBatchSelected = false;
        bool pageBatchBindingsStarted = false;
        bool pageBatchConsumed = false;
        std::uint64_t pageBatchCancelledAttemptGeneration = 0;
        PreparedPageBatchTransferProof pageBatchTransferProof{};
        std::array<FrontierEventBinding, 5> pageBatchPlannedBindings{};
        WorldGeneration pageBatchWorld = 0;
        TransportEpoch pageBatchTransport = 0;
        std::array<ConsumedEvent, 5> pageBatchOriginalEvents{};
        void ReleaseLaneTransferProofs() noexcept {
            for (auto& proof : laneTransferProofs) proof.Release();
        }
        bool LaneTransferHeld() const noexcept {
            for (const auto& original : laneOriginals) if (original.Held()) return true;
            for (const auto& proof : laneTransferProofs) if (proof.Valid()) return true;
            return false;
        }
        LogicCommitSequence sequence = 0;
        std::uint64_t frame = 0;
        std::uint64_t attemptGeneration = 0;
        DecisionReserveRequest coordinatorRequest{};
        DecisionToken coordinator{};
        ArmGroupToken coordinatorGroup{};
        std::array<PreparedPlan03GroupSlice, 2> sides{};
        std::array<Plan03GroupSliceInput, 2> descriptors{};
        std::array<TimelineArmToken, 2> members{};
    } privateInitialVcPageCohort{};
    std::uint32_t frontierCurrentVoiceOwnerCount = 0;
    struct PreparedVariableControlExposureOwnerContext {
        Engine* engine = nullptr;
        EngineActionPlayScratch* scratch = nullptr;
        EnginePlayableServiceRow* row = nullptr;
    } variableControlExposureOwnerContext{};
    struct PreparedTimelineVoiceOwnerContext {
        Engine* engine = nullptr;
        EnginePreparedTimelineVoiceStart* token = nullptr;
        bool committed = false;
        TimelineArmTable::PreparedCommitProof armCommitProof{};
        VoiceLedger::PreparedCommitProof ledgerCommitProof{};
        VoicePool::PreparedCommitProof voiceCommitProof{};
        std::array<EffectPoolBank::PreparedCommitProof, kTimelineMaxVoiceEffects> effectCommitProofs{};
        bool HasEffectCommitProofs() const noexcept {
            for (const auto& proof : effectCommitProofs) if (proof.Valid()) return true;
            return false;
        }
        bool EffectsProved() const noexcept {
            if (token == nullptr || token->effectCount > effectCommitProofs.size()) return false;
            for (std::uint32_t i = 0; i < token->effectCount; ++i)
                if (!effectCommitProofs[i].Valid()) return false;
            return true;
        }
        bool preparedCommitProofsRequired = false;
        void ReleaseCommitProofs() noexcept {
            armCommitProof.Release();
            ledgerCommitProof.Release();
            voiceCommitProof.Release();
            for (auto& proof : effectCommitProofs) proof.Release();
        }
    };
    using NativeVoiceSide = EngineNativeVoiceSide<
        EnginePreparedTimelineVoiceStart, PreparedTimelineVoiceOwnerContext>;

    struct PreparedSelectionOwnerContext {
        SelectionMemoryTableV1* table = nullptr;
        PreparedSelectionPlayV1* token = nullptr;
        bool committed = false;
    };
    struct PreparedPendingSelectionOwnerContext {
        EngineActionPlayStorage* storage = nullptr;
        std::uint64_t generation = 0;
        SelectionMemoryTableV1* table = nullptr;
        static bool Validate(void* opaque) noexcept {
            const auto* context = static_cast<PreparedPendingSelectionOwnerContext*>(opaque);
            return context != nullptr && context->storage != nullptr && context->generation != 0 &&
                context->storage->generation == context->generation &&
                context->storage->initialSelection.BoundFor(context->generation) &&
                context->storage->initialSelectionBatch.MatchesOwner(
                    context->table, context->storage->initialSelection.identity);
        }
        static bool Commit(void* opaque, std::uint64_t ticket) noexcept {
            auto* context = static_cast<PreparedPendingSelectionOwnerContext*>(opaque);
            return Validate(opaque) && context->storage->initialSelectionBatch.Accept(ticket);
        }
        static bool Cancel(void* opaque) noexcept {
            auto* context = static_cast<PreparedPendingSelectionOwnerContext*>(opaque);
            return context != nullptr && context->storage != nullptr && context->generation != 0 &&
                context->storage->generation == context->generation &&
                (context->storage->initialSelectionBatch.Empty() ||
                 (context->storage->initialSelectionBatch.MatchesOwner(
                      context->table, context->storage->initialSelection.identity) &&
                  context->storage->initialSelectionBatch.Cancel()));
        }
    } pendingSelectionOwnerContext{};
    struct PreparedDirectSelectionOwnerContext {
        SelectionMemoryTableV1* table = nullptr;
        SelectionDeltaRefV1 delta{};
    };
    struct PreparedDirectExposureOwnerContext {
        Engine* engine = nullptr;
        EngineActionPlayScratch* scratch = nullptr;
    } directExposureOwnerContext{};
    std::array<PreparedDirectSelectionOwnerContext, kSelectionPathNodeCapacity>
        directSelectionOwnerContexts{};
    std::uint32_t directSelectionOwnerCount = 0;
    CommandQueue::BatchReservation directCommandReservation{};
    bool directCommandsReserved = false;
    EnginePlayableServiceReference pendingVariableReplacementService{};
    EngineFrontierAttemptToken pendingVariableReplacementProducer{};
    struct PendingVariableCohortClose {
        OriginalPendingVariableProducerRetention::Pin loan;
        const EngineImmediateEntryAttempt* attempt = nullptr;
        std::uint64_t generation = 0;
        EngineActionPlayStorage* nextStorage = nullptr;
        bool groupReturned = false;
    } pendingVariableCohortClose;
    std::uint64_t pendingVariableReplacementEntryGeneration = 0;
    VariableReadCutRef pendingVariableReplacementCut{};
    std::uint32_t pendingVariableReplacementCommandCount = 0;
    struct PendingVariableReplacementTop {
        PlayableRefV1 root{};
        std::uint32_t frontierPolicyIndex = UINT32_MAX;
        std::uint32_t authoredChildOrdinal = 0;
        WeightSegmentV1 weight{};
        VariableControlSide side = VariableControlSide::Discrete;
        std::uint64_t logicalCompletionOffset = 0;
        bool hasLogicalCompletion = false;
    };
    std::array<PendingVariableReplacementTop, kMaxKnownScheduleLeaves> pendingVariableReplacementTops{};
    std::uint32_t pendingVariableReplacementTopCount = 0;
    class PendingVariableReplacementTopology {
        friend struct Engine;
        struct Node {
            PlayableInstanceHandle handle = 0;
            PlayableInstanceHandle parent = 0;
            std::uint64_t epoch = 0;
            PlayableKind kind = PlayableKind::Sound;
            PlayableInstanceTable::OriginalLifetimePin original;
        };
        std::array<Node, 2u * kMaxKnownScheduleLeaves + 1u> nodes{};
        std::array<std::uint32_t, kMaxKnownScheduleLeaves> leafNodes{}, leafTops{};
        std::uint32_t nodeCount = 0;
        bool sealed = false;
        EngineFrontierAttemptToken futureParent{}, futurePreparation{};
        EnginePlayableServiceReference futureService{};
        EnginePlayablePartitionToken futurePartition{};
        QueueInstanceToken futureQueue{};
        PlayableInstanceHandle futureRoot = 0;
    public:
        void ReturnOriginalLifetimes() noexcept {
            for (std::uint32_t index = 0; index < nodeCount; ++index) nodes[index].original.Release();
        }
    } pendingVariableReplacementTopology;
    struct PendingVariableReplacementControl {
        EnginePlayableServiceReference service{};
        VariableControlInstanceToken instance{};
        VariableControlSide side = VariableControlSide::Discrete;
        DecisionToken decision{};
        BankVersionToken bank{};
        DecisionReserveRequest input{};
        VariableControlInstanceTable::OriginalInitialSideReceipt original;
        std::uint32_t preparationIndex = UINT32_MAX;
        ArmGroupToken armGroup{};
        std::uint16_t nativeMemberCount = 0;
    };
    static constexpr std::size_t kPendingVariableReplacementControlCapacity =
        2u * (kMaxActionVariableControlOwnerTokens + 1u);
    std::vector<PendingVariableReplacementControl> pendingVariableReplacementControls;
    std::uint32_t pendingVariableReplacementControlCount = 0;
    struct PendingVariableReplacementLeaf {
        PlayableRefV1 leaf{};
        std::uint32_t authoredChildOrdinal = 0;
        WeightSegmentV1 weight{};
        VariableControlSide side = VariableControlSide::Discrete;
        std::uint16_t memberOrdinal = 0;
        std::uint32_t controlIndex = UINT32_MAX;
        std::uint32_t controlChildOrdinal = 0;
        std::uint32_t frontierPolicyIndex = UINT32_MAX;
        std::uint16_t nativeMemberOrdinal = 0;
        std::uint64_t authoredOffset = 0;
        EnginePlayableMemberReference storageReference{};
        TimelineSemanticTarget target{};
        LogicCommitSequence sequence = 0;
        TimelineTransactionProgramRef program{};
    };
    std::array<PendingVariableReplacementLeaf, kMaxKnownScheduleLeaves> pendingVariableReplacementLeaves{};
    std::uint32_t pendingVariableReplacementLeafCount = 0;
    std::uint64_t pendingVariableReplacementAnchorFrame = 0;
    ImmediateLaneHandle pendingVariableReplacementLane{};
    ImmediateLaneDemand pendingVariableReplacementDemand{};
    std::uint32_t pendingVariableReplacementPlacedEventCount = 0;
    static constexpr std::size_t kPendingVariableReplacementQueueCapacity = 2u * kMaxKnownScheduleLeaves;
    static constexpr std::size_t kPendingVariableReplacementGroupCapacity =
        kPendingVariableReplacementControlCapacity + 1u + 4u * kPendingVariableReplacementQueueCapacity;
    std::vector<Plan03GroupSliceIdentity> pendingVariableReplacementGroups;
    struct PendingVariableReplacementQueueLane {
        std::uint32_t queueIndex = UINT32_MAX;
        std::uint32_t recordIndex = UINT32_MAX;
        std::uint64_t recordGeneration = 0;
        std::uint32_t groupOffset = UINT32_MAX;
        std::uint32_t groupCount = 0;
        bool continuation = false;
        bool natural = false;
        std::array<bool, 4> groupBound{};
        std::array<bool, 2> decisionBound{};
        std::array<bool, 2> decisionCommitted{};
        bool activationPrepared = false;
        bool coordinatorCommitted = false;
    };
    std::vector<PendingVariableReplacementQueueLane> pendingVariableReplacementQueueLanes;
    std::uint32_t pendingVariableReplacementQueueLaneCount = 0;
    PreparedDecisionReservation pendingVariableReplacementCoordinator;
    DecisionFuturePin pendingVariableReplacementPublicationPin;
    std::array<OriginalDecisionLifetimePin, 2> pendingVariableReplacementSideLifetimes;
    struct PendingVariableReplacementEntry {
        DecisionToken coordinator{};
        DecisionReserveRequest coordinatorInput{};
        OriginalDecisionLifetimePin* coordinatorBaseline = nullptr;
        LogicCommitSequence ticket = 0;
        ArmGroupToken coordinatorGroup{};
        bool ticketIssued = false;
        bool activationPrepared = false;
        bool coordinatorCommitted = false;
        bool entryReadUsePrepared = false;
        bool primaryBound = false;
        std::array<ArmGroupToken, kPendingVariableReplacementControlCapacity> sideGroups{};
        std::array<bool, kPendingVariableReplacementControlCapacity> sideCommitted{};
        std::array<bool, kPendingVariableReplacementControlCapacity> groupBound{};
        std::array<bool, kPendingVariableReplacementControlCapacity> sideBound{};
        std::array<bool, kPendingVariableReplacementControlCapacity> decisionBound{};
        std::array<bool, kMaxKnownScheduleLeaves> armPinned{};
        std::array<bool, kMaxKnownScheduleLeaves> admissionBound{};
    } pendingVariableReplacementEntry{};
    bool directExecution = false;
    bool continuationExecution = false;
    bool queueFutureExecution = false;
    PreparedQueueInstanceV1* queueFuturePreparedQueue = nullptr;
    EngineQueueProgramBank* queueFutureProgramBank = nullptr;
    DecisionToken queueFutureCurrentDecision{};
    EngineFrontierQueueCurrentOwnerContext* queueFutureCanonicalCurrent = nullptr;
    EngineFrontierAttemptToken queueFutureParent{};
    std::array<std::size_t, kMaxKnownScheduleLeaves> queueFutureMemberSlots{};
    std::array<EnginePlayableMemberReference, kMaxKnownScheduleLeaves> queueFuturePriorMemberReferences{};
    std::size_t queueFutureMemberBase = 0;
    std::uint32_t queueFutureMemberCount = 0;
    EnginePlayableServiceReference continuationService{};
    EnginePlayablePartitionToken continuationPartition{};
    TimelineExecutionOrder continuationOrder{};
    std::uint64_t continuationTransportEpoch = 0;
    ContainerPlaybackRow continuationParentSnapshot{};
    PlayableInstanceHandle continuationChildRoot = 0;
    bool continuationParentLinked = false;
    DecisionToken frontierCoordinator{};
    bool frontierCoordinatorCancelled = false;
    ArmGroupToken frontierCoordinatorGroup{};
    PreparedPlan03GroupSlice frontierPreparedPrimary{};
    CommittedPlan03GroupSlice frontierPrimary{};
    FrontierActivationDescriptor frontierDescriptor{};
    PreparedQueueSignalAdvanceV1 frontierSignal{};
    TimelineArmReaderPin frontierArmPin{};
    EngineQueueProgramBank* frontierProgramBank = nullptr;
    using PreparedDecisionOwnerContext = EnginePreparedDecisionOwnerContext;
    struct PreparedActionResourceOwnerContext {
        Engine* engine = nullptr;
        EngineActionPlayScratch* scratch = nullptr;
        EnginePlayableServiceRow* row = nullptr;
        std::uint32_t sourceArmsCommitted = 0;
        std::uint32_t groupSlicesCommitted = 0;
        bool transitionCommitted = false;
        bool armGroupCommitted = false;
        bool queueInitialLaneArmGroupCommitted = false;
        bool conditionalCommitted = false;
        bool transactionsCommitted = false;
        PreparedQueueInstanceV1* preparedQueue = nullptr;
        QueueInstanceToken conditionalQueue{};
        ConditionalQueueVersionToken conditionalVersion{};
        DecisionToken conditionalDecision{};
    };
    struct PreparedOccurrenceOwnerContext {
        OccurrenceStatePool* pool = nullptr;
        OccurrenceToken* token = nullptr;
        bool* prepared = nullptr;
        CompletionOwnerRef* completionOwner = nullptr;
        std::uint64_t observedCompletionBlockSequence = 0;
    };
    struct PreparedQueueInstanceOwnerContext {
        QueueInstanceTable* table = nullptr;
        PreparedQueueInstanceV1* prepared = nullptr;
        QueueInstanceToken* output = nullptr;
        std::uint32_t* cancelAttemptsForTest = nullptr;
        bool committed = false;
        Engine* engine = nullptr;
        EnginePlayableServiceRow* service = nullptr;
        PreparedActionResourceOwnerContext* resources = nullptr;
    };
    struct PreparedVariableControlOwnerContext {
        PreparedVariableControlOwnerContext() noexcept = default;
        PreparedVariableControlOwnerContext(const PreparedVariableControlOwnerContext&) = delete;
        PreparedVariableControlOwnerContext& operator=(const PreparedVariableControlOwnerContext&) = delete;
        PreparedVariableControlOwnerContext(PreparedVariableControlOwnerContext&& other) noexcept {
            *this = std::move(other);
        }
        PreparedVariableControlOwnerContext& operator=(PreparedVariableControlOwnerContext&& other) noexcept {
            if (this == &other || serviceProofRequired_ || other.serviceProofRequired_ ||
                serviceCommitProof.Valid() || other.serviceCommitProof.Valid()) return *this;
            table = other.table;
            prepared = other.prepared;
            output = other.output;
            discreteDecision = other.discreteDecision;
            continuousDecision = other.continuousDecision;
            activationDecision = other.activationDecision;
            activationBank = other.activationBank;
            activationSide = other.activationSide;
            row = other.row;
            nestedIndex = other.nestedIndex;
            nestedCount = other.nestedCount;
            activate = other.activate;
            committed = other.committed;
            activated = other.activated;
            engine = other.engine;
            service = other.service;
            commitProof = std::move(other.commitProof);
            commitProofRequired = other.commitProofRequired;
            originalServiceInput_ = other.originalServiceInput_;
            originalServiceEngine_ = other.originalServiceEngine_;
            originalService_ = other.originalService_;
            originalServiceReference_ = other.originalServiceReference_;
            originalInvocation_ = other.originalInvocation_;
            other.originalServiceInput_.Reset();
            other.originalServiceEngine_ = nullptr;
            other.originalService_ = nullptr;
            other.originalServiceReference_ = {};
            other.originalInvocation_ = nullptr;
            return *this;
        }
        bool CanPrepareOriginalService() const noexcept {
            return !serviceProofRequired_ && !serviceCommitProof.Valid() &&
                !originalServiceInput_.Valid() && originalService_ == nullptr;
        }
        EnginePlayablePartitionToken ReserveOriginalService(EnginePlayablePartitionPool& pool,
            Engine* originalEngine, EnginePlayableServiceRow& originalRow,
            EnginePlayablePartitionPool::OriginalStoragePin* originalStorage = nullptr) noexcept {
            if (!CanPrepareOriginalService()) return {};
            const auto token = pool.Reserve(originalRow.identity, &originalServiceInput_, originalStorage);
            if (token.Valid()) {
                originalServiceEngine_ = originalEngine;
                originalService_ = &originalRow;
            }
            return token;
        }
        void DiscardUnassociatedServiceReservation() noexcept {
            assert(!originalServiceReference_.Valid() && !serviceProofRequired_);
            const bool reset = originalServiceInput_.Reset();
            assert(reset);
            (void)reset;
            originalServiceEngine_ = nullptr;
            originalService_ = nullptr;
        }
        bool RecordOriginalServiceAssociation() noexcept {
            if (originalService_ == nullptr || !originalServiceInput_.Valid() ||
                originalServiceReference_.Valid() || serviceProofRequired_ ||
                !originalService_->InvocationValid() || originalService_->ownerInstance == 0 ||
                originalService_->ownerEpoch == 0) return false;
            originalServiceReference_ = {originalService_->identity, originalService_->ownerInstance,
                originalService_->ownerEpoch, originalService_->invocationGeneration};
            originalInvocation_ = originalService_->invocation;
            return true;
        }
        bool OriginalServiceAssociationMatches(Engine* expectedEngine,
            const EnginePlayableServiceRow* expectedService) const noexcept {
            return originalServiceEngine_ == expectedEngine && expectedService != nullptr &&
                originalService_ == expectedService && originalServiceInput_.Valid() &&
                originalServiceReference_.Valid() &&
                expectedService->identity == originalServiceInput_.Owner() &&
                expectedService->identity == originalServiceReference_.token &&
                expectedService->partition.slot == originalServiceInput_.Token().slot &&
                expectedService->partition.generation == originalServiceInput_.Token().generation &&
                expectedService->ownerInstance == originalServiceReference_.owner &&
                expectedService->ownerEpoch == originalServiceReference_.epoch &&
                expectedService->invocationGeneration == originalServiceReference_.invocationGeneration &&
                expectedService->invocation == originalInvocation_ && expectedService->InvocationValid() &&
                originalServiceInput_.Pool()->Owns(originalServiceInput_.Token(), originalServiceInput_.Owner());
        }
        bool ServiceCommitProofRequired() const noexcept { return serviceProofRequired_; }
        void SelectServiceCommitProof() noexcept { serviceProofRequired_ = true; }
        const EnginePlayablePartitionPool::ReservationInput& OriginalServiceInput() const noexcept {
            return originalServiceInput_;
        }
        const EnginePlayableServiceRow* OriginalService() const noexcept { return originalService_; }
        VariableControlInstanceTable* table = nullptr;
        PreparedVariableControlInstance* prepared = nullptr;
        VariableControlInstanceToken* output = nullptr;
        DecisionToken* discreteDecision = nullptr;
        DecisionToken* continuousDecision = nullptr;
        DecisionToken* activationDecision = nullptr;
        BankVersionToken* activationBank = nullptr;
        VariableControlSide activationSide = VariableControlSide::Discrete;
        EnginePlayableServiceRow* row = nullptr;
        std::uint32_t nestedIndex = UINT32_MAX;
        std::uint32_t* nestedCount = nullptr;
        bool activate = false;
        bool committed = false;
        bool activated = false;
        Engine* engine = nullptr;
        EnginePlayableServiceRow* service = nullptr;
        VariableControlInstanceTable::PreparedCommitProof commitProof{};
        bool commitProofRequired = false;
        EnginePlayablePartitionPool::PreparedCommitProof serviceCommitProof{};
    private:
        EnginePlayablePartitionPool::ReservationInput originalServiceInput_{};
        Engine* originalServiceEngine_ = nullptr;
        EnginePlayableServiceRow* originalService_ = nullptr;
        EnginePlayableServiceReference originalServiceReference_{};
        EnginePlayableStartRow* originalInvocation_ = nullptr;
        bool serviceProofRequired_ = false;
    };
    struct PreparedCompositeOwnerContext {
        CompositeCompletionOwnerPool* pool = nullptr;
        CompositeCompletionOwnerToken* token = nullptr;
        OccurrenceStatePool* occurrences = nullptr;
        bool* preparedFlag = nullptr;
        bool committed = false;
    };

    EnginePlayableServiceRow* row = nullptr;
    PlayableTreeExecutor& playableTreeExecutor;
    PreparedPlayableInstance instance{};
    PreparedVariableControlInstance variableControlInstance{};
    PreparedContainerPlaybackRow variableControlControllerRow{};
    PreparedPlayableInstance variableControlOuterRoot{};
    PreparedContainerPlaybackRow variableControlOuterContainerRow{};
    std::array<PreparedPlayableInstance, kSelectionPathNodeCapacity>
        variableControlOuterPathChildren{};
    std::array<PreparedContainerPlaybackRow, kSelectionPathNodeCapacity>
        variableControlOuterPathContainerRows{};
    std::uint32_t variableControlOuterPathChildCount = 0;
    std::uint32_t variableControlOuterPathContainerRowCount = 0;
    std::array<std::uint32_t, kSelectionPathNodeCapacity>
        variableControlNestedPrefixOrdinals{};
    std::array<SelectionPathNodeV1, kSelectionPathNodeCapacity>
        variableControlNestedPrefixPath{};
    std::uint32_t variableControlNestedPrefixCount = 0;
    PlayableInstanceHandle variableControlParentInstance = 0;
    std::uint64_t variableControlParentEpoch = 0;
    bool variableControlNested = false;
    // The outer path reservations are owned by playableTreeExecutor once
    // nested VariableControl preparation has completed.
    bool variableControlOuterTreeTokensAdopted = false;
    PlayableRootMetadata variableControlOuterRootMetadata{};
    std::vector<PreparedPlayableInstance>& variableControlChildren;
    std::vector<PlayableKind>& variableControlChildKinds;
    std::vector<PreparedPlayableInstance>& variableControlLeafChildren;
    std::vector<PreparedContainerPlaybackRow>& variableControlContainerRows;
    std::vector<std::uint16_t>& variableControlNestedParentIndices;
    std::vector<std::uint32_t>& variableControlNestedPathOrdinals;
    std::vector<std::uint8_t>& variableControlNestedModes;
    std::array<PreparedVariableControlInstance,
               kMaxKnownScheduleLeaves + 64u>
        variableControlNestedVariableControls{};
    std::array<EnginePlayableServiceReference, kMaxKnownScheduleLeaves + 64u>
        variableControlNestedServices{};
    std::uint32_t variableControlNestedVariableControlCount = 0;
    std::array<PreparedDecisionReservation,
               kMaxKnownScheduleLeaves + 64u>
        variableControlNestedDecisions{};
    std::array<bool, kMaxKnownScheduleLeaves + 64u>
        variableControlNestedDecisionContinuous{};
    std::array<std::uint32_t, kMaxKnownScheduleLeaves + 64u>
        variableControlNestedSelectedChildOrdinals{};
    std::array<std::uint32_t, kMaxKnownScheduleLeaves + 64u>
        variableControlNestedContinuationOrdinals{};
    std::uint32_t variableControlNestedQueueOwnerIndex = UINT32_MAX;
    std::array<PlayableInstanceHandle, kMaxKnownScheduleLeaves>
        variableControlLeafHandles{};
    std::array<std::uint64_t, kMaxKnownScheduleLeaves>
        variableControlLeafEpochs{};
    PreparedPlayableInstance variableControlNestedQueueSuccessor{};
    std::uint32_t variableControlNestedQueueSuccessorOrdinal = 0;
    std::uint32_t variableControlLeafChildCount = 0;
    std::uint32_t variableControlContainerRowCount = 0;
    std::uint32_t variableControlNestedFrontierCount = 0;
    std::uint32_t variableControlChildCount = 0;
    PreparedDecisionReservation variableControlDiscreteDecision{};
    PreparedDecisionReservation variableControlContinuousDecision{};
    std::vector<CommittedPlan03GroupSlice>& variableControlInitialCommittedSlices;
    std::uint32_t variableControlInitialCommittedSliceCount = 0;
    PreparedQueueInstanceV1 queueInstance{};
    // Parent Queue token for an Action that enters a nested Queue. The
    // nested Queue remains the active row queue until it naturally retires.
    PreparedQueueInstanceV1 queueParentInstance{};
    QueueInstanceToken queueInstanceCommitted{};
    QueueInstanceToken queueParentInstanceCommitted{};
    PreparedDecisionReservation queueDecision{};
    PreparedDecisionReservation queueSuccessorDecision{};
    OccurrenceToken queueCurrentOccurrence{};
    std::array<OccurrenceToken, kMaxKnownScheduleLeaves>
        queueCurrentLeafOccurrences{};
    std::array<bool, kMaxKnownScheduleLeaves>
        queueCurrentLeafOccurrencesPrepared{};
    CompositeCompletionOwnerToken queueCurrentCompositeOwner{};
    CompletionOwnerRef queueCurrentOwner{};
    EngineOriginalCompositeCompletionPreparation* queueCurrentOriginalComposite = nullptr;
    EngineOriginalCompositeCompletionPreparation* queueSuccessorOriginalComposite = nullptr;
    OccurrenceToken queueSuccessorOccurrence{};
    CompletionOwnerRef queueSuccessorOwner{};
    NativeVoiceSide queueSuccessorVoiceStarts;
    std::array<OccurrenceToken, kMaxKnownScheduleLeaves>
        queueSuccessorLeafOccurrences{};
    std::array<bool, kMaxKnownScheduleLeaves>
        queueSuccessorLeafOccurrencesPrepared{};
    PreparedOccurrenceOwnerContext queueCurrentOccurrenceOwnerContext{};
    std::array<PreparedOccurrenceOwnerContext, kMaxKnownScheduleLeaves>
        queueCurrentLeafOccurrenceOwnerContexts{};
    PreparedOccurrenceOwnerContext queueSuccessorOccurrenceOwnerContext{};
    std::array<PreparedOccurrenceOwnerContext, kMaxKnownScheduleLeaves>
        queueSuccessorLeafOccurrenceOwnerContexts{};
    CompositeCompletionOwnerToken queueSuccessorCompositeOwner{};
    PreparedTransactionBatch queueTransactions{};
    std::array<PreparedPlan03GroupSlice, 2> queueGroupSlices{};
    std::array<CommittedPlan03GroupSlice, 2> queueCommittedGroupSlices{};
    PreparedQueueConditionalV1 queueConditional{};
    // Same-call native output alias, never reconstructed from a Queue snapshot.
    QueueInstanceTable::OriginalConditionalFutureReservation* initialOriginalConditional = nullptr;
    ConditionalQueueVersionToken initialOriginalConditionalVersion{};
    QueueInstanceToken initialOriginalConditionalQueue{};
    std::uint64_t initialOriginalGroupRoot = 0;

    std::uint64_t initialOriginalGroupAttemptGeneration = 0;
    EnginePlayableServiceToken initialOriginalScratchService{};
    std::uint64_t initialOriginalScratchInvocationGeneration = 0;
    EnginePlayableServiceToken initialOriginalCohortService{};
    std::uint64_t initialOriginalCohortInvocationGeneration = 0;
    bool initialOriginalGroupsDisposed = false;
    ArmGroupToken queueArmGroup{};
    ArmGroupToken queueInitialLaneArmGroup{};
    TransitionInstanceToken queueTransition{};
    std::array<CommandRecord, kMaxKnownScheduleLeaves>
        queueSourceCommands{};
    std::array<TimelineArmTable::PreparedCommitInput, kMaxKnownScheduleLeaves>
        queueSourceArmPreparations{};
    const ContainerProgramViewV3* queueProgram = nullptr;
    std::uint32_t queueCurrentLeafOccurrenceCount = 0;
    std::uint32_t queueCompletionMutationCount = 0;
    bool queueCurrentOccurrencePrepared = false;
    bool queueCurrentCompositeOwnerPrepared = false;
    PreparedCompositeOwnerContext queueCurrentCompositeOwnerContext{};
    bool queueCompletionOwnerPinned = false;
    std::uint32_t queueSuccessorVoiceStartCount = 0;
    std::uint32_t queueSuccessorLeafOccurrenceCount = 0;
    std::uint32_t queueSuccessorCompletionMutationCount = 0;
    std::uint32_t queueGroupSliceCount = 0;
    std::uint32_t queueSourceControlArmCount = 0;
    std::uint32_t queueSourceCommandCount = 0;
    bool queueSuccessorOccurrencePrepared = false;
    bool queueSuccessorCompletionOwnerPinned = false;
    bool queueSuccessorCompositeOwnerPrepared = false;
    PreparedCompositeOwnerContext queueSuccessorCompositeOwnerContext{};
    PreparedQueueInstanceOwnerContext queueInstanceOwnerContext{};
    PreparedQueueInstanceOwnerContext queueParentInstanceOwnerContext{};
    bool queueArmGroupPrepared = false;
    bool queueInitialLaneArmGroupPrepared = false;
    bool queueTransitionPrepared = false;
    bool queueHasContinuation = false;
    EffectiveQueueState queueInitialState{};
    EffectiveQueueState queueInitialNextState{};
    bool queueInitialResourcesPrepared = false;
    std::array<PreparedSelectionPlayV1, kSelectionPathNodeCapacity>
        selectionPlays{};
    std::array<PreparedSelectionOwnerContext, kSelectionPathNodeCapacity>
        selectionOwnerContexts{};
    PreparedDecisionOwnerContext queueDecisionOwnerContext{};
    PreparedDecisionOwnerContext queueSuccessorDecisionOwnerContext{};
    PreparedDecisionOwnerContext variableControlDiscreteDecisionOwnerContext{};
    PreparedDecisionOwnerContext variableControlContinuousDecisionOwnerContext{};
    std::array<PreparedDecisionOwnerContext,
               kMaxActionVariableControlOwnerTokens>
        variableControlNestedDecisionOwnerContexts{};
    PreparedVariableControlOwnerContext variableControlOwnerContext{};
    std::array<PreparedVariableControlOwnerContext,
               kMaxActionVariableControlOwnerTokens>
        variableControlNestedOwnerContexts{};
    PreparedActionResourceOwnerContext actionResourceOwnerContext{};
    std::uint32_t selectionPlayCount = 0;
    NativeVoiceSide voiceStarts;
    bool currentNativeBorrowed = false;
    bool successorNativeBorrowed = false;
    bool queueRolesBorrowed = false;
    QueueCheckpointTable::BackingPreparation queueCheckpointBacking;
    std::uint32_t voiceStartCount = 0;
    std::uint32_t initialCommandBase = 0;
    std::uint32_t initialCommandCount = 0;
    std::uint32_t plannedStepIndex = UINT32_MAX;
    PackPartitionLease packLease{};
    PackPlaybackView packPlaybackView{};
    enum class PackLeaseProvenance : std::uint8_t {
        None,
        ScratchAcquired,
        DeferredRecordBorrowed,
        RowOwned,
    };
    PackLeaseProvenance packLeaseProvenance = PackLeaseProvenance::None;
    PackLeaseProvenance packLeaseOrigin = PackLeaseProvenance::None;
    bool packLeaseOwned = false;
    bool packLeaseBorrowed = false;
    bool variableControl = false;
    bool variableControlTreeTokensAdopted = false;
    // Remains set after a VariableControl play commits so a later sibling
    // failure can still select the VC-specific rollback path.
    bool variableControlCommitted = false;
    bool prepared = false;

    auto VoiceOwnerContextSides() noexcept {
        return std::array{voiceStarts.Owners(), queueSuccessorVoiceStarts.Owners()};
    }

    auto VoiceOwnerContextSides() const noexcept {
        return std::array{voiceStarts.Owners(), queueSuccessorVoiceStarts.Owners()};
    }

    bool VoiceOwnerCommitProofsHeld() const noexcept {
        for (const auto side : VoiceOwnerContextSides()) {
            for (const auto& owner : side) {
                if (owner.armCommitProof.Valid() || owner.ledgerCommitProof.Valid() ||
                    owner.voiceCommitProof.Valid() || owner.HasEffectCommitProofs()) return true;
            }
        }
        return false;
    }

    void ResetVariableControlSelection() noexcept {
        for (std::uint32_t index = 0;
             index < selectionPlayCount && index < selectionPlays.size();
             ++index) {
            selectionPlays[index] = {};
        }
        selectionPlayCount = 0;
    }
};

// Logic-thread retarget frontier storage. Buffers are sized once during
// Engine construction and reset in place so retarget preparation never grows
// or places a configured playable-capacity tree on the stack.
struct VariableControlRetargetTreeScratch {
    struct PolicyStorage {
        std::vector<EngineFrontierWorkItem> frontierPolicies;
        explicit PolicyStorage(std::size_t capacity) : frontierPolicies(capacity) {}
    } storage;
    std::uint32_t frontierPolicyCount = 0;
    bool frontierSelectedQueue = false;
    std::vector<PreparedSelectionPlayV1> selectionPlays;
    std::uint32_t selectionPlayCount = 0;
    std::array<SelectionPathNodeV1, kSelectionPathNodeCapacity>
        selectionPath{};
    std::array<std::uint32_t, kSelectionPathNodeCapacity>
        selectedOrdinals{};
    std::uint32_t selectionCount = 0;
    std::vector<PreparedPlayableInstance> nestedInstances;
    std::vector<PlayableInstanceHandle> nestedParents;
    std::vector<std::uint32_t> nestedOrdinals;
    std::vector<PlayableKind> nestedKinds;
    std::array<PreparedVariableControlInstance,
               kMaxKnownScheduleLeaves + 64u>
        nestedVariableControlInstances{};
    std::array<PreparedDecisionReservation,
               kMaxKnownScheduleLeaves + 64u>
        nestedVariableControlDecisions{};
    std::array<PreparedDecisionReservation, kMaxKnownScheduleLeaves + 64u>
        nestedContinuousDecisions{};
    std::array<DecisionToken, kMaxKnownScheduleLeaves + 64u>
        committedNestedContinuousDecisions{};
    std::array<bool, kMaxKnownScheduleLeaves + 64u>
        nestedVariableControlDecisionContinuous{};
    std::array<std::uint32_t, kMaxKnownScheduleLeaves + 64u>
        nestedVariableControlSelectedChildOrdinals{};
    std::array<std::uint32_t, kMaxKnownScheduleLeaves + 64u>
        nestedVariableControlContinuationOrdinals{};
    std::array<VariableControlInstanceToken,
               kMaxKnownScheduleLeaves + 64u>
        committedNestedVariableControls{};
    std::array<EnginePlayableServiceReference, kMaxKnownScheduleLeaves + 64u>
        nestedServices{};
    std::array<DecisionToken, kMaxKnownScheduleLeaves + 64u>
        committedNestedVariableControlDecisions{};
    std::array<BankVersionToken, kMaxKnownScheduleLeaves + 64u>
        nestedVariableControlBanks{};
    std::uint32_t nestedVariableControlCount = 0;
    std::vector<PreparedContainerPlaybackRow> containerRows;
    std::vector<PlayableInstanceHandle> containerInstances;
    std::vector<const ContainerProgramViewV3*> containerPrograms;
    std::vector<PlayableInstanceHandle> containerActiveChildren;
    std::vector<EnginePlayableServiceReference> containerServices;
    std::vector<EngineRetargetQueueOwnerContext> containerQueues;
    PreparedPlayableInstance queueSuccessor{};
    PlayableInstanceHandle queueSuccessorOwner = 0;
    std::uint64_t queueSuccessorOwnerEpoch = 0;
    const ContainerProgramViewV3* queueProgram = nullptr;
    std::uint32_t queueOwnerContainerIndex = UINT32_MAX;
    std::uint32_t queueSuccessorOrdinal = 0;
    LogicCommitSequence selectionTicket = 0;
    std::uint32_t nestedCount = 0;
    std::uint32_t containerCount = 0;
    std::uint32_t committedNestedCount = 0;
    std::uint32_t committedContainerCount = 0;

    explicit VariableControlRetargetTreeScratch(std::size_t capacity = 0)
        : storage(capacity), selectionPlays(kSelectionPathNodeCapacity),
          nestedInstances(capacity),
          nestedParents(capacity),
          nestedOrdinals(capacity),
          nestedKinds(capacity),
          containerRows(capacity),
          containerInstances(capacity),
          containerPrograms(capacity),
          containerActiveChildren(capacity),
          containerServices(capacity),
          containerQueues(capacity) {}

    VariableControlRetargetTreeScratch(
        const VariableControlRetargetTreeScratch&) = delete;
    VariableControlRetargetTreeScratch& operator=(
        const VariableControlRetargetTreeScratch&) = delete;
    VariableControlRetargetTreeScratch(
        VariableControlRetargetTreeScratch&&) noexcept = default;
    VariableControlRetargetTreeScratch& operator=(
        VariableControlRetargetTreeScratch&&) noexcept = default;

    void Reset() noexcept {
        for (std::uint32_t index = 0;
             index < selectionPlayCount && index < selectionPlays.size();
             ++index) {
            selectionPlays[index] = {};
        }
        for (std::uint32_t index = 0;
             index < nestedCount && index < nestedInstances.size(); ++index) {
            nestedInstances[index] = {};
        }
        for (std::uint32_t index = 0;
             index < nestedVariableControlCount; ++index) {
            nestedVariableControlInstances[index] = {};
            committedNestedVariableControls[index] = {};
            nestedServices[index] = {};
            committedNestedVariableControlDecisions[index] = {};
            nestedVariableControlDecisions[index] = {};
            nestedContinuousDecisions[index] = {};
            committedNestedContinuousDecisions[index] = {};
            nestedVariableControlDecisionContinuous[index] = false;
            nestedVariableControlSelectedChildOrdinals[index] = 0;
            nestedVariableControlContinuationOrdinals[index] = 0;
            nestedVariableControlBanks[index] = {};
        }
        for (std::uint32_t index = 0;
             index < containerCount && index < containerRows.size(); ++index) {
            containerRows[index] = {};
            containerServices[index] = {};
            containerQueues[index] = {};
        }
        frontierPolicyCount = 0;
        frontierSelectedQueue = false;
        queueSuccessor = {};
        selectionPlayCount = 0;
        queueSuccessorOwner = 0;
        queueSuccessorOwnerEpoch = 0;
        queueProgram = nullptr;
        queueOwnerContainerIndex = UINT32_MAX;
        queueSuccessorOrdinal = 0;
        selectionTicket = 0;
        selectionCount = 0;
        selectionPath = {};
        selectedOrdinals = {};
        nestedCount = 0;
        nestedVariableControlCount = 0;
        containerCount = 0;
        committedNestedCount = 0;
        committedContainerCount = 0;
    }
};

struct EngineActionStopScratch {
    EnginePlayableServiceRow* row = nullptr;
    PreparedPlayableCancellation cancellation{};
    EnginePreparedAuthoredStop authoredStop{};
    MonitorControlCause controlCause{};
    PlayableRetirementObserver retirementObserver{};
    std::uint64_t fadeStartFrame = 0;
    std::uint64_t silenceFrame = 0;
    std::uint32_t plannedStepIndex = UINT32_MAX;
    std::uint32_t commandBase = 0;
    std::uint32_t commandCount = 0;
    bool scheduledControl = false;
    bool treeCancellation = false;
    bool treeCancellationRequired = false;
    bool treeObserver = false;
    bool prepared = false;
};

struct EngineActionQueueReplacementScratch {
    MonitorControlCause resetCause{};
    EnginePlayableServiceRow* row = nullptr;
    QueueInstanceToken nativeReplacement{};
    DecisionToken nativeMemberDecision{}, nativeSourceDecision{};
    std::uint64_t resetExecuteFrame = 0;
    std::uint32_t resetExecutionOrdinal = 0;
    EngineQueueProgramBank* programBank = nullptr;
    QueueContinuationLane* oldContinuation = nullptr;
    EngineActionPlayScratch* successorTree = nullptr;
    std::vector<EnginePlayableMemberReference> successorMembers;
    StableStoragePreparation successorRelationBacking;
    PreparedTransactionBatch transactions{};
    PreparedDecisionReservation decision{};
    PreparedDecisionReservation successorDecision{};
    EnginePreparedAuthoredStop inheritedStop{};
    PreparedQueueConditionalReplacementV1 conditional{};
    std::array<PreparedPlan03GroupSliceReplacement, 2> groupSlices{};
    ArmGroupToken armGroup{};
    EffectiveQueueState next{};
    KnownCompletionPredicateV1 replacementCompletion{};
    std::vector<CapturedTimelineVoiceInstall> successorVoices;
    std::vector<PlayableRefV1> successorLeaves;
    std::vector<EnginePreparedTimelineVoiceStart> successorStarts;
    OccurrenceToken successorOccurrence{};
    std::vector<OccurrenceToken> successorLeafOccurrences;
    KnownCompletionPredicateV1 successorCompletion{};
    std::vector<KnownCompletionPredicateV1> successorLeafCompletions;
    CompletionOwnerRef successorOwner{};
    CompositeCompletionOwnerToken successorCompositeOwner{};
    EngineOriginalCompositeCompletionPreparation* successorOriginalComposite = nullptr;
    std::vector<std::uint64_t> successorStartFrames;
    std::vector<std::uint32_t> successorLeafEventCounts;
    std::span<TimelineEventPayload> successorPayloads;
    std::span<EventTemplate> successorEvents;
    std::span<TimelineProgramView> successorRuntimes;
    std::span<ProgramView> successorSchedules;
    std::vector<VnVoiceHandle> sourceRenderHandles;
    std::vector<TimelineSemanticTarget> sourceTargets;
    std::vector<TimelineArmToken> sourceArms;
    std::vector<CommandRecord> sourceCommands;
    std::span<TimelineEventPayload> sourcePayloads;
    std::span<EventTemplate> sourceEvents;
    std::vector<std::uint32_t> sourceLeafEventCounts;
    std::span<TimelineProgramView> sourceRuntimes;
    std::span<ProgramView> sourceSchedules;
    std::vector<TimelineArmToken> replacementGroupArms;
    TransitionInstanceToken transition{};
    CompletionOwnerRef oldCompletionOwner{};
    CompletionOwnerRef replacementCompletionOwner{};
    VoiceReleaseObserver retainedCurrentRelease{};
    bool retainsOriginalCurrent = false;
    std::uint32_t currentLedgerSlot = UINT32_MAX;
    std::uint32_t successorEventCount = 1;
    std::uint32_t successorLeafCount = 0;
    std::uint32_t sourceControlCount = 0;
    std::uint32_t sourceEventCount = 0;
    std::uint64_t sourcePhaseFrame = 0;
    std::uint64_t sourceStopFrame = 0;
    std::uint64_t successorZeroWorkCompletionFrame = 0;
    bool naturalEnd = false;
    std::uint16_t groupSliceCount = 0;
    std::uint8_t successorProgramPlane = 0;
    std::uint64_t transitionEndFrame = 0;
    bool sameProgramKey = true;
    bool successorHasZeroWorkCompletion = false;
    bool successorOccurrencePrepared = false;
    bool successorHasContinuation = false;
    bool oldDecisionCommitted = false;
    bool prepared = false;
};

inline std::size_t ActionPreparationSlotCapacity(
    const EngineConfig& config) noexcept {
    return static_cast<std::size_t>(kMaxActionSteps) *
           config.maxTriggerDepth;
}

inline std::size_t QueueProgramBankCapacity(const EngineConfig& config) {
    const auto count = 2ull * config.maxPlayableInstances + config.maxScheduledTransactions;
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(EngineQueueProgramBank)) {
        throw std::length_error("Queue program bank capacity overflow");
    }
    return static_cast<std::size_t>(count);
}

inline std::size_t QueueContinuationRecordCapacity(
    const EngineConfig& config) {
    if (config.maxScheduledTransactions == 0 ||
        static_cast<std::size_t>(config.maxScheduledTransactions) >
            std::vector<EngineQueueContinuationRecord>{}.max_size()) {
        throw std::length_error("Queue continuation record capacity overflow");
    }
    return config.maxScheduledTransactions;
}

inline std::size_t PackIdentityLeaseCapacity(const EngineConfig& config) {
    const std::uint64_t total =
        static_cast<std::uint64_t>(config.maxActionFirings) +
        config.maxDeferredIntents + config.maxPlayableInstances;
    if (total > std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("pack identity lease capacity overflow");
    }
    return static_cast<std::size_t>(total);
}

inline std::size_t PackRetirementCapacity(const EngineConfig& config) {
    const std::uint64_t total =
        static_cast<std::uint64_t>(config.maxPacks) +
        PackIdentityLeaseCapacity(config);
    if (total > std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("pack retirement capacity overflow");
    }
    return static_cast<std::size_t>(total);
}

inline std::uint32_t TimelineArmCapacity(const EngineConfig& config) {
    const std::uint64_t total = static_cast<std::uint64_t>(config.voiceCapacity.Render()) +
        config.maxScheduledTransactions;
    if (total > UINT32_MAX) {
        throw std::length_error("timeline arm capacity overflow");
    }
    return static_cast<std::uint32_t>(total);
}

struct Engine;

struct ProjectGenerationReservationDomainV1 {
    Engine* liveEngine = nullptr;
    std::uint64_t epoch = 1;
    bool live = true;
};


struct Engine {
    // Outlives every preparation that holds physical relation backing.
    StableRowStorage<EnginePlayableMemberRelation> memberRelations;
    std::uint32_t freeMemberRelation = UINT32_MAX;
    std::uint32_t occupiedMemberRelations = 0;
    enum class OriginalPlayableServicePackDuty : std::uint8_t {
        Unbound, WorldNoPack, OriginalScratchOwns, OriginalDeferredRecordOwns,
        OriginalInvocationOwns, Released
    };
    class OriginalPlayableServiceLifetimePin;
    class OriginalPlayableServiceLifetimeHolder {
    public:
        OriginalPlayableServiceLifetimeHolder() noexcept = default;
        OriginalPlayableServiceLifetimeHolder(const OriginalPlayableServiceLifetimeHolder&) = delete;
        OriginalPlayableServiceLifetimeHolder& operator=(const OriginalPlayableServiceLifetimeHolder&) = delete;
        OriginalPlayableServiceLifetimeHolder(OriginalPlayableServiceLifetimeHolder&&) noexcept = default;
        OriginalPlayableServiceLifetimeHolder& operator=(OriginalPlayableServiceLifetimeHolder&&) noexcept = default;
        bool Valid() const noexcept { return engine_ != nullptr && reference_.Valid() && baseline_.Valid(); }
        bool Borrow(OriginalPlayableServiceLifetimePin* out) const noexcept;
        EnginePlayableServiceReference FactoryReference() const noexcept { return reference_; }
        OriginalPlayableServicePackDuty OriginalPackDuty() const noexcept { return packDuty_; }
        PackPartitionLease OriginalPackDutyLease() const noexcept { return packDutyLease_; }
        bool HasOriginalWorldNoPackDuty() const noexcept {
            return packDuty_ == OriginalPlayableServicePackDuty::WorldNoPack;
        }
        std::uint64_t OriginalWorldNoPackGeneration() const noexcept { return worldNoPackGeneration_; }
        VnObjectId OriginalWorldNoPackProject() const noexcept { return worldNoPackProject_; }
        const ActionObjectRefV1& OriginalWorldNoPackTarget() const noexcept { return worldNoPackTarget_; }
    private:
        friend struct Engine;
        friend struct detail::EngineOriginalServiceStorageTestAccess;
        Engine* engine_ = nullptr;
        EnginePlayableServiceRow* service_ = nullptr;
        EnginePlayableStartRow* invocation_ = nullptr;
        EnginePlayableServiceReference reference_{};
        EngineActionPlayStorage* preparingStorage_ = nullptr;
        std::uint64_t preparingStorageGeneration_ = 0;
        EnginePlayablePartitionPool::OriginalStoragePin baseline_;
        OriginalPlayableServicePackDuty packDuty_ = OriginalPlayableServicePackDuty::Unbound;
        const EngineOriginalPackIdentityLease* packDutyOwner_ = nullptr;
        PackPartitionLease packDutyLease_{};
        std::size_t packDutyNativeRow_ = SIZE_MAX;
        std::uint64_t packDutyNativeToken_ = 0;
        std::uint64_t packDutyNativeIncarnation_ = 0;
        ActionDeferredRecordToken packDutyDeferredRecord_{};
        std::uint64_t worldNoPackGeneration_ = 0;
        VnObjectId worldNoPackProject_{};
        std::uint64_t worldNoPackFingerprint_ = 0;
        ActionObjectRefV1 worldNoPackTarget_{};
        void ClearIdentity() noexcept {
            engine_ = nullptr;
            service_ = nullptr;
            invocation_ = nullptr;
            reference_ = {};
            preparingStorage_ = nullptr;
            preparingStorageGeneration_ = 0;
            packDuty_ = OriginalPlayableServicePackDuty::Unbound;
            packDutyOwner_ = nullptr;
            packDutyLease_ = {};
            packDutyNativeRow_ = SIZE_MAX;
            packDutyNativeToken_ = 0;
            packDutyNativeIncarnation_ = 0;
            packDutyDeferredRecord_ = {};
            worldNoPackGeneration_ = 0;
            worldNoPackProject_ = {};
            worldNoPackFingerprint_ = 0;
            worldNoPackTarget_ = {};
        }
    };
    class OriginalPlayableServiceLifetimePin {
    public:
        OriginalPlayableServiceLifetimePin() noexcept = default;
        OriginalPlayableServiceLifetimePin(const OriginalPlayableServiceLifetimePin&) = delete;
        OriginalPlayableServiceLifetimePin& operator=(const OriginalPlayableServiceLifetimePin&) = delete;
        OriginalPlayableServiceLifetimePin(OriginalPlayableServiceLifetimePin&& other) noexcept {
            *this = std::move(other);
        }
        OriginalPlayableServiceLifetimePin& operator=(OriginalPlayableServiceLifetimePin&& other) noexcept {
            if (this != &other) {
                Return();
                engine_ = other.engine_;
                original_ = other.original_;
                reference_ = other.reference_;
                partition_ = other.partition_;
                storage_ = std::move(other.storage_);
                other.engine_ = nullptr;
                other.original_ = nullptr;
                other.reference_ = {};
                other.partition_ = {};
            }
            return *this;
        }
        ~OriginalPlayableServiceLifetimePin() { Return(); }
        bool Valid() const noexcept;
        bool Borrow(OriginalPlayableServiceLifetimePin* out) const noexcept;
        EnginePlayableServiceReference FactoryReference() const noexcept {
            return Valid() ? reference_ : EnginePlayableServiceReference{};
        }
        void Return() noexcept {
            engine_ = nullptr;
            original_ = nullptr;
            reference_ = {};
            partition_ = {};
            storage_.Return();
        }
    private:
        friend struct Engine;
        Engine* engine_ = nullptr;
        const OriginalPlayableServiceLifetimeHolder* original_ = nullptr;
        EnginePlayableServiceReference reference_{};
        EnginePlayablePartitionToken partition_{};
        EnginePlayablePartitionPool::OriginalStoragePin storage_;
    };
    struct OriginalPendingOrdinarySelectionHolder {
        Engine* engine = nullptr;
        EngineActionPlayStorage* storage = nullptr;
        std::uint64_t generation = 0;
        EnginePlayableServiceReference reference{};
        OriginalPendingVariableProducerRetention::Pin producer;
        OriginalPlayableServiceLifetimePin service;
        OriginalPlayableServiceLifetimePin resource;
        EnginePlayableServiceReference resourceReference{};
        PlayableInstanceTable::OriginalLifetimePin root;
        OriginalSourceLifetimePin source;
        VoicePool::OriginalSourceBodyLifetimePin body;
        TimelineArmToken arm{};
        VoiceReleaseObserver release{};
        OriginalSourceSubmissionRegistry::PendingInstalledSourceRetirementReceipt retirement;
        EngineActionPlayScratch::NativeVoiceSide nativeReplacement;
        CapturedTimelineVoiceInstall replacementVoice;
        StableStoragePreparation nativeVoiceBacking;
        VoiceLedger::BackingPreparation nativeLedgerBacking;
        VoiceLedger::MonitorSelectionBacking nativeMonitorBacking;
        StableSlotStorage<EngineActionPlayScratch>::Preparation nativePlayBacking;
        StableSlotStorage<EnginePlayableMemberRecord>::Preparation nativeMemberBacking;
        StableStoragePreparation nativeRelationBacking;
        StableSlotStorage<EngineQueueProgramBank>::Preparation nativeProgramBacking;
        EngineQueueScheduleStorage::Preparation nativeScheduleBacking;
        EngineFrontierAttemptToken replacement{};
        std::array<EnginePlayableServiceReference, kSelectionPathNodeCapacity> retiredServices{};
        std::uint32_t retiredServiceCount = 0;
        PlayableInstanceHandle retiredChild = 0;
        std::uint64_t retiredChildEpoch = 0;
        bool sourceRetired = false;
        bool accepted = false;

        bool Empty() const noexcept {
            return engine == nullptr && storage == nullptr && generation == 0 && !reference.Valid() &&
                !producer.Valid() && !service.Valid() && !resource.Valid() && !resourceReference.Valid() &&
                !root.Valid() && !source.Valid() &&
                !body.Valid() && !arm.Valid() && !release.Valid() && !retirement.Valid() &&
                nativeReplacement.Empty() && !replacementVoice.HasStorage() &&
                !nativeVoiceBacking.Valid() && nativeLedgerBacking.Empty() && !nativeMonitorBacking.Valid() &&
                !nativePlayBacking.Valid() &&
                !nativeMemberBacking.Valid() && !nativeRelationBacking.Valid() &&
                !nativeProgramBacking.Valid() && !nativeScheduleBacking.Valid() &&
                !replacement.Valid() && retiredServiceCount == 0 && retiredChild == 0 &&
                retiredChildEpoch == 0 && !sourceRetired && !accepted;
        }
    };

    struct NativeReplacementSlot {
        EngineActionPlayScratch::NativeVoiceSide side;
        EngineFrontierAttemptToken borrower{};
        std::uint32_t producerOrdinal = UINT32_MAX;
        std::uint32_t capacity = 0;
    };

    struct QueueRoleReservationSlot {
        enum class State : std::uint32_t { Available, Borrowed, Consumed };
        EngineQueueRoleDirectory directory;
        std::vector<EngineActionPlayScratch::PendingVariableReplacementControl> controls;
        std::vector<Plan03GroupSliceIdentity> groups;
        std::vector<EngineActionPlayScratch::PendingVariableReplacementQueueLane> lanes;
        QueueCheckpointTable::BackingPreparation checkpoints;
        EngineFrontierAttemptToken borrower{};
        std::uint32_t capacity = 0;
        std::uint32_t controlCapacity = 0;
        State state = State::Available;
        std::size_t LaneCapacity() const noexcept {
            return std::min<std::size_t>(capacity, EngineActionPlayScratch::kPendingVariableReplacementQueueCapacity);
        }
        std::size_t GroupCapacity() const noexcept { return controlCapacity + 1u + 4u * LaneCapacity(); }
        bool ReplacementStorageEmpty() const noexcept {
            return controls.empty() && groups.empty() && lanes.empty() && !checkpoints.Valid();
        }
        bool ReplacementStorageComplete() const noexcept {
            return controlCapacity >= 2u &&
                controlCapacity <= EngineActionPlayScratch::kPendingVariableReplacementControlCapacity &&
                controls.size() == controlCapacity && groups.size() == GroupCapacity() && lanes.size() == LaneCapacity() &&
                checkpoints.Valid();
        }
    };

    struct OriginalPendingVariableGroupHolder;
    struct PendingVariableSelectionSnapshot {
        std::array<ContinuousGridWeight, 64> weights{};
        DiscreteGridSelection discrete{};
        std::uint32_t weightCount = 0;
        bool discreteMatched = false;
    };
    struct NativeRuntimeBacking {
        struct RootInitialArms {
            EnginePlayableServiceToken service{};
            EngineVariableInitialArms arms;
        };
        struct RootMemberWeights {
            EnginePlayableServiceToken service{};
            EngineVariableMemberWeights weights;
        };
        std::vector<RootMemberWeights> rootMemberWeights;
        std::vector<EngineVariableMemberWeights> memberWeights;
        std::vector<RootInitialArms> rootInitialArms;
        std::vector<EngineVariableInitialArms> initialArms;
        std::vector<EngineActionPlayScratch*> reprepareProducers;
        std::vector<PendingVariableSelectionSnapshot> selections;
        std::vector<EngineActionPlayScratch*> publicationProducers;
        std::vector<EngineActionPlayScratch*> publicationChildren;
        std::vector<OriginalPendingVariableGroupHolder*> publicationOwners;
        DecisionTable::BackingPreparation decisions;
        CompositeCompletionOwnerPool::BackingPreparation completions;
        StableStoragePreparation voices;
        VoiceLedger::BackingPreparation ledger;
        VoiceLedger::MonitorSelectionBacking monitor;
        SelectionMemoryTableV1::BackingPreparation selectionHistory;
        StableStoragePreparation arms;
        ArmGroupTable::BackingPreparation groups;
        ScheduledTransactionPool::ImmediateLaneBackingPreparation laneGroups;
        StableStoragePreparation laneEntries;
        StableStoragePreparation laneAttempts;
        StableStoragePreparation services;
        StableSlotStorage<EngineActionPlayScratch>::Preparation actionPlays;
        StableSlotStorage<EngineMemberProgramRecord>::Preparation memberPrograms;
        StableSlotStorage<EnginePlayableMemberRecord>::Preparation members;
        StableStoragePreparation relations;
        StableSlotStorage<EngineQueueProgramBank>::Preparation queueBanks;
        EngineQueueScheduleStorage::Preparation queueSchedules;
        QueueInstanceTable::BackingPreparation queues;
        QueueCheckpointTable::BackingPreparation checkpoints;
        VariableServiceV1::ReadCutBackingPreparation readCuts;
        StableStoragePreparation queueOrigins;
        EffectPoolBank::BackingPreparation effects;
        ParamSystem::BackingPreparation bindings;
    };
    NativeRuntimeBacking* nativeRuntimeBackingInUse = nullptr;
    struct NativeRuntimeBackingUse {
        std::array<StableStoragePreparedUse, 13> columns;
        VoiceLedger::BackingUse ledger;
        VoiceLedger::MonitorSelectionUse monitor;
        SelectionMemoryTableV1::BackingUse selectionHistory;
        ArmGroupTable::PreparedUse groups;
        ScheduledTransactionPool::ImmediateLaneBackingUse laneGroups;
        StableSlotStorage<EngineActionPlayScratch>::PreparedUse actionPlays;
        StableSlotStorage<EngineMemberProgramRecord>::PreparedUse memberPrograms;
        StableSlotStorage<EnginePlayableMemberRecord>::PreparedUse members;
        StableStoragePreparedUse relations;
        StableSlotStorage<EngineQueueProgramBank>::PreparedUse queueBanks;
        EngineQueueScheduleStorage::PreparedUse queueSchedules;
        QueueInstanceTable::BackingUse queues;
        QueueCheckpointTable::BackingUse checkpoints;
        VariableServiceV1::ReadCutBackingUse readCuts;
        EffectPoolBank::BackingUse effects;
        ParamSystem::PreparedUse bindings;
        Engine& owner;
        NativeRuntimeBacking* previous;
        NativeRuntimeBackingUse(Engine& engine, NativeRuntimeBacking* backing) noexcept
            : columns{Use(backing, 0), Use(backing, 1), Use(backing, 2), Use(backing, 3),
                      Use(backing, 4), Use(backing, 5), Use(backing, 6), Use(backing, 7), Use(backing, 8),
                      Use(backing, 9), Use(backing, 10), Use(backing, 11), Use(backing, 12)},
              ledger(backing == nullptr ? VoiceLedger::BackingUse{} : backing->ledger.Use()),
              monitor(backing == nullptr ? VoiceLedger::MonitorSelectionUse{} : backing->monitor.Use()),
              selectionHistory(backing == nullptr ? SelectionMemoryTableV1::BackingUse{} : backing->selectionHistory.Use()),
              groups(engine.armGroups, backing == nullptr ? nullptr : &backing->groups),
              laneGroups(engine.timelinePool, backing == nullptr ? nullptr : &backing->laneGroups),
              actionPlays(backing == nullptr ? StableSlotStorage<EngineActionPlayScratch>::PreparedUse{}
                  : backing->actionPlays.Use()),
              memberPrograms(backing == nullptr ? StableSlotStorage<EngineMemberProgramRecord>::PreparedUse{}
                  : backing->memberPrograms.Use()),
              members(backing == nullptr ? StableSlotStorage<EnginePlayableMemberRecord>::PreparedUse{}
                  : backing->members.Use()),
              relations(backing == nullptr ? StableStoragePreparedUse{} : backing->relations.Use()),
              queueBanks(backing == nullptr ? StableSlotStorage<EngineQueueProgramBank>::PreparedUse{}
                  : backing->queueBanks.Use()),
              queueSchedules(backing == nullptr ? EngineQueueScheduleStorage::PreparedUse{}
                  : backing->queueSchedules.Use()),
              queues(backing == nullptr ? nullptr : &backing->queues),
              checkpoints(backing == nullptr ? QueueCheckpointTable::BackingUse{} : backing->checkpoints.Use()),
              readCuts(backing == nullptr ? VariableServiceV1::ReadCutBackingUse{}
                  : backing->readCuts.Use()),
              effects(EffectPoolBank::UseBacking(backing == nullptr ? nullptr : &backing->effects)),
              bindings(engine.params, backing == nullptr ? nullptr : &backing->bindings),
              owner(engine), previous(std::exchange(engine.nativeRuntimeBackingInUse, backing)) {}
        ~NativeRuntimeBackingUse() { owner.nativeRuntimeBackingInUse = previous; }
    private:
        static StableStoragePreparedUse Use(NativeRuntimeBacking* backing, std::uint32_t index) noexcept {
            if (backing == nullptr) return {};
            if (index < 3u) return backing->decisions[index].Use();
            if (index < 7u) return backing->completions[index - 3u].Use();
            if (index == 7u) return backing->voices.Use();
            if (index == 8u) return backing->queueOrigins.Use();
            if (index == 9u) return backing->laneEntries.Use();
            if (index == 10u) return backing->laneAttempts.Use();
            if (index == 12u) return backing->services.Use();
            return backing->arms.Use();
        }
    };
    struct OriginalPendingVariableLeafSource {
        enum class Domain : std::uint8_t { Unbound, Current, QueueFuture };
        OriginalSourceLifetimePin source;
        VoicePool::OriginalSourceBodyLifetimePin body;
        TimelineArmToken arm{};
        VnVoiceHandle sourceIdentity = 0;
        ProgramGeneration programGeneration = 0;
        PlayableInstanceHandle parent = 0;
        std::uint64_t preparedStartFrame = 0;
        VoiceReleaseObserver physicalRelease{};
        OriginalPendingVariableGroupHolder* group = nullptr;
        VariableReadCutRef cut{};
        std::uint32_t leafIndex = UINT32_MAX;
        Domain domain = Domain::Unbound;
        bool Empty() const noexcept { return !source.Valid() && !body.Valid() && group == nullptr; }
    };
    struct OriginalPendingVariableFutureSource {
        OriginalPendingVariableGroupHolder* group = nullptr;
        EnginePlayableServiceReference service{};
        EnginePlayablePartitionToken partition{};
        QueueInstanceToken queue{};
        TimelineArmToken arm{};
        ParentAudioGroupRef parentGroup{};
        std::uint16_t memberIndex = UINT16_MAX;
        std::uint32_t ordinal = UINT32_MAX;
        VnVoiceHandle sourceIdentity = 0;
        ProgramGeneration programGeneration = 0;
        PlayableInstanceHandle parent = 0;
        std::uint64_t epoch = 0;
        std::uint64_t preparedStartFrame = 0;
        VoiceReleaseObserver release{};
        OriginalSourceSubmissionRegistry::PendingInstalledSourceRetirementReceipt retirement;
        bool retained = false;
        bool Empty() const noexcept { return group == nullptr && !retirement.Valid() && !retained; }
    };
    struct OriginalPendingVariableQueueNativeOrigin {
        enum class RecordKind : std::uint8_t { None, Continuation, Future, Singleton };
        enum class Role : std::uint8_t { Primary, Current, Successor };
        OriginalPendingVariableGroupHolder* group = nullptr;
        EnginePlayableServiceReference service{};
        EnginePlayablePartitionToken partition{};
        QueueInstanceToken queue{};
        Plan03GroupSliceIdentity identity{};
        OriginalQueueFutureGroupScope scope{};
        std::uint64_t attemptGeneration = 0;
        std::uint64_t recordGeneration = 0;
        std::uint32_t recordSlot = UINT32_MAX;
        std::uint32_t recordNext = UINT32_MAX;
        std::uint32_t ordinal = UINT32_MAX;
        std::size_t memberOffset = SIZE_MAX;
        RecordKind recordKind = RecordKind::None;
        Role role = Role::Primary;
        Plan03GroupSliceIdentity prerequisite{};
        EnginePlayableServiceReference prerequisiteService{};
        FrontierEntryReceiptRef prerequisiteEntry{};
        bool disposed = false;
        bool Empty() const noexcept { return group == nullptr && !identity.Valid(); }
    };
    struct OriginalPendingVariableFutureNode {
        OriginalPendingVariableGroupHolder* group = nullptr;
        EnginePlayableServiceReference service{};
        EnginePlayablePartitionToken partition{};
        QueueInstanceToken queue{};
        EngineFrontierAttemptToken preparation{};
        PlayableInstanceHandle handle = 0;
        PlayableInstanceHandle parent = 0;
        PlayableInstanceHandle futureRoot = 0;
        std::uint64_t epoch = 0;
        PlayableKind kind = PlayableKind::Sound;
        std::uint32_t ordinal = UINT32_MAX;
        bool disposed = false;
        bool Empty() const noexcept { return group == nullptr && handle == 0; }
    };
    struct OriginalPendingVariableGroupHolder {
        OriginalPendingVariableGroupHolder() noexcept = default;
        OriginalPendingVariableGroupHolder(const OriginalPendingVariableGroupHolder&) = delete;
        OriginalPendingVariableGroupHolder& operator=(const OriginalPendingVariableGroupHolder&) = delete;
        OriginalPendingVariableGroupHolder(OriginalPendingVariableGroupHolder&& other) noexcept {
            *this = std::move(other);
        }
        OriginalPendingVariableGroupHolder& operator=(OriginalPendingVariableGroupHolder&& other) noexcept {
            if (this != &other) {
                const bool moved = other.MoveTo(*this);
                assert(moved);
                (void)moved;
            }
            return *this;
        }
        bool MoveTo(OriginalPendingVariableGroupHolder& receiver) noexcept {
            if (&receiver == this || !receiver.Empty() || initialSelectionBatch.ReplacementPending() ||
                (engine == nullptr ? !Empty() : !engine->CanMovePendingVariableGroupSources(*this))) return false;
            // Transfer the only fallible owner before moving any group identity or loan.
            if (!initialSelectionBatch.MoveTo(receiver.initialSelectionBatch)) return false;
            receiver.MoveGroupStateFrom(*this);
            if (receiver.engine != nullptr) receiver.engine->RebindMovedPendingVariableGroupSources(*this, receiver);
            return true;
        }
    private:
        void MoveGroupStateFrom(OriginalPendingVariableGroupHolder& other) noexcept {
            engine = std::exchange(other.engine, nullptr);
            cut = std::exchange(other.cut, VariableReadCutRef{});
            producer = std::move(other.producer);
            service = std::move(other.service);
            root = std::move(other.root);
            reference = std::exchange(other.reference, EnginePlayableServiceReference{});
            accepted = std::exchange(other.accepted, false);
            leafArms.swap(other.leafArms);
            leafRetained.swap(other.leafRetained);
            leafCount = std::exchange(other.leafCount, 0u);
            futureSourceCount = std::exchange(other.futureSourceCount, 0u);
            futureNodeCount = std::exchange(other.futureNodeCount, 0u);
            queueNativeOriginCount = std::exchange(other.queueNativeOriginCount, 0u);
            queueNativeMembers.swap(other.queueNativeMembers);
            leafRetirement = std::exchange(other.leafRetirement, LeafRetirement::None);
            reselectionRequested = std::exchange(other.reselectionRequested, false);
            nextSelectionFrame = std::exchange(other.nextSelectionFrame, 0);
            selectionReservations = std::move(other.selectionReservations);
            nativeReplacementSlots = std::move(other.nativeReplacementSlots);
            queueRoleReservations = std::move(other.queueRoleReservations);
            nativeRuntimeBacking = std::move(other.nativeRuntimeBacking);
            leafRetirementReceipts.swap(other.leafRetirementReceipts);
            initialSides = std::move(other.initialSides);
            initialDecisionInputs = std::exchange(other.initialDecisionInputs, {});
            nestedInitialSides.swap(other.nestedInitialSides);
            nestedInitialSideCount = std::exchange(other.nestedInitialSideCount, 0u);
            topology_ = std::exchange(other.topology_, {});
            retiredDecisions = std::exchange(other.retiredDecisions, {});
            reprepare_ = std::exchange(other.reprepare_, {});
            boundAbort = std::exchange(other.boundAbort, {});
        }
    public:
        bool Empty() const noexcept {
            return engine == nullptr && !cut.Valid() && !producer.Valid() &&
                !service.Valid() && !root.Valid() && leafCount == 0 && futureSourceCount == 0 && futureNodeCount == 0 && queueNativeOriginCount == 0 &&
                leafRetirement == LeafRetirement::None &&
                std::none_of(leafRetirementReceipts.begin(), leafRetirementReceipts.end(),
                    [](const auto& receipt) { return receipt.Valid(); }) &&
                !retiredDecisions[0].token.Valid() && !retiredDecisions[1].token.Valid() &&
                !initialSides[0].Valid() && !initialSides[1].Valid() && reprepare_.engine == nullptr &&
                !reprepare_.replacementCommandsReserved && reprepare_.replacementCommandCount == 0 &&
                !reprepare_.replacementLane.Valid() && reprepare_.replacementDemand.events == 0 &&
                !selectionReservations[0].Valid() && !selectionReservations[1].Valid() && !topology_.sealed &&
                nativeReplacementSlots.empty() && queueRoleReservations.empty() && !nativeRuntimeBacking &&
                leafArms.capacity() == 0 && leafRetained.capacity() == 0 &&
                leafRetirementReceipts.capacity() == 0 && reprepare_.StorageBytes() == 0 &&
                boundAbort.queues.capacity() == 0 &&
                topology_.nodes.capacity() == 0 && topology_.topNodes.capacity() == 0 &&
                topology_.leafNodes.capacity() == 0 && topology_.leafTops.capacity() == 0 &&
                queueNativeMembers.capacity() == 0 &&
                nestedInitialSideCount == 0 && nestedInitialSides.capacity() == 0 &&
                initialSelectionBatch.Empty() && !boundAbort.captured;
        }
        bool Close(Engine& receiver) noexcept {
            if (Empty()) return true;
            if (engine != &receiver || !cut.Valid() || !producer.Valid() ||
                !receiver.CanClosePendingVariableCohortGroup(*this) ||
                !receiver.CancelPendingVariableGroupChildren(*this) ||
                !receiver.variables.CanReleaseReadCut(cut) ||
                !receiver.ReclaimPendingVariableGroupDecisions(*this) ||
                !receiver.ReturnPendingVariableGroupLeafSources(*this) ||
                !receiver.ReturnDisposedPendingVariableQueueNativeOrigins(*this)) return false;
            receiver.ReturnPendingVariableGroupFutureNodes(*this);
            for (auto& reservation : selectionReservations)
                if (reservation.Valid() && !receiver.decisions.CancelPreparedReservation(&reservation)) return false;
            if (!initialSelectionBatch.Cancel()) return false;
            if (!receiver.ReturnPendingSelectionCensusProducer(producer, reference)) return false;
            if (!receiver.ReleasePendingVariableNativeBacking(*this)) return false;
            if (!receiver.variables.ReleaseReadCut(cut)) return false;
            cut = {};
            for (auto& side : initialSides) side.Release();
            for (auto& side : nestedInitialSides) side.original.Release();
            std::vector<NestedInitialSide>().swap(nestedInitialSides);
            nestedInitialSideCount = 0;
            producer.Return();
            service.Return();
            root.Release();
            reference = {};
            initialDecisionInputs = {};
            topology_ = {};
            accepted = false;
            reselectionRequested = false;
            nextSelectionFrame = 0;
            reprepare_ = {};
            boundAbort = {};
            engine = nullptr;
            return true;
        }
        Engine* engine = nullptr;
        VariableReadCutRef cut{};
        OriginalPendingVariableProducerRetention::Pin producer;
        OriginalPlayableServiceLifetimePin service;
        PlayableInstanceTable::OriginalLifetimePin root;
        EnginePlayableServiceReference reference{};
        bool accepted = false;
        std::vector<TimelineArmToken> leafArms;
        std::vector<std::uint8_t> leafRetained;
        std::uint32_t leafCount = 0;
        std::uint32_t futureSourceCount = 0;
        std::uint32_t futureNodeCount = 0;
        std::uint32_t queueNativeOriginCount = 0;
        std::vector<TimelineArmToken> queueNativeMembers;
        enum class LeafRetirement : std::uint8_t { None, Requesting, Settled, Failed };
        LeafRetirement leafRetirement = LeafRetirement::None;
        bool reselectionRequested = false;
        std::uint64_t nextSelectionFrame = 0;
        EnginePendingSelectionBatch initialSelectionBatch;
        std::array<PreparedDecisionReservation, 2> selectionReservations;
        std::vector<NativeReplacementSlot> nativeReplacementSlots;
        std::vector<QueueRoleReservationSlot> queueRoleReservations;
        std::unique_ptr<NativeRuntimeBacking> nativeRuntimeBacking;
        std::vector<OriginalSourceSubmissionRegistry::PendingInstalledSourceRetirementReceipt>
            leafRetirementReceipts;
        std::array<VariableControlInstanceTable::OriginalInitialSideReceipt, 2> initialSides{};
        std::array<DecisionReserveRequest, 2> initialDecisionInputs{};
        struct NestedInitialSide {
            EnginePlayableServiceReference service{};
            VariableControlInstanceToken instance{};
            VariableControlSide side = VariableControlSide::Discrete;
            DecisionToken decision{};
            DecisionReserveRequest input{};
            BankVersionToken bank{};
            ArmGroupToken armGroup{};
            Plan03GroupSliceIdentity group{};
            std::uint32_t memberCount = 0;
            VariableControlInstanceTable::OriginalInitialSideReceipt original;
            bool sideReturned = false;
            bool decisionReturned = false;
        };
        std::vector<NestedInitialSide> nestedInitialSides;
        std::uint32_t nestedInitialSideCount = 0;
        struct RetiredDecision {
            DecisionToken token{};
            DecisionReserveRequest input{};
            LogicCommitSequence sequence = 0;
        };
        std::array<RetiredDecision, 2> retiredDecisions{};
        bool RepreparePending() const noexcept { return reprepare_.engine != nullptr; }
        bool ReprepareReady() const noexcept { return reprepare_.phase == InitialReprepare::Phase::MembersReturned; }
        struct QueueRetirement {
            EnginePlayableServiceReference service{};
            std::uint32_t recordSlot = UINT32_MAX;
            std::uint32_t recordNext = UINT32_MAX;
            std::uint64_t recordGeneration = 0;
            bool continuation = true;
            bool controlReturned = false;
            DecisionToken initialDecision{};
            ArmGroupToken initialGroup{};
            bool canonicalInitial = false;
            bool initialCommitted = false;
            bool initialTransferred = false;
            bool readersReturned = false;
            bool recordReturned = false;
            bool physicalReturned = false;
            bool decisionsReturned = false;
            bool factoriesReturned = false;
        };
        struct BoundAbort {
            const EngineImmediateEntryAttempt* attempt = nullptr;
            EngineActionPlayStorage* storage = nullptr;
            std::uint64_t attemptGeneration = 0;
            std::uint64_t storageGeneration = 0;
            bool captured = false;
            std::vector<QueueRetirement> queues;
            std::uint32_t queueCount = 0;
        } boundAbort;
    private:
        friend struct Engine;
        struct InitialTopology {
            struct Node {
                PlayableInstanceHandle handle = 0;
                PlayableInstanceHandle parent = 0;
                std::uint64_t epoch = 0;
                PlayableKind kind = PlayableKind::Sound;
                PlayableInstanceTable::OriginalLifetimePin loan;
                bool nativeDisposition = false;
            };
            std::vector<Node> nodes;
            std::vector<std::uint32_t> topNodes;
            std::vector<std::uint32_t> leafNodes;
            std::vector<std::uint32_t> leafTops;
            std::uint32_t nodeCount = 0, structuralCount = 0, topCount = 0, leafCount = 0;
            bool sealed = false;
        } topology_;
        struct InitialReprepare {
            enum class Phase : std::uint8_t {
                None, CohortCaptured, Issued, LaneReturned, GroupsReturned, ChildrenReturned, MemberStorageReturned, MembersReturned
            };
            enum class CohortPhase : std::uint8_t { None, Captured, LaneCancelRequested, LaneReturned, GroupsReturned, Closing };
            Engine* engine = nullptr;
            EngineActionPlayStorage* storage = nullptr;
            const EngineImmediateEntryAttempt* attempt = nullptr;
            std::uint64_t storageGeneration = 0;
            std::uint64_t attemptGeneration = 0;
            std::uint64_t producerMarker = 0;
            VariableReadCutRef cut{};
            OriginalPendingVariableGroupHolder* cohortHead = nullptr;
            EngineActionPlayStorage* cohortNextStorage = nullptr;
            EngineFrontierAttemptToken cohortNext{};
            std::uint32_t cohortOrdinal = UINT32_MAX;
            std::uint32_t cohortCount = 0;
            CohortPhase cohortPhase = CohortPhase::None;
            ImmediateLaneHandle lane{};
            ImmediateLaneDemand demand{};
            DecisionToken coordinator{};
            DecisionReserveRequest coordinatorInput{};
            ArmGroupToken coordinatorGroup{};
            std::vector<DecisionToken> decisions;
            std::vector<ArmGroupToken> armGroups;
            std::vector<Plan03GroupSliceIdentity> nativeGroups;
            std::vector<PlayableInstanceHandle> children;
            std::vector<std::uint64_t> childEpochs;
            std::uint32_t nativeGroupCount = 0;
            std::uint32_t childCount = 0;
            std::vector<std::uint8_t> armGroupsReturned;
            bool coordinatorGroupReturned = false;
            bool coordinatorReturned = false;
            std::vector<QueueRetirement> queues;
            std::uint32_t queueCount = 0;
            std::vector<std::uint8_t> childrenReturned;
            struct MemberStorage {
                EnginePlayableMemberReference reference{};
                EngineMemberProgramToken program{};
                std::size_t logicalSlot = SIZE_MAX;
                std::size_t physicalSlot = SIZE_MAX;
                TimelineArmToken arm{};
                VnVoiceHandle render = 0;
                VoiceReleaseObserver release{};
                SourceTerminalToken source{};
            };
            std::vector<MemberStorage> members;
            bool membersCaptured = false;
            EngineFrontierAttemptToken replacementChildren{};
            CommandQueue::BatchReservation replacementCommandReservation{};
            std::uint32_t replacementCommandCount = 0;
            bool replacementCommandsReserved = false;
            bool reserveForReplacement = true;
            ImmediateLaneHandle replacementLane{};
            ImmediateLaneDemand replacementDemand{};
            Phase phase = Phase::None;
            std::size_t StorageBytes() const noexcept {
                return decisions.capacity() * sizeof(DecisionToken) +
                    armGroups.capacity() * sizeof(ArmGroupToken) +
                    nativeGroups.capacity() * sizeof(Plan03GroupSliceIdentity) +
                    children.capacity() * sizeof(PlayableInstanceHandle) +
                    childEpochs.capacity() * sizeof(std::uint64_t) +
                    armGroupsReturned.capacity() * sizeof(std::uint8_t) +
                    queues.capacity() * sizeof(QueueRetirement) +
                    childrenReturned.capacity() * sizeof(std::uint8_t) +
                    members.capacity() * sizeof(MemberStorage);
            }
        } reprepare_;
    };
    struct PendingVariableGroupChild {
        TimelineVoiceInstallDesc voice{};
        PlayableRefV1 leaf{};
        std::uint32_t frontierPolicyIndex = UINT32_MAX;
        std::uint32_t authoredChildOrdinal = 0;
        WeightSegmentV1 weight{};
        std::uint64_t authoredOffset = 0;
        VariableControlSide side = VariableControlSide::Discrete;
        std::uint32_t topIndex = UINT32_MAX;
    };
    using PendingVariableGroupTop = EngineActionPlayScratch::PendingVariableReplacementTop;

    struct NativeBackingReturnPlan {
        EngineActionPlayScratch::NativeVoiceSide* current = nullptr;
        EngineFrontierAttemptToken* currentBorrower = nullptr;
        std::uint32_t* currentCapacity = nullptr;
        EngineActionPlayScratch::NativeVoiceSide* successor = nullptr;
        EngineFrontierAttemptToken* successorBorrower = nullptr;
        std::uint32_t* successorCapacity = nullptr;
        QueueRoleReservationSlot* queue = nullptr;
    };
    bool AllowNativeBackingAllocation() noexcept {
        ++nativeBackingAllocationCountForTest;
        if (forcePreparedScheduleAllocationFailureForTest || nativeBackingAllocationFailureCountdownForTest == 0)
            return false;
        if (nativeBackingAllocationFailureCountdownForTest > 0) --nativeBackingAllocationFailureCountdownForTest;
        return true;
    }
    template <typename Values>
    bool PrepareVariableControlWeightColumn(EnginePlayableServiceRow& row, std::uint32_t count,
        std::shared_ptr<Values> EngineVariableMemberWeights::*column) noexcept {
        if (count == 0 && !row.HasFeatureArrays()) return true;
        auto& storage = row.VariablePayload().variableControlMemberWeights.*column;
        if (count <= (storage ? storage->size() : 0)) return true;
        if (memguard::IsAudioTagged()) return false;
        if (nativeRuntimeBackingInUse != nullptr) {
            std::shared_ptr<Values>* selected = nullptr;
            for (auto& root : nativeRuntimeBackingInUse->rootMemberWeights) {
                if (root.service != row.identity) continue;
                auto& candidate = root.weights.*column;
                if (!candidate || candidate->size() < count) return false;
                selected = &candidate;
                break;
            }
            if (selected == nullptr) {
                for (auto& weights : nativeRuntimeBackingInUse->memberWeights) {
                    auto& candidate = weights.*column;
                    if (candidate.use_count() != 1 || candidate->size() < count) continue;
                    if (selected == nullptr || candidate->size() < (*selected)->size()) selected = &candidate;
                }
            }
            if (selected == nullptr) return false;
            std::fill((*selected)->begin(), (*selected)->end(), typename Values::value_type{});
            if (storage) std::copy(storage->begin(), storage->end(), (*selected)->begin());
            storage = *selected;
            return true;
        }
        try {
            if (!storage) {
                if (!AllowNativeBackingAllocation()) return false;
                storage = std::make_shared<Values>(count);
            } else {
                if (count > storage->capacity() && !AllowNativeBackingAllocation()) return false;
                storage->resize(count);
            }
        }
        catch (...) { return false; }
        return true;
    }
    bool PrepareVariableControlOwners(EnginePlayableServiceRow& row,
        std::uint32_t controllers, std::uint32_t containers) noexcept {
        if (controllers > kMaxKnownScheduleLeaves + 64u || containers > kMaxKnownScheduleLeaves + 64u)
            return false;
        if (controllers <= row.variableControlControllers.size() && containers <= row.variableControlContainers.size())
            return true;
        if (memguard::IsAudioTagged() || nativeRuntimeBackingInUse != nullptr) return false;
        try {
            const auto grow = [&](auto& column, std::uint32_t count) {
                if (count <= column.size()) return true;
                if (count > column.capacity() && !AllowNativeBackingAllocation()) return false;
                column.resize(count);
                return true;
            };
            return grow(row.variableControlControllers, controllers) && grow(row.variableControlContainers, containers);
        } catch (...) { return false; }
    }
    bool PrepareVariableControlMemberWeights(EnginePlayableServiceRow& row, std::uint32_t count) noexcept {
        return count <= config.maxVariableControlLiveRows &&
            PrepareVariableControlWeightColumn(row, count, &EngineVariableMemberWeights::storage);
    }
    bool PrepareVariableControlChildWeights(EnginePlayableServiceRow& row, std::uint32_t count) noexcept {
        return count <= kMaxKnownScheduleLeaves &&
            PrepareVariableControlWeightColumn(row, count, &EngineVariableMemberWeights::childStorage);
    }
    bool PrepareVariableControlInitialArms(EnginePlayableServiceRow& row, std::uint32_t count) noexcept {
        if (count > config.maxKnownScheduleLeaves + config.maxVariableControlLiveRows) return false;
        auto& arms = row.variableControlInitialArms;
        if (count <= arms.size()) return true;
        if (memguard::IsAudioTagged()) return false;
        if (nativeRuntimeBackingInUse != nullptr) {
            EngineVariableInitialArms* selected = nullptr;
            for (auto& root : nativeRuntimeBackingInUse->rootInitialArms) {
                if (root.service != row.identity) continue;
                if (root.arms.size() < count) return false;
                selected = &root.arms;
                break;
            }
            if (selected == nullptr) {
                for (auto& candidate : nativeRuntimeBackingInUse->initialArms) {
                    if (candidate.storage.use_count() != 1 || candidate.size() < count) continue;
                    if (selected == nullptr || candidate.size() < selected->size()) selected = &candidate;
                }
            }
            if (selected == nullptr) return false;
            std::fill(selected->storage->begin(), selected->storage->end(), TimelineArmToken{});
            if (arms.storage)
                std::copy(arms.storage->begin(), arms.storage->end(), selected->storage->begin());
            arms.storage = selected->storage;
            return true;
        }
        try {
            if (!arms.storage) {
                if (!AllowNativeBackingAllocation()) return false;
                arms.storage = std::make_shared<EngineVariableInitialArms::Values>(count);
            } else {
                if (count > arms.storage->capacity() && !AllowNativeBackingAllocation()) return false;
                arms.storage->resize(count);
            }
        }
        catch (...) { return false; }
        return true;
    }
    EngineFrontierAttemptToken NativeBackingTokenFor(const EngineActionPlayScratch& scratch) const noexcept;
    EngineActionPlayScratch* NativeBackingParentFor(const EngineActionPlayScratch& future) noexcept;
    OriginalPendingVariableGroupHolder* NativeBackingGroupFor(const EngineActionPlayScratch& child) noexcept;
    OriginalPendingVariableGroupHolder* NativeBackingOwnerFor(
        OriginalPendingVariableGroupHolder& group, std::uint32_t* ordinal) noexcept;
    bool PrepareNativeVoiceSide(EngineActionPlayScratch& scratch, bool successor, std::uint32_t shape) noexcept;
    void RecycleOrdinaryVoiceBackingAfterSafeReset(EngineActionPlayScratch& scratch) noexcept;
    bool AdoptPendingVariableQueueRoles(EngineActionPlayScratch& scratch) noexcept;
    bool PrepareNativeBackingReturn(const EngineActionPlayScratch& scratch, NativeBackingReturnPlan* plan) noexcept;
    void CommitNativeBackingReturn(EngineActionPlayScratch& scratch, const NativeBackingReturnPlan& plan) noexcept;
    void CommitNativeBackingTransferEnd(EngineActionPlayScratch& scratch, const NativeBackingReturnPlan& plan) noexcept;
    bool ReleasePendingVariableNativeBacking(OriginalPendingVariableGroupHolder& group) noexcept {
        for (const auto& slot : group.nativeReplacementSlots) if (slot.borrower.Valid()) return false;
        for (const auto& slot : group.queueRoleReservations) {
            if (slot.borrower.Valid() || slot.state == QueueRoleReservationSlot::State::Borrowed ||
                (slot.state == QueueRoleReservationSlot::State::Available
                    ? slot.directory.size() != slot.capacity :
                    slot.state != QueueRoleReservationSlot::State::Consumed || !slot.directory.Empty())) return false;
        }
        std::vector<NativeReplacementSlot>().swap(group.nativeReplacementSlots);
        std::vector<QueueRoleReservationSlot>().swap(group.queueRoleReservations);
        group.nativeRuntimeBacking.reset();
        return true;
    }
    bool EnsurePendingVariableNativeBacking(EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, bool* waiting) noexcept;
    bool ReserveRegisteredPendingVariableNativeBacking(EngineImmediateEntryAttempt& attempt, bool* waiting) noexcept;
    OriginalPendingVariableGroupHolder* PreparedPendingVariableNativeBackingGroup(
        EngineActionPlayScratch& producer, const EngineImmediateEntryAttempt& attempt) noexcept;
    bool PregrantPendingVariableCohortNativeBacking(const EngineImmediateEntryAttempt& attempt,
        std::span<const EngineFrontierAttemptToken> producers) noexcept;
    bool BuildPendingVariableNativeBacking(const EngineImmediateEntryAttempt& attempt,
        EngineActionPlayScratch* single, std::vector<NativeReplacementSlot>* slots,
        std::vector<QueueRoleReservationSlot>* queueSlots,
        std::unique_ptr<NativeRuntimeBacking>* runtimeBacking, bool* waiting,
        std::span<const EngineFrontierAttemptToken> preparedProducers = {}) noexcept;
    bool MatchesPendingVariableNativeBacking(OriginalPendingVariableGroupHolder& owner,
        std::uint32_t producerCount) noexcept;
private:
    class PendingVariableGroupReplacementScope {
        friend struct Engine;
        PendingVariableGroupReplacementScope(OriginalPendingVariableGroupHolder& holder,
            EngineActionPlayScratch& producer, const EngineImmediateEntryAttempt& attempt,
            const std::array<bool, 2>& sides, const std::array<DecisionToken, 2>& next,
            const std::array<DecisionTable::PreparedTicketCommitProof, 2>& proofs,
            const std::array<VariableControlInstanceTable::InitialSideReplacement, 2>& held) noexcept
            : group_(&holder), producer_(&producer), attempt_(&attempt),
              producerGeneration_(producer.storage.generation), attemptGeneration_(attempt.generation),
              cut_(holder.cut), sides_(sides), next_(next), proofs_(&proofs), held_(&held) {}
        PendingVariableGroupReplacementScope(const PendingVariableGroupReplacementScope&) = delete;
        PendingVariableGroupReplacementScope& operator=(const PendingVariableGroupReplacementScope&) = delete;
        OriginalPendingVariableGroupHolder* group_;
        EngineActionPlayScratch* producer_;
        const EngineImmediateEntryAttempt* attempt_;
        const std::uint64_t producerGeneration_, attemptGeneration_;
        const VariableReadCutRef cut_;
        const std::array<bool, 2> sides_;
        const std::array<DecisionToken, 2> next_;
        const std::array<DecisionTable::PreparedTicketCommitProof, 2>* proofs_;
        const std::array<VariableControlInstanceTable::InitialSideReplacement, 2>* held_;
    };
    const PendingVariableGroupReplacementScope* activePendingVariableGroupReplacement_ = nullptr;
    bool CapturePendingVariableGroupRetiredMemberStorage(OriginalPendingVariableGroupHolder& group,
        EnginePlayableServiceRow& row) noexcept;
    bool MatchesPendingVariableGroupRetiredMemberStorage(const OriginalPendingVariableGroupHolder& group,
        EnginePlayableServiceRow& row, std::uint32_t index) noexcept;
    bool CapturePendingVariableGroupTopology(EngineActionPlayScratch& scratch,
        const PlayableInstanceTable::PreparedParentLinksProof& links,
        OriginalPendingVariableGroupHolder& group) noexcept;
    bool CapturePendingVariableGroupFutureNodes(EngineActionPlayScratch& scratch,
        const PlayableInstanceTable::PreparedParentLinksProof& links,
        OriginalPendingVariableGroupHolder& group) noexcept;
    bool MatchesPendingVariableGroupFutureNodes(const OriginalPendingVariableGroupHolder& group,
        bool retiredSources = false) const noexcept;
    void ReturnPendingVariableGroupFutureNodes(OriginalPendingVariableGroupHolder& group) noexcept {
        for (auto& node : pendingVariableFutureNodes)
            if (node.group == &group) node = {};
        group.futureNodeCount = 0;
    }
    bool CapturePendingVariableReplacementTopology(EngineActionPlayScratch& child,
        std::span<const PendingVariableGroupChild> leaves) noexcept;
    bool MatchesPendingVariableReplacementTopology(const EngineActionPlayScratch& child,
        bool committed) noexcept;
    bool CapturePendingVariableReplacementFutureTopology(EngineActionPlayScratch& child) noexcept;
    bool MatchesPendingVariableReplacementFutureTopology(const EngineActionPlayScratch& child,
        bool committed) noexcept;
    bool MatchesPendingVariableReplacementLeafOrigin(const EngineActionPlayScratch& child,
        std::uint32_t index) noexcept;
    bool MatchesPendingVariableReplacementControl(const EngineActionPlayScratch& child,
        const OriginalPendingVariableGroupHolder& group, std::uint32_t index) noexcept;
    bool CapturePendingVariableReplacementControlOwner(const EngineActionPlayScratch& child,
        std::uint32_t index, FrontierOwnerIdentity* out) noexcept;
    bool MatchesPendingVariableReplacementBoundSide(const EngineActionPlayScratch& child,
        const OriginalPendingVariableGroupHolder& group, std::uint32_t index,
        const FrontierGroupBinding& binding) noexcept;
public:
    bool MatchesPendingVariableGroupTopology(const OriginalPendingVariableGroupHolder& group,
        const EnginePlayableServiceRow& row, bool retiredSources = false) noexcept;
    EngineActionPlayScratch* PreparePendingVariableGroupFactory(EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, std::uint64_t anchor, bool replaceSelection) noexcept;
    bool PreparePendingVariableGroupChildren(EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, std::uint64_t anchor,
        std::span<const PendingVariableGroupChild> leaves) noexcept;
    bool PreparePendingVariableGroupChildren(EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, std::uint64_t anchor,
        std::span<const PendingVariableGroupChild> leaves,
        std::span<const PendingVariableGroupTop> tops) noexcept;
    bool MatchesPendingVariableReplacementProducer(const EngineActionPlayScratch& child,
        const EngineActionPlayScratch& producer, const EngineImmediateEntryAttempt& attempt) noexcept;
    bool MatchesPendingVariableReplacementQueueIssuer(EngineActionPlayScratch& child,
        const EngineFrontierQueueOwnerContext& queue, ActionFiringHandle root,
        std::uint64_t attemptGeneration) noexcept;
    bool PreparePendingVariableReplacementQueueFutures(EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, EngineActionPlayScratch& child,
        VnResult& result, bool& programCapacityRefused) noexcept;
    const EngineActionPlayScratch* OriginalStagedPendingVariableCohortChildren(EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt) noexcept;
    bool PrepareRegisteredPendingVariableCohortCommandBatch(EngineImmediateEntryAttempt& attempt) noexcept;
    bool MatchesPendingVariableReplacementBatch(const EngineActionPlayScratch& child,
        const EngineActionPlayScratch& producer, const EngineImmediateEntryAttempt& attempt) noexcept;
    bool MatchesPendingVariableReplacementLane(const EngineActionPlayScratch& child,
        const EngineActionPlayScratch& producer, const EngineImmediateEntryAttempt& attempt) noexcept;
    bool CancelRegisteredPendingVariableCohortCommandBatch(const EngineImmediateEntryAttempt& attempt) noexcept;
    bool PrepareRegisteredPendingVariableCohortLane(EngineImmediateEntryAttempt& attempt,
        PrivateImmediateLanePlacementProof* placement) noexcept;
    bool CancelRegisteredPendingVariableCohortLane(EngineImmediateEntryAttempt& attempt) noexcept;
    bool PreparePendingVariableGroupLaneEvents(EngineActionPlayScratch& producer,
        EngineImmediateEntryAttempt& attempt, std::uint32_t firstEvent) noexcept;
    bool PreparePendingVariableReplacementQueueLaneGroups(EngineActionPlayScratch& child,
        EngineActionPlayScratch& producer, EngineImmediateEntryAttempt& attempt) noexcept;
    bool CapturePendingVariableReplacementQueueOwner(const EngineActionPlayScratch& child,
        const EnginePlayableServiceRow& service, FrontierOwnerIdentity* out) noexcept;
    bool ReturnPendingVariableReplacementQueueLaneReaders(EngineActionPlayScratch& child,
        EngineImmediateEntryAttempt& attempt) noexcept;
    bool CancelPendingVariableReplacementQueueLaneRecords(EngineActionPlayScratch& child,
        EngineImmediateEntryAttempt& attempt) noexcept;
    bool DeclarePendingVariableGroupLane(EngineActionPlayScratch& child,
        EngineImmediateEntryAttempt& attempt, std::uint32_t placedEventCount) noexcept;
    bool CanClosePendingVariableCohortGroup(const OriginalPendingVariableGroupHolder& group) noexcept;
    bool MatchesFinishedPendingVariableCohortSides(const EngineActionPlayScratch& child,
        EngineActionPlayScratch& producer, const EngineImmediateEntryAttempt& attempt,
        bool requireOpen = true) noexcept;
    bool MatchesPendingVariableReplacementMemberStorage(const EngineActionPlayScratch& child,
        std::uint32_t index) noexcept;
    bool PreparePendingVariableGroupLane(EngineActionPlayScratch& producer,
        EngineImmediateEntryAttempt& attempt, PrivateImmediateLanePlacementProof* placement) noexcept;
    const EngineActionPlayScratch* OriginalPreparedPendingVariableGroupChildren(EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement) noexcept;
    const EngineActionPlayScratch* OriginalCancelledPendingVariableGroupChildren(EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt) noexcept;
    bool FreezePreparedPendingVariableGroupChildren(EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement,
        bool* outWaiting = nullptr) noexcept;
    bool StagePreparedPendingVariableGroupChildren(EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement) noexcept;
    bool CommitPrivatePendingVariableGroupSources(EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement) noexcept;
    const EngineActionPlayScratch* OriginalPrivateCommittedPendingVariableGroupChildren(EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement) noexcept;
    bool CanCancelPendingVariableReplacementVoiceOwner(
        const EngineActionPlayScratch::PreparedTimelineVoiceOwnerContext& owner) noexcept;
    bool OwnsPrivatePendingVariableReplacementSourceTerminal(
        SourceTerminalToken token, const SourceTerminalRecord& source) noexcept;
    bool BindPrivatePendingVariableGroupEntry(EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement) noexcept;
    bool BindPrivatePendingVariableQueueEntries(EngineActionPlayScratch& child,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement) noexcept;
    bool CancelPrivatePendingVariableQueueEntries(EngineActionPlayScratch& child) noexcept;
    bool MatchesPrivatePendingVariableQueueEntries(const EngineActionPlayScratch& child,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement,
        std::uint32_t* eventCount, std::uint32_t* memberCount) noexcept;
    const EngineActionPlayScratch* OriginalEntryBoundPendingVariableGroupChildren(EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement) noexcept;
    bool PreparePendingVariableGroupInstallationPublication(EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement) noexcept;
    bool PublishPendingVariableGroupReplacement(EngineActionPlayScratch& producer,
        EngineImmediateEntryAttempt& attempt, PrivateImmediateLanePlacementProof& placement) noexcept;
    bool PublishRegisteredPendingVariableCohortReplacement(EngineImmediateEntryAttempt& attempt,
        PrivateImmediateLanePlacementProof& placement) noexcept;
private:
    bool TransferPendingVariableGroupReplacementPublication(EngineActionPlayScratch& producer,
        EngineActionPlayScratch& child, OriginalPendingVariableGroupHolder& group,
        const EngineImmediateEntryAttempt& attempt,
        const DecisionTable::FrontierPublicationProof& activation,
        const PrivateImmediateLaneGroupsPublicationProof& groups) noexcept;
public:
    bool CancelPrivatePendingVariableGroupEntry(OriginalPendingVariableGroupHolder& group,
        EngineActionPlayScratch& child) noexcept;
    bool CancelPendingVariableGroupChildren(OriginalPendingVariableGroupHolder& group,
        const MonitorControlCause* cause = nullptr) noexcept;
    class OriginalQueueFactoryLifetimePin {
    public:
        OriginalQueueFactoryLifetimePin() noexcept = default;
        OriginalQueueFactoryLifetimePin(const OriginalQueueFactoryLifetimePin&) = delete;
        OriginalQueueFactoryLifetimePin& operator=(const OriginalQueueFactoryLifetimePin&) = delete;
        OriginalQueueFactoryLifetimePin(OriginalQueueFactoryLifetimePin&& other) noexcept { *this = std::move(other); }
        OriginalQueueFactoryLifetimePin& operator=(OriginalQueueFactoryLifetimePin&& other) noexcept {
            if (this != &other) {
                Release();
                engine_ = std::exchange(other.engine_, nullptr);
                create_ = std::exchange(other.create_, QueueInstanceCreateRequest{});
                queue_ = std::move(other.queue_);
                service_ = std::move(other.service_);
                root_ = std::move(other.root_);
            }
            return *this;
        }
        bool Valid() const noexcept;
        bool Borrow(OriginalQueueFactoryLifetimePin* out) const noexcept;
        QueueInstanceToken FactoryQueue() const noexcept { return queue_.Token(); }
        QueueInstanceCreateRequest FactoryCreate() const noexcept { return create_; }
        EnginePlayableServiceReference FactoryService() const noexcept { return service_.FactoryReference(); }
        void Release() noexcept {
            root_.Release();
            queue_.Release();
            service_.Return();
            engine_ = nullptr;
            create_ = {};
        }
    private:
        friend struct Engine;
        Engine* engine_ = nullptr;
        QueueInstanceCreateRequest create_{};
        QueueInstanceTable::OriginalLifetimePin queue_;
        OriginalPlayableServiceLifetimePin service_;
        PlayableInstanceTable::OriginalLifetimePin root_;
    };
    class OriginalQueueFactoryLifetimeHolder {
    public:
        OriginalQueueFactoryLifetimeHolder() noexcept = default;
        bool Borrow(OriginalQueueFactoryLifetimePin* out) const noexcept;
        QueueInstanceToken FactoryQueue() const noexcept { return queue_; }
        QueueInstanceCreateRequest FactoryCreate() const noexcept { return create_; }
    private:
        friend struct Engine;
        enum class State : std::uint8_t { Empty, Acquired, Joined, Closing };
        Engine* engine_ = nullptr;
        const PreparedQueueInstanceV1* preparation_ = nullptr;
        const OriginalPlayableServiceLifetimeHolder* service_ = nullptr;
        QueueInstanceToken queue_{};
        QueueInstanceCreateRequest create_{};
        QueueCheckpointTable::BackingPreparation checkpointBacking_;
        EngineFrontierAttemptToken checkpointBorrower_{};
        State state_ = State::Empty;
    };
    struct OriginalQueueBankHolder {
        EngineQueueProgramBank::OriginalReservation reservation;
        EngineQueueProgramBank::OriginalLifetimePin baseline;
    };

    explicit Engine(const EngineConfig& resolved, std::uint64_t packGenerationFloor = 0,
                    std::uint32_t originalConditionalCapacity = 0)
        : config(resolved),
          playableStorage(resolved),
          logicPacks(resolved.maxPacks),
          packComposition(PackIdentityLeaseCapacity(resolved) -
                              resolved.maxPlayableInstances,
                          resolved.maxPlayableInstances, resolved.maxPacks,
                          packGenerationFloor),
          packs(resolved.maxPacks, PackRetirementCapacity(resolved),
                PackIdentityLeaseCapacity(resolved)),
          streams(resolved),
          effectBank(resolved),
          decisions(resolved.maxDecisionRows,
                    resolved.maxParticipantsPerDecision),
          transitionInstances(resolved.maxTransitionInstances),
          armGroups(resolved.maxArmGroups,
                    resolved.maxArmsPerGroup),
          timelineArms(TimelineArmCapacity(resolved)),
          initialRootArmPublicationRows(TimelineArmCapacity(resolved), nullptr),
          initialRootActivationPublicationProofs(resolved.maxDecisionRows),
          queueInstances(resolved.maxContainerInstances, originalConditionalCapacity),
          topologyPlans(resolved.topologyPlanPoolCapacity, resolved.maxBuses,
                        resolved.busSendSlotsPerBus),
          preparedTopologyPlans(resolved.topologyPlanPoolCapacity,
                                resolved.maxBuses,
                                resolved.busSendSlotsPerBus),
          preparedOwnerClaims(resolved.maxBusTopologyOwners,
                              PreparedOwnerClaimV1{}),
          preparedPreviewOwnerSlots(resolved.maxBusTopologyOwners, UINT32_MAX),
          projectGenerationInstallAckSideTable(
              resolved.topologyPlanPoolCapacity,
              ProjectGenerationInstallAckIdentityV1{}),
          projectGenerationInstallAckRing(resolved.topologyPlanPoolCapacity),
          queueCheckpoints(resolved.maxQueueCheckpoints),
          dynamicRoutes(resolved),
          dynamicRouteCaptureScratch(
              resolved.dynamicRouteCapacity == 0
                  ? 1u
                  : resolved.dynamicRouteCapacity),
          influenceVisited(resolved.maxBuses, 0),
          influenceStack(resolved.maxBuses, 0),
          sourceTerminals(resolved.maxPlayableInstances),
          timelineOccurrences(resolved.maxDeferredIntents),
          voices(resolved, &tables, &multirateTables, &biquadTrig, effectBank,
                 dynamicRoutes, &timelineArms),
          buses(resolved, topologyPlans, effectBank, dynamicRoutes,
                &biquadTrig),
          actionCommitBundle(16),
          playableInstances(resolved.maxPlayableInstances),
          authoredStopRoots(resolved.maxPlayableInstances),
          params(resolved, 0),
          variables(resolved.maxEmitters + 1u, resolved.emitterVariableSlots,
                     resolved.maxPropertiesPerCurrentSlice,
                     resolved.maxActions, logicCommits, transportEpochOwner,
                     variableSlices, timelinePool,
                     static_cast<std::uint64_t>(resolved.maxPlayableInstances) +
                         resolved.voiceCapacity.Logical() +
                         resolved.maxDeferredIntents,
                     voices.Capacity(), resolved.maxBuses),
          actionFiringPlanScratch(1),
          actionFiringOwnerReleaseScratch(resolved.maxActionFirings),
          originalActionFiringPackHolders(resolved.maxActionFirings),
          actionStepPlanScratch(1),
          actionVariableWriteScratch(1),
          actionSelectionResetScratch(1),
          actionFixedSelectionCommitInputs(1),
          actionQueueResetScratch(1),
           actionQueueCheckpointResetScratch(1),
           actionDeferredScratch(resolved.maxDeferredIntents),
           originalDeferredPackHolders(resolved.maxDeferredIntents),
          containerPlayback(resolved.maxPlayableInstances),
          variableControlInstances(resolved.maxContainerInstances),
          // A bounded diagnostic budget, independent of playback admission.
          ledger(resolved.voiceCapacity.Logical(),
                 resolved.voiceCapacity.Render(),
                 2 * (static_cast<std::size_t>(TimelineArmCapacity(resolved)) +
                      resolved.voiceCapacity.Logical()),
                 [](void* context, std::uint32_t slot) noexcept {
                     auto& engine = *static_cast<Engine*>(context);
                     return engine.pendingStarts.Emplace(slot) && engine.parkRecords.Emplace(slot);
                 }, this),
          playablePartitions(resolved.maxPlayableInstances),
           actionPlayAttemptTokens(1),
           initialPendingVariableOwnerScratch(1),
           initialQueueEntryDecisionOwnersScratch(
               resolved.maxPlayableInstances),
           initialQueueCoordinatorParticipantsScratch(
               resolved.maxPlayableInstances),
          actionStopScratch(resolved.maxPlayableInstances),
          actionPlayFrameScratch(resolved.commandQueueDepth),
          initialLaneDemandEventQuantaScratch(std::max(
              resolved.maxTransactionEvents,
              resolved.maxKnownScheduleLeaves * 4u)),
          initialLaneDemandGroupQuantaScratch(std::max(
              resolved.maxTransactionEvents,
              resolved.maxKnownScheduleLeaves * 4u)),
          decisionRetirements(decisions.Capacity()),
          playableStarts(resolved.maxPlayableInstances),
          frontierExpansion(resolved.maxPlayableInstances),
          ordinaryRetiringSources(resolved.maxPlayableInstances),
          ordinaryRetiringLeaves(resolved.maxPlayableInstances),
          queueSelectionChildScratch(resolved.maxQueueChildren),
          queueRemainingScratch(resolved.maxQueueChildren),
          queuePermutationScratch(resolved.maxQueueChildren),
          queueProjectionScratch(static_cast<std::size_t>(resolved.maxQueueChildren) + 1u),
          bindings(resolved.maxBindings, resolved.maxBindingAttachments,
                   resolved.maxBuses, params.PreparationCapacity()),
          busTargets(resolved.maxBuses,
                     resolved.maxVariableSliceTargetsTotal),
          effectParamTargets(
              resolved.mixRate.SampleRate(),
              // Every installed, stale-until-Update, Pending-claim prepared,
              // or Parked-return prepared voice record set is owned by one
              // Logical ledger identity. A physical Render slot can advance
              // generations before Update clears an old identity's records,
              // so Logical capacity, not Render capacity, is the hard bound.
              // Bus records retain one installed generation; claimed records
              // can retire into the fixed lifecycle headroom below. Validated
              // capacity/slot ceilings keep this product within uint32_t.
              (resolved.voiceCapacity.Logical() *
                   resolved.fxSlotsPerVoice +
               static_cast<std::uint32_t>(resolved.maxBuses) *
                   resolved.fxSlotsPerBus) *
                  harken::kMaxEffectParamCount,
              resolved.maxVariableSliceTargetsTotal),
          meterDetectors(resolved.maxBuses, resolved.blockFrames,
                         resolved.mixRate.SampleRate()),
          meterProbes(resolved.maxBindingAttachments,
                      resolved.maxPropertiesPerCurrentSlice),
          meterRing(resolved.meterRingBlocks, resolved.maxBuses),
          spatial(resolved, 0),
          emitters(resolved),
          monitorEmitters(emitters, resolved.maxEmitters, monitorRequests.EngineInstance()),
          rng(resolved.randomSeed == 0 ? harken::SplitMix64::kDefaultSeed
                                       : resolved.randomSeed),
          commands(resolved.commandQueueDepth, &timelineArms),
          originalSourceSubmissions(timelineArms, voices, commands, effectBank),
          originalSingleDecisionBaselines(decisions.Capacity()),
          originalSingleOccurrenceBaselines(timelineOccurrences.Capacity()),
          originalSingleOccurrenceCommitProofs(timelineOccurrences.Capacity()),
          actionPlayCommandScratch(commands.Capacity()),
          actionSeekCommandScratch(commands.Capacity()),
          actionSeekPreparationScratch(commands.Capacity()),
          playLedBatchScratch(commands.Capacity()),
          propertyPublisher(commands.Capacity()),
          spatialPropertyGroupIndices(resolved.voiceCapacity.Render(), ~0u),
          toneSectionCommandsPublished(resolved.voiceCapacity.Render(), 0),
          toneSectionPublishedRampMs(
              static_cast<std::size_t>(resolved.voiceCapacity.Render()) * 4,
              0),
          notifications(resolved.notificationQueueDepth),
          voiceManager(resolved.voiceCapacity.Render(),
                       resolved.voiceCapacity.Logical()),
          logicNotifications(resolved.voiceCapacity.Logical()),
          sourceCursors(resolved.voiceCapacity.Render()),
          compositeCompletionOwners(
              resolved.maxCompositeCompletionOwners,
              resolved.maxKnownScheduleLeaves),
          originalCompositeCompletionPreparations(resolved.maxCompositeCompletionOwners),
          originalOccurrencePreparations(resolved.maxDeferredIntents),
          timelineDeferred(resolved.maxDeferredIntents,
                           resolved.maxDeferredIntents),
          timelinePrograms(2),
           timelineSink(voices, timelineArms),
           mixer(resolved, voices, buses, dynamicRoutes, commands, actionSeeks,
                streamSeekHandoffs,
                notifications, logRing, sourceCursors, meterRing,
                &timelineArms, &timelineExecutor, &timelineSink,
                &timelinePool, &variableSlices,
                projectGenerationInstallAckSideTable.data(),
                static_cast<std::uint32_t>(
                    projectGenerationInstallAckSideTable.size()),
                &projectGenerationInstallAckRing, &originalSourceSubmissions),
           nullBackend(mixer, resolved) {
        voices.AttachTimelineVariableStartAuthority({&variables,
            [](void* context, VariableTimelineReadUseRef use, std::uint64_t nativeHandle) noexcept {
                return static_cast<VariableServiceV1*>(context)->TimelineReadUseStartStatus(use, nativeHandle);
            },
            [](void* context, VariableTimelineReadUseRef use, std::uint64_t nativeHandle,
                const TimelineArmTable::PlainAudioProof& proof, VariableTimelineInitializerProof* out) noexcept {
                return static_cast<VariableServiceV1*>(context)->ProveTimelineReadUseInitializer(use, nativeHandle, proof, out);
            },
            [](void* context, VariableTimelineReadUseRef use, std::uint64_t nativeHandle,
                const TimelineArmTable::LaneAudioProof& proof, VariableTimelineInitializerProof* out) noexcept {
                return static_cast<VariableServiceV1*>(context)->ProveTimelineReadUseInitializer(use, nativeHandle, proof, out);
            }, &VariableServiceV1::ApplyTimelineReadUseInitializer,
            [](void* context, VariableTimelineReadUseRef use, std::uint64_t nativeHandle,
                const TimelineArmTable::BoundAudioProof& proof, VariableTimelineInitializerProof* out) noexcept {
                return static_cast<VariableServiceV1*>(context)->ProveTimelineReadUseInitializer(use, nativeHandle, proof, out);
            },
            [](void* context, VariableTimelineReadUseRef use, std::uint64_t nativeHandle,
                const TimelineArmTable::AudioOperationClaim& proof, VariableTimelineInitializerProof* out) noexcept {
                return static_cast<VariableServiceV1*>(context)->ProveTimelineReadUseInitializer(use, nativeHandle, proof, out);
            },
            [](void* context, const FrontierActivationDescriptor& descriptor) noexcept {
                return static_cast<VariableServiceV1*>(context)->FrontierEntrySourcesStatus(descriptor);
            },
            [](void* context, VariableTimelineReadUseRef use, std::uint64_t nativeHandle,
                TimelineArmToken arm) noexcept {
                return static_cast<VariableServiceV1*>(context)->PendingTimelineReadUseMatchesArm(use, nativeHandle, arm);
            }});
        voices.AttachNativeVariableStartAuthority({&variables,
            [](void* context, const VariableNativeStartIdentity& identity) noexcept {
                return static_cast<VariableServiceV1*>(context)->MarkNativeReadCutInstalled(identity);
            },
            [](void* context, const VariableNativeStartIdentity& identity) noexcept {
                auto& service = *static_cast<VariableServiceV1*>(context);
                const auto receipt = service.NativeReadCutStatus(identity);
                if (receipt == VariableNativeStartReceipt::Pending) return VariableReadCutStatus::Waiting;
                if (receipt != VariableNativeStartReceipt::Installed) return VariableReadCutStatus::Failed;
                const auto sources = service.NativeReadCutSourcesStatus(identity);
                if (sources == VariableReadCutStatus::Failed) return sources;
                return service.NativeReadCutCalendarConsumed(identity) ? sources : VariableReadCutStatus::Waiting;
            },
            [](void* context, const VariableNativeStartIdentity& identity, bool activated) noexcept {
                return static_cast<VariableServiceV1*>(context)->FinishNativeReadCut(identity, activated);
            }});
        mixer.SetNativeStartCalendarConsumer(&variables,
            [](void* context, const ConsumedCurrentSlice& consumed) noexcept {
                return static_cast<VariableServiceV1*>(context)->ConsumeNativeReadCutCalendar(consumed);
            });
        for (std::size_t index = 0; index < playableStarts.size(); ++index) {
            playableStarts[index].slot = static_cast<std::uint32_t>(index);
        }
        if (!originalPlayableLifetimeHolders.Reserve(resolved.maxPlayableInstances) ||
            !originalPlayableServiceLifetimeHolders.Reserve(resolved.maxPlayableInstances) ||
            !originalSingleCurrentHolders.Reserve(resolved.maxPlayableInstances) ||
            !authoredStopResources.Reserve(resolved.maxScheduledTransactions) ||
            !queueContinuationRecords.Reserve(static_cast<std::uint32_t>(QueueContinuationRecordCapacity(resolved))) ||
            !queueFutureRecords.Reserve(resolved.maxPlayableInstances) ||
            !pendingVariableLeafSources.Reserve(timelineArms.Capacity()) ||
            !pendingVariableFutureSources.Reserve(timelineArms.Capacity()) ||
            !playableServices.Reserve(resolved.maxPlayableInstances) ||
            !timelineStartScratch.starts.Reserve(kMaxKnownScheduleLeaves) ||
            !timelineStartScratch.transactions.Reserve(2u * kMaxKnownScheduleLeaves + 2u) ||
            !timelineStartScratch.extraCommands.Reserve(kMaxKnownScheduleLeaves) ||
            !timelineStartScratch.queueEvents.Reserve(EngineQueueProgramBank::kEventCapacity) ||
            !acceptedPendingVariableGroups.Reserve(resolved.maxPlayableInstances)) throw std::bad_alloc{};
        const auto playSlotCapacity = ActionPreparationSlotCapacity(resolved);
        const auto queueBankCapacity = QueueProgramBankCapacity(resolved);
        const auto playableMemberCapacity = static_cast<std::uint64_t>(playableStorage.memberCount) +
            static_cast<std::uint64_t>(queueBankCapacity) * EngineQueueProgramBankBody::kProgramCapacity;
        const auto privateLaneMetadataCapacity = std::max(resolved.maxTransactionEvents,
            resolved.maxKnownScheduleLeaves * 4u);
        if (playSlotCapacity > UINT32_MAX || queueBankCapacity > UINT32_MAX ||
            !queueScheduleStorage.Configure(static_cast<std::uint32_t>(queueBankCapacity)) ||
            !queueProgramBanks.Reserve(static_cast<std::uint32_t>(queueBankCapacity)) ||
            !actionPlayStorage.Reserve(static_cast<std::uint32_t>(playSlotCapacity)) ||
            !pendingOrdinarySelections.Reserve(static_cast<std::uint32_t>(playSlotCapacity)) ||
            !preparedPendingVariableGroups.Reserve(static_cast<std::uint32_t>(playSlotCapacity)) ||
            !actionPlayScratch.Reserve(static_cast<std::uint32_t>(playSlotCapacity))) throw std::bad_alloc{};
        if (playableMemberCapacity > UINT32_MAX || playableStorage.initialProgramCount > UINT32_MAX ||
            playableStorage.continuousCount > UINT32_MAX ||
            !memberProgramRecords.Reserve(static_cast<std::uint32_t>(playableStorage.initialProgramCount)) ||
            !playableMemberRecords.Reserve(static_cast<std::uint32_t>(playableMemberCapacity)) ||
            !memberRelations.Reserve(static_cast<std::uint32_t>(playableStorage.memberCount)) ||
            !variableControlRetargetPrograms.Reserve(resolved.maxScheduledTransactions) ||
            !memberOnsets.Reserve(static_cast<std::uint32_t>(playableStorage.memberCount)) ||
            !playableServiceWork.Reserve(resolved.maxPlayableInstances) ||
            !originalSinglePreparationHolders.Reserve(timelineArms.Capacity()) ||
            !originalQueueSourcePreparationHolders.Reserve(timelineArms.Capacity()))
            throw std::bad_alloc{};
        if (!timelineArms.AttachRowStoragePreparation(this,
            [](void* context, std::uint32_t count, bool fullSourceCommandBacking) noexcept {
                return static_cast<Engine*>(context)->PrepareOriginalSourceRowBacking(count, fullSourceCommandBacking);
            })) throw std::bad_alloc{};
        const bool originalPlayableResolver = playableInstances.RegisterOriginalLifetimeBaselineResolver(
            this, &ResolveOriginalPlayableBaseline, &DisposeOriginalPlayableAssociation);
        assert(originalPlayableResolver);
        (void)originalPlayableResolver;
        const bool originalDecisionResolver = decisions.RegisterOriginalLifetimeBaselineResolver(
            this, &ResolveOriginalSingleDecisionBaseline);
        const bool originalOccurrenceResolvers = timelineOccurrences.RegisterOriginalSingleOwnerResolvers(
            this, &ResolveOriginalSingleOccurrenceBaseline, &ResolveOriginalSingleOccurrenceCommit);
        const bool originalOccurrenceStorageResolver = timelineOccurrences.RegisterOriginalStorageBaselineResolver(
            this, &ResolveOriginalOccurrenceStorageBaseline);
        const bool originalCompositeCommitResolver = timelineOccurrences.RegisterOriginalCompositePreparedCommitResolver(
            this, &ResolveOriginalCompositeOccurrenceCommit);
        assert(originalCompositeCommitResolver);
        (void)originalCompositeCommitResolver;
        assert(originalOccurrenceStorageResolver);
        (void)originalOccurrenceStorageResolver;
        assert(originalDecisionResolver && originalOccurrenceResolvers);
        (void)originalDecisionResolver;
        (void)originalOccurrenceResolvers;
        const bool originalCompositeResolver = compositeCompletionOwners.RegisterOriginalLifetimeBaselineResolver(
            this, &ResolveOriginalCompositeBaseline);
        assert(originalCompositeResolver);
        (void)originalCompositeResolver;
        // Polyphase tables are shared read-only across voices; building them
        // here keeps every transcendental off the audio thread.
        tables.Init();
        multirateTables.Init();
        ScheduledTransactionPool::Config timelinePoolConfig{};
        timelinePoolConfig.quantumFrames = resolved.blockFrames;
        timelinePoolConfig.scheduleHorizonFrames =
            resolved.scheduleHorizonFrames;
        timelinePoolConfig.publishLeadQuanta = resolved.publishLeadQuanta;
        timelinePoolConfig.maxScheduledTransactions =
            resolved.maxScheduledTransactions;
        timelinePoolConfig.maxTransactionSpanFrames =
            resolved.maxTransactionSpanFrames;
        timelinePoolConfig.maxIngressTransactionsPerQuantum =
            resolved.maxIngressTransactionsPerQuantum;
        timelinePoolConfig.maxFrameBatchesPerQuantum =
            resolved.maxFrameBatchesPerQuantum;
        timelinePoolConfig.maxEventsPerQuantum = resolved.maxEventsPerQuantum;
        timelinePoolConfig.maxTransactionEvents =
            resolved.maxTransactionEvents;
        timelinePoolConfig.maxCurrentSlicesPerQuantum =
            resolved.maxEventsPerQuantum;
        timelinePoolConfig.maxNativeStartCalendarsPerQuantum = variables.ReadCutCapacity();
        timelinePoolConfig.maxPlan03GroupHeadersPerPage =
            resolved.maxPlan03GroupHeadersPerPage;
        timelinePoolConfig.maxArmsPerGroup = resolved.maxArmsPerGroup;
        const auto laneHorizonQuanta =
            resolved.scheduleHorizonFrames / resolved.blockFrames;
        const auto laneGroupLimit =
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
        const auto groupsPerBucket = resolved.maxPlan03GroupHeadersPerPage;
        if (groupsPerBucket != 0 &&
            (laneHorizonQuanta > laneGroupLimit / groupsPerBucket ||
             resolved.maxDeferredIntents >
                 laneGroupLimit / groupsPerBucket - laneHorizonQuanta))
            throw std::bad_alloc{};
        timelinePoolConfig.maxImmediateLaneGroupRows =
            static_cast<std::uint32_t>(
                (laneHorizonQuanta + resolved.maxDeferredIntents) *
                groupsPerBucket);
        timelinePoolConfig.maxImmediateLaneOwnerGates = resolved.maxPlayableInstances;
        timelinePoolConfig.maxImmediateLaneDecisionGates = resolved.maxDecisionRows;
        if (!decisions.Valid() || !transitionInstances.Valid() ||
            !armGroups.Valid() ||
            !timelinePool.Reset(timelinePoolConfig, 0,
                                transportEpochOwner.Current()) ||
            !immediateActionLane.Reset({
                resolved.maxActionFirings,
                privateLaneMetadataCapacity,
                resolved.maxEventsPerQuantum,
                resolved.blockFrames, true,
                resolved.maxPlan03GroupHeadersPerPage}) ||
            !timelineSink.AttachImmediateLane(immediateActionLane, resolved.blockFrames) ||
            !timelineSink.AttachPlan03ScratchAuthority(decisions,
                                                       timelinePool) ||
            !timelineSink.AttachTransitionAuthority(
                transitionInstances) ||
            !timelineSink.AttachQueueAuthority(
                queueInstances, armGroups, timelineOccurrences,
                compositeCompletionOwners) ||
            !timelineSink.AttachVariableControlAuthority(
                variableControlInstances) ||
            !variableSlices.Reset(variables.CompletionCapacity(),
                                  resolved.maxPropertiesPerCurrentSlice,
                                  resolved.maxVariableSliceTargetsTotal,
                                  variables.ApplyPayloadCapacity())) {
            throw std::bad_alloc{};
        }
        if (!params.AttachBindingDependencies(&bindings.Dependencies())) throw std::bad_alloc{};
        effectParamTargets.AttachBindingDependencies(&bindings.Dependencies());
        variables.AttachTargetRetirementStorage(this, [](void* context) noexcept {
            auto& engine = *static_cast<Engine*>(context);
            return engine.params.PrepareRetiringTargetStorage() && engine.busTargets.PrepareRetiringTargetStorage();
        });
        emitters.SetVariableChangeObserver(this,
            [](void* context, VnEmitterHandle emitter, VnVariableId variable) noexcept {
                static_cast<Engine*>(context)->params.NoteVariableWrite(variable, emitter);
            });
        mixer.InitializeVariableSliceConsumptionStorage();
        if (!initialQueueTimingMembersScratch.Reserve(resolved.maxPlayableInstances) ||
            !initialQueueTransitionTimingProofsScratch.Reserve(resolved.maxPlayableInstances) ||
            !initialQueueConditionalTimingProofsScratch.Reserve(resolved.maxPlayableInstances) ||
            !initialQueueCurrentCompositeProofsScratch.Reserve(resolved.maxPlayableInstances) ||
            !initialQueueSuccessorCompositeProofsScratch.Reserve(resolved.maxPlayableInstances) ||
            !actionQueueReplacementScratch.Reserve(resolved.maxPlayableInstances))
            throw std::bad_alloc{};
        if (!originalQueueBankHolders.Reserve(queueProgramBanks.size()) ||
            !originalQueueLifetimeHolders.Reserve(queueInstances.Capacity()) ||
            !originalQueueFactoryLifetimeHolders.Reserve(queueInstances.Capacity()))
            throw std::bad_alloc{};
        if (queueInstances.originalConditionalCapacity_ != 0) {
            if (!originalQueueConditionalHolders.Reserve(queueInstances.Capacity()) ||
                !originalQueueReplacementHolders.Reserve(queueInstances.Capacity()))
                throw std::bad_alloc{};
            originalQueueReplacementSpans =
                std::make_unique<PrivateSelectedQueueFutureSpanPool>(queueInstances.Capacity());
            originalQueueReplacementBatch =
                std::make_unique<OriginalConditionalReplacementBatch>(queueInstances.Capacity());
            const bool registered = queueInstances.RegisterOriginalConditionalOwner(this,
                &ResolveOriginalQueueConditionalBaseline, &EndOriginalQueueConditionalAssociation);
            assert(registered);
            (void)registered;
        }
        if (!queueInstances.AttachRowStoragePreparation(this,
                [](void* context, std::uint32_t count) noexcept {
                    return static_cast<Engine*>(context)->PrepareQueueInstanceHolders(count);
                })) throw std::bad_alloc{};
        originalQueueGroupCapacity = timelinePoolConfig.maxImmediateLaneGroupRows;
        if (!originalQueueGroupHolders.Reserve(timelinePoolConfig.maxImmediateLaneGroupRows))
            throw std::bad_alloc{};
        if (timelinePool.MaxArmsPerGroup() == 0)
            throw std::bad_alloc{};
        if (!pendingVariableQueueNativeOrigins.Reserve(originalQueueGroupCapacity)) throw std::bad_alloc{};
        if (!pendingVariableFutureNodes.Reserve(playableInstances.Capacity())) throw std::bad_alloc{};
        timelinePool.SetImmediateLaneGroupDisposalObserver(this, &ObservePendingVariableQueueNativeDisposal);
        const auto plainCapacity = immediateActionLane.EventsPerAttempt();
        const auto plainRows = static_cast<std::uint64_t>(resolved.maxActionFirings) * plainCapacity;
        if (plainRows > std::numeric_limits<std::size_t>::max() / sizeof(EnginePlainImmediateSource) ||
            plainRows > std::numeric_limits<std::size_t>::max() / sizeof(EnginePlainImmediateStopRef))
            throw std::bad_alloc{};
        if (!immediateEntryAttempts.Reserve(resolved.maxActionFirings)) throw std::bad_alloc{};
        const auto pageGroupRows = static_cast<std::uint64_t>(timelinePool.PageCount()) *
            resolved.maxPlan03GroupHeadersPerPage;
        if (pageGroupRows >
            std::numeric_limits<std::size_t>::max() /
                sizeof(Plan03GroupSliceInput))
            throw std::bad_alloc{};
        const auto groupScratchCount = pageGroupRows +
            timelinePoolConfig.maxImmediateLaneGroupRows;
        if (groupScratchCount > UINT32_MAX ||
            groupScratchCount > std::numeric_limits<std::size_t>::max() / sizeof(Plan03PageGroupView))
            throw std::bad_alloc{};
        if (!timelineGroupCaptureScratch.Reserve(static_cast<std::uint32_t>(groupScratchCount)))
            throw std::bad_alloc{};
        if (!PrepareTimelineGroupCaptureStorage(
                timelinePool.CapturePendingEventStorageDemand().nativeGroups, 0))
            throw std::bad_alloc{};
        timelinePool.AttachPendingEventStorage(timelineGroupCapturePreparation, this,
            [](void* context, ScheduledTransactionPool::PendingEventStorageDemand demand) noexcept {
                return static_cast<Engine*>(context)->PrepareTimelineGroupCaptureStorage(demand.nativeGroups, 0);
            });
        publishedFrontierProgramRefsScratch.resize(
            static_cast<std::size_t>(playableStorage.initialProgramStride) + 1u);
        const auto queueEventCapacity =
            static_cast<std::uint64_t>(timelinePool.PageCount()) *
            resolved.maxEventsPerQuantum;
        if (queueEventCapacity > UINT32_MAX || queueEventCapacity >
            std::numeric_limits<std::size_t>::max() /
                sizeof(EngineInitialQueueTimingEvent))
            throw std::bad_alloc{};
        if (!initialQueueTimingProgramsScratch.Reserve(resolved.maxScheduledTransactions) ||
            !initialQueueTimingEventsScratch.Reserve(static_cast<std::uint32_t>(queueEventCapacity)) ||
            !initialQueueTimingGroupsScratch.Reserve(static_cast<std::uint32_t>(pageGroupRows)))
            throw std::bad_alloc{};
        static_assert(std::numeric_limits<std::uint32_t>::max() <=
            std::numeric_limits<std::size_t>::max() /
                sizeof(ImmediateLaneEvent));
        static_assert(std::numeric_limits<std::uint32_t>::max() <=
            std::numeric_limits<std::size_t>::max() /
                sizeof(Plan03ImmediateLaneGroupDescriptor));
        if (!initialQueueLaneGroupsScratch.Reserve(timelinePoolConfig.maxImmediateLaneGroupRows))
            throw std::bad_alloc{};
        if (!timelinePool.AttachImmediateLaneRowStoragePreparation(this,
                [](void* context, std::uint32_t required) noexcept {
                    auto& engine = *static_cast<Engine*>(context);
                    return engine.PrepareTimelineGroupCaptureStorage(0, required) &&
                        engine.initialQueueLaneGroupsScratch.ConstructThrough(required) &&
                        engine.originalQueueGroupHolders.ConstructThrough(required);
                })) throw std::bad_alloc{};
        if (!initialFrontierEvents.Reserve(4ull * resolved.maxPlayableInstances + 1u) ||
            !initialFrontierRequests.Reserve(initialFrontierEvents.Capacity())) throw std::bad_alloc{};
        const std::uint64_t frontierNodes = resolved.maxPlayableInstances;
        const auto boundedFrontierCount = [](std::uint64_t proposed, std::uint32_t limit) {
            return static_cast<std::uint32_t>(std::min<std::uint64_t>(proposed, limit));
        };
        const EngineFrontierOwnerLedger::Limits frontierOwnerLimits{
            boundedFrontierCount(frontierNodes, resolved.maxPendingSelectionDeltasTotal),
            boundedFrontierCount(2u * frontierNodes + 1u, resolved.maxDecisionRows),
            boundedFrontierCount(2u * frontierNodes, timelineOccurrences.Capacity()),
            boundedFrontierCount(frontierNodes, resolved.maxCompositeCompletionOwners),
            boundedFrontierCount(frontierNodes, resolved.voiceCapacity.Render()),
            resolved.maxPlayableInstances, resolved.maxPlayableInstances,
            resolved.maxScheduledTransactions,
            EngineActionPlayScratch::kFrontierExposureOwnerCapacity,
            resolved.maxTransitionInstances};
        actionPlayOwnerLimits = frontierOwnerLimits;
        SelectionMemoryConfigV1 selectionConfig{};
        selectionConfig.maxEmitters = resolved.maxEmitters + 1u;
        selectionConfig.maxGlobalRows = resolved.maxSelectionRowsGlobal;
        selectionConfig.maxEmitterRowsPerEmitter =
            resolved.maxSelectionRowsPerEmitter;
        selectionConfig.maxPendingDeltasTotal =
            resolved.maxPendingSelectionDeltasTotal;
        selectionConfig.maxPendingGlobalPerRow =
            resolved.maxPendingSelectionDeltasGlobalPerRow;
        selectionConfig.maxPendingEmitterPerRow =
            resolved.maxPendingSelectionDeltasEmitterPerRow;
        ResolvedPlayablePropertyCutConfigV1 cutConfig{};
        cutConfig.maxSends = std::max(1u, resolved.voiceSendSlots);
        cutConfig.maxEffectNodes = std::max(1u, resolved.fxSlotsPerVoice);
        cutConfig.maxEffectParams =
            cutConfig.maxEffectNodes * kMaxEffectParamCount;
        cutConfig.maxEffectEdges = cutConfig.maxEffectNodes;
        if (resolved.maxPlayableInstances == 0 ||
            resolved.maxPlayableInstances > kMaxPlayableInstances ||
            resolved.maxKnownScheduleLeaves == 0 ||
            resolved.maxKnownScheduleLeaves > kMaxKnownScheduleLeaves ||
            resolved.maxVariableControlActiveRows == 0 ||
            resolved.maxVariableControlActiveRows > 64 ||
            resolved.maxVariableControlLiveRows <
                resolved.maxVariableControlActiveRows ||
            resolved.maxVariableControlLiveRows > 64 ||
            resolved.maxTransitionInstances == 0 ||
            resolved.maxTransitionInstances >
                kMaxTransitionInstancesHard ||
            resolved.maxCompositeCompletionOwners == 0 ||
            resolved.maxCompositeCompletionOwners >
                kMaxCompositeCompletionOwners ||
            resolved.maxContainerInstances == 0 ||
            resolved.maxContainerInstances > kMaxContainerInstances ||
            resolved.maxQueueCheckpoints == 0 ||
            resolved.maxQueueCheckpoints > kMaxQueueCheckpoints ||
            resolved.maxQueueChildren == 0 ||
            resolved.maxQueueChildren > kMaxQueueChildren ||
            !queueInstances.Valid() ||
            !queueCheckpoints.Valid() ||
            !actionFirings.Reset(resolved.maxActionFirings) ||
            !actionSeeks.Reset(resolved.maxActionFirings) ||
            !streamSeekHandoffs.Reset(resolved.maxStreams) ||
            !actionDeferredRecords.Reset(resolved.maxDeferredIntents) ||
            !actionCooldowns.Reset(resolved.maxActionCooldownRows) ||
            !selectionMemory.Reset(selectionConfig) ||
            !variables.NoteEmitterCreated(emitters.DefaultState()) ||
            !playablePropertyCutScratch.Reset(cutConfig)) {
            throw std::bad_alloc{};
        }
        for (std::uint32_t bus = 0; bus < buses.BusCount(); ++bus) {
            if (buses.AliveAt(bus)) {
                busTargets.InitializeBusGeneration(
                    bus, buses.GenerationAt(bus));
            }
        }
        TimelinePlanner::Config timelinePlannerConfig{};
        timelinePlannerConfig.maxRollingOccurrences =
            resolved.maxDeferredIntents;
        if (!timelinePlanner.Reset(timelinePlannerConfig, &timelinePool,
                                   &timelineDeferred, &playableInstances,
                                   &timelineOccurrences, &timelineArms) ||
            !timelineExecutor.Reset(resolved.blockFrames,
                                    resolved.maxEventsPerQuantum,
                                    &timelinePool, &timelineArms,
                                    &timelineSink, &decisions)) {
            throw std::bad_alloc{};
        }
        timelineService = std::make_unique<TimelineServiceCoordinator>(*this);
        // Section-10.4 capacity contract: binding-instance growth consults
        // the publication worst-case bound from the first bind on;
        // refusals log through the logic-side ring.
        params.AttachPublicationCapacity(&propertyPublisher, &logicLogRing);
        // Config observability: when every voice's spatially drivable
        // targets plus every bus target row exceed the command queue's
        // capacity, one simultaneous full-load tick cannot publish whole
        // and will chunk. One INFO with both numbers at init; nothing is
        // refused and nothing changes behavior.
        const std::uint64_t worstCaseEstimate =
            static_cast<std::uint64_t>(resolved.voiceCapacity.Render()) *
                kSpatialDrivenTargetsPerVoice +
            static_cast<std::uint64_t>(resolved.maxBuses) * kBusTargetCount;
        if (worstCaseEstimate > commands.Capacity()) {
            LogRecord estimate{};
            estimate.level = VN_LOG_INFO;
            estimate.code = kLogCodePropertyCapacityEstimate;
            estimate.arg0 = worstCaseEstimate;
            estimate.arg1 = commands.Capacity();
            logicLogRing.Push(estimate);
        }
        // One staging record per Logical Voice entry, indexed by ledger slot;
        // only deferred starts are active.
        if (!pendingStarts.Reserve(resolved.voiceCapacity.Logical())) throw std::bad_alloc();
        bindings.AttachVoiceCatalogStorage(this, [](void* context) noexcept {
            return static_cast<Engine*>(context)->PrepareVoiceBindingCatalogStorage();
        });
        const auto renderCapacity = resolved.voiceCapacity.Render();
        if (!voiceOriginPlayable.Reserve(renderCapacity) ||
            !voicePlayFacts.Reserve(renderCapacity) || !playbackPaths.Reserve(renderCapacity) ||
            !timelineVoiceInstallIdentities.Reserve(renderCapacity) ||
            !effectSlotTypes.Reserve(renderCapacity) || !voiceEffectInstanceRefs.Reserve(renderCapacity) ||
            !effectSlotConfig.Reserve(renderCapacity) || !voiceSendConfig.Reserve(renderCapacity) ||
            !voices.AttachSlotStoragePreparation(logicVoiceStorageRegistration, this,
                [](void* context, std::uint32_t count) noexcept {
                    return static_cast<Engine*>(context)->PrepareLogicVoiceStorage(count);
                })) throw std::bad_alloc();
        // One park record per Logical Voice entry, indexed by ledger slot; only
        // Virtual Voices are active.
        if (!parkRecords.Reserve(resolved.voiceCapacity.Logical())) throw std::bad_alloc();
        if (resolved.audibilityParkEnabled != 0) {
            detail::CheckedGrow(audibilityCursorRows, sourceCursors.SlotCount(),
                                detail::LogicAllocSite::AudibilityRows);
            audibilityCursorRows.resize(sourceCursors.SlotCount());
            detail::CheckedGrow(audibilityParkedSlots, resolved.voiceCapacity.Virtual(),
                                detail::LogicAllocSite::AudibilityParked);
            audibilityParkedSlots.reserve(resolved.voiceCapacity.Virtual());
            detail::CheckedGrow(audibilityParkedHandles, resolved.voiceCapacity.Virtual(),
                                detail::LogicAllocSite::AudibilityParked);
            audibilityParkedHandles.reserve(resolved.voiceCapacity.Virtual());
        }
        spatial.AttachRetainedProfileStorage(this, [](void* context) noexcept {
            return static_cast<Engine*>(context)->PrepareRetainedProfileKeyStorage();
        });
        // Logic-side source of truth for bus->bus send-edge membership, one row
        // of busSendSlotsPerBus target dense indices per bus (kInvalidBusIndex =
        // empty). The topology planner reads it (with the tree parents) to
        // cycle-check and sort; a publish copies it into the plan.
        busSendAdj.assign(static_cast<std::size_t>(resolved.maxBuses) *
                              resolved.busSendSlotsPerBus,
                          kInvalidBusIndex);
        busOwnerCookies.assign(resolved.maxBuses, 0);
        busTopologyOwnerIds.assign(resolved.maxBuses, 0);
        busSendOwnerCookies.assign(
            static_cast<std::size_t>(resolved.maxBuses) *
                resolved.busSendSlotsPerBus,
            0);
        busSendTopologyOwnerIds.assign(
            static_cast<std::size_t>(resolved.maxBuses) *
                resolved.busSendSlotsPerBus,
            0);
        busSendLevelDb.assign(
            static_cast<std::size_t>(resolved.maxBuses) *
                resolved.busSendSlotsPerBus,
            0.0f);
        busTopologyOwners.assign(resolved.maxBusTopologyOwners,
                                 BusTopologyOwnerRecord{});
        previewMasterHolders.assign(resolved.maxBusTopologyOwners, PreviewMasterHolderV1{});
        busRetirements.assign(resolved.maxBuses, TopologyRetirementV1{});
        currentWorldBusAdmission.assign(resolved.maxBusTopologyOwners,
                                        CurrentWorldBusAdmissionRowV1{});
        // Logic-side mirror of the effect type installed in each bus slot, used
        // to validate Vn_SetBusEffectParam against the correct type (the voice
        // effectSlotTypes analogue). One row per possible bus, all empty.
        // Dense indices are append-only and tombstones are never reused, so no
        // per-create reset is needed — a fresh bus's row is already all-none.
        std::array<std::uint32_t, kMaxFxSlotsPerBus> emptyBusSlots;
        emptyBusSlots.fill(kEffectTypeNone);
        busEffectSlotTypes.assign(resolved.maxBuses, emptyBusSlots);
        std::array<EffectInstanceRef, kMaxFxSlotsPerBus> emptyBusRefs;
        emptyBusRefs.fill(EffectInstanceRef{});
        busEffectInstanceRefs.assign(resolved.maxBuses, emptyBusRefs);
        std::array<VnPackHandle, kMaxFxSlotsPerBus> emptyBusOwners{};
        busEffectOwnerCookies.assign(resolved.maxBuses, emptyBusOwners);
        std::array<std::uint8_t, kMaxFxSlotsPerBus> emptyBusBypass{};
        emptyBusBypass.fill(0);
        busEffectSlotBypass.assign(resolved.maxBuses, emptyBusBypass);
        busFaderAfterSlots.assign(resolved.maxBuses, resolved.fxSlotsPerBus);
        // Per-bus mute/solo audibility state: every bus starts unmuted and
        // unsoloed (fully audible).
        busMuted.assign(resolved.maxBuses, 0);
        busSoloed.assign(resolved.maxBuses, 0);
        busSoloActiveCount = 0;
        // Logic-side authoritative per-bus output fold config (multichannel
        // amendment). Every bus starts unfolded (output layout == the mix
        // layout); the master (index 0) presents at the device layout and
        // carries the mix->device default matrix. A non-master bus's matrix is
        // seeded to the identity — unused until Vn_SetBusOutputLayout narrows it
        // (which reseeds a default from the new pair). FillPlan copies this into
        // every published plan.
        busOutputLayout.assign(resolved.maxBuses, resolved.mixChannelLayout);
        busOutputLayout[0] = resolved.deviceChannelLayout;
        busDownmixCoeffs.assign(
            static_cast<std::size_t>(resolved.maxBuses) * 64, 0.0f);
        DefaultDownmixMatrix(resolved.mixChannelLayout,
                             resolved.deviceChannelLayout,
                             busDownmixCoeffs.data());
        for (std::uint32_t i = 1; i < resolved.maxBuses; ++i) {
            DefaultDownmixMatrix(
                resolved.mixChannelLayout, resolved.mixChannelLayout,
                busDownmixCoeffs.data() + static_cast<std::size_t>(i) * 64);
        }
        // QPC rate cached here, on the logic thread, for the Vn_Update
        // self-measurement conversions.
        cpuTicksPerSecond = CpuClockTicksPerSecond();
    }

    VnObjectId CurrentProjectId() const noexcept {
        if (packComposition.Authority() == PackAuthorityMode::Pack) {
            const auto* published = packComposition.Published();
            return published ? published->projectId : VnObjectId{};
        }
        const auto* identity = variables.CurrentWorldIdentity();
        return identity ? identity->projectId : VnObjectId{};
    }

    using OriginalPackIdentityLease = EngineOriginalPackIdentityLease;
#if defined(HARKEN_TEST_PACK_RESOURCE_OBSERVATION)
    void (*originalPackResourceAcquiredHookForTest)(Engine&, const PackPartitionLease&,
        PackResourceClosureToken, void*) = nullptr;
    void* originalPackResourceAcquiredContextForTest = nullptr;
#endif
    VnResult ReleaseOriginalPackIdentityLease(OriginalPackIdentityLease& original) noexcept {
        if (original.receiver_ != this || !original.native_.Valid()) return VN_STALE_HANDLE;
        const auto row = original.native_.index_;
        const auto token = original.native_.token_;
        const auto incarnation = original.native_.incarnation_;
        const auto result = ReleaseOriginalPackEpisode(original.native_);
        if (result == VN_OK) {
            // Record only this actual owner's successful native disposal.
            for (auto& service : originalPlayableServiceLifetimeHolders) {
                if (service.engine_ != this || service.packDutyOwner_ != &original ||
                    service.packDutyNativeRow_ != row || service.packDutyNativeToken_ != token ||
                    service.packDutyNativeIncarnation_ != incarnation) continue;
                service.packDuty_ = OriginalPlayableServicePackDuty::Released;
                service.packDutyOwner_ = nullptr;
            }
            original.receiver_ = nullptr;
        }
        return result;
    }
    VnResult RetagOriginalPackIdentityLease(OriginalPackIdentityLease& original,
        PackLeaseSlice to) noexcept {
        if (original.receiver_ != this || !original.native_.Valid()) return VN_STALE_HANDLE;
        return packComposition.RetagLiveLease(original.OriginalLease(), to, &original.native_);
    }
private:
    VnResult ReleaseOriginalPackEpisode(PackComposition::OriginalLeaseEpisode& original) noexcept {
        if (!packComposition.MatchesOriginalLease(original)) return VN_STALE_HANDLE;
        auto& record = packComposition.originalLeaseRecords_[original.index_];
        if (record.resourceReceiver != &packs || record.phase == PackComposition::OriginalLeasePhase::Faulted)
            return VN_RUNTIME_RECREATE_REQUIRED;
        if (record.originalInvocation != nullptr && !record.originalInvocationEnded &&
            !record.originalInvocationQuiesced) return VN_IN_USE;
        if (!record.resourceReturned) {
            record.resourceResult = packs.ReleaseResourceClosure(record.acquiredResource);
            if (record.resourceResult == VN_OK) record.resourceReturned = true;
            else record.phase = PackComposition::OriginalLeasePhase::Faulted;
        }
        if (!record.definitionEnded) (void)packComposition.EndOriginalDefinition(original.index_);
        if (record.phase == PackComposition::OriginalLeasePhase::Faulted) {
            runtimeRecreateRequired = true;
            return VN_RUNTIME_RECREATE_REQUIRED;
        }
        return packComposition.DischargeOriginalLease(original);
    }
    VnResult AcquireOriginalPackIdentityLease(VnObjectId stableId, PackLeaseSlice slice,
        PackIdentityLookup* out, OriginalPackIdentityLease& original) noexcept {
        if (out == nullptr) return VN_INVALID_ARG;
        if (original.receiver_ != nullptr || original.native_.owner_ != nullptr) return VN_IN_USE;
        *out = {};
        PackIdentityLookup prepared;
        PackComposition::OriginalLeaseEpisode native;
        auto result = packComposition.PrepareLookup(stableId, slice, &prepared, &native);
        if (result != VN_OK) { *out = prepared; return result; }
        const auto abortBeforeResource = [&](VnResult failure) noexcept {
            const auto returned = packComposition.AbortOriginalPreparedLease(native);
            if (returned != VN_OK) runtimeRecreateRequired = true;
            prepared.result = returned == VN_OK ? failure : VN_RUNTIME_RECREATE_REQUIRED;
            prepared.lease = {}; prepared.action = nullptr; prepared.playableRow = nullptr;
            *out = prepared;
            return prepared.result;
        };
        std::shared_ptr<const std::vector<PackResourceRevisionToken>> resourceTokens;
        result = packComposition.PreparedResourceClosureBacking(prepared.lease, &resourceTokens);
        if (result != VN_OK) return abortBeforeResource(result);
        PackResourceClosureToken closure = 0;
        result = packs.AcquirePreparedResourceClosure(resourceTokens, &closure);
        if (result != VN_OK) return abortBeforeResource(result);
        const bool bound = packComposition.BindOriginalResources(native, &packs, closure);
        assert(bound);
        (void)bound;
#if defined(HARKEN_TEST_PACK_RESOURCE_OBSERVATION)
        if (originalPackResourceAcquiredHookForTest != nullptr)
            originalPackResourceAcquiredHookForTest(*this, native.OriginalLease(), closure,
                originalPackResourceAcquiredContextForTest);
#endif
        result = packComposition.CommitPreparedLease(&prepared.lease, closure, &native);
        if (result != VN_OK) {
            const auto returned = ReleaseOriginalPackEpisode(native);
            prepared.result = returned == VN_OK ? result : VN_RUNTIME_RECREATE_REQUIRED;
            prepared.lease = {}; prepared.action = nullptr; prepared.playableRow = nullptr;
            *out = prepared;
            return prepared.result;
        }
        original.receiver_ = this; original.native_ = std::move(native);
        prepared.result = VN_OK; *out = prepared;
        return VN_OK;
    }
    void ShutdownOriginalPackIdentityLeasesAfterQuiescence() noexcept {
        packComposition.originalLeasesQuiesced_ = true;
        for (std::size_t index = 0; index < packComposition.originalLeaseRecords_.size(); ++index) {
            auto& record = packComposition.originalLeaseRecords_[index];
            if (!record.active) continue;
            // Actual joined native storage Return authorizes only quiescent exit.
            record.originalInvocationQuiesced = true;
            if (record.phase != PackComposition::OriginalLeasePhase::Faulted && record.handle != nullptr &&
                record.resourceReceiver == &packs) (void)ReleaseOriginalPackEpisode(*record.handle);
            if (record.active) packComposition.QuiesceOriginalLease(index);
        }
    }
public:
    VnResult AcquirePackIdentityLease(
        VnObjectId stableId, PackIdentityLookup* out) noexcept {
        return AcquirePackIdentityLease(
            stableId, PackLeaseSlice::SharedActionAndDeferred, out);
    }

    VnResult AcquirePackIdentityLease(
        VnObjectId stableId, PackLeaseSlice slice,
        PackIdentityLookup* out, OriginalPackIdentityLease* original = nullptr) noexcept {
        if (original != nullptr) return AcquireOriginalPackIdentityLease(stableId, slice, out, *original);
        if (out == nullptr) {
            return VN_INVALID_ARG;
        }
        *out = {};
        PackIdentityLookup prepared;
        VnResult result =
            packComposition.PrepareLookup(stableId, slice, &prepared);
        if (result != VN_OK) {
            *out = prepared;
            return result;
        }
        std::shared_ptr<const std::vector<PackResourceRevisionToken>> resourceTokens;
        result = packComposition.PreparedResourceClosureBacking(prepared.lease, &resourceTokens);
        if (result != VN_OK) {
            packComposition.AbandonPreparedLease(prepared.lease);
            prepared.result = result;
            prepared.lease = {};
            prepared.action = nullptr;
            prepared.playableRow = nullptr;
            *out = prepared;
            return result;
        }
        PackResourceClosureToken closureToken = 0;
        result = packs.AcquirePreparedResourceClosure(resourceTokens, &closureToken);
        if (result != VN_OK) {
            packComposition.AbandonPreparedLease(prepared.lease);
            prepared.result = result;
            prepared.lease = {};
            prepared.action = nullptr;
            prepared.playableRow = nullptr;
            *out = prepared;
            return result;
        }
        result = packComposition.CommitPreparedLease(&prepared.lease,
                                                     closureToken);
        if (result != VN_OK) {
            const VnResult released =
                packs.ReleaseResourceClosure(closureToken);
            packComposition.AbandonPreparedLease(prepared.lease);
            prepared.result = released == VN_OK
                                  ? result
                                  : VN_RUNTIME_RECREATE_REQUIRED;
            prepared.lease = {};
            prepared.action = nullptr;
            prepared.playableRow = nullptr;
            *out = prepared;
            return prepared.result;
        }
        prepared.result = VN_OK;
        *out = prepared;
        return VN_OK;
    }

    VnResult ReleasePackIdentityLease(
        const PackPartitionLease& lease) noexcept {
        if (packComposition.HasOriginalLeaseEpisode(lease)) return VN_IN_USE;
        if (packComposition.ValidateLease(lease) != VN_OK ||
            packs.ValidateResourceClosure(lease.resourceClosureToken) !=
                VN_OK) {
            return VN_STALE_HANDLE;
        }
        const VnResult releasedResources =
            packs.ReleaseResourceClosure(lease.resourceClosureToken);
        const VnResult releasedDefinition =
            packComposition.ReleaseLease(lease);
        return releasedResources == VN_OK && releasedDefinition == VN_OK
                   ? VN_OK
                   : VN_RUNTIME_RECREATE_REQUIRED;
    }

    bool CaptureTimelineVoiceInstallIdentity(
        VnVoiceHandle render, EngineTimelineVoiceInstallIdentity* out) const noexcept {
        const auto slot = static_cast<std::uint32_t>(render);
        if (out == nullptr || render == 0 || slot >= timelineVoiceInstallIdentities.size() ||
            timelineVoiceInstallIdentities[slot].renderHandle != render) return false;
        *out = timelineVoiceInstallIdentities[slot];
        return true;
    }

    bool ClearTimelineVoiceInstallIdentity(VnVoiceHandle render) noexcept {
        const auto slot = static_cast<std::uint32_t>(render);
        if (render == 0 || slot >= timelineVoiceInstallIdentities.size() ||
            timelineVoiceInstallIdentities[slot].renderHandle != render) return false;
        timelineVoiceInstallIdentities[slot] = {};
        return true;
    }

    bool ClearOriginalTimelineVoiceInstallIdentity(VnVoiceHandle render,
        VoiceReleaseObserver originalRelease, TimelineArmToken originalArm) noexcept {
        EngineTimelineVoiceInstallIdentity actual{};
        if (!CaptureTimelineVoiceInstallIdentity(render, &actual) ||
            actual.releaseObserver.slot != originalRelease.slot ||
            actual.releaseObserver.targetSequence != originalRelease.targetSequence ||
            !OriginalSourceSubmissionRegistry::SameToken(actual.startArm, originalArm)) return false;
        return ClearTimelineVoiceInstallIdentity(render);
    }

    void InvalidateProjectGenerationReservations() noexcept {
        if (projectGenerationReservationDomain != nullptr &&
            projectGenerationReservationDomain->live) {
            projectGenerationReservationDomain->live = false;
            projectGenerationReservationDomain->liveEngine = nullptr;
            projectGenerationReservationDomain->epoch = 2;
        }
    }

    const OriginalPackIdentityLease* OriginalActionFiringPackLease(ActionFiringHandle firing) const noexcept {
        for (const auto& holder : originalActionFiringPackHolders)
            if (holder.firing_ == firing) return &holder.original_;
        return nullptr;
    }

    bool OriginalActionFiringOwnsLease(ActionFiringHandle root,
        const PackPartitionLease& lease) const noexcept {
        for (const auto& holder : originalActionFiringPackHolders)
            if (holder.firing_ != 0 && holder.root_ == root &&
                packComposition.SameFullOriginalLease(holder.owner_.pack, lease)) return true;
        return false;
    }

    bool OriginalActionFiringCapacity(std::uint32_t count) const noexcept {
        std::uint32_t free = 0;
        for (const auto& holder : originalActionFiringPackHolders)
            if (holder.firing_ == 0 && !holder.original_.Valid()) ++free;
        return count <= free;
    }

    bool AdoptOriginalActionFiringPack(ActionFiringHandle firing,
        OriginalPackIdentityLease& original, const ActionFiringOwnerStamp& owner) noexcept {
        ActionFiringSnapshot actual{};
        if (!OriginalPackIdentityLeaseMatches(original, owner.pack) ||
            original.Phase() != PackComposition::OriginalLeasePhase::Live ||
            owner.kind != ActionFiringOwnerKind::Pack ||
            !actionFirings.Capture(firing, &actual) || actual.owner != owner ||
            actual.acceptedLogicSequence == 0) return false;
        const auto action = packComposition.FindActionAtLease(owner.pack, actual.actionId);
        if (action == nullptr || action->programSemanticFingerprint != actual.programSemanticFingerprint)
            return false;
        ActionFiringHandle root = firing;
        for (std::size_t depth = 0; actual.parent != 0; ++depth) {
            if (depth >= originalActionFiringPackHolders.size()) return false;
            root = actual.parent;
            if (!actionFirings.Capture(root, &actual)) return false;
        }
        for (const auto& holder : originalActionFiringPackHolders)
            if (holder.firing_ == firing) return false;
        for (auto& holder : originalActionFiringPackHolders) {
            if (holder.firing_ != 0 || holder.original_.Valid()) continue;
            holder.original_ = std::move(original);
            holder.firing_ = firing; holder.root_ = root; holder.owner_ = owner;
            holder.returned_ = false;
            return true;
        }
        return false;
    }

    bool TransferOriginalDeferredPostActionPack(ActionDeferredRecordToken token,
        ActionFiringHandle firing, const ActionFiringOwnerStamp& owner) noexcept {
        if (!MatchesOriginalDeferredPackRecord(token) ||
            actionDeferredRecords.State(token) != ActionDeferredRecordState::Claimed) return false;
        auto& holder = originalDeferredPackHolders[token.slot];
        ActionDeferredPairState pair{};
        ActionDeferredRecord record{};
        ActionFiringSnapshot child{};
        if (!actionDeferredRecords.InspectPair(timelineDeferred, holder.pool_, token, &pair) ||
            pair.recordState != ActionDeferredRecordState::Claimed ||
            pair.poolState != DeferredIntentEntryState::Claimed ||
            !actionDeferredRecords.Capture(token, &record) ||
            record.step.opcode != ActionOpcodeV1::PostAction ||
            !actionFirings.Capture(firing, &child) || child.parent != record.firing ||
            child.actionId != record.step.target.stableId ||
            child.programSemanticFingerprint != record.step.target.expectedProgramSemanticFingerprint)
            return false;
        if (holder.transferred_ || holder.returned_ ||
            !packComposition.SameFullOriginalLease(holder.lease_, owner.pack) ||
            !AdoptOriginalActionFiringPack(firing, holder.original_, owner)) return false;
        holder.transferred_ = true; holder.transferredActionFiring_ = firing;
        return true;
    }

    bool OriginalDeferredPostActionTransferred(ActionDeferredRecordToken token,
        ActionFiringHandle firing) const noexcept {
        if (!MatchesOriginalDeferredPackRecord(token)) return false;
        const auto& holder = originalDeferredPackHolders[token.slot];
        if (!holder.transferred_ || holder.transferredActionFiring_ != firing ||
            holder.original_.Valid()) return false;
        for (const auto& native : originalActionFiringPackHolders)
            if (native.firing_ == firing && native.original_.Valid() &&
                packComposition.SameFullOriginalLease(native.owner_.pack, holder.lease_)) return true;
        return false;
    }

    VnResult ReleaseOriginalActionFiringTree(ActionFiringHandle root) noexcept {
        for (auto& holder : originalActionFiringPackHolders) {
            if (holder.firing_ == 0 || holder.root_ != root) continue;
            ActionFiringSnapshot actual{};
            if (!actionFirings.BelongsToTree(root, holder.firing_) ||
                !actionFirings.Capture(holder.firing_, &actual) || actual.owner != holder.owner_ ||
                actual.state != ActionFiringState::TerminalRetained ||
                actual.ownedDeferredCount != 0 || actual.livePlayableCount != 0) {
                runtimeRecreateRequired = true; return VN_RUNTIME_RECREATE_REQUIRED;
            }
            if (holder.returned_) continue;
            if (!OriginalPackIdentityLeaseMatches(holder.original_, holder.owner_.pack)) {
                runtimeRecreateRequired = true; return VN_RUNTIME_RECREATE_REQUIRED;
            }
            const auto result = ReleaseOriginalPackIdentityLease(holder.original_);
            if (result != VN_OK) {
                if (result != VN_IN_USE) runtimeRecreateRequired = true;
                return result;
            }
            holder.returned_ = true;
        }
        return VN_OK;
    }

    void ForgetReleasedOriginalActionFiringTree(ActionFiringHandle root) noexcept {
        for (auto& holder : originalActionFiringPackHolders) {
            if (holder.firing_ == 0 || holder.root_ != root) continue;
            if (!holder.returned_ || holder.original_.Valid()) {
                runtimeRecreateRequired = true; continue;
            }
            holder.firing_ = 0; holder.root_ = 0; holder.owner_ = {}; holder.returned_ = false;
        }
    }

    bool MatchesOriginalDeferredPackRecord(ActionDeferredRecordToken token) const noexcept {
        if (!token.Valid() || token.slot >= originalDeferredPackHolders.size()) return false;
        const auto& original = originalDeferredPackHolders[token.slot];
        return original.record_.slot == token.slot && original.record_.generation == token.generation;
    }

    const OriginalPackIdentityLease* OriginalDeferredPackLease(ActionDeferredRecordToken token) const noexcept {
        if (!MatchesOriginalDeferredPackRecord(token)) return nullptr;
        const auto& original = originalDeferredPackHolders[token.slot];
        return original.original_.Valid() ? &original.original_ : nullptr;
    }

    bool OriginalDeferredPackFinalizationReady(ActionDeferredRecordToken token) const noexcept {
        if (!MatchesOriginalDeferredPackRecord(token)) return true;
        const auto& original = originalDeferredPackHolders[token.slot];
        return !original.original_.Valid() && (original.returned_ || original.transferred_);
    }

    void ForgetFinalizedOriginalDeferredPack(ActionDeferredRecordToken token) noexcept {
        if (!MatchesOriginalDeferredPackRecord(token)) return;
        assert(OriginalDeferredPackFinalizationReady(token));
        if (!OriginalDeferredPackFinalizationReady(token)) { runtimeRecreateRequired = true; return; }
        auto& original = originalDeferredPackHolders[token.slot];
        original.record_ = {}; original.pool_ = {}; original.lease_ = {};
        original.transferred_ = false; original.transferredActionFiring_ = 0; original.returned_ = false;
        original.heldCancellationOwned_ = false;
        original.claimFinalizationPending_ = false;
    }

    VnResult ReleaseOriginalDeferredPack(ActionDeferredRecordToken token,
        const PackPartitionLease& lease) noexcept {
        if (!MatchesOriginalDeferredPackRecord(token)) return ReleasePackIdentityLease(lease);
        auto& original = originalDeferredPackHolders[token.slot];
        if (!packComposition.SameFullOriginalLease(original.lease_, lease)) {
            runtimeRecreateRequired = true;
            return VN_RUNTIME_RECREATE_REQUIRED;
        }
        if (original.returned_) return VN_OK;
        if (original.transferred_ || !original.original_.Valid() ||
            !packComposition.SameFullOriginalLease(original.original_.OriginalLease(), lease))
            { runtimeRecreateRequired = true; return VN_RUNTIME_RECREATE_REQUIRED; }
        const auto result = ReleaseOriginalPackIdentityLease(original.original_);
        if (result == VN_OK) original.returned_ = true;
        else if (result == VN_IN_USE) {
            const auto state = actionDeferredRecords.State(token);
            if (state == ActionDeferredRecordState::Claimed || state == ActionDeferredRecordState::CancelRequested)
                original.claimFinalizationPending_ = true;
        } else runtimeRecreateRequired = true;
        return result;
    }

    VnResult RetagOriginalDeferredPack(ActionDeferredRecordToken token,
        const PackPartitionLease& lease) noexcept {
        if (!MatchesOriginalDeferredPackRecord(token))
            return packComposition.RetagLiveLease(lease, PackLeaseSlice::Playable);
        auto& original = originalDeferredPackHolders[token.slot];
        if (!original.original_.Valid() || original.transferred_ || original.returned_ ||
            !packComposition.SameFullOriginalLease(original.original_.OriginalLease(), lease)) {
            runtimeRecreateRequired = true;
            return VN_RUNTIME_RECREATE_REQUIRED;
        }
        return RetagOriginalPackIdentityLease(original.original_, PackLeaseSlice::Playable);
    }

    bool OriginalPackIdentityLeaseMatches(const OriginalPackIdentityLease& original,
        const PackPartitionLease& lease) const noexcept {
        return original.receiver_ == this && packComposition.MatchesOriginalLease(original.native_) &&
            packComposition.SameFullOriginalLease(original.OriginalLease(), lease);
    }

    bool OriginalDeferredClaimRetained(ActionDeferredRecordToken token,
        DeferredIntentToken pool, const PackPartitionLease& lease) const noexcept {
        if (!MatchesOriginalDeferredPackRecord(token)) return false;
        const auto& original = originalDeferredPackHolders[token.slot];
        if (!original.claimFinalizationPending_ || original.returned_ || original.transferred_ ||
            !OriginalPackIdentityLeaseMatches(original.original_, lease) ||
            original.original_.Phase() != PackComposition::OriginalLeasePhase::Live ||
            original.pool_.slot != pool.slot || original.pool_.generation != pool.generation) return false;
        ActionDeferredPairState pair{};
        return actionDeferredRecords.InspectPair(timelineDeferred, pool, token, &pair) &&
            pair.recordToken.slot == token.slot && pair.recordToken.generation == token.generation &&
            pair.poolToken.slot == pool.slot && pair.poolToken.generation == pool.generation &&
            (pair.recordState == ActionDeferredRecordState::Claimed ||
             pair.recordState == ActionDeferredRecordState::CancelRequested) &&
            pair.poolState == DeferredIntentEntryState::Claimed;
    }

    bool PrepareOriginalDeferredPackCommit(const EngineActionDeferredScratch& scratch) const noexcept {
        if (!scratch.pairCommitProof.Covers(actionDeferredRecords, scratch.recordToken,
                                           timelineDeferred, scratch.poolToken)) return false;
        if (!scratch.originalPackLease.Valid()) return true;
        if (!scratch.recordToken.Valid() || scratch.recordToken.slot >= originalDeferredPackHolders.size() ||
            !scratch.poolToken.Valid() || !scratch.pairCommitProof.Valid() ||
            actionDeferredRecords.State(scratch.recordToken) != ActionDeferredRecordState::Prepared ||
            scratch.originalPackLease.receiver_ != this ||
            !packComposition.MatchesOriginalLease(scratch.originalPackLease.native_) ||
            !packComposition.SameFullOriginalLease(scratch.originalPackLease.OriginalLease(), scratch.targetPackLease))
            return false;
        const auto& original = originalDeferredPackHolders[scratch.recordToken.slot];
        return !original.record_.Valid() && !original.original_.Valid();
    }

    void ConsumeOriginalDeferredPackPreparation(EngineActionDeferredScratch& scratch) noexcept {
        const bool ready = PrepareOriginalDeferredPackCommit(scratch);
        assert(ready);
        if (!ready) { runtimeRecreateRequired = true; return; }
        const auto token = scratch.recordToken;
        const auto pool = scratch.poolToken;
        actionDeferredRecords.ConsumePreparedPair(std::move(scratch.pairCommitProof));
        if (scratch.originalPackLease.Valid()) {
            auto& original = originalDeferredPackHolders[token.slot];
            original.lease_ = scratch.originalPackLease.OriginalLease();
            original.original_ = std::move(scratch.originalPackLease);
            original.record_ = token; original.pool_ = pool;
        }
    }

    bool BindOriginalDeferredPlayAttempt(EngineActionPlayStorage& storage,
        ActionDeferredRecordToken token, const PackPartitionLease& lease) noexcept {
        if (!MatchesOriginalDeferredPackRecord(token)) return true;
        auto& original = originalDeferredPackHolders[token.slot];
        if (!original.original_.Valid() || original.transferred_ || original.returned_ ||
            actionDeferredRecords.State(token) != ActionDeferredRecordState::Claimed ||
            !packComposition.SameFullOriginalLease(original.original_.OriginalLease(), lease) ||
            storage.originalPackLease.Valid() || storage.originalDeferredPackRecord.Valid()) return false;
        storage.originalDeferredPackRecord = token;
        return true;
    }

    void MarkOriginalDeferredHeldCancellation(ActionDeferredRecordToken token) noexcept {
        if (MatchesOriginalDeferredPackRecord(token))
            originalDeferredPackHolders[token.slot].heldCancellationOwned_ = true;
    }

    bool OriginalDeferredHeldCancellationPending(ActionDeferredRecordToken token) const noexcept {
        return MatchesOriginalDeferredPackRecord(token) &&
            originalDeferredPackHolders[token.slot].heldCancellationOwned_;
    }

    bool OriginalDeferredClaimFinalizationPending(ActionDeferredRecordToken token) const noexcept {
        return MatchesOriginalDeferredPackRecord(token) &&
            originalDeferredPackHolders[token.slot].claimFinalizationPending_;
    }

private:
    OriginalPackIdentityLease* OriginalPreparingPackOwner(EngineActionPlayStorage& storage) noexcept {
        if (storage.originalPackLease.Valid()) return &storage.originalPackLease;
        const auto token = storage.originalDeferredPackRecord;
        if (!MatchesOriginalDeferredPackRecord(token)) return nullptr;
        auto& original = originalDeferredPackHolders[token.slot];
        return original.original_.Valid() ? &original.original_ : nullptr;
    }
public:
    bool InvocationBorrowsOriginalDeferredPack(const EnginePlayableStartRow& invocation) const noexcept {
        const auto* storage = invocation.preparingStorage;
        return storage != nullptr && storage->generation == invocation.preparingStorageGeneration &&
            storage->originalDeferredPackRecord.Valid() &&
            OriginalDeferredPackLease(storage->originalDeferredPackRecord) != nullptr;
    }

    bool ActionPlanWorkspaceFits(std::uint32_t steps, std::uint32_t firings) const noexcept {
        const auto plays = std::min<std::size_t>(steps, actionPlayScratch.size());
        return firings <= actionFiringPlanScratch.size() && steps <= actionStepPlanScratch.size() &&
            steps <= actionVariableWriteScratch.size() && steps <= actionSelectionResetScratch.size() &&
            steps <= actionFixedSelectionCommitInputs.size() && steps <= actionQueueResetScratch.size() &&
            steps <= actionQueueCheckpointResetScratch.size() && plays <= actionPlayAttemptTokens.size() &&
            plays <= initialPendingVariableOwnerScratch.size();
    }
    bool PrepareActionPlanWorkspace(std::uint32_t steps, std::uint32_t firings) noexcept {
        if (steps > kHardActionExpandedWorkCapacity || firings > kHardActionExpandedWorkCapacity ||
            memguard::IsAudioTagged()) return false;
        const auto grow = [](auto& column, std::size_t count) {
            if (column.size() < count) column.resize(count);
        };
        try {
            grow(actionFiringPlanScratch, firings);
            grow(actionStepPlanScratch, steps);
            grow(actionVariableWriteScratch, steps);
            grow(actionSelectionResetScratch, steps);
            grow(actionFixedSelectionCommitInputs, steps);
            grow(actionQueueResetScratch, steps);
            grow(actionQueueCheckpointResetScratch, steps);
            grow(actionPlayAttemptTokens, std::min<std::size_t>(steps, actionPlayScratch.size()));
            grow(initialPendingVariableOwnerScratch, std::min<std::size_t>(steps, actionPlayScratch.size()));
            return true;
        } catch (...) { return false; }
    }
    bool PrepareImmediateEntryStorage(EngineImmediateEntryAttempt& attempt,
                                     EngineImmediateEntryStorageDemand demand) noexcept {
        if (attempt.active || attempt.selectionProducerCount != 0 || attempt.selectionCensusRoot != 0 ||
            attempt.selectionCensusGeneration != 0 || memguard::IsAudioTagged()) return false;
        for (std::uint32_t source = 0; source < attempt.plainSourceCount; ++source)
            if (attempt.plainSources[source].bindingReader.Valid()) return false;
        try {
            if (demand.queueCoordinators && !attempt.HasQueueCoordinators()) {
                detail::LogicAllocCheckpoint(detail::LogicAllocSite::ImmediateEntryStorage);
                attempt.queueCoordinators_ = std::make_unique<EngineImmediateQueueCoordinators>();
            }
            const auto grow = [](auto& column, std::uint32_t count) {
                if (column.size() >= count) return;
                detail::CheckedGrow(column, count, detail::LogicAllocSite::ImmediateEntryStorage);
                column.resize(count);
            };
            grow(attempt.plainSources, demand.plainSources);
            grow(attempt.plainStops, demand.plainStops);
            grow(attempt.privateVoices, demand.privateVoices);
            const auto count = demand.selectionProducers;
            if (attempt.selectionProducers.size() < count) attempt.selectionProducers.resize(count);
            if (attempt.selectionCommitInputs.size() < count) attempt.selectionCommitInputs.resize(count);
            if (attempt.selectionPublicationCells.size() < count) attempt.selectionPublicationCells.resize(count);
            return true;
        } catch (...) { return false; }
    }
    bool EnsureImmediateEntryAttempt(std::uint32_t slot) noexcept {
        if (slot < immediateEntryAttempts.size()) return true;
        if (slot != immediateEntryAttempts.size()) return false;
        return immediateEntryAttempts.EmplaceBack();
    }
    std::size_t ImmediateEntryAttemptStorageBytes() const noexcept {
        auto bytes = immediateEntryAttempts.RequestedBytes();
        const auto column = []<class T>(const std::vector<T>& values) noexcept {
            return values.capacity() * sizeof(T);
        };
        for (const auto& attempt : immediateEntryAttempts)
            bytes += column(attempt.selectionProducers) + column(attempt.selectionCommitInputs) +
                column(attempt.selectionPublicationCells) +
                column(attempt.plainSources) + column(attempt.plainStops) + column(attempt.privateVoices) +
                (attempt.originalC0Factory == nullptr ? 0u : sizeof(OriginalInitialC0FactoryRecord)) +
                (attempt.HasQueueCoordinators() ? sizeof(EngineImmediateQueueCoordinators) : 0u);
        return bytes;
    }

    bool EnsureActionPlaySlot(std::uint32_t slot) noexcept {
        if (actionPlayScratch.IsConstructed(slot)) return true;
        if (slot >= actionPlayScratch.size() || actionPlayScratch.PreparedUseActive()) return false;
        if (!actionPlayStorage.Emplace(slot, config.maxPlayableInstances, playableStorage.memberStride,
                std::max(config.maxTransactionEvents, config.maxKnownScheduleLeaves * 4u),
                &actionPlayOwnerLimits)) return false;
        if (!pendingOrdinarySelections.Emplace(slot) || !preparedPendingVariableGroups.Emplace(slot)) return false;
        return actionPlayScratch.Emplace(slot, actionPlayStorage[slot]);
    }
    bool PreparePrivateInitialVcLaneStorage(EngineActionPlayScratch& scratch) noexcept {
        auto& held = scratch.privateInitialVcPageCohort;
        if (memguard::IsAudioTagged() || !scratch.prepared || !scratch.variableControl ||
            held.backing != EngineActionPlayScratch::PrivateInitialVcPageCohort::Backing::None ||
            held.sequence != 0 || held.laneMemberCount != 0 || held.laneOwnedGroupCount != 0 ||
            held.laneArmPreparationCount != 0 || held.laneMembersDeclared ||
            held.laneTransferRequired || held.pageTransferRequired || held.laneAcceptedOriginals) return false;
        auto& storage = scratch.storage;
        const auto capacity = storage.privateInitialVcLaneCapacity;
        try {
            const auto grow = [capacity](auto& column) {
                if (column.size() >= capacity) return;
                detail::CheckedGrow(column, capacity, detail::LogicAllocSite::PrivateVcLaneStorage);
                column.resize(capacity);
            };
            grow(storage.privateInitialVcLaneMembers);
            grow(storage.privateInitialVcLaneGroups);
            grow(storage.privateInitialVcArmPreparations);
            // No reader borrows these arrays before cohort preparation. Retain
            // their full backing once prepared, including across failed retries.
            held.laneMemberManifests = storage.privateInitialVcLaneMembers;
            return true;
        } catch (...) { return false; }
    }
    static bool ActionPlaySlotReusable(const EngineActionPlayScratch& scratch) noexcept {
        return scratch.storage.phase == EngineFrontierAttemptPhase::Free && scratch.storage.generation != UINT64_MAX;
    }
    bool PrepareActionPlayBacking(std::uint32_t low, std::uint32_t high,
        StableSlotStorage<EngineActionPlayScratch>::Preparation* out) noexcept {
        return actionPlayScratch.PrepareBacking(low, high, ActionPlaySlotReusable,
            [this](std::uint32_t slot) noexcept { return EnsureActionPlaySlot(slot); }, out);
    }
    bool ProtectActionPlayBackingForAcquire(std::uint32_t slot) noexcept {
        return actionPlayScratch.ProtectBackingForAcquire(slot, ActionPlaySlotReusable,
            [this](std::uint32_t next) noexcept { return EnsureActionPlaySlot(next); });
    }
    std::size_t ActionPlayStorageBytes() const noexcept {
        return actionPlayStorage.RequestedBytes() + actionPlayScratch.RequestedBytes();
    }
    std::size_t ActionPlayDirectoryBytes() const noexcept {
        return actionPlayStorage.DirectoryBytes() + actionPlayScratch.DirectoryBytes();
    }
    std::size_t ActionPlayCommittedBytes() const noexcept {
        return actionPlayStorage.CommittedBytes() + actionPlayScratch.CommittedBytes();
    }
    std::size_t ActionPlayReservedBytes() const noexcept {
        return actionPlayStorage.ReservedBytes() + actionPlayScratch.ReservedBytes();
    }
    // Disjoint owned allocations; row requests, committed pages and VA remain separate measures.
    std::size_t ActionPlayDynamicBytes() const noexcept;
    std::size_t PrivateImmediateEntryResultStorageBytes() const noexcept {
        std::size_t bytes = 0;
        for (const auto& service : playableServices)
            bytes += service.privateImmediateEntry.results.capacity() * sizeof(ImmediateLanePrivateEntryResultCell);
        return bytes;
    }
    std::size_t PendingSelectionStorageBytes() const noexcept;
    std::size_t CapturedVoiceStorageBytes() const noexcept;
    mutable std::uint64_t capturedVoiceAccountingEpoch = 0;
    std::size_t PreparationStorageBytes() const noexcept;

    bool RecordOriginalWorldNoPackPreparation(EngineActionPlayScratch& scratch,
        const ActionObjectRefV1& target) noexcept {
        return RecordOriginalWorldNoPackPreparation(scratch, target, variables.CurrentPlayableWorld());
    }
    bool RecordOriginalWorldNoPackPreparation(EngineActionPlayScratch& scratch,
        const ActionObjectRefV1& target, const PlayableWorldViewV1& world) noexcept {
        if (packComposition.Authority() == PackAuthorityMode::Pack ||
            scratch.packLeaseOwned || scratch.packLease.leaseToken != 0 ||
            scratch.storage.originalPackLease.Valid() || scratch.storage.originalDeferredPackRecord.Valid() ||
            scratch.storage.generation == 0 || scratch.storage.phase == EngineFrontierAttemptPhase::Free ||
            scratch.storage.phase == EngineFrontierAttemptPhase::Quarantined ||
            !std::any_of(actionPlayScratch.begin(), actionPlayScratch.end(),
                [&scratch](const auto& actual) noexcept { return &actual == &scratch; })) return false;
        if (world.generation == 0) return false;
        auto& original = scratch.storage.originalWorldNoPackPreparation;
        if (original.receiver_ != nullptr)
            return original.receiver_ == this && original.storage_ == &scratch.storage &&
                original.storageGeneration_ == scratch.storage.generation &&
                original.worldGeneration_ == world.generation && original.project_ == world.identity.projectId &&
                original.worldFingerprint_ == world.identity.runtimeSemanticFingerprint &&
                original.target_.kind == target.kind && original.target_.stableId == target.stableId &&
                original.target_.expectedProgramSemanticFingerprint == target.expectedProgramSemanticFingerprint;
        original.receiver_ = this;
        original.storage_ = &scratch.storage;
        original.storageGeneration_ = scratch.storage.generation;
        original.worldGeneration_ = world.generation;
        original.project_ = world.identity.projectId;
        original.worldFingerprint_ = world.identity.runtimeSemanticFingerprint;
        original.target_ = target;
        return true;
    }
    bool HasOriginalWorldNoPackPreparation(const EngineActionPlayStorage& storage) const noexcept {
        const auto& original = storage.originalWorldNoPackPreparation;
        return original.receiver_ == this && original.storage_ == &storage && storage.generation != 0 &&
            original.storageGeneration_ == storage.generation && original.worldGeneration_ != 0 &&
            !storage.originalPackLease.Valid() && !storage.originalDeferredPackRecord.Valid();
    }
    bool PrepareOriginalPlayableServiceStorage(EnginePlayableServiceRow& row,
        PlayableInstanceHandle owner, std::uint64_t epoch, EnginePlayableStartRow& invocation,
        bool rootService, EngineActionPlayScratch::PreparedVariableControlOwnerContext* context,
        EngineActionPlayStorage* preparingStorage = nullptr,
        const EnginePlayableServiceRow* preparingParent = nullptr) noexcept {
        if (owner == 0 || epoch == 0 || row.phase != EnginePlayableStartPhase::Free ||
            row.actionPrepared || row.partition.Valid() || row.quarantined || invocation.quarantined ||
            invocation.generation == 0 || invocation.serviceCount == UINT32_MAX ||
            (rootService ? invocation.serviceCount != 0 : invocation.serviceCount == 0) ||
            row.identity.slot >= playableServices.size() ||
            &playableServices[row.identity.slot] != &row ||
            invocation.slot >= playableStarts.size() || &playableStarts[invocation.slot] != &invocation)
            return false;
        const OriginalPlayableServiceLifetimeHolder* parentOriginal = nullptr;
        if (preparingParent != nullptr) {
            if (!MatchesOriginalPlayableServiceStorage(*preparingParent) ||
                preparingParent->invocation != &invocation ||
                preparingParent->invocationGeneration != invocation.generation) return false;
            parentOriginal = &originalPlayableServiceLifetimeHolders[preparingParent->identity.slot];
            if (preparingStorage == nullptr && parentOriginal->packDutyOwner_ != nullptr &&
                !MatchesOriginalPlayableServicePackDuty(*parentOriginal)) return false;
        }
        if (preparingStorage != nullptr) {
            const bool actualStorage = std::any_of(actionPlayStorage.begin(), actionPlayStorage.end(),
                [preparingStorage](const auto& actual) noexcept { return &actual == preparingStorage; });
            if (!actualStorage || preparingStorage->generation == 0 ||
                preparingStorage->phase == EngineFrontierAttemptPhase::Free ||
                preparingStorage->phase == EngineFrontierAttemptPhase::Quarantined) return false;
            const auto& association = preparingStorage->originalInvocationStorage;
            if (association.engine_ != nullptr) return false;
            if (!preparingStorage->originalPendingVariableProducer.Empty()) return false;
            if (nextOriginalPendingVariableProducerMarker == UINT64_MAX) {
                runtimeRecreateRequired = true;
                return false;
            }
            if (preparingStorage->originalDeferredPackRecord.Valid() &&
                OriginalPreparingPackOwner(*preparingStorage) == nullptr) {
                runtimeRecreateRequired = true;
                return false;
            }
            if (auto* issuedPack = OriginalPreparingPackOwner(*preparingStorage)) {
                const auto& owner = *issuedPack;
                if (owner.receiver_ != this || !packComposition.MatchesOriginalLease(owner.native_) ||
                    (preparingStorage->originalPackLease.Valid() &&
                     preparingStorage->originalPackLeaseGeneration != preparingStorage->generation) ||
                    invocation.originalPackLease.Valid()) return false;
                const auto& pack = packComposition.originalLeaseRecords_[owner.native_.index_];
                if (pack.phase != PackComposition::OriginalLeasePhase::Live ||
                    (pack.originalInvocation != nullptr && !pack.originalInvocationEnded)) return false;
            }
        }
        auto& original = originalPlayableServiceLifetimeHolders[row.identity.slot];
        if (original.engine_ != nullptr || original.baseline_.Valid()) return false;
        if (!playableServiceWork.Prepare({row.identity.generation, row.identity.slot, 0}))
            return false;
        const auto partition = context == nullptr
            ? playablePartitions.Reserve(row.identity, nullptr, &original.baseline_)
            : context->ReserveOriginalService(playablePartitions, this, row, &original.baseline_);
        if (!partition.Valid()) {
            if (playablePartitions.OriginalStorageIssuerExhausted()) runtimeRecreateRequired = true;
            return false;
        }
        const auto memberBase = playableStorage.MemberBase(partition.slot);
        const auto memberEnd = memberBase + playableStorage.memberStride;
        for (auto slot = memberBase; slot < memberEnd;
             slot = (slot / 256u + 1u) * 256u) {
            if (memberOnsets.Prepare({epoch, static_cast<std::uint32_t>(slot), 0})) continue;
            original.baseline_.Return();
            const bool cancelled = playablePartitions.Cancel(partition, row.identity);
            assert(cancelled);
            (void)cancelled;
            if (context != nullptr) context->DiscardUnassociatedServiceReservation();
            return false;
        }
        row.invocation = &invocation;
        row.invocationGeneration = invocation.generation;
        ++invocation.serviceCount;
        if (rootService) invocation.rootService = row.identity;
        row.partition = partition;
        row.ownerInstance = owner;
        row.ownerEpoch = epoch;
        row.actionPrepared = true;
        SetPlayableServiceOccupied(row, true);
        original.engine_ = this;
        original.service_ = &row;
        original.invocation_ = &invocation;
        original.reference_ = {row.identity, owner, epoch, invocation.generation};
        if (preparingStorage != nullptr) {
            auto& association = preparingStorage->originalInvocationStorage;
            association.engine_ = this;
            association.attempt_ = preparingStorage;
            association.attemptGeneration_ = preparingStorage->generation;
            association.invocation_ = &invocation;
            association.invocationGeneration_ = invocation.generation;
            if (rootService) {
                invocation.preparingStorage = preparingStorage;
                invocation.preparingStorageGeneration = preparingStorage->generation;
            }
            if (auto* issuedPack = OriginalPreparingPackOwner(*preparingStorage)) {
                auto& pack = packComposition.originalLeaseRecords_[issuedPack->native_.index_];
                pack.originalPreparingStorage = preparingStorage;
                pack.originalPreparingGeneration = preparingStorage->generation;
                pack.originalInvocation = &invocation;
                pack.originalInvocationGeneration = invocation.generation;
                pack.originalInvocationSlot = invocation.slot;
                pack.originalInvocationEnded = false;
                original.packDuty_ = preparingStorage->originalPackLease.Valid()
                    ? OriginalPlayableServicePackDuty::OriginalScratchOwns
                    : OriginalPlayableServicePackDuty::OriginalDeferredRecordOwns;
                original.packDutyOwner_ = issuedPack;
                original.packDutyLease_ = issuedPack->OriginalLease();
                original.packDutyNativeRow_ = issuedPack->native_.index_;
                original.packDutyNativeToken_ = issuedPack->native_.token_;
                original.packDutyNativeIncarnation_ = issuedPack->native_.incarnation_;
                original.packDutyDeferredRecord_ = preparingStorage->originalDeferredPackRecord;
            }
        } else if (parentOriginal != nullptr && parentOriginal->packDutyOwner_ != nullptr) {
            // The same native invocation owns the parent's actual lease episode.
            original.packDuty_ = parentOriginal->packDuty_;
            original.packDutyOwner_ = parentOriginal->packDutyOwner_;
            original.packDutyLease_ = parentOriginal->packDutyLease_;
            original.packDutyNativeRow_ = parentOriginal->packDutyNativeRow_;
            original.packDutyNativeToken_ = parentOriginal->packDutyNativeToken_;
            original.packDutyNativeIncarnation_ = parentOriginal->packDutyNativeIncarnation_;
            original.packDutyDeferredRecord_ = parentOriginal->packDutyDeferredRecord_;
        }
        original.preparingStorage_ = preparingStorage != nullptr ? preparingStorage :
            parentOriginal == nullptr ? nullptr : parentOriginal->preparingStorage_;
        original.preparingStorageGeneration_ = preparingStorage != nullptr ? preparingStorage->generation :
            parentOriginal == nullptr ? 0 : parentOriginal->preparingStorageGeneration_;
        if (preparingStorage != nullptr && HasOriginalWorldNoPackPreparation(*preparingStorage)) {
            const auto& duty = preparingStorage->originalWorldNoPackPreparation;
            original.packDuty_ = OriginalPlayableServicePackDuty::WorldNoPack;
            original.worldNoPackGeneration_ = duty.worldGeneration_;
            original.worldNoPackProject_ = duty.project_;
            original.worldNoPackFingerprint_ = duty.worldFingerprint_;
            original.worldNoPackTarget_ = duty.target_;
        } else if (preparingStorage == nullptr && parentOriginal != nullptr && parentOriginal->HasOriginalWorldNoPackDuty()) {
            original.packDuty_ = OriginalPlayableServicePackDuty::WorldNoPack;
            original.worldNoPackGeneration_ = parentOriginal->worldNoPackGeneration_;
            original.worldNoPackProject_ = parentOriginal->worldNoPackProject_;
            original.worldNoPackFingerprint_ = parentOriginal->worldNoPackFingerprint_;
            original.worldNoPackTarget_ = parentOriginal->worldNoPackTarget_;
        }
        if (context != nullptr) {
            const bool recorded = context->RecordOriginalServiceAssociation();
            assert(recorded);
            (void)recorded;
        }
        if (preparingStorage != nullptr) {
            auto& producer = preparingStorage->originalPendingVariableProducer;
            producer.engine_ = this;
            producer.storage_ = preparingStorage;
            producer.storageGeneration_ = preparingStorage->generation;
            producer.service_ = &row;
            producer.invocation_ = &invocation;
            producer.reference_ = original.reference_;
            producer.marker_ = ++nextOriginalPendingVariableProducerMarker;
        }
        if (originalPlayableServiceReservedHookForTest != nullptr)
            originalPlayableServiceReservedHookForTest(*this, original, originalPlayableServiceReservedContextForTest);
        return true;
    }

    bool MatchesOriginalPlayableServiceStorage(const EnginePlayableServiceRow& row) const noexcept {
        if (row.identity.slot >= originalPlayableServiceLifetimeHolders.size()) return false;
        const auto& original = originalPlayableServiceLifetimeHolders[row.identity.slot];
        return original.engine_ == this && original.service_ == &row &&
            original.invocation_ == row.invocation && original.reference_.token == row.identity &&
            original.reference_.owner == row.ownerInstance && original.reference_.epoch == row.ownerEpoch &&
            original.reference_.invocationGeneration == row.invocationGeneration && row.InvocationValid() &&
            playablePartitions.OriginalStorageMatches(original.baseline_, row.partition, row.identity);
    }

    bool MatchesOriginalPlayableServicePackDuty(
        const OriginalPlayableServiceLifetimeHolder& original) const noexcept {
        if (original.engine_ != this || original.service_ == nullptr ||
            original.service_->identity.slot >= originalPlayableServiceLifetimeHolders.size() ||
            &originalPlayableServiceLifetimeHolders[original.service_->identity.slot] != &original ||
            !MatchesOriginalPlayableServiceStorage(*original.service_)) return false;
        if (original.packDuty_ == OriginalPlayableServicePackDuty::WorldNoPack)
            return original.worldNoPackGeneration_ != 0 && original.packDutyOwner_ == nullptr &&
                original.invocation_->packLease.leaseToken == 0 && !original.invocation_->originalPackLease.Valid();
        const auto* owner = original.packDutyOwner_;
        if (owner == nullptr || owner->receiver_ != this ||
            !packComposition.MatchesOriginalLease(owner->native_) ||
            owner->native_.index_ != original.packDutyNativeRow_ ||
            owner->native_.token_ != original.packDutyNativeToken_ ||
            owner->native_.incarnation_ != original.packDutyNativeIncarnation_ ||
            !packComposition.SameFullOriginalLease(owner->OriginalLease(), original.packDutyLease_)) return false;
        const auto& pack = packComposition.originalLeaseRecords_[owner->native_.index_];
        if (pack.phase != PackComposition::OriginalLeasePhase::Live ||
            pack.originalInvocation != original.invocation_ ||
            pack.originalInvocationGeneration != original.reference_.invocationGeneration ||
            pack.originalInvocationSlot != original.invocation_->slot || pack.originalInvocationEnded) return false;
        if (original.packDuty_ == OriginalPlayableServicePackDuty::OriginalInvocationOwns)
            return owner == &original.invocation_->originalPackLease &&
                packComposition.SameFullOriginalLease(original.invocation_->packLease, original.packDutyLease_);
        const auto* storage = original.preparingStorage_;
        if (storage == nullptr || storage->generation != original.preparingStorageGeneration_ ||
            pack.originalPreparingStorage != storage ||
            pack.originalPreparingGeneration != original.preparingStorageGeneration_ ||
            original.invocation_->preparingStorage != storage ||
            original.invocation_->preparingStorageGeneration != original.preparingStorageGeneration_) return false;
        if (original.packDuty_ == OriginalPlayableServicePackDuty::OriginalScratchOwns)
            return owner == &storage->originalPackLease &&
                storage->originalPackLeaseGeneration == original.preparingStorageGeneration_ &&
                !original.packDutyDeferredRecord_.Valid();
        if (original.packDuty_ != OriginalPlayableServicePackDuty::OriginalDeferredRecordOwns ||
            !MatchesOriginalDeferredPackRecord(original.packDutyDeferredRecord_) ||
            storage->originalDeferredPackRecord.slot != original.packDutyDeferredRecord_.slot ||
            storage->originalDeferredPackRecord.generation != original.packDutyDeferredRecord_.generation)
            return false;
        const auto& deferred = originalDeferredPackHolders[original.packDutyDeferredRecord_.slot];
        const auto state = actionDeferredRecords.State(original.packDutyDeferredRecord_);
        return !deferred.transferred_ && !deferred.returned_ && owner == &deferred.original_ &&
            (state == ActionDeferredRecordState::Claimed || state == ActionDeferredRecordState::CancelRequested);
    }

    bool BorrowOriginalPlayableServiceLifetime(const OriginalPlayableServiceLifetimeHolder& original,
        OriginalPlayableServiceLifetimePin* out) noexcept {
        if (out == nullptr || out->engine_ != nullptr || out->original_ != nullptr || out->storage_.Valid() ||
            original.engine_ != this || original.service_ == nullptr ||
            !MatchesOriginalPlayableServiceStorage(*original.service_) ||
            !MatchesOriginalPlayableServicePackDuty(original)) return false;
        OriginalPlayableServiceLifetimePin loan;
        if (!playablePartitions.BorrowOriginalStorage(original.baseline_, &loan.storage_)) return false;
        loan.engine_ = this;
        loan.original_ = &original;
        loan.reference_ = original.reference_;
        loan.partition_ = original.service_->partition;
        *out = std::move(loan);
        return true;
    }

    bool MatchesOriginalPlayableServiceLifetime(const OriginalPlayableServiceLifetimePin& loan) const noexcept {
        const auto* original = loan.original_;
        if (loan.engine_ != this || original == nullptr || original->engine_ != this ||
            original->service_ == nullptr || original->reference_.token.slot >= originalPlayableServiceLifetimeHolders.size() ||
            &originalPlayableServiceLifetimeHolders[original->reference_.token.slot] != original ||
            !MatchesOriginalPlayableServiceStorage(*original->service_) ||
            !MatchesOriginalPlayableServicePackDuty(*original)) return false;
        return original->reference_.token == loan.reference_.token &&
            original->reference_.owner == loan.reference_.owner && original->reference_.epoch == loan.reference_.epoch &&
            original->reference_.invocationGeneration == loan.reference_.invocationGeneration &&
            original->service_->partition.slot == loan.partition_.slot &&
            original->service_->partition.generation == loan.partition_.generation &&
            playablePartitions.OriginalStorageMatches(loan.storage_, loan.partition_, loan.reference_.token);
    }

    bool CanReclaimOriginalPlayableServiceStorage(const EnginePlayableServiceRow& row) const noexcept {
        return MatchesOriginalPlayableServiceStorage(row) && playablePartitions.OriginalStorageReclaimReady(
            originalPlayableServiceLifetimeHolders[row.identity.slot].baseline_);
    }

    OriginalPendingVariableGroupHolder* PreparedPendingVariableGroupFor(
        const EngineActionPlayStorage& storage) noexcept {
        const auto slot = actionPlayStorage.IndexOf(storage);
        return preparedPendingVariableGroups.IsConstructed(slot) ? &preparedPendingVariableGroups[slot] : nullptr;
    }

    OriginalPendingOrdinarySelectionHolder* PendingOrdinarySelectionFor(
        const EngineActionPlayStorage& storage) noexcept {
        const auto slot = actionPlayStorage.IndexOf(storage);
        return pendingOrdinarySelections.IsConstructed(slot) ? &pendingOrdinarySelections[slot] : nullptr;
    }

    static bool SameSelectionServiceReference(const EnginePlayableServiceReference& left,
        const EnginePlayableServiceReference& right) noexcept {
        return left.token == right.token && left.owner == right.owner && left.epoch == right.epoch &&
            left.invocationGeneration == right.invocationGeneration;
    }

    static bool SameSelectionProducerIdentity(const SelectionPendingBatchIdentityV1& left,
        const SelectionPendingBatchIdentityV1& right) noexcept {
        return left.world == right.world && left.worldSemanticFingerprint == right.worldSemanticFingerprint &&
            left.rootStableId == right.rootStableId && left.callerSeed == right.callerSeed &&
            left.acceptedPostOrdinal == right.acceptedPostOrdinal && left.ticket == right.ticket &&
            left.emitterSlot == right.emitterSlot && left.emitterGeneration == right.emitterGeneration &&
            left.eventBegin == right.eventBegin && left.eventCount == right.eventCount;
    }

    EnginePendingSelectionBatch* ResolvePendingSelectionCensusProducer(
        const EngineInitialSelectionProducer& cell) noexcept {
        if (cell.returned || !cell.token.Valid() || !actionPlayScratch.IsConstructed(cell.token.slot) ||
            !actionPlayStorage.IsConstructed(cell.token.slot)) return nullptr;
        auto& scratch = actionPlayScratch[cell.token.slot];
        auto& storage = actionPlayStorage[cell.token.slot];
        auto* row = ResolvePlayableService(cell.service);
        if (&scratch.storage != &storage || storage.generation != cell.token.generation ||
            storage.phase == EngineFrontierAttemptPhase::Free || scratch.row != row || row == nullptr ||
            !row->InvocationValid() || row->Invocation().actionFiring != cell.firing ||
            !storage.initialSelection.BoundFor(cell.token.generation) ||
            !SameSelectionProducerIdentity(storage.initialSelection.identity, cell.identity)) return nullptr;
        EnginePendingSelectionBatch* batch = nullptr;
        if (!cell.variableControl) {
            auto* holder = PendingOrdinarySelectionFor(storage);
            if (holder == nullptr || !MatchesPendingOrdinarySelection(*holder, storage, *row) ||
                !SameSelectionServiceReference(holder->reference, cell.service)) return nullptr;
            batch = &storage.initialSelectionBatch;
        } else {
            auto* prepared = PreparedPendingVariableGroupFor(storage);
            if (prepared == nullptr || !acceptedPendingVariableGroups.IsConstructed(row->identity.slot)) return nullptr;
            auto& accepted = acceptedPendingVariableGroups[row->identity.slot];
            const bool hasPrepared = !prepared->Empty();
            const bool hasAccepted = !accepted.Empty();
            if (hasPrepared == hasAccepted) return nullptr;
            auto& holder = hasPrepared ? *prepared : accepted;
            const auto root = row->Invocation().variableControlOuterRootInstance != 0
                ? row->Invocation().variableControlOuterRootInstance : row->rootInstance;
            if (holder.engine != this || !holder.cut.Valid() || !holder.producer.Valid() ||
                holder.producer.original_ != &storage.originalPendingVariableProducer || !holder.service.Valid() ||
                !playableInstances.IsOriginalLifetimePinFor(holder.root, root) ||
                !SameSelectionServiceReference(holder.reference, cell.service) ||
                !SameSelectionServiceReference(holder.service.FactoryReference(), cell.service)) return nullptr;
            // Before acceptance the storage owns it; afterwards only the VC holder does.
            if (hasPrepared) {
                if (!holder.initialSelectionBatch.Empty()) return nullptr;
                batch = &storage.initialSelectionBatch;
            } else {
                if (!storage.initialSelectionBatch.Empty() || !holder.accepted) return nullptr;
                batch = &holder.initialSelectionBatch;
            }
        }
        return batch->MatchesOwner(&selectionMemory, cell.identity) ? batch : nullptr;
    }

    bool MatchesPendingSelectionCensus(EngineImmediateEntryAttempt& attempt) noexcept {
        if (attempt.selectionProducerCount == 0 || attempt.selectionCensusRoot == 0 ||
            attempt.selectionCensusGeneration != attempt.generation ||
            (attempt.active && attempt.root != attempt.selectionCensusRoot) ||
            attempt.selectionProducerCount > attempt.selectionProducers.size()) return false;
        for (std::uint32_t index = 0; index < attempt.selectionProducerCount; ++index) {
            const auto& cell = attempt.selectionProducers[index];
            if (!cell.returned && ResolvePendingSelectionCensusProducer(cell) == nullptr) return false;
            if (!cell.returned) {
                const auto& association = actionPlayStorage[cell.token.slot].initialSelectionCensus;
                if (association.attempt != &attempt || association.root != attempt.selectionCensusRoot ||
                    association.generation != attempt.generation || association.index != index) return false;
            }
            if (index != 0) {
                const auto& previous = attempt.selectionProducers[index - 1u];
                if (cell.executionOrdinal <= previous.executionOrdinal ||
                    cell.identity.ticket != previous.identity.ticket ||
                    cell.executeFrame < previous.executeFrame ||
                    (cell.executeFrame == previous.executeFrame &&
                     cell.identity.eventBegin <= previous.identity.eventBegin)) return false;
            }
            for (std::uint32_t previous = 0; previous < index; ++previous)
                if (attempt.selectionProducers[previous].token.slot == cell.token.slot) return false;
        }
        return true;
    }

    bool PreparePendingSelectionCensusCommitInputs(EngineImmediateEntryAttempt& attempt,
        std::uint32_t* count) noexcept {
        if (count == nullptr || !MatchesPendingSelectionCensus(attempt) ||
            attempt.selectionProducerCount > attempt.selectionCommitInputs.size()) return false;
        std::uint32_t prepared = 0;
        for (std::uint32_t index = 0; index < attempt.selectionProducerCount; ++index) {
            const auto& cell = attempt.selectionProducers[index];
            if (cell.returned) continue;
            auto* batch = ResolvePendingSelectionCensusProducer(cell);
            if (batch == nullptr || !batch->Accepted()) return false;
            const auto input = batch->CommitInput();
            if (input.batch == nullptr) return false;
            attempt.selectionCommitInputs[prepared++] = input;
        }
        *count = prepared;
        return true;
    }

    bool ReturnPendingSelectionCensusProducer(const OriginalPendingVariableProducerRetention::Pin& producer,
        const EnginePlayableServiceReference& reference) noexcept {
        if (!producer.Valid() || producer.original_ == nullptr) return false;
        auto* storage = producer.original_->storage_;
        if (storage == nullptr || producer.original_->engine_ != this ||
            producer.original_->storageGeneration_ != storage->generation ||
            !SameSelectionServiceReference(producer.original_->reference_, reference)) return false;
        const auto association = storage->initialSelectionCensus;
        if (association.Empty()) return true;
        bool originalAttempt = false;
        for (std::uint32_t slot = 0; slot < immediateEntryAttempts.size(); ++slot)
            originalAttempt = originalAttempt || association.attempt == &immediateEntryAttempts[slot];
        if (!originalAttempt) return false;
        auto* found = association.attempt;
        if (association.generation != found->generation || found->selectionCensusGeneration != found->generation ||
            association.root != found->selectionCensusRoot || found->selectionCensusRoot == 0 ||
            found->selectionProducerCount > found->selectionProducers.size() ||
            association.index >= found->selectionProducerCount) return false;
        auto* cell = &found->selectionProducers[association.index];
        if (cell->returned || !actionPlayStorage.IsConstructed(cell->token.slot) ||
            &actionPlayStorage[cell->token.slot] != storage || cell->token.generation != storage->generation ||
            !SameSelectionServiceReference(cell->service, reference) ||
            !SameSelectionProducerIdentity(cell->identity, storage->initialSelection.identity)) return false;
        // A prepared VC still has its batch in storage; Close owns its cancellation.
        if (!storage->initialSelectionBatch.Empty() &&
            (!storage->initialSelectionBatch.MatchesOwner(&selectionMemory, cell->identity) ||
             !storage->initialSelectionBatch.Cancel())) return false;
        cell->returned = true;
        storage->initialSelectionCensus = {};
        if (std::all_of(found->selectionProducers.begin(),
                found->selectionProducers.begin() + found->selectionProducerCount,
                [](const auto& item) { return item.returned; })) {
            for (std::uint32_t index = 0; index < found->selectionProducerCount; ++index) {
                found->selectionProducers[index] = {};
                found->selectionCommitInputs[index] = {};
            }
            found->selectionProducerCount = 0;
            found->selectionCensusRoot = 0;
            found->selectionCensusGeneration = 0;
        }
        return true;
    }

    bool MatchesPendingOrdinarySelection(const OriginalPendingOrdinarySelectionHolder& holder,
        const EngineActionPlayStorage& storage, const EnginePlayableServiceRow& row) const noexcept {
        const auto reference = holder.service.FactoryReference();
        return holder.engine == this && holder.storage == &storage && holder.generation == storage.generation &&
            storage.initialSelection.BoundFor(holder.generation) && holder.producer.Valid() &&
            holder.producer.original_ == &storage.originalPendingVariableProducer && holder.service.Valid() &&
            playableInstances.IsOriginalLifetimePinFor(holder.root, row.rootInstance) &&
            holder.reference.token == row.identity && holder.reference.owner == row.ownerInstance &&
            holder.reference.epoch == row.ownerEpoch &&
            holder.reference.invocationGeneration == row.invocationGeneration &&
            reference.token == holder.reference.token && reference.owner == holder.reference.owner &&
            reference.epoch == holder.reference.epoch &&
            reference.invocationGeneration == holder.reference.invocationGeneration;
    }

    bool PreparePendingOrdinarySelection(EngineActionPlayScratch& scratch,
        const PlayableInstanceTable::PreparedParentLinksProof& links) noexcept {
        auto* holder = PendingOrdinarySelectionFor(scratch.storage);
        if (holder == nullptr || !holder->Empty() || scratch.row == nullptr || scratch.variableControl ||
            scratch.row->queueMode || !scratch.row->InvocationValid() || !scratch.row->pendingImmediateEntry ||
            scratch.storage.phase != EngineFrontierAttemptPhase::Preparing ||
            !scratch.storage.initialSelection.BoundFor(scratch.storage.generation) ||
            !scratch.storage.initialSelectionBatch.MatchesOwner(&selectionMemory, scratch.storage.initialSelection.identity) ||
            !playableInstances.MatchesPreparedOriginalAncestry(links, scratch.row->ownerInstance, scratch.row->rootInstance) ||
            scratch.row->identity.slot >= originalPlayableServiceLifetimeHolders.size()) return false;
        auto* originalRoot = ResolveOriginalPlayableBaseline(this, playableInstances, scratch.row->rootInstance);
        OriginalPendingVariableProducerRetention::Pin producer;
        OriginalPlayableServiceLifetimePin service;
        PlayableInstanceTable::OriginalLifetimePin root;
        if (originalRoot == nullptr || !originalRoot->Borrow(&root) ||
            !originalPlayableServiceLifetimeHolders[scratch.row->identity.slot].Borrow(&service) ||
            !BorrowOriginalPendingVariableProducer(scratch.storage.originalPendingVariableProducer,
                scratch.storage, *scratch.row, &producer)) return false;
        if (scratch.voiceStartCount != 1 || scratch.voiceStarts[0].commandCount == 0 ||
            scratch.voiceStarts[0].commandCount > scratch.voiceStarts[0].commands.size()) return false;
        auto* originalSource = OriginalSinglePreparationForArm(scratch.voiceStarts[0].arm);
        OriginalSourceLifetimePin source;
        VoicePool::OriginalSourceBodyLifetimePin body;
        if (scratch.storage.initialSelection.preparation.mode == EngineSelectionPreparationMode::PendingInitial &&
            (originalSource == nullptr || !BorrowOriginalSingleSourcePreparation(*originalSource, &source, &body)))
            return false;
        auto* resourceRow = scratch.frontierResourceService.Valid()
            ? ResolvePlayableService(scratch.frontierResourceService) : scratch.row;
        OriginalPlayableServiceLifetimePin resource;
        if (resourceRow == nullptr || !resourceRow->InvocationValid() ||
            resourceRow->invocation != scratch.row->invocation ||
            resourceRow->invocationGeneration != scratch.row->invocationGeneration ||
            resourceRow->identity.slot >= originalPlayableServiceLifetimeHolders.size() ||
            !originalPlayableServiceLifetimeHolders[resourceRow->identity.slot].Borrow(&resource)) return false;
        VoiceLedger::BackingPreparation ledgerBacking;
        VoiceLedger::MonitorSelectionBacking monitorBacking;
        if (!ledger.PrepareBacking(1, &ledgerBacking) ||
            !ledger.PrepareMonitorSelectionBacking(1, &monitorBacking)) return false;
        holder->nativeLedgerBacking = std::move(ledgerBacking);
        holder->nativeMonitorBacking = std::move(monitorBacking);
        holder->engine = this;
        holder->storage = &scratch.storage;
        holder->generation = scratch.storage.generation;
        holder->reference = service.FactoryReference();
        holder->producer = std::move(producer);
        holder->service = std::move(service);
        holder->resource = std::move(resource);
        holder->resourceReference = {resourceRow->identity, resourceRow->ownerInstance, resourceRow->ownerEpoch,
            resourceRow->invocationGeneration};
        holder->root = std::move(root);
        holder->source = std::move(source);
        holder->body = std::move(body);
        holder->arm = scratch.voiceStarts[0].arm;
        holder->release = scratch.voiceStarts[0].releaseObserver;
        return true;
    }

    bool AcceptPendingOrdinarySelection(EngineActionPlayScratch& scratch) noexcept {
        auto* holder = PendingOrdinarySelectionFor(scratch.storage);
        if (holder == nullptr || holder->accepted || !holder->nativeLedgerBacking.Valid() ||
            !holder->nativeMonitorBacking.Valid() ||
            scratch.row == nullptr || scratch.variableControlCommitted ||
            scratch.row->actionPrepared || !scratch.row->pendingImmediateEntry ||
            scratch.storage.phase != EngineFrontierAttemptPhase::Published ||
            !scratch.storage.ownerLedger.Empty() ||
            !MatchesPendingOrdinarySelection(*holder, scratch.storage, *scratch.row) ||
            !scratch.storage.initialSelectionBatch.MatchesOwner(&selectionMemory, scratch.storage.initialSelection.identity) ||
            !scratch.storage.initialSelectionBatch.Accepted()) return false;
        holder->accepted = true;
        return true;
    }

    bool CancelPendingOrdinarySelectionCandidate(OriginalPendingOrdinarySelectionHolder& holder) noexcept;
    bool HasPendingOrdinarySelection(const EngineImmediateEntryAttempt& attempt) const noexcept {
        if (attempt.selectionProducerCount > attempt.selectionProducers.size()) return false;
        for (std::uint32_t index = 0; index < attempt.selectionProducerCount; ++index) {
            const auto& cell = attempt.selectionProducers[index];
            if (cell.returned || cell.variableControl || !cell.token.Valid() ||
                !actionPlayStorage.IsConstructed(cell.token.slot)) continue;
            const auto& storage = actionPlayStorage[cell.token.slot];
            if (storage.generation == cell.token.generation && storage.initialSelection.BoundFor(cell.token.generation) &&
                storage.initialSelection.preparation.mode == EngineSelectionPreparationMode::PendingInitial) return true;
        }
        return false;
    }
    enum class PendingOrdinarySelectionResult : std::uint8_t { Ready, Waiting, Refused, Rejected };
    PendingOrdinarySelectionResult ServicePendingOrdinarySelections(
        EngineImmediateEntryAttempt& attempt, std::uint64_t anchor) noexcept;

    bool ClosePendingOrdinarySelection(OriginalPendingOrdinarySelectionHolder& holder) noexcept {
        if (holder.Empty()) return true;
        auto* storage = holder.storage;
        const auto* row = ResolvePlayableService(holder.reference);
        if (storage == nullptr || PendingOrdinarySelectionFor(*storage) != &holder || row == nullptr ||
            !MatchesPendingOrdinarySelection(holder, *storage, *row) ||
            (!storage->initialSelectionBatch.Empty() &&
             !storage->initialSelectionBatch.MatchesOwner(&selectionMemory, storage->initialSelection.identity)) ||
            !CancelPendingOrdinarySelectionCandidate(holder) ||
            !storage->initialSelectionBatch.Cancel()) return false;
        if ((storage->initialVariableService != nullptr) != storage->initialVariableCut.Valid() ||
            (storage->initialVariableCut.Valid() &&
             (storage->initialVariableService != &variables ||
              !variables.CanReleaseReadCut(storage->initialVariableCut)))) return false;
        if (!ReturnPendingSelectionCensusProducer(holder.producer, holder.reference)) return false;
        if (holder.source.Valid() &&
            !originalSourceSubmissions.CanReturnUnpublishedOriginalSourcePairStorage(holder.source, holder.body) &&
            !originalSourceSubmissions.CanReturnPublishedOriginalSourcePairStorage(holder.source, holder.body, holder.release))
            return false;
        if (storage->initialVariableCut.Valid()) {
            if (!variables.ReleaseReadCut(storage->initialVariableCut)) return false;
            storage->initialVariableCut = {};
            storage->initialVariableService = nullptr;
        }
        holder.body.Release();
        holder.source.Release();
        holder.resource.Return();
        holder.resourceReference = {};
        holder.arm = {};
        holder.release = {};
        holder.retirement.Release();
        holder.sourceRetired = false;
        holder.retiredServices = {};
        holder.retiredServiceCount = 0;
        holder.retiredChild = 0;
        holder.retiredChildEpoch = 0;
        holder.nativeReplacement = {};
        holder.replacementVoice = {};
        holder.nativeVoiceBacking.Release();
        holder.nativeLedgerBacking.Release();
        holder.nativeMonitorBacking.Release();
        holder.nativePlayBacking.Release();
        holder.nativeMemberBacking.Release();
        holder.nativeRelationBacking.Release();
        holder.nativeProgramBacking.Release();
        holder.nativeScheduleBacking.Release();
        // Disposition may already be ending the root; its original loan still identifies it.
        holder.producer.Return();
        holder.service.Return();
        holder.root.Release();
        const bool accepted = holder.accepted;
        holder.engine = nullptr;
        holder.storage = nullptr;
        holder.generation = 0;
        holder.reference = {};
        holder.accepted = false;
        if (accepted && storage->phase == EngineFrontierAttemptPhase::PrivateCommitted && storage->ownerLedger.Empty())
            storage->phase = storage->originalPendingVariableProducer.ReclaimReady()
                ? EngineFrontierAttemptPhase::Free : EngineFrontierAttemptPhase::PublishedGroupReclaim;
        return true;
    }

    bool BorrowOriginalPendingVariableProducer(OriginalPendingVariableProducerRetention& original,
        EngineActionPlayStorage& storage, const EnginePlayableServiceRow& row,
        OriginalPendingVariableProducerRetention::Pin* out) noexcept {
        if (out == nullptr || out->original_ != nullptr || out->marker_ != 0 ||
            original.engine_ != this || original.storage_ != &storage ||
            &storage.originalPendingVariableProducer != &original ||
            original.storageGeneration_ != storage.generation || original.marker_ == 0 ||
            original.service_ != &row || original.invocation_ != row.invocation ||
            original.reference_.token != row.identity || original.reference_.owner != row.ownerInstance ||
            original.reference_.epoch != row.ownerEpoch ||
            original.reference_.invocationGeneration != row.invocationGeneration ||
            !MatchesOriginalPlayableServiceStorage(row) || original.loanCount_ == UINT32_MAX) return false;
        ++original.loanCount_;
        out->original_ = &original;
        out->marker_ = original.marker_;
        return true;
    }

    bool StagePendingVariableLeafSource(const CommandRecord& header,
        const TimelineArmTable::PreparedCommitProof& arm,
        const VoicePool::PreparedCommitProof& voice,
        OriginalSourceLifetimePin&& source, VoicePool::OriginalSourceBodyLifetimePin&& body,
        ProgramGeneration program, PlayableInstanceHandle parent, std::uint64_t startFrame,
        VoiceReleaseObserver release) noexcept {
        const auto token = arm.OriginalPreparedToken();
        VoiceReleaseObserver actualRelease{};
        if (arm.Owner() != &timelineArms || voice.Owner() != &voices ||
            !token.Valid() || token.slot >= pendingVariableLeafSources.size() ||
            !pendingVariableLeafSources[token.slot].Empty() || !release.Valid() ||
            voice.OriginalPreparedHandle() != OriginalSourceSubmissionRegistry::RenderOf(token) ||
            !voices.CaptureReleaseObserver(voice.OriginalPreparedHandle(), &actualRelease) ||
            release.slot != actualRelease.slot || release.targetSequence != actualRelease.targetSequence ||
            header.opcode != kOpcodePlay ||
            !OriginalSourceSubmissionRegistry::SameToken(OriginalSourceSubmissionRegistry::TokenOf(header), token) ||
            !originalSourceSubmissions.IsOriginalSourceLifetimePinFor(source, token) ||
            !voices.IsOriginalSourceBodyLifetimePinFor(body, token, header.payload.play.sourceIdentity)) return false;
        auto& holder = pendingVariableLeafSources[token.slot];
        holder.source = std::move(source);
        holder.body = std::move(body);
        holder.arm = token;
        holder.sourceIdentity = header.payload.play.sourceIdentity;
        holder.programGeneration = program;
        holder.parent = parent;
        holder.preparedStartFrame = startFrame;
        holder.physicalRelease = release;
        ++pendingVariableLeafSourceCount;
        return true;
    }

    bool MatchesPendingVariableGroupLeaf(const OriginalPendingVariableGroupHolder& group,
        std::uint32_t index) const noexcept {
        if (group.engine != this || index >= group.leafCount || index >= group.leafArms.size() ||
            !group.leafRetained[index]) return false;
        const auto arm = group.leafArms[index];
        if (!arm.Valid() || arm.slot >= pendingVariableLeafSources.size()) return false;
        const auto& holder = pendingVariableLeafSources[arm.slot];
        return holder.group == &group && holder.leafIndex == index && holder.cut == group.cut &&
            holder.domain == OriginalPendingVariableLeafSource::Domain::Current &&
            OriginalSourceSubmissionRegistry::SameToken(holder.arm, arm) &&
            originalSourceSubmissions.IsOriginalSourceLifetimePinFor(holder.source, arm) &&
            voices.IsOriginalSourceBodyLifetimePinFor(holder.body, arm, holder.sourceIdentity);
    }

    bool MatchesPendingVariableGroupFutureSource(const OriginalPendingVariableFutureSource& original) const noexcept {
        if (original.group == nullptr || original.group->engine != this || !original.retained ||
            original.ordinal >= original.group->futureSourceCount || !original.service.Valid() ||
            !original.partition.Valid() || !original.queue.Valid() || !original.parentGroup.Valid() ||
            !original.arm.Valid() || original.arm.slot >= pendingVariableLeafSources.size()) return false;
        const auto& holder = pendingVariableLeafSources[original.arm.slot];
        return holder.domain == OriginalPendingVariableLeafSource::Domain::QueueFuture &&
            holder.group == original.group && holder.cut == original.group->cut &&
            holder.leafIndex < pendingVariableFutureSources.size() &&
            &pendingVariableFutureSources[holder.leafIndex] == &original &&
            OriginalSourceSubmissionRegistry::SameToken(holder.arm, original.arm) &&
            holder.sourceIdentity == original.sourceIdentity && holder.programGeneration == original.programGeneration &&
            holder.parent == original.parent && holder.preparedStartFrame == original.preparedStartFrame &&
            holder.physicalRelease.slot == original.release.slot &&
            holder.physicalRelease.targetSequence == original.release.targetSequence &&
            originalSourceSubmissions.IsOriginalSourceLifetimePinFor(holder.source, original.arm) &&
            voices.IsOriginalSourceBodyLifetimePinFor(holder.body, original.arm, original.sourceIdentity);
    }

    bool MatchesPendingVariableSourceAssociation(const OriginalPendingVariableLeafSource& holder) const noexcept {
        if (holder.group == nullptr) return holder.domain == OriginalPendingVariableLeafSource::Domain::Unbound;
        if (holder.domain == OriginalPendingVariableLeafSource::Domain::Current)
            return MatchesPendingVariableGroupLeaf(*holder.group, holder.leafIndex);
        return holder.domain == OriginalPendingVariableLeafSource::Domain::QueueFuture &&
            holder.leafIndex < pendingVariableFutureSources.size() &&
            MatchesPendingVariableGroupFutureSource(pendingVariableFutureSources[holder.leafIndex]);
    }

    bool MatchesPendingVariableGroupRetirementFutureCensus(
        const OriginalPendingVariableGroupHolder& group) const noexcept {
        if (group.engine != this || group.futureSourceCount > pendingVariableFutureSources.size() ||
            !MatchesPendingVariableQueueNativeOrigins(group)) return false;
        std::uint32_t count = 0;
        for (const auto& original : pendingVariableFutureSources) {
            if (original.group != &group) continue;
            if (++count > group.futureSourceCount || original.ordinal >= group.futureSourceCount ||
                !original.arm.Valid() || !original.service.Valid() || !original.partition.Valid() ||
                !original.queue.Valid() || !original.parentGroup.Valid() || original.memberIndex == UINT16_MAX ||
                original.sourceIdentity == 0 || original.programGeneration == 0 || original.parent == 0 ||
                original.epoch == 0 || (original.retained && !MatchesPendingVariableGroupFutureSource(original)) ||
                (!original.retained && !original.retirement.Valid()) ||
                (original.retirement.Valid() && !originalSourceSubmissions.MatchesPendingInstalledSourceRetirementReceipt(
                    original.retirement, original.arm))) return false;
            for (const auto& other : pendingVariableFutureSources)
                if (&other != &original && other.group == &group &&
                    (other.ordinal == original.ordinal || other.arm.slot == original.arm.slot ||
                     (other.parentGroup.decision.slot == original.parentGroup.decision.slot &&
                      other.parentGroup.decision.generation == original.parentGroup.decision.generation &&
                      other.memberIndex == original.memberIndex))) return false;
            for (std::uint32_t leaf = 0; leaf < group.leafCount; ++leaf)
                if (group.leafArms[leaf].slot == original.arm.slot) return false;
            bool nativeMember = false;
            for (const auto& origin : pendingVariableQueueNativeOrigins) {
                if (origin.group != &group || origin.role != OriginalPendingVariableQueueNativeOrigin::Role::Successor ||
                    origin.service.token != original.service.token || origin.service.owner != original.service.owner ||
                    origin.service.epoch != original.service.epoch ||
                    origin.service.invocationGeneration != original.service.invocationGeneration ||
                    origin.partition.slot != original.partition.slot || origin.partition.generation != original.partition.generation ||
                    origin.queue.slot != original.queue.slot || origin.queue.generation != original.queue.generation ||
                    origin.scope.input.decision.slot != original.parentGroup.decision.slot ||
                    origin.scope.input.decision.generation != original.parentGroup.decision.generation ||
                    original.memberIndex >= origin.scope.input.memberCount) continue;
                nativeMember |= OriginalSourceSubmissionRegistry::SameToken(
                    origin.scope.input.members[original.memberIndex], original.arm);
            }
            if (!nativeMember) return false;
        }
        return count == group.futureSourceCount;
    }

    bool CanMovePendingVariableGroupSources(const OriginalPendingVariableGroupHolder& group) const noexcept {
        if (group.engine != this || group.leafCount > group.leafArms.size() ||
            group.futureSourceCount > pendingVariableFutureSources.size() ||
            !MatchesPendingVariableQueueNativeOrigins(group) ||
            !MatchesPendingVariableGroupFutureNodes(group, true)) return false;
        for (const auto& slot : group.nativeReplacementSlots) {
            if (slot.capacity > kMaxKnownScheduleLeaves ||
                (slot.borrower.Valid() ? !slot.side.Empty() :
                    slot.side.size() != slot.capacity || slot.side.Shape() != 0)) return false;
            if (slot.borrower.Valid()) {
                if (!actionPlayScratch.IsConstructed(slot.borrower.slot)) return false;
                const auto& child = actionPlayScratch[slot.borrower.slot];
                const auto& side = slot.producerOrdinal == UINT32_MAX
                    ? child.queueSuccessorVoiceStarts : child.voiceStarts;
                if (child.storage.generation != slot.borrower.generation || side.size() != slot.capacity ||
                    !(slot.producerOrdinal == UINT32_MAX ? child.successorNativeBorrowed : child.currentNativeBorrowed))
                    return false;
            }
        }
        for (const auto& slot : group.queueRoleReservations) {
            if (slot.state == QueueRoleReservationSlot::State::Available) {
                if (slot.borrower.Valid() || slot.directory.size() != slot.capacity) return false;
            } else if (slot.state == QueueRoleReservationSlot::State::Consumed) {
                if (slot.borrower.Valid() || !slot.directory.Empty()) return false;
            } else if (slot.state == QueueRoleReservationSlot::State::Borrowed) {
                if (!slot.borrower.Valid() || !actionPlayScratch.IsConstructed(slot.borrower.slot) ||
                    !slot.directory.Empty()) return false;
                const auto& child = actionPlayScratch[slot.borrower.slot];
                if (child.storage.generation != slot.borrower.generation || !child.queueRolesBorrowed ||
                    child.storage.frontierQueues.size() != slot.capacity ||
                    child.frontierQueueCount > slot.capacity) return false;
            } else return false;
        }
        for (std::uint32_t index = 0; index < group.leafCount; ++index)
            if (group.leafRetained[index] && !MatchesPendingVariableGroupLeaf(group, index)) return false;
        std::uint32_t futures = 0;
        for (const auto& original : pendingVariableFutureSources) {
            if (original.group != &group) continue;
            if (++futures > group.futureSourceCount || original.ordinal >= group.futureSourceCount ||
                !original.service.Valid() || !original.partition.Valid() || !original.queue.Valid() ||
                !original.arm.Valid() || !original.parentGroup.Valid() || original.memberIndex == UINT16_MAX ||
                (original.retained && !MatchesPendingVariableGroupFutureSource(original))) return false;
            for (const auto& other : pendingVariableFutureSources)
                if (&original != &other && other.group == &group && other.ordinal == original.ordinal) return false;
        }
        if (futures != group.futureSourceCount) return false;
        for (const auto& holder : pendingVariableLeafSources)
            if (holder.group == &group && !MatchesPendingVariableSourceAssociation(holder)) return false;
        return true;
    }

    void RebindMovedPendingVariableGroupSources(OriginalPendingVariableGroupHolder& old,
        OriginalPendingVariableGroupHolder& moved) noexcept {
        const auto rebindHead = [&](auto& holder) noexcept {
            if (holder.reprepare_.cohortHead == &old) holder.reprepare_.cohortHead = &moved;
        };
        rebindHead(moved);
        for (auto& holder : preparedPendingVariableGroups) rebindHead(holder);
        for (auto& holder : acceptedPendingVariableGroups) rebindHead(holder);
        for (std::uint32_t index = 0; index < moved.leafCount; ++index)
            if (moved.leafRetained[index]) pendingVariableLeafSources[moved.leafArms[index].slot].group = &moved;
        for (auto& original : pendingVariableFutureSources) {
            if (original.group != &old) continue;
            original.group = &moved;
            // A returned episode may share its old slot with a different Arm generation.
            if (original.retained) pendingVariableLeafSources[original.arm.slot].group = &moved;
        }
        for (auto& original : pendingVariableQueueNativeOrigins)
            if (original.group == &old) original.group = &moved;
        for (auto& original : pendingVariableFutureNodes)
            if (original.group == &old) original.group = &moved;
    }

    void ReturnPendingVariableLeafSource(OriginalPendingVariableLeafSource& holder) noexcept {
        const bool retained = !holder.Empty();
        if (holder.group != nullptr) {
            assert(MatchesPendingVariableSourceAssociation(holder));
            if (holder.domain == OriginalPendingVariableLeafSource::Domain::Current)
                holder.group->leafRetained[holder.leafIndex] = false;
            else pendingVariableFutureSources[holder.leafIndex].retained = false;
        }
        // Physical release is observed separately. These loans only retain storage.
        holder.body.Release();
        holder.source.Release();
        holder = {};
        if (retained) {
            assert(pendingVariableLeafSourceCount != 0);
            --pendingVariableLeafSourceCount;
        }
    }

    bool ReturnPendingVariableLeafSourceAfterPhysicalRelease(TimelineArmToken arm) noexcept {
        if (!arm.Valid() || arm.slot >= pendingVariableLeafSources.size()) return true;
        auto& holder = pendingVariableLeafSources[arm.slot];
        if (holder.Empty() || !OriginalSourceSubmissionRegistry::SameToken(holder.arm, arm)) return true;
        if (!voices.ReleaseObserved(holder.physicalRelease) ||
            !MatchesPendingVariableSourceAssociation(holder))
            return false;
        if (holder.group == nullptr &&
            !originalSourceSubmissions.CanReturnUnpublishedOriginalSourcePairStorage(holder.source, holder.body) &&
            !originalSourceSubmissions.CanReturnPublishedOriginalSourcePairStorage(
                holder.source, holder.body, holder.physicalRelease))
            return false;
        ReturnPendingVariableLeafSource(holder);
        return true;
    }

    void ServicePendingVariableLeafSources() noexcept {
        if (pendingVariableLeafSourceCount == 0) return;
        for (auto& holder : pendingVariableLeafSources)
            if (!holder.Empty()) (void)ReturnPendingVariableLeafSourceAfterPhysicalRelease(holder.arm);
    }

    bool ReturnPendingVariableGroupLeafSources(OriginalPendingVariableGroupHolder& group) noexcept {
        if (group.engine != this || group.leafCount > group.leafArms.size() ||
            !MatchesPendingVariableQueueNativeOrigins(group)) return false;
        std::uint32_t futures = 0;
        for (const auto& original : pendingVariableFutureSources) {
            if (original.group != &group) continue;
            if (++futures > group.futureSourceCount || original.ordinal >= group.futureSourceCount ||
                (original.retained && !MatchesPendingVariableGroupFutureSource(original))) return false;
        }
        if (futures != group.futureSourceCount) return false;
        for (std::uint32_t index = 0; index < group.leafCount; ++index)
            if (group.leafRetained[index] && !MatchesPendingVariableGroupLeaf(group, index)) return false;
        for (std::uint32_t index = 0; index < group.leafCount; ++index)
            if (group.leafRetained[index]) ReturnPendingVariableLeafSource(pendingVariableLeafSources[group.leafArms[index].slot]);
        for (auto& original : pendingVariableFutureSources) {
            if (original.group != &group) continue;
            if (original.retained) ReturnPendingVariableLeafSource(pendingVariableLeafSources[original.arm.slot]);
            original.retirement.Release();
            original = {};
        }
        group.futureSourceCount = 0;
        for (auto& receipt : group.leafRetirementReceipts) receipt.Release();
        group.leafRetirement = OriginalPendingVariableGroupHolder::LeafRetirement::None;
        std::vector<TimelineArmToken>().swap(group.leafArms);
        std::vector<std::uint8_t>().swap(group.leafRetained);
        decltype(group.leafRetirementReceipts)().swap(group.leafRetirementReceipts);
        group.leafCount = 0;
        return true;
    }

    enum class PendingVariableGroupLeafRetirementResult : std::uint8_t { Waiting, Settled, Refused };

    PendingVariableGroupLeafRetirementResult RequestPendingVariableGroupLeafRetirement(
        EngineActionPlayScratch& scratch, const EngineImmediateEntryAttempt& attempt,
        const PrivateImmediateLanePlacementProof& placement) noexcept {
        using Result = PendingVariableGroupLeafRetirementResult;
        using Phase = OriginalPendingVariableGroupHolder::LeafRetirement;
        auto* group = OriginalPendingVariableGroupForPlacement(scratch, attempt, placement);
        if (group == nullptr ||
            group->leafCount > group->leafArms.size() || group->leafCount != scratch.voiceStartCount ||
            group->leafCount != scratch.row->scheduleLeafCount ||
            !MatchesPendingVariableGroupRetirementFutureCensus(*group)) return Result::Refused;
        if (group->leafRetirement == Phase::Failed) return Result::Refused;
        if (group->leafRetirement != Phase::None && group->reprepare_.reserveForReplacement) {
            bool waiting = false;
            if (!EnsurePendingVariableNativeBacking(scratch, attempt, &waiting)) return Result::Refused;
        }
        if (group->leafRetirement == Phase::None) {
            if (!variables.CanReleaseReadCut(group->cut) &&
                !variables.OriginalPendingVariableGroupUsesMatch(*this, scratch, attempt, placement))
                return Result::Refused;
            const auto pendingSource = [&](const OriginalPendingVariableLeafSource& holder) noexcept {
                const auto* arm = timelineArms.Resolve(holder.arm);
                const auto state = arm == nullptr ? ArmState::Free : arm->state.load(std::memory_order_acquire);
                return arm != nullptr && arm->pendingImmediateStart && !arm->startBound &&
                    arm->startFrame == 0 && arm->armDeadline == 0 && arm->programGeneration == holder.programGeneration &&
                    (state == ArmState::Armed || state == ArmState::CancelRequested) &&
                    arm->startedBlockSequence.load(std::memory_order_acquire) == 0 &&
                    arm->logicalCompletionSequence.load(std::memory_order_acquire) == 0 &&
                    arm->frontierReaderPins.load(std::memory_order_acquire) == 0 &&
                    arm->frontierLastReadSequence.load(std::memory_order_acquire) == 0 &&
                    !arm->frontierOperation.Valid() && arm->frontierAdmission == nullptr &&
                    arm->frontierAdmissionLease == nullptr &&
                    arm->frontierActiveLease.load(std::memory_order_acquire) == nullptr &&
                    arm->authoredControl.operation == nullptr &&
                    arm->authoredControl.stage == TimelineArmTable::AuthoredControlStage::None &&
                    !arm->finalSourcePublicationIssued && !arm->finalSourcePublicationConsumed &&
                    originalSourceSubmissions.IsOriginalInstalledSourceCurrent(holder.source, holder.sourceIdentity) &&
                    voices.IsOriginalSourceBodyCurrent(holder.body);
            };
            // Authenticate current and future Sources before requesting the first retirement.
            for (std::uint32_t index = 0; index < group->leafCount; ++index) {
                if (group->leafRetirementReceipts[index].Valid() || !MatchesPendingVariableGroupLeaf(*group, index))
                    return Result::Refused;
                const auto& holder = pendingVariableLeafSources[group->leafArms[index].slot];
                if (!pendingSource(holder)) return Result::Refused;
            }
            for (const auto& original : pendingVariableFutureSources) {
                if (original.group != group) continue;
                if (original.retirement.Valid() || !MatchesPendingVariableGroupFutureSource(original)) return Result::Refused;
                const auto& holder = pendingVariableLeafSources[original.arm.slot];
                const auto* arm = timelineArms.Resolve(original.arm);
                const auto* owner = ResolvePlayableService(original.service);
                const auto* baseline = ResolveOriginalPlayableBaseline(this, playableInstances, original.programGeneration);
                const auto leaf = playableInstances.Classify(original.programGeneration);
                if (owner == nullptr || owner->partition.slot != original.partition.slot ||
                    owner->partition.generation != original.partition.generation ||
                    !playablePartitions.Owns(original.partition, owner->identity) ||
                    owner->queueInstance.slot != original.queue.slot || owner->queueInstance.generation != original.queue.generation ||
                    owner->invocation != scratch.row->invocation || owner->invocationGeneration != scratch.row->invocationGeneration ||
                    owner->PendingImmediateCohortRoot() != attempt.root || baseline == nullptr ||
                    !playableInstances.IsOriginalLifetimeCurrent(*baseline) || baseline->OriginalParent() != original.parent ||
                    baseline->OriginalScheduleEpoch() != original.epoch || !leaf.live || leaf.kind != PlayableKind::Sound ||
                    leaf.parent != original.parent || leaf.scheduleEpoch != original.epoch ||
                    !pendingSource(holder) || arm->parentGroup.decision.slot != original.parentGroup.decision.slot ||
                    arm->parentGroup.decision.generation != original.parentGroup.decision.generation ||
                    arm->memberIndex != original.memberIndex) return Result::Refused;
            }
            if (group->reprepare_.reserveForReplacement) {
                bool waiting = false;
                if (!EnsurePendingVariableNativeBacking(scratch, attempt, &waiting))
                    return waiting ? Result::Waiting : Result::Refused;
            }
            group->leafRetirement = Phase::Requesting;
        }
        bool settled = true;
        for (std::uint32_t index = 0; index < group->leafCount; ++index) {
            auto& receipt = group->leafRetirementReceipts[index];
            if (!receipt.Valid()) {
                if (!MatchesPendingVariableGroupLeaf(*group, index)) {
                    group->leafRetirement = Phase::Failed;
                    return Result::Refused;
                }
                auto& holder = pendingVariableLeafSources[group->leafArms[index].slot];
                const auto requested = originalSourceSubmissions.RequestPendingInstalledSourceRetirementWithReceipt(
                    holder.source, holder.body, &receipt);
                if (requested == OriginalSourceSubmissionRegistry::InstalledSourceRetirementRequest::Deferred) {
                    settled = false;
                    continue;
                }
                if (requested != OriginalSourceSubmissionRegistry::InstalledSourceRetirementRequest::Requested) {
                    group->leafRetirement = Phase::Failed;
                    return Result::Refused;
                }
            }
            if (!originalSourceSubmissions.PendingInstalledSourceRetirementSettled(receipt, group->leafArms[index]))
                settled = false;
        }
        for (auto& original : pendingVariableFutureSources) {
            if (original.group != group) continue;
            if (!original.retirement.Valid()) {
                if (!MatchesPendingVariableGroupFutureSource(original)) {
                    group->leafRetirement = Phase::Failed;
                    return Result::Refused;
                }
                auto& holder = pendingVariableLeafSources[original.arm.slot];
                const auto requested = originalSourceSubmissions.RequestPendingInstalledSourceRetirementWithReceipt(
                    holder.source, holder.body, &original.retirement);
                if (requested == OriginalSourceSubmissionRegistry::InstalledSourceRetirementRequest::Deferred) {
                    settled = false;
                    continue;
                }
                if (requested != OriginalSourceSubmissionRegistry::InstalledSourceRetirementRequest::Requested) {
                    group->leafRetirement = Phase::Failed;
                    return Result::Refused;
                }
            }
            if (!originalSourceSubmissions.PendingInstalledSourceRetirementSettled(original.retirement, original.arm))
                settled = false;
        }
        if (!settled || !variables.CanReleaseReadCut(group->cut)) return Result::Waiting;
        group->leafRetirement = Phase::Settled;
        return Result::Settled;
    }

    bool ReturnPendingVariableGroupRetiredLeafStorage(EngineActionPlayScratch& scratch,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement) noexcept {
        auto* group = OriginalPendingVariableGroupForPlacement(scratch, attempt, placement);
        return group != nullptr && ReturnPendingVariableGroupRetiredLeafStorage(*group);
    }

    bool PendingVariableGroupSourcesSettled(const OriginalPendingVariableGroupHolder& group) const noexcept {
        if (group.engine != this || group.leafRetirement != OriginalPendingVariableGroupHolder::LeafRetirement::Settled ||
            group.leafCount > group.leafArms.size() ||
            !MatchesPendingVariableGroupRetirementFutureCensus(group)) return false;
        for (std::uint32_t index = 0; index < group.leafCount; ++index) {
            if (!originalSourceSubmissions.PendingInstalledSourceRetirementSettled(
                    group.leafRetirementReceipts[index], group.leafArms[index])) return false;
            for (std::uint32_t prior = 0; prior < index; ++prior)
                if (group.leafArms[prior].slot == group.leafArms[index].slot) return false;
        }
        for (const auto& original : pendingVariableFutureSources)
            if (original.group == &group && !originalSourceSubmissions.PendingInstalledSourceRetirementSettled(
                    original.retirement, original.arm)) return false;
        return true;
    }

    bool PendingVariableGroupHasRetiredArm(const OriginalPendingVariableGroupHolder& group,
        TimelineArmToken arm) const noexcept {
        for (std::uint32_t index = 0; index < group.leafCount; ++index)
            if (OriginalSourceSubmissionRegistry::SameToken(group.leafArms[index], arm))
                return originalSourceSubmissions.PendingInstalledSourceRetirementSettled(group.leafRetirementReceipts[index], arm);
        for (const auto& source : pendingVariableFutureSources)
            if (source.group == &group && OriginalSourceSubmissionRegistry::SameToken(source.arm, arm))
                return originalSourceSubmissions.PendingInstalledSourceRetirementSettled(source.retirement, arm);
        return false;
    }

    bool CapturePendingVariableRetiredFutureTerminal(const OriginalPendingVariableFutureSource& original,
        SourceTerminalToken* out) const noexcept {
        if (original.group == nullptr || original.group->engine != this || !original.group->accepted ||
            !std::any_of(pendingVariableFutureSources.begin(), pendingVariableFutureSources.end(),
                [&](const auto& member) { return &member == &original; })) return false;
        return originalSourceSubmissions.CapturePendingInstalledSourceRetirementTerminal(
            original.retirement, original.arm, original.sourceIdentity, original.release, out);
    }

    bool ReturnPendingVariableGroupRetiredLeafStorage(OriginalPendingVariableGroupHolder& originalGroup) noexcept {
        auto* group = &originalGroup;
        if (!PendingVariableGroupSourcesSettled(*group)) return false;
        for (std::uint32_t index = 0; index < group->leafCount; ++index)
            if (!originalSourceSubmissions.PendingInstalledSourceRetirementSettled(
                    group->leafRetirementReceipts[index], group->leafArms[index]) ||
                (group->leafRetained[index] && !MatchesPendingVariableGroupLeaf(*group, index))) return false;
        for (const auto& original : pendingVariableFutureSources)
            if (original.group == group && !originalSourceSubmissions.PendingInstalledSourceRetirementSettled(
                    original.retirement, original.arm)) return false;
        for (std::uint32_t index = 0; index < group->leafCount; ++index) {
            if (!originalSourceSubmissions.ReturnPendingInstalledSourceRetirementStorage(
                    group->leafRetirementReceipts[index], group->leafArms[index])) return false;
            if (group->leafRetained[index])
                ReturnPendingVariableLeafSource(pendingVariableLeafSources[group->leafArms[index].slot]);
        }
        for (auto& original : pendingVariableFutureSources) {
            if (original.group != group) continue;
            if (!originalSourceSubmissions.ReturnPendingInstalledSourceRetirementStorage(original.retirement, original.arm))
                return false;
            if (original.retained) ReturnPendingVariableLeafSource(pendingVariableLeafSources[original.arm.slot]);
        }
        return true;
    }

    bool PendingVariableRetiredSourceMatches(const EnginePreparedTimelineVoiceStart& start,
        bool requireTerminalReturn = false) const noexcept {
        if (!start.arm.Valid() || start.renderHandle != OriginalSourceSubmissionRegistry::RenderOf(start.arm) ||
            start.commandCount == 0 || start.commands[0].opcode != kOpcodePlay ||
            start.commands[0].payload.play.handle != start.renderHandle) return false;
        const auto matches = [&](const auto& receipt) noexcept {
            return originalSourceSubmissions.MatchesPendingInstalledSourceRetirementIdentity(receipt, start.arm,
                start.commands[0].payload.play.sourceIdentity, start.releaseObserver, start.sourceTerminal) &&
                (!requireTerminalReturn || originalSourceSubmissions.PendingInstalledSourceTerminalReturned(receipt, start.arm));
        };
        for (const auto& group : acceptedPendingVariableGroups) {
            if (group.engine != this || !group.accepted || !group.producer.Valid() ||
                group.leafCount > group.leafArms.size() ||
                group.leafRetirement != OriginalPendingVariableGroupHolder::LeafRetirement::Settled) continue;
            for (std::uint32_t index = 0; index < group.leafCount; ++index)
                if (OriginalSourceSubmissionRegistry::SameToken(group.leafArms[index], start.arm))
                    return matches(group.leafRetirementReceipts[index]);
            for (const auto& source : pendingVariableFutureSources)
                if (source.group == &group && OriginalSourceSubmissionRegistry::SameToken(source.arm, start.arm))
                    return matches(source.retirement);
        }
        return false;
    }

    void ObservePendingVariableSourceTerminalReturn(SourceTerminalToken token,
        const SourceTerminalRecord& source) noexcept {
        for (auto& group : acceptedPendingVariableGroups) {
            if (group.engine != this || group.leafCount > group.leafArms.size()) continue;
            for (std::uint32_t index = 0; index < group.leafCount; ++index)
                originalSourceSubmissions.ObservePendingInstalledSourceTerminalReturn(
                    group.leafRetirementReceipts[index], group.leafArms[index], token, source);
        }
        for (auto& future : pendingVariableFutureSources)
            if (future.group != nullptr)
                originalSourceSubmissions.ObservePendingInstalledSourceTerminalReturn(future.retirement, future.arm, token, source);
    }

    bool BindPendingVariableGroupInitialLeafSources(EngineActionPlayScratch& scratch,
        const PlayableInstanceTable::PreparedParentLinksProof& links) noexcept {
        auto* group = PreparedPendingVariableGroupFor(scratch.storage);
        if (group == nullptr || group->engine != this || !group->cut.Valid() ||
            !group->producer.Valid() || group->accepted || group->leafCount != 0 ||
            scratch.row == nullptr || scratch.voiceStartCount != scratch.row->scheduleLeafCount ||
            scratch.voiceStartCount != scratch.frontierCurrentVoiceOwnerCount ||
            scratch.voiceStartCount > group->leafArms.size()) return false;
        const auto rootHandle = group->root.OriginalHandle();
        // Authenticate the complete current-leaf census before attaching any loan.
        for (std::uint32_t index = 0; index < scratch.voiceStartCount; ++index) {
            const auto& start = scratch.voiceStarts[index];
            const auto& owner = scratch.voiceStarts.Owners()[index];
            if (!start.prepared || owner.token != &start || owner.engine != this ||
                !start.armPreparation.pendingImmediateStart || !start.armPreparation.parentGroup.Valid() ||
                start.arm.slot >= pendingVariableLeafSources.size() || start.commandCount == 0 ||
                start.commandCount > start.commands.size()) return false;
            const auto& holder = pendingVariableLeafSources[start.arm.slot];
            if (holder.group != nullptr || holder.cut.Valid() ||
                !OriginalSourceSubmissionRegistry::SameToken(holder.arm, start.arm) ||
                holder.programGeneration != start.armPreparation.programGeneration ||
                holder.parent != start.ledgerPreparation.parent.internalToken ||
                holder.physicalRelease.slot != start.releaseObserver.slot ||
                holder.physicalRelease.targetSequence != start.releaseObserver.targetSequence ||
                holder.physicalRelease.slot != start.voicePreparation.originalRelease.slot ||
                holder.physicalRelease.targetSequence != start.voicePreparation.originalRelease.targetSequence ||
                holder.preparedStartFrame < scratch.row->startFrame ||
                !playableInstances.MatchesPreparedOriginalAncestry(links, holder.programGeneration, rootHandle) ||
                !playableInstances.MatchesPreparedOriginalAncestry(links, holder.parent, rootHandle) ||
                !originalSourceSubmissions.MatchesPreparedInitialGroupSourcePair(holder.source, holder.body,
                    owner.armCommitProof, owner.voiceCommitProof, start.voicePreparation,
                    preparedVoiceCommandBatchProof, start.arm, holder.sourceIdentity)) return false;
            if (start.initialVariableReadUse.Valid()) {
                const auto nativeUse = voices.TimelineVariableSourceRead(start.arm.renderSlot, start.arm.renderGeneration);
                if (nativeUse.slot != start.initialVariableReadUse.slot ||
                    nativeUse.generation != start.initialVariableReadUse.generation ||
                    !variables.MatchesPreparedTimelineReadUse(start.initialVariableReadUse, group->cut,
                        start.renderHandle, owner.armCommitProof, originalSourceSubmissions,
                        holder.preparedStartFrame - scratch.row->startFrame)) return false;
            }
            for (std::uint32_t prior = 0; prior < index; ++prior) {
                const auto& previous = scratch.voiceStarts[prior];
                if (previous.arm.slot == start.arm.slot || previous.renderHandle == start.renderHandle ||
                    (previous.armPreparation.parentGroup.decision.slot == start.armPreparation.parentGroup.decision.slot &&
                     previous.armPreparation.parentGroup.decision.generation == start.armPreparation.parentGroup.decision.generation &&
                     previous.armPreparation.memberIndex == start.armPreparation.memberIndex)) return false;
            }
        }
        group->leafCount = scratch.voiceStartCount;
        for (std::uint32_t index = 0; index < group->leafCount; ++index) {
            const auto arm = scratch.voiceStarts[index].arm;
            auto& holder = pendingVariableLeafSources[arm.slot];
            group->leafArms[index] = arm;
            group->leafRetained[index] = true;
            holder.group = group;
            holder.cut = group->cut;
            holder.leafIndex = index;
            holder.domain = OriginalPendingVariableLeafSource::Domain::Current;
        }
        return true;
    }

    bool BindPendingVariableGroupQueueFutureSources(EngineActionPlayScratch& scratch,
        const PlayableInstanceTable::PreparedParentLinksProof& links) noexcept;
    bool CapturePendingVariableQueueNativeOrigins(EngineActionPlayScratch& scratch,
        const PlayableInstanceTable::PreparedParentLinksProof& links) noexcept;
    bool MatchesPendingVariableQueueRecordChains(const EngineImmediateEntryAttempt& attempt) const noexcept;
    bool MatchesPendingVariableQueueNativeOrigins(const OriginalPendingVariableGroupHolder& group) const noexcept;
    bool CanTransferPendingVariableQueueNativeOrigins(EngineActionPlayScratch& scratch,
        const EngineImmediateEntryAttempt& attempt) noexcept;
    void TransferPendingVariableQueueNativeOrigins(OriginalPendingVariableGroupHolder& group) noexcept;
    bool ReturnDisposedPendingVariableQueueNativeOrigins(OriginalPendingVariableGroupHolder& group) noexcept;
    static void ObservePendingVariableQueueNativeDisposal(void* context,
        Plan03GroupSliceIdentity identity, std::uint64_t root, std::uint64_t domain) noexcept;

    bool PreparePendingVariableGroupRetentions(EngineActionPlayScratch& scratch,
        const PlayableInstanceTable::PreparedParentLinksProof& links,
        OriginalPendingVariableGroupHolder* prepared) noexcept {
        auto* holder = PreparedPendingVariableGroupFor(scratch.storage);
        if (prepared == nullptr || !prepared->Empty() || holder == nullptr || !holder->Empty() ||
            scratch.row == nullptr || !scratch.row->InvocationValid() ||
            scratch.storage.phase != EngineFrontierAttemptPhase::Preparing) return false;
        auto& row = *scratch.row;
        if (!acceptedPendingVariableGroups.Emplace(row.identity.slot) ||
            !acceptedPendingVariableGroups[row.identity.slot].Empty()) return false;
        if (scratch.variableControlNestedVariableControlCount > scratch.variableControlNestedServices.size()) return false;
        std::uint32_t nestedSideCount = 0;
        for (std::uint32_t index = 0; index < scratch.variableControlNestedVariableControlCount; ++index) {
            const auto* nested = ResolvePlayableService(scratch.variableControlNestedServices[index]);
            if (nested == nullptr || &nested->Invocation() != &row.Invocation() ||
                !playableInstances.MatchesPreparedOriginalAncestry(links, nested->ownerInstance, row.ownerInstance))
                return false;
            nestedSideCount += static_cast<std::uint32_t>(nested->variableControlDiscreteDecision.Valid()) +
                static_cast<std::uint32_t>(nested->variableControlContinuousDecision.Valid());
        }
        if (nestedSideCount > EngineActionPlayScratch::kPendingVariableReplacementControlCapacity - 2u) return false;
        if (scratch.voiceStartCount > kMaxKnownScheduleLeaves) return false;
        std::vector<OriginalPendingVariableGroupHolder::NestedInitialSide> nestedSides;
        try {
            const auto allocateColumn = [&](auto& column, std::size_t count) {
                if (count == 0) return true;
                if (!AllowNativeBackingAllocation()) return false;
                column.resize(count);
                return true;
            };
            if (!allocateColumn(nestedSides, nestedSideCount) ||
                !allocateColumn(prepared->leafArms, scratch.voiceStartCount) ||
                !allocateColumn(prepared->leafRetained, scratch.voiceStartCount) ||
                !allocateColumn(prepared->leafRetirementReceipts, scratch.voiceStartCount) ||
                !allocateColumn(prepared->reprepare_.members, scratch.voiceStartCount) ||
                !allocateColumn(prepared->reprepare_.decisions, 2u + nestedSideCount) ||
                !allocateColumn(prepared->reprepare_.armGroups, 2u + nestedSideCount) ||
                !allocateColumn(prepared->reprepare_.armGroupsReturned, 2u + nestedSideCount) ||
                !allocateColumn(prepared->reprepare_.nativeGroups, 3u + nestedSideCount) ||
                !allocateColumn(prepared->reprepare_.children, row.variableControlChildCount) ||
                !allocateColumn(prepared->reprepare_.childEpochs, row.variableControlChildCount) ||
                !allocateColumn(prepared->reprepare_.childrenReturned, row.variableControlChildCount)) return false;
        } catch (const std::bad_alloc&) {
            return false;
        } catch (const std::length_error&) {
            return false;
        }
        const auto rootHandle = row.Invocation().variableControlOuterRootInstance != 0
            ? row.Invocation().variableControlOuterRootInstance : row.rootInstance;
        auto* originalRoot = ResolveOriginalPlayableBaseline(this, playableInstances, rootHandle);
        if (originalRoot == nullptr ||
            !playableInstances.MatchesPreparedOriginalAncestry(links, row.ownerInstance, rootHandle) ||
            row.identity.slot >= originalPlayableServiceLifetimeHolders.size()) return false;
        if (!originalRoot->Borrow(&prepared->root) ||
            !originalPlayableServiceLifetimeHolders[row.identity.slot].Borrow(&prepared->service) ||
            !BorrowOriginalPendingVariableProducer(scratch.storage.originalPendingVariableProducer,
                scratch.storage, row, &prepared->producer)) return false;
        if ((row.variableControlDiscreteBank.Valid() &&
                !variableControlInstances.IssueOriginalInitialSideReceipt(scratch.variableControlInstance,
                    VariableControlSide::Discrete, &prepared->initialSides[0])) ||
            (row.variableControlContinuousBank.Valid() &&
                !variableControlInstances.IssueOriginalInitialSideReceipt(scratch.variableControlInstance,
                    VariableControlSide::Continuous, &prepared->initialSides[1]))) return false;
        for (std::uint32_t side = 0; side < 2; ++side) {
            const auto token = side == 0 ? row.variableControlDiscreteDecision : row.variableControlContinuousDecision;
            if (!token.Valid()) {
                const auto bank = side == 0 ? row.variableControlDiscreteBank : row.variableControlContinuousBank;
                if (!bank.Valid()) continue;
                if (!variableControlInstances.MatchesOriginalInitialSideReceipt(prepared->initialSides[side],
                        row.variableControlInstance, side == 0 ? VariableControlSide::Discrete : VariableControlSide::Continuous,
                        token, bank) || scratch.continuationTransportEpoch != timelinePool.Epoch()) return false;
                auto& input = prepared->initialDecisionInputs[side];
                input.world = row.Invocation().worldGeneration;
                input.transport = scratch.continuationTransportEpoch;
                input.expectedContinuationGeneration = scratch.instance.ScheduleEpoch();
                input.sliceCount = 1;
                continue;
            }
            const auto& reservation = side == 0 ? scratch.variableControlDiscreteDecision : scratch.variableControlContinuousDecision;
            const auto* baseline = OriginalSingleDecisionPreparation(reservation);
            if (baseline == nullptr || reservation.Token().slot != token.slot ||
                reservation.Token().generation != token.generation) return false;
            prepared->initialDecisionInputs[side] = reservation.OriginalRequest();
        }
        for (std::uint32_t index = 0; index < scratch.variableControlNestedVariableControlCount; ++index) {
            auto* nested = ResolvePlayableService(scratch.variableControlNestedServices[index]);
            if (nested == nullptr || &nested->Invocation() != &row.Invocation() ||
                !playableInstances.MatchesPreparedOriginalAncestry(links, nested->ownerInstance, row.ownerInstance))
                return false;
            for (std::uint32_t side = 0; side < 2; ++side) {
                const auto decision = side == 0 ? nested->variableControlDiscreteDecision : nested->variableControlContinuousDecision;
                const auto bank = side == 0 ? nested->variableControlDiscreteBank : nested->variableControlContinuousBank;
                if (!decision.Valid()) continue;
                if (!bank.Valid() || prepared->nestedInitialSideCount == nestedSides.size()) return false;
                const auto& reservation = scratch.variableControlNestedDecisionContinuous[index] == (side == 1u)
                    ? scratch.variableControlNestedDecisions[index] : scratch.storage.nestedContinuousDecisions[index];
                auto& captured = nestedSides[prepared->nestedInitialSideCount++];
                captured.service = scratch.variableControlNestedServices[index];
                captured.instance = nested->variableControlInstance;
                captured.side = side == 0 ? VariableControlSide::Discrete : VariableControlSide::Continuous;
                captured.decision = decision;
                captured.input = reservation.OriginalRequest();
                captured.bank = bank;
                captured.armGroup = side == 0 ? nested->variableControlDiscreteArmGroup : nested->variableControlContinuousArmGroup;
                captured.group = side == 0 ? nested->InitialLaneDiscreteGroup() : nested->InitialLaneContinuousGroup();
                captured.memberCount = side == 0 ? nested->variableControlInitialDiscreteMemberCount
                    : nested->variableControlInitialContinuousMemberCount;
                if (reservation.Token().slot != decision.slot || reservation.Token().generation != decision.generation ||
                    OriginalSingleDecisionPreparation(reservation) == nullptr ||
                    !variableControlInstances.IssueOriginalInitialSideReceipt(scratch.variableControlNestedVariableControls[index],
                        captured.side, &captured.original)) return false;
            }
        }
        prepared->nestedInitialSides = std::move(nestedSides);
        prepared->engine = this;
        prepared->reference = prepared->service.FactoryReference();
        return CapturePendingVariableGroupTopology(scratch, links, *prepared);
    }

    bool SealPendingVariableGroupInitialSideNativeGroup(EngineActionPlayScratch& scratch,
        EnginePlayableServiceRow& service, bool discrete, const Plan03GroupSliceIdentity& identity,
        const Plan03GroupSliceInput& input) noexcept;

    bool AcceptPendingVariableGroup(EngineActionPlayScratch& scratch) noexcept {
        auto* prepared = PreparedPendingVariableGroupFor(scratch.storage);
        if (prepared == nullptr) return false;
        if (prepared->Empty()) return true;
        if (scratch.row == nullptr || prepared->engine != this || !prepared->cut.Valid() ||
            !prepared->producer.Valid() || !prepared->service.Valid() ||
            !prepared->root.Valid() || scratch.row->actionPrepared ||
            prepared->reference.token != scratch.row->identity ||
            prepared->reference.owner != scratch.row->ownerInstance ||
            prepared->reference.epoch != scratch.row->ownerEpoch ||
            prepared->reference.invocationGeneration != scratch.row->invocationGeneration ||
            !acceptedPendingVariableGroups.IsConstructed(scratch.row->identity.slot)) return false;
        auto& accepted = acceptedPendingVariableGroups[scratch.row->identity.slot];
        if (!accepted.Empty() || prepared->initialSelectionBatch.ReplacementPending() ||
            !CanMovePendingVariableGroupSources(*prepared)) return false;
        if (!scratch.storage.initialSelectionBatch.Empty() &&
            (!scratch.storage.initialSelectionBatch.MatchesOwner(&selectionMemory, scratch.storage.initialSelection.identity) ||
             !scratch.storage.initialSelectionBatch.Accepted() ||
             !prepared->initialSelectionBatch.Empty())) return false;
        for (std::uint32_t index = 0; index < prepared->leafCount; ++index)
            if (!prepared->leafRetained[index] || !MatchesPendingVariableGroupLeaf(*prepared, index)) return false;
        std::uint32_t futures = 0;
        for (const auto& original : pendingVariableFutureSources) {
            if (original.group != prepared) continue;
            if (++futures > prepared->futureSourceCount || !MatchesPendingVariableGroupFutureSource(original)) return false;
        }
        if (futures != prepared->futureSourceCount) return false;
        if (!scratch.storage.initialSelectionBatch.MoveTo(prepared->initialSelectionBatch)) return false;
        const bool moved = prepared->MoveTo(accepted);
        assert(moved);
        if (!moved) return false;
        accepted.accepted = true;
        return true;
    }

    bool ClosePendingVariableGroupsForAttempt(EngineActionPlayStorage& storage) noexcept {
        auto* ordinary = PendingOrdinarySelectionFor(storage);
        if (ordinary == nullptr || !ClosePendingOrdinarySelection(*ordinary)) return false;
        auto* prepared = PreparedPendingVariableGroupFor(storage);
        if (prepared == nullptr || !prepared->Close(*this)) return false;
        for (auto& accepted : acceptedPendingVariableGroups)
            if (accepted.engine == this && accepted.producer.original_ == &storage.originalPendingVariableProducer &&
                !accepted.Close(*this)) return false;
        return storage.initialSelectionBatch.Cancel() && storage.originalPendingVariableProducer.ReclaimReady();
    }

    void ServicePublishedPendingVariableGroups() noexcept {
        for (auto& storage : actionPlayStorage) {
            if (storage.phase == EngineFrontierAttemptPhase::PublishedGroupReclaim &&
                ClosePendingVariableGroupsForAttempt(storage))
                storage.phase = EngineFrontierAttemptPhase::Free;
        }
    }

    bool ClosePendingVariableGroupForService(const EnginePlayableServiceRow& row) noexcept {
        for (auto& ordinary : pendingOrdinarySelections) {
            if (ordinary.reference.token != row.identity) continue;
            if (ordinary.storage == nullptr || !MatchesPendingOrdinarySelection(ordinary, *ordinary.storage, row) ||
                !ClosePendingOrdinarySelection(ordinary)) return false;
        }
        if (row.identity.slot >= playableServices.size()) return false;
        if (!acceptedPendingVariableGroups.IsConstructed(row.identity.slot)) return true;
        auto& holder = acceptedPendingVariableGroups[row.identity.slot];
        if (holder.Empty()) return true;
        return holder.reference.token == row.identity && holder.reference.owner == row.ownerInstance &&
            holder.reference.epoch == row.ownerEpoch &&
            holder.reference.invocationGeneration == row.invocationGeneration && holder.Close(*this);
    }

    bool CancelInitialSelectionBatchesForEmitter(VnEmitterHandle emitter) noexcept {
        const auto slot = static_cast<std::uint32_t>(emitter);
        const auto generation = static_cast<std::uint32_t>(emitter >> 32);
        const auto cancel = [&](EnginePendingSelectionBatch& batch) noexcept {
            return !batch.MatchesEmitter(slot, generation) || batch.Cancel();
        };
        for (auto& storage : actionPlayStorage)
            if (!cancel(storage.initialSelectionBatch)) return false;
        for (auto& group : preparedPendingVariableGroups)
            if (!cancel(group.initialSelectionBatch)) return false;
        for (auto& group : acceptedPendingVariableGroups)
            if (!cancel(group.initialSelectionBatch)) return false;
        return true;
    }

    bool IsRegisteredPrivateVcProducerFor(const EngineActionPlayScratch& scratch,
        const EngineImmediateEntryAttempt& attempt, bool requirePreparedLeaves = true) noexcept;

    OriginalPendingVariableGroupHolder* OriginalPendingVariableGroupForProducer(
        EngineActionPlayScratch& scratch, const EngineImmediateEntryAttempt& attempt) noexcept {
        return OriginalPendingVariableGroupForPreparing(scratch, attempt, false);
    }

    OriginalPendingVariableGroupHolder* OriginalPendingVariableGroupForPublication(
        EngineActionPlayScratch& scratch, const EngineImmediateEntryAttempt& attempt) noexcept {
        return OriginalPendingVariableGroupForPreparing(scratch, attempt, true);
    }

    OriginalPendingVariableGroupHolder* OriginalPendingVariableGroupForRetirementAbort(
        EngineActionPlayScratch& scratch, const EngineImmediateEntryAttempt& attempt) noexcept {
        if (!attempt.producerAbortPending || !attempt.producerAbortRetiredSources ||
            !attempt.producerAbortPendingVariableStop || attempt.privateVcProducerCount != 0) return nullptr;
        auto* group = OriginalPendingVariableGroupForPreparing(scratch, attempt, false, true);
        return group != nullptr && PendingVariableGroupSourcesSettled(*group) ? group : nullptr;
    }

    bool MatchesPendingVariableRetiredInitialSide(const OriginalPendingVariableGroupHolder& group,
        const EnginePlayableServiceRow& service,
        const VariableControlInstanceTable::OriginalInitialSideReceipt& receipt,
        VariableControlSide side) noexcept;

    bool MatchesPendingVariableRetiredNativeScope(const OriginalPendingVariableGroupHolder& group,
        const Plan03GroupSliceIdentity& identity, std::uint64_t root, std::uint64_t anchor,
        const Plan03GroupSliceInput& input) noexcept;

    OriginalPendingVariableGroupHolder* OriginalPendingVariableGroupForPreparing(
        EngineActionPlayScratch& scratch, const EngineImmediateEntryAttempt& attempt,
        bool publicationPreflight, bool retirementAbort = false) noexcept {
        const bool capturedCohort = MatchesRegisteredPendingVariableCohortReprepare(scratch, attempt);
        if (!OwnsImmediateEntryAttemptStorage(attempt) || !attempt.active || attempt.generation == 0 ||
            (attempt.privateVcProducerCount != 0
                ? !(capturedCohort ||
                    IsRegisteredPrivateVcProducerFor(scratch, attempt, !publicationPreflight))
                : !actionPlayScratch.IsConstructed(attempt.producerSlot) ||
                  &actionPlayScratch[attempt.producerSlot] != &scratch ||
                  scratch.storage.generation != attempt.producerGeneration) ||
            scratch.storage.phase != EngineFrontierAttemptPhase::PrivateCommitted ||
            !scratch.variableControlCommitted || scratch.row == nullptr ||
            attempt.gate.Observe(attempt.generation) != ImmediateEntryAttemptState::Preparing ||
            (attempt.producerAbortPending && !capturedCohort && !retirementAbort) ||
            (attempt.producerAbortFullyPrivate && !publicationPreflight && !retirementAbort) ||
            attempt.producerAbortPartialArmPublished)
            return nullptr;
        const auto& row = *scratch.row;
        if (!MatchesOriginalPlayableServiceStorage(row) || !row.InvocationValid() ||
            (attempt.privateVcProducerCount == 0 &&
                (row.identity != attempt.initialService || row.invocationGeneration != attempt.initialInvocationGeneration)) ||
            row.Invocation().actionFiring != attempt.root || row.PendingImmediateCohortRoot() != attempt.root ||
            (row.phase != EnginePlayableStartPhase::PendingImmediateEntry &&
             !(retirementAbort && row.phase == EnginePlayableStartPhase::Cancelling)) ||
            (!row.pendingImmediateEntry && !retirementAbort) ||
            row.actionPrepared || row.partitionRetiring || (row.pendingCancellationIntent && !capturedCohort && !retirementAbort) ||
            row.controlLifecycle != PlayableControlLifecycle::Preparing ||
            row.pendingImmediateTimeDomain != ImmediateLaneTimeDomain::RelativeToAnchor ||
            (row.Invocation().actionTerminalReason != ActionTerminalReason::None && !retirementAbort) ||
            !acceptedPendingVariableGroups.IsConstructed(row.identity.slot)) return nullptr;
        auto& group = acceptedPendingVariableGroups[row.identity.slot];
        const auto& original = scratch.storage.originalPendingVariableProducer;
        const auto service = group.service.FactoryReference();
        const auto root = row.Invocation().variableControlOuterRootInstance != 0
            ? row.Invocation().variableControlOuterRootInstance : row.rootInstance;
        if (group.engine != this || !group.accepted || !group.cut.Valid() || !group.producer.Valid() ||
            group.producer.original_ != &original || group.producer.marker_ != original.marker_ ||
            original.engine_ != this || original.storage_ != &scratch.storage ||
            original.storageGeneration_ != scratch.storage.generation || original.service_ != &row ||
            original.invocation_ != row.invocation || original.reference_.token != row.identity ||
            original.reference_.owner != row.ownerInstance || original.reference_.epoch != row.ownerEpoch ||
            original.reference_.invocationGeneration != row.invocationGeneration ||
            group.reference.token != row.identity || group.reference.owner != row.ownerInstance ||
            group.reference.epoch != row.ownerEpoch ||
            group.reference.invocationGeneration != row.invocationGeneration ||
            !MatchesOriginalPlayableServiceLifetime(group.service) || service.token != row.identity ||
            service.owner != row.ownerInstance || service.epoch != row.ownerEpoch ||
            service.invocationGeneration != row.invocationGeneration ||
            !playableInstances.IsOriginalLifetimePinFor(group.root, root) ||
            !playableInstances.IsOriginalLifetimeCurrent(group.root)) return nullptr;
        return &group;
    }

    OriginalPendingVariableGroupHolder* OriginalPendingVariableGroupForPlacement(
        EngineActionPlayScratch& scratch, const EngineImmediateEntryAttempt& attempt,
        const PrivateImmediateLanePlacementProof& placement) noexcept {
        if (attempt.originalLaneFactoryRoot != attempt.root ||
            attempt.originalLaneFactoryAttemptGeneration != attempt.generation ||
            !immediateActionLane.IsOriginalAttemptLifetimePinFor(
                attempt.originalLaneLifetime, attempt.lane, attempt.root) ||
            !immediateActionLane.IsPrivatePlacementProofFor(placement, attempt.lane, attempt.root)) return nullptr;
        return OriginalPendingVariableGroupForProducer(scratch, attempt);
    }

    bool CanBeginPendingVariableGroupReprepare(EngineActionPlayScratch& scratch,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement,
        bool settled, std::array<TimelineArmObservation, kMaxKnownScheduleLeaves>* outObservations = nullptr) noexcept {
        auto* group = OriginalPendingVariableGroupForPlacement(scratch, attempt, placement);
        const bool registered = attempt.privateVcProducerCount != 0;
        if (group == nullptr || (group->RepreparePending() &&
                (!registered || group->reprepare_.phase != OriginalPendingVariableGroupHolder::InitialReprepare::Phase::CohortCaptured)) ||
            group->leafRetirement != (settled ? OriginalPendingVariableGroupHolder::LeafRetirement::Settled
                : OriginalPendingVariableGroupHolder::LeafRetirement::None) ||
            (settled || !registered ? !variables.CanReleaseReadCut(group->cut)
                : !variables.OriginalPendingVariableGroupUsesMatch(*this, scratch, attempt, placement)) ||
            scratch.row == nullptr) return false;
        auto& row = *scratch.row;
        // Only the factory-certified deterministic subtree domain is eligible.
        const bool queueDomain = group->queueNativeOriginCount != 0;
        if ((!queueDomain && ((!registered && (attempt.queueContinuationCount != 0 || attempt.queueFutureCount != 0)) ||
            attempt.QueueCoordinators().initialQueueCoordinator.Valid() || row.Invocation().variableControlOuterRootInstance != 0 ||
            scratch.frontierQueueCount != 0 ||
            scratch.queueFutureAttemptCount != 0)) ||
            !MatchesPendingVariableGroupTopology(*group, row, settled) ||
            group->leafCount > group->leafArms.size() || row.variableControlChildCount > kMaxKnownScheduleLeaves ||
            row.variableControlChildCount > group->reprepare_.children.size() ||
            decisions.ResolveFrontierActivation(scratch.frontierCoordinator) != nullptr ||
            decisions.HasFrontierDecisionBinding(scratch.frontierCoordinator)) return false;
        for (const auto& candidate : playableServices) {
            if (&candidate == &row || candidate.PendingImmediateCohortRoot() != attempt.root ||
                candidate.phase == EnginePlayableStartPhase::Free) continue;
            bool participant = false;
            if (registered) {
                auto token = attempt.privateVcProducerHead;
                for (std::uint32_t index = 0; index < attempt.privateVcProducerCount; ++index) {
                    if (!actionPlayScratch.IsConstructed(token.slot)) return false;
                    const auto& producer = actionPlayScratch[token.slot];
                    if (producer.storage.generation != token.generation) return false;
                    const bool registeredProducer = MatchesRegisteredPendingVariableCohortReprepare(producer, attempt) ||
                        IsRegisteredPrivateVcProducerFor(producer, attempt);
                    participant |= registeredProducer && (producer.row == &candidate ||
                        std::any_of(producer.variableControlNestedServices.begin(),
                            producer.variableControlNestedServices.begin() + producer.variableControlNestedVariableControlCount,
                            [&](const auto& reference) { return ResolvePlayableService(reference) == &candidate; }) ||
                        std::any_of(producer.storage.frontierServices.begin(),
                            producer.storage.frontierServices.begin() + producer.frontierServiceCount,
                            [&](const auto& owner) { return ResolvePlayableService(owner.service) == &candidate; }));
                    token = producer.privateInitialVcPageCohort.rootProducerNext;
                }
            }
            participant |= std::any_of(scratch.variableControlNestedServices.begin(),
                scratch.variableControlNestedServices.begin() + scratch.variableControlNestedVariableControlCount,
                [&](const auto& reference) { return ResolvePlayableService(reference) == &candidate; }) ||
                std::any_of(scratch.storage.frontierServices.begin(),
                    scratch.storage.frontierServices.begin() + scratch.frontierServiceCount,
                    [&](const auto& owner) { return ResolvePlayableService(owner.service) == &candidate; });
            if (!participant) return false;
        }
        const auto root = playableInstances.Classify(row.ownerInstance);
        if (!root.live || root.lifecycle != PlayableLifecycle::Active || root.scheduleEpoch != row.ownerEpoch)
            return false;
        const bool capturedRetirement = registered && settled;
        if (capturedRetirement && (!group->reprepare_.membersCaptured ||
            group->reprepare_.childCount != row.variableControlChildCount)) return false;
        std::uint32_t actualChildren = 0;
        for (auto child = root.firstChild; child != 0; child = playableInstances.NextSibling(child)) {
            if (++actualChildren > row.variableControlChildCount) return false;
            std::uint32_t matches = 0;
            for (std::uint32_t index = 0; index < row.variableControlChildCount; ++index)
                matches += row.variableControlChildInstances[index] == child;
            if (matches != 1) return false;
        }
        if (!capturedRetirement && actualChildren != row.variableControlChildCount) return false;
        if (queueDomain && !CanBeginPendingVariableQueueReprepare(scratch, attempt, *group, settled)) return false;
        ArmGroupSnapshot coordinator{};
        DecisionReserveRequest coordinatorInput{};
        coordinatorInput.ticket.sequence = row.ticket;
        coordinatorInput.world = row.Invocation().worldGeneration;
        coordinatorInput.transport = timelinePool.Epoch();
        coordinatorInput.expectedContinuationGeneration = scratch.storage.generation;
        coordinatorInput.sliceCount = 1;
        if (!scratch.frontierDescriptor.primary.Exact() ||
            !decisions.MatchesOpenReservation(scratch.frontierCoordinator, coordinatorInput) ||
            !armGroups.Capture(scratch.frontierCoordinatorGroup, &coordinator) || coordinator.memberCount != 0 ||
            coordinator.parent.slot != scratch.frontierCoordinator.slot ||
            coordinator.parent.generation != scratch.frontierCoordinator.generation) return false;
        std::array<DecisionToken, EngineActionPlayScratch::kPendingVariableReplacementControlCapacity>
            sideDecisions{row.variableControlDiscreteDecision, row.variableControlContinuousDecision};
        std::array<ArmGroupToken, EngineActionPlayScratch::kPendingVariableReplacementControlCapacity>
            sideGroups{row.variableControlDiscreteArmGroup, row.variableControlContinuousArmGroup};
        auto& nativeGroups = group->reprepare_.nativeGroups;
        const auto groupSpan = std::size_t{3} + group->nestedInitialSideCount + group->queueNativeOriginCount;
        if (groupSpan > EngineActionPlayScratch::kPendingVariableReplacementGroupCapacity ||
            groupSpan > nativeGroups.size()) return false;
        std::fill(nativeGroups.begin(), nativeGroups.end(), Plan03GroupSliceIdentity{});
        nativeGroups[0] = scratch.frontierDescriptor.primary;
        nativeGroups[1] = row.InitialLaneDiscreteGroup();
        nativeGroups[2] = row.InitialLaneContinuousGroup();
        std::array<std::uint32_t, EngineActionPlayScratch::kPendingVariableReplacementControlCapacity>
            nativeCounts{row.variableControlInitialDiscreteMemberCount, row.variableControlInitialContinuousMemberCount};
        std::uint32_t nativeCount = 1;
        for (std::uint32_t side = 0; side < 2; ++side) {
            const auto bank = side == 0 ? row.variableControlDiscreteBank : row.variableControlContinuousBank;
            if (!bank.Valid()) {
                if (sideDecisions[side].Valid() || sideGroups[side].Valid() || nativeGroups[side + 1].Valid()) return false;
                continue;
            }
            if (!variableControlInstances.MatchesOriginalInitialSideReceipt(group->initialSides[side],
                    row.variableControlInstance, side == 0 ? VariableControlSide::Discrete : VariableControlSide::Continuous,
                    sideDecisions[side], bank)) return false;
            if (!sideDecisions[side].Valid()) {
                if (sideGroups[side].Valid() || nativeGroups[side + 1].Valid()) return false;
                continue;
            }
            auto current = group->initialDecisionInputs[side];
            current.ticket.sequence = row.ticket;
            auto* baseline = ResolveOriginalSingleDecisionBaseline(this, decisions, sideDecisions[side]);
            ArmGroupSnapshot arms{};
            const auto expectedCount = side == 0 ? row.variableControlInitialDiscreteMemberCount
                : row.variableControlInitialContinuousMemberCount;
            if (baseline == nullptr || !decisions.MatchesOriginalDecisionFactory(*baseline, group->initialDecisionInputs[side]) ||
                !decisions.MatchesOpenReservation(sideDecisions[side], current) ||
                decisions.ResolveFrontierActivation(sideDecisions[side]) != nullptr ||
                decisions.HasFrontierDecisionBinding(sideDecisions[side]) ||
                !nativeGroups[side + 1].Exact() || !armGroups.Capture(sideGroups[side], &arms) ||
                arms.parent.slot != sideDecisions[side].slot || arms.parent.generation != sideDecisions[side].generation ||
                arms.memberCount != expectedCount) return false;
            ++nativeCount;
        }
        if (group->nestedInitialSideCount > group->nestedInitialSides.size()) return false;
        for (std::uint32_t index = 0; index < group->nestedInitialSideCount; ++index) {
            const auto& captured = group->nestedInitialSides[index];
            const auto* service = ResolvePlayableService(captured.service);
            const auto side = index + 2u;
            const bool discrete = captured.side == VariableControlSide::Discrete;
            VariableControlInstanceSnapshot instance{};
            ArmGroupSnapshot arms{};
            auto current = captured.input;
            current.ticket.sequence = row.ticket;
            const auto* baseline = ResolveOriginalSingleDecisionBaseline(this, decisions, captured.decision);
            if (service == nullptr || &service->Invocation() != &row.Invocation() ||
                service->variableControlInstance.slot != captured.instance.slot ||
                service->variableControlInstance.generation != captured.instance.generation ||
                service->ownerInstance == row.ownerInstance ||
                !variableControlInstances.Capture(captured.instance, &instance) ||
                instance.rootInstance != service->ownerInstance || instance.world != row.Invocation().worldGeneration ||
                instance.containerStableId != service->root.stableId ||
                instance.programSemanticFingerprint != service->root.expectedProgramSemanticFingerprint ||
                !variableControlInstances.MatchesOriginalInitialSideReceipt(captured.original, captured.instance,
                    captured.side, captured.decision, captured.bank) ||
                baseline == nullptr || !decisions.MatchesOriginalDecisionFactory(*baseline, captured.input) ||
                !decisions.MatchesOpenReservation(captured.decision, current) ||
                decisions.ResolveFrontierActivation(captured.decision) != nullptr ||
                decisions.HasFrontierDecisionBinding(captured.decision) ||
                !captured.group.Exact() || !armGroups.Capture(captured.armGroup, &arms) ||
                arms.parent.slot != captured.decision.slot || arms.parent.generation != captured.decision.generation ||
                arms.memberCount != captured.memberCount ||
                (discrete ? service->variableControlDiscreteDecision.slot : service->variableControlContinuousDecision.slot) != captured.decision.slot ||
                (discrete ? service->variableControlDiscreteDecision.generation : service->variableControlContinuousDecision.generation) != captured.decision.generation ||
                (discrete ? service->InitialLaneDiscreteGroup() : service->InitialLaneContinuousGroup()) != captured.group)
                return false;
            sideDecisions[side] = captured.decision;
            sideGroups[side] = captured.armGroup;
            nativeGroups[side + 1u] = captured.group;
            nativeCounts[side] = captured.memberCount;
            ++nativeCount;
        }
        std::uint32_t queueNative = 3u + group->nestedInitialSideCount;
        for (const auto& origin : pendingVariableQueueNativeOrigins) {
            if (origin.group != group) continue;
            if (queueNative >= nativeGroups.size()) return false;
            nativeGroups[queueNative++] = origin.identity;
            ++nativeCount;
        }
        std::array<TimelineArmObservation, kMaxKnownScheduleLeaves> observations{};
        for (std::uint32_t leaf = 0; leaf < group->leafCount; ++leaf) {
            auto& observation = observations[leaf];
            if (!timelineArms.Observe(group->leafArms[leaf], &observation) ||
                observation.startedBlockSequence != 0 || !observation.parentGroup.Valid()) return false;
            if (capturedRetirement &&
                !MatchesPendingVariableGroupRetiredMemberStorage(*group, row, leaf)) return false;
            if (settled) {
                if (!originalSourceSubmissions.PendingInstalledSourceRetirementSettled(
                        group->leafRetirementReceipts[leaf], group->leafArms[leaf]) ||
                    group->leafRetirementReceipts[leaf].StorageReturned() ||
                    observation.state != ArmState::Tombstoned || observation.tombstoneAudioSequence == 0) return false;
            } else if (!MatchesPendingVariableGroupLeaf(*group, leaf) ||
                (observation.state != ArmState::PendingApply && observation.state != ArmState::Armed &&
                 observation.state != ArmState::Published)) return false;
            std::uint32_t side = UINT32_MAX;
            for (std::uint32_t index = 0; index < sideDecisions.size(); ++index)
                if (sideDecisions[index].Valid() && observation.parentGroup.decision.slot == sideDecisions[index].slot &&
                    observation.parentGroup.decision.generation == sideDecisions[index].generation) {
                    if (side != UINT32_MAX) return false;
                    side = index;
                }
            ArmGroupMemberSnapshot member{};
            if (side == UINT32_MAX || observation.parentGroup.decision.slot != sideDecisions[side].slot ||
                observation.parentGroup.decision.generation != sideDecisions[side].generation ||
                !armGroups.CaptureMember(sideGroups[side], observation.memberIndex, &member) ||
                !OriginalSourceSubmissionRegistry::SameToken(member.token, group->leafArms[leaf])) return false;
        }
        auto& captures = initialQueueLaneGroupsScratch;
        const auto count = timelinePool.CaptureLiveImmediateLaneCohortGroups(attempt.root, captures.data(), captures.size());
        if (count > captures.size() || (!registered && count != nativeCount) ||
            (registered && count != attempt.originalLaneDemand.groups)) return false;
        std::uint32_t nativeMembers = 0;
        for (std::size_t index = 0; index < count; ++index) {
            const auto& identity = captures[index].identity;
            if (registered && std::count(nativeGroups.begin(), nativeGroups.end(), identity) == 0) continue;
            Plan03PageGroupView native{};
            if (std::count(nativeGroups.begin(), nativeGroups.end(), identity) != 1 ||
                identity.committedRow >= originalQueueGroupHolders.size() ||
                !timelinePool.IsOriginalQueueFutureGroupRegistrationFor(
                    originalQueueGroupHolders[identity.committedRow], identity, attempt.root) ||
                !timelinePool.PrivatePlan03GroupBindable(identity) ||
                !timelinePool.ResolvePlan03GroupSliceProducer(identity, &native) || native.frontier != nullptr ||
                native.lease == nullptr || native.lease->Capture().state != AudioSliceState::Ready) return false;
            const auto& owner = timelinePool.immediateLaneGroups_[identity.committedRow];
            const bool unbound = owner.laneOwner == nullptr && owner.laneSlot == UINT32_MAX && owner.laneGeneration == 0;
            const bool bound = owner.laneOwner == &immediateActionLane && owner.laneSlot == attempt.lane.slot &&
                owner.laneGeneration == attempt.lane.generation;
            if (!unbound && !bound) return false;
            const auto queueOrigin = std::find_if(pendingVariableQueueNativeOrigins.begin(), pendingVariableQueueNativeOrigins.end(),
                [&](const auto& origin) { return origin.group == group && origin.identity == identity; });
            if (queueOrigin != pendingVariableQueueNativeOrigins.end()) {
                if (queueOrigin->disposed || native.memberCount != queueOrigin->scope.input.memberCount ||
                    native.sourceControlCount != queueOrigin->scope.input.sourceControlCount) return false;
                continue;
            }
            if (identity.sliceIndex != 0 || native.sourceControlCount != 0) return false;
            const auto role = static_cast<std::uint32_t>(std::find(nativeGroups.begin(), nativeGroups.end(), identity) - nativeGroups.begin());
            const auto decision = role == 0 ? scratch.frontierCoordinator : sideDecisions[role - 1];
            const auto memberCount = role == 0 ? 0u : nativeCounts[role - 1u];
            if (identity.decision.slot != decision.slot || identity.decision.generation != decision.generation ||
                native.memberCount != memberCount || (memberCount != 0 && native.members == nullptr)) return false;
            for (std::uint32_t member = 0; member < memberCount; ++member) {
                std::uint32_t matches = 0;
                for (std::uint32_t leaf = 0; leaf < group->leafCount; ++leaf) {
                    const auto& observation = observations[leaf];
                    if (observation.parentGroup.decision.slot == decision.slot &&
                        observation.parentGroup.decision.generation == decision.generation &&
                        observation.memberIndex == member &&
                        OriginalSourceSubmissionRegistry::SameToken(native.members[member], group->leafArms[leaf])) ++matches;
                }
                if (matches != 1) return false;
            }
            nativeMembers += memberCount;
        }
        if (nativeMembers != group->leafCount) return false;
        if (outObservations != nullptr) *outObservations = observations;
        return true;
    }

    bool BeginPendingVariableGroupReprepare(EngineActionPlayScratch& scratch,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement) noexcept {
        std::array<TimelineArmObservation, kMaxKnownScheduleLeaves> observations{};
        if (!CanBeginPendingVariableGroupReprepare(scratch, attempt, placement, true, &observations)) return false;
        auto* group = OriginalPendingVariableGroupForPlacement(scratch, attempt, placement);
        if (group == nullptr || scratch.row == nullptr) return false;
        auto& row = *scratch.row;
        std::array<DecisionToken, EngineActionPlayScratch::kPendingVariableReplacementControlCapacity>
            sideDecisions{row.variableControlDiscreteDecision, row.variableControlContinuousDecision};
        std::array<ArmGroupToken, EngineActionPlayScratch::kPendingVariableReplacementControlCapacity>
            sideGroups{row.variableControlDiscreteArmGroup, row.variableControlContinuousArmGroup};
        auto& nativeGroups = group->reprepare_.nativeGroups;
        for (std::uint32_t index = 0; index < group->nestedInitialSideCount; ++index) {
            const auto& captured = group->nestedInitialSides[index];
            sideDecisions[index + 2u] = captured.decision;
            sideGroups[index + 2u] = captured.armGroup;
            nativeGroups[index + 3u] = captured.group;
        }
        std::uint32_t queueNative = 3u + group->nestedInitialSideCount;
        for (const auto& origin : pendingVariableQueueNativeOrigins) {
            if (origin.group != group) continue;
            if (queueNative == nativeGroups.size()) return false;
            nativeGroups[queueNative++] = origin.identity;
        }
        const auto nativeCount = static_cast<std::uint32_t>(std::count_if(nativeGroups.begin(), nativeGroups.end(),
            [](const auto& identity) { return identity.Valid(); }));
        DecisionReserveRequest coordinatorInput{};
        coordinatorInput.ticket.sequence = row.ticket;
        coordinatorInput.world = row.Invocation().worldGeneration;
        coordinatorInput.transport = timelinePool.Epoch();
        coordinatorInput.expectedContinuationGeneration = scratch.storage.generation;
        coordinatorInput.sliceCount = 1;
        if (attempt.privateVcProducerCount == 0 &&
            !CapturePendingVariableGroupRetiredMemberStorage(*group, row)) return false;
        if (group->queueNativeOriginCount != 0 && !BeginPendingVariableQueueReprepare(scratch, attempt, *group))
            return false;
        // Record the real audio sequence while the original Arm rows still exist.
        for (std::uint32_t leaf = 0; leaf < group->leafCount; ++leaf) {
            const auto& observation = observations[leaf];
            std::uint32_t side = UINT32_MAX;
            for (std::uint32_t index = 0; index < sideDecisions.size(); ++index)
                if (sideDecisions[index].Valid() && observation.parentGroup.decision.slot == sideDecisions[index].slot &&
                    observation.parentGroup.decision.generation == sideDecisions[index].generation) side = index;
            if (side == UINT32_MAX) return false;
            ArmGroupSnapshot arms{};
            ArmGroupMemberSnapshot member{};
            if (!armGroups.Capture(sideGroups[side], &arms) ||
                (arms.state != ArmGroupState::Retiring && !armGroups.BeginRetire(sideGroups[side])) ||
                !armGroups.CaptureMember(sideGroups[side], observation.memberIndex, &member) ||
                !OriginalSourceSubmissionRegistry::SameToken(member.token, group->leafArms[leaf]) ||
                (!member.tombstoned && !armGroups.Tombstone(
                    sideGroups[side], group->leafArms[leaf], observation.tombstoneAudioSequence))) return false;
        }
        for (const auto sideGroup : sideGroups) {
            if (!sideGroup.Valid()) continue;
            ArmGroupSnapshot arms{};
            if (!armGroups.Capture(sideGroup, &arms) ||
                (arms.state != ArmGroupState::Retiring && !armGroups.BeginRetire(sideGroup)) ||
                arms.tombstonedCount != arms.memberCount) return false;
            for (std::uint16_t member = 0; member < arms.memberCount; ++member) {
                ArmGroupMemberSnapshot actual{};
                if (!armGroups.CaptureMember(sideGroup, member, &actual) || !actual.tombstoned) return false;
            }
        }
        auto& issued = group->reprepare_;
        issued.engine = this;
        issued.storage = &scratch.storage;
        issued.attempt = &attempt;
        issued.storageGeneration = scratch.storage.generation;
        issued.attemptGeneration = attempt.generation;
        issued.producerMarker = scratch.storage.originalPendingVariableProducer.marker_;
        issued.cut = group->cut;
        issued.lane = attempt.lane;
        issued.demand = attempt.originalLaneDemand;
        issued.coordinator = scratch.frontierCoordinator;
        issued.coordinatorInput = coordinatorInput;
        issued.coordinatorGroup = scratch.frontierCoordinatorGroup;
        std::copy_n(sideDecisions.begin(), 2u + group->nestedInitialSideCount, issued.decisions.begin());
        std::copy_n(sideGroups.begin(), 2u + group->nestedInitialSideCount, issued.armGroups.begin());
        issued.nativeGroupCount = nativeCount;
        issued.phase = OriginalPendingVariableGroupHolder::InitialReprepare::Phase::Issued;
        return true;
    }

    OriginalPendingVariableGroupHolder* OriginalPendingVariableGroupForReprepare(
        EngineActionPlayScratch& scratch, const EngineImmediateEntryAttempt& attempt) noexcept {
        auto* group = OriginalPendingVariableGroupForProducer(scratch, attempt);
        if (group == nullptr) return nullptr;
        const auto& issued = group->reprepare_;
        if (issued.engine != this || issued.storage != &scratch.storage || issued.attempt != &attempt ||
            issued.storageGeneration != scratch.storage.generation || issued.attemptGeneration != attempt.generation ||
            issued.producerMarker != scratch.storage.originalPendingVariableProducer.marker_ || issued.cut != group->cut ||
            issued.phase == OriginalPendingVariableGroupHolder::InitialReprepare::Phase::None ||
            group->leafRetirement != OriginalPendingVariableGroupHolder::LeafRetirement::Settled) return nullptr;
        for (std::uint32_t leaf = 0; leaf < group->leafCount; ++leaf)
            if (!originalSourceSubmissions.PendingInstalledSourceRetirementSettled(
                    group->leafRetirementReceipts[leaf], group->leafArms[leaf])) return nullptr;
        return group;
    }

    bool ServicePendingVariableGroupReprepare(EngineActionPlayScratch& scratch,
        EngineImmediateEntryAttempt& attempt) noexcept;
    bool CanBeginPendingVariableQueueReprepare(EngineActionPlayScratch& scratch,
        const EngineImmediateEntryAttempt& attempt, const OriginalPendingVariableGroupHolder& group,
        bool settled, std::array<TimelineArmObservation, kMaxKnownScheduleLeaves>* observations = nullptr) noexcept;
    bool BeginPendingVariableQueueReprepare(EngineActionPlayScratch& scratch,
        const EngineImmediateEntryAttempt& attempt, OriginalPendingVariableGroupHolder& group) noexcept;
    bool ReturnPendingVariableQueueReprepareReaders(EngineActionPlayScratch& scratch,
        EngineImmediateEntryAttempt& attempt, OriginalPendingVariableGroupHolder& group) noexcept;
    bool ReturnPendingVariableQueueReprepareRecords(EngineImmediateEntryAttempt& attempt) noexcept;
    bool ServicePendingVariableQueueReprepare(EngineActionPlayScratch& scratch,
        EngineImmediateEntryAttempt& attempt, OriginalPendingVariableGroupHolder& group) noexcept;

    bool BeginRegisteredPendingVariableCohortReprepare(EngineImmediateEntryAttempt& attempt,
        const PrivateImmediateLanePlacementProof& placement, bool reserveForReplacement = true,
        bool* waiting = nullptr) noexcept;
    bool RegisteredPendingVariableCohortRepreparePending(const EngineImmediateEntryAttempt& attempt) noexcept;
    bool MatchesRegisteredPendingVariableCohortReprepare(const EngineActionPlayScratch& producer,
        const EngineImmediateEntryAttempt& attempt) noexcept;
    bool ServiceRegisteredPendingVariableCohortReprepare(EngineImmediateEntryAttempt& attempt) noexcept;
    bool CancelRegisteredPendingVariableCohortReprepare(EngineImmediateEntryAttempt& attempt) noexcept;

    // A feature view includes fields embedded in other rows; do not add it to owned totals.
    std::size_t PendingVariableGroupStorageBytes() const noexcept {
        std::size_t censusBytes = 0;
        const auto replacementBytes = [](const auto& controls, const auto& groups, const auto& lanes) noexcept {
            return controls.capacity() * sizeof(EngineActionPlayScratch::PendingVariableReplacementControl) +
                groups.capacity() * sizeof(Plan03GroupSliceIdentity) +
                lanes.capacity() * sizeof(EngineActionPlayScratch::PendingVariableReplacementQueueLane);
        };
        for (const auto& scratch : actionPlayScratch)
            censusBytes += replacementBytes(scratch.pendingVariableReplacementControls,
                scratch.pendingVariableReplacementGroups, scratch.pendingVariableReplacementQueueLanes);
        const auto addGroupBacking = [&](const auto& groups) noexcept {
            const auto columnBytes = [](const auto& column) noexcept {
                return column.capacity() * sizeof(typename std::decay_t<decltype(column)>::value_type);
            };
            for (const auto& group : groups) {
                censusBytes += group.nestedInitialSides.capacity() *
                    sizeof(OriginalPendingVariableGroupHolder::NestedInitialSide);
                censusBytes += columnBytes(group.leafArms) + columnBytes(group.leafRetained) +
                    columnBytes(group.leafRetirementReceipts) + group.reprepare_.StorageBytes() +
                    columnBytes(group.boundAbort.queues) +
                    columnBytes(group.queueNativeMembers) +
                    columnBytes(group.topology_.nodes) + columnBytes(group.topology_.topNodes) +
                    columnBytes(group.topology_.leafNodes) + columnBytes(group.topology_.leafTops);
                for (const auto& slot : group.queueRoleReservations)
                    censusBytes += replacementBytes(slot.controls, slot.groups, slot.lanes);
            }
        };
        addGroupBacking(preparedPendingVariableGroups);
        addGroupBacking(acceptedPendingVariableGroups);
        for (const auto& storage : actionPlayStorage) {
            censusBytes += storage.privateInitialVcLaneMembers.capacity() * sizeof(ImmediateLaneMemberManifest) +
                storage.privateInitialVcLaneGroups.capacity() * sizeof(ImmediateLaneAcceptedPrivateGroup) +
                storage.privateInitialVcArmPreparations.capacity() * sizeof(EngineActionPlayStorage::PrivateInitialVcArmPreparation);
        }
        for (std::uint32_t index = 0; index < immediateEntryAttempts.size(); ++index) {
            const auto& attempt = immediateEntryAttempts[index];
            censusBytes += attempt.selectionProducers.capacity() * sizeof(EngineInitialSelectionProducer) +
                attempt.selectionCommitInputs.capacity() * sizeof(PendingSelectionBatchCommitV1);
        }
        return preparedPendingVariableGroups.RequestedBytes() +
            acceptedPendingVariableGroups.RequestedBytes() +
            pendingOrdinarySelections.RequestedBytes() +
            pendingVariableLeafSources.RequestedBytes() +
            pendingVariableFutureSources.RequestedBytes() +
            pendingVariableFutureNodes.RequestedBytes() +
            pendingVariableQueueNativeOrigins.RequestedBytes() +
            actionPlayStorage.ConstructedCount() * sizeof(OriginalPendingVariableProducerRetention) +
            actionPlayScratch.ConstructedCount() * kMaxKnownScheduleLeaves *
                sizeof(EngineActionPlayScratch::PendingVariableReplacementLeaf) +
            actionPlayScratch.ConstructedCount() * (sizeof(ImmediateLaneHandle) + sizeof(ImmediateLaneDemand) +
                sizeof(PreparedDecisionReservation) +
                2u * sizeof(OriginalDecisionLifetimePin)) +
            actionPlayScratch.ConstructedCount() * sizeof(EngineActionPlayScratch::PrivateInitialVcPageCohort) +
            playableServices.size() * sizeof(EnginePlayableServiceRow::PrivateImmediateEntryReceiver) +
            PrivateImmediateEntryResultStorageBytes() +
            immediateActionLane.PrivateEntryStorageBytes() + censusBytes;
    }

    bool ReclaimPendingVariableGroupDecisions(OriginalPendingVariableGroupHolder& group) noexcept {
        if (group.engine != this) return false;
        for (auto& retired : group.retiredDecisions) {
            if (!retired.token.Valid()) continue;
            auto* baseline = ResolveOriginalSingleDecisionBaseline(this, decisions, retired.token);
            if (baseline == nullptr || !decisions.MatchesOriginalDecisionFactory(*baseline, retired.input)) return false;
            auto current = retired.input;
            current.ticket.sequence = retired.sequence;
            if (decisions.MatchesOpenReservation(retired.token, current) && !decisions.Cancel(retired.token)) return false;
            if (!decisions.MatchesCancelledReservation(retired.token, current)) return false;
            if (!decisions.ReclaimCancelled(retired.token, baseline) && decisions.Contains(retired.token)) return false;
            retired = {};
        }
        return true;
    }

    // Acquire every requested side before preparing any shared-cut leaf use.
    // The synchronous closure cannot publish, call Update or reenter another producer.
    template <typename PrepareChildren>
    bool WithPendingVariableGroupInitialSidesReplacement(EngineActionPlayScratch& scratch,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement,
        const std::array<bool, 2> sides,
        const std::array<PreparedDecisionReservation*, 2>& replacements,
        PrepareChildren&& prepareChildren) noexcept {
        if (pendingVariableGroupReplacementActive || attempt.producerAbortPending || (!sides[0] && !sides[1]) ||
            (!sides[0] && replacements[0] != nullptr) || (!sides[1] && replacements[1] != nullptr)) return false;
        auto* group = placement.Valid() ? OriginalPendingVariableGroupForPlacement(scratch, attempt, placement)
            : OriginalPendingVariableGroupForReprepare(scratch, attempt);
        if (group == nullptr || !variables.CanReleaseReadCut(group->cut) ||
            (!placement.Valid() && (!group->ReprepareReady() || attempt.lane.Valid())) ||
            !ReclaimPendingVariableGroupDecisions(*group)) return false;
        if (group->leafRetirement == OriginalPendingVariableGroupHolder::LeafRetirement::Requesting ||
            group->leafRetirement == OriginalPendingVariableGroupHolder::LeafRetirement::Failed) return false;
        auto& row = *scratch.row;
        const auto controller = playableInstances.Classify(row.ownerInstance);
        VariableControlInstanceSnapshot control{};
        if (!controller.live || controller.kind != PlayableKind::Container ||
            controller.lifecycle != PlayableLifecycle::Active || controller.scheduleEpoch != row.ownerEpoch ||
            row.variableControlAcceptedEpoch != row.ownerEpoch || row.ticket == 0 ||
            scratch.continuationTransportEpoch != timelinePool.Epoch() ||
            !variableControlInstances.Capture(row.variableControlInstance, &control) ||
            control.rootInstance != row.ownerInstance || control.world != row.Invocation().worldGeneration ||
            control.containerStableId != row.root.stableId ||
            control.programSemanticFingerprint != row.root.expectedProgramSemanticFingerprint) return false;
        const std::array<DecisionToken, 2> old{row.variableControlDiscreteDecision, row.variableControlContinuousDecision};
        const std::array<BankVersionToken, 2> banks{row.variableControlDiscreteBank, row.variableControlContinuousBank};
        const std::array<LogicCommitSequence, 2> tickets{row.variableControlDiscreteTicket, row.variableControlContinuousTicket};
        for (std::uint32_t slot = 0; slot < sides.size(); ++slot) {
            if (!sides[slot]) continue;
            const auto side = slot == 0 ? VariableControlSide::Discrete : VariableControlSide::Continuous;
            if (!variableControlInstances.MatchesOriginalInitialSideReceipt(group->initialSides[slot],
                    row.variableControlInstance, side, old[slot], banks[slot])) return false;
            if (!old[slot].Valid()) {
                if (tickets[slot] != 0) return false;
                continue;
            }
            const auto* baseline = ResolveOriginalSingleDecisionBaseline(this, decisions, old[slot]);
            const auto& input = group->initialDecisionInputs[slot];
            auto current = input;
            current.ticket.sequence = tickets[slot];
            if (baseline == nullptr || tickets[slot] != row.ticket ||
                input.world != control.world || input.transport != timelinePool.Epoch() ||
                input.expectedContinuationGeneration != row.ownerEpoch || input.ticket.Valid() ||
                !decisions.MatchesOriginalDecisionFactory(*baseline, input) ||
                !decisions.MatchesOpenReservation(old[slot], current) ||
                decisions.ResolveFrontierActivation(old[slot]) != nullptr || decisions.HasFrontierDecisionBinding(old[slot])) return false;
        }
        for (std::uint32_t leaf = 0; leaf < group->leafCount; ++leaf) {
            if (group->leafRetirement != OriginalPendingVariableGroupHolder::LeafRetirement::None) {
                if (group->leafRetirement != OriginalPendingVariableGroupHolder::LeafRetirement::Settled ||
                    !originalSourceSubmissions.PendingInstalledSourceRetirementSettled(
                        group->leafRetirementReceipts[leaf], group->leafArms[leaf])) return false;
                continue;
            }
            if (!MatchesPendingVariableGroupLeaf(*group, leaf)) return false;
            const auto* arm = timelineArms.Resolve(group->leafArms[leaf]);
            if (arm == nullptr || !arm->pendingImmediateStart || arm->startBound ||
                arm->startedBlockSequence.load(std::memory_order_acquire) != 0 ||
                arm->frontierOperation.Valid() || arm->frontierAdmission != nullptr ||
                arm->frontierAdmissionLease != nullptr ||
                arm->frontierActiveLease.load(std::memory_order_acquire) != nullptr) return false;
        }
        std::array<DecisionToken, 2> next = old;
        std::array<DecisionReserveRequest, 2> nextInputs{};
        std::array<DecisionTable::PreparedTicketCommitProof, 2> nextProofs;
        for (std::uint32_t slot = 0; slot < sides.size(); ++slot) {
            if (!sides[slot]) continue;
            next[slot] = {};
            auto* replacement = replacements[slot];
            if (replacement == nullptr) continue;
            const auto* original = OriginalSingleDecisionPreparation(*replacement);
            auto& input = nextInputs[slot];
            input = replacement->OriginalRequest();
            if (original == nullptr || !decisions.OwnsOpenPreparedReservation(*replacement) ||
                input.ticket.Valid() || input.world != control.world ||
                input.transport != timelinePool.Epoch() || input.expectedContinuationGeneration != row.ownerEpoch ||
                input.sliceCount != 1 || input.transitionGate.Valid() ||
                decisions.PrepareFrontierControlProgram(replacement->Token(), scratch.storage.generation) == nullptr ||
                !decisions.ProvePreparedReservationCommit(*replacement, input, row.ticket, &nextProofs[slot])) return false;
            next[slot] = nextProofs[slot].Token();
        }
        if (next[0].Valid() && next[0].slot == next[1].slot && next[0].generation == next[1].generation) return false;
        std::array<VariableControlInstanceTable::InitialSideReplacement, 2> held;
        for (std::uint32_t slot = 0; slot < sides.size(); ++slot)
            if (sides[slot] && variableControlInstances.BeginInitialSideReplacement(group->initialSides[slot],
                    next[slot], &held[slot]) != VariableControlInstanceStatus::Ok) return false;
        PendingVariableGroupReplacementScope scope(*group, scratch, attempt, sides, next, nextProofs, held);
        struct Guard {
            bool& active;
            const PendingVariableGroupReplacementScope*& current;
            Guard(bool& value, const PendingVariableGroupReplacementScope*& currentScope,
                const PendingVariableGroupReplacementScope& scope) : active(value), current(currentScope) {
                active = true;
                current = &scope;
            }
            ~Guard() { current = nullptr; active = false; }
        } guard(pendingVariableGroupReplacementActive, activePendingVariableGroupReplacement_, scope);
        if (!prepareChildren(std::as_const(next))) return false;
        for (std::uint32_t slot = 0; slot < sides.size(); ++slot) {
            if (!sides[slot]) continue;
            if (next[slot].Valid()) DecisionTable::ConsumePreparedReservationCommit(std::move(nextProofs[slot]));
            group->retiredDecisions[slot] = {old[slot], group->initialDecisionInputs[slot], tickets[slot]};
            const bool finished = variableControlInstances.FinishInitialSideReplacement(held[slot], true);
            assert(finished);
            if (!finished) { runtimeRecreateRequired = true; return false; }
            if (slot == 0) { row.variableControlDiscreteDecision = next[slot]; row.variableControlDiscreteTicket = next[slot].Valid() ? row.ticket : 0; }
            else { row.variableControlContinuousDecision = next[slot]; row.variableControlContinuousTicket = next[slot].Valid() ? row.ticket : 0; }
            group->initialDecisionInputs[slot] = nextInputs[slot];
        }
        return true;
    }

    template <typename PrepareChildren>
    bool WithPendingVariableGroupInitialSideReplacement(EngineActionPlayScratch& scratch,
        const EngineImmediateEntryAttempt& attempt, const PrivateImmediateLanePlacementProof& placement,
        VariableControlSide side, PreparedDecisionReservation* replacement,
        PrepareChildren&& prepareChildren) noexcept {
        if (side != VariableControlSide::Discrete && side != VariableControlSide::Continuous) return false;
        const auto slot = side == VariableControlSide::Discrete ? 0u : 1u;
        std::array<bool, 2> sides{};
        std::array<PreparedDecisionReservation*, 2> replacements{};
        sides[slot] = true;
        replacements[slot] = replacement;
        return WithPendingVariableGroupInitialSidesReplacement(scratch, attempt, placement, sides, replacements,
            [&](const std::array<DecisionToken, 2>&) noexcept { return prepareChildren(); });
    }

    bool OriginalPreparingInvocationStorageReady(const EngineActionPlayStorage& storage) const noexcept {
        const auto& original = storage.originalInvocationStorage;
        if (original.engine_ == nullptr) return true;
        if (original.engine_ != this || original.attempt_ != &storage ||
            original.attemptGeneration_ != storage.generation || original.invocation_ == nullptr ||
            original.invocation_->slot >= playableStarts.size() ||
            &playableStarts[original.invocation_->slot] != original.invocation_) return false;
        if (original.ended_) return true;
        if (original.invocation_->generation != original.invocationGeneration_ ||
            original.invocation_->serviceCount == 0) return false;
        std::uint32_t actualServices = 0;
        for (const auto& row : playableServices) {
            if (!row.partition.Valid() || row.invocation != original.invocation_ ||
                row.invocationGeneration != original.invocationGeneration_) continue;
            if (!CanReclaimOriginalPlayableServiceStorage(row)) return false;
            ++actualServices;
        }
        return actualServices == original.invocation_->serviceCount;
    }

    bool OriginalPreparingStorageBelongsToInvocation(const EngineActionPlayStorage& storage,
        const EnginePlayableStartRow& invocation) const noexcept {
        const auto& original = storage.originalInvocationStorage;
        return original.engine_ == this && original.attempt_ == &storage &&
            original.attemptGeneration_ == storage.generation && !original.ended_ &&
            original.invocation_ == &invocation && original.invocationGeneration_ == invocation.generation;
    }

    void ObserveOriginalPreparingStorageWait(const EnginePlayableServiceRow& row) noexcept {
        if (!MatchesOriginalPlayableServiceStorage(row) || CanReclaimOriginalPlayableServiceStorage(row)) return;
        const auto& serviceOriginal = originalPlayableServiceLifetimeHolders[row.identity.slot];
        auto* storage = serviceOriginal.preparingStorage_;
        if (storage == nullptr || storage->generation != serviceOriginal.preparingStorageGeneration_) return;
        auto& original = storage->originalInvocationStorage;
        if (original.engine_ == this && original.attempt_ == storage &&
            original.attemptGeneration_ == storage->generation && original.invocation_ == row.invocation &&
            original.invocationGeneration_ == row.invocationGeneration && !original.ended_)
            original.blocked_ = originalPlayableServiceLifetimeHolders[row.identity.slot].reference_;
    }

    bool HasObservedOriginalPreparingStorageWait(const EngineActionPlayStorage& storage) const noexcept {
        const auto& original = storage.originalInvocationStorage;
        if (original.engine_ != this || original.attempt_ != &storage ||
            original.attemptGeneration_ != storage.generation || original.ended_ || !original.blocked_.Valid())
            return false;
        if (original.blocked_.token.slot >= playableServices.size()) return false;
        const auto* row = &playableServices[original.blocked_.token.slot];
        return row->identity == original.blocked_.token && row->ownerInstance == original.blocked_.owner &&
            row->ownerEpoch == original.blocked_.epoch && row->invocation == original.invocation_ &&
            row->invocationGeneration == original.invocationGeneration_ &&
            MatchesOriginalPlayableServiceStorage(*row) && !CanReclaimOriginalPlayableServiceStorage(*row);
    }

    void TransferOriginalPreparingInvocationStorage(EngineActionPlayStorage& storage,
        EnginePlayableStartRow& invocation) noexcept {
        auto& original = storage.originalInvocationStorage;
        if (original.engine_ != this || original.attempt_ != &storage ||
            original.attemptGeneration_ != storage.generation || original.invocation_ != &invocation ||
            original.invocationGeneration_ != invocation.generation || invocation.packLease.leaseToken == 0) return;
        if (auto* issuedPack = OriginalPreparingPackOwner(storage)) {
            auto& owner = *issuedPack;
            const auto& pack = packComposition.originalLeaseRecords_[owner.native_.index_];
            const bool exact = owner.receiver_ == this && packComposition.MatchesOriginalLease(owner.native_) &&
                (!storage.originalPackLease.Valid() || storage.originalPackLeaseGeneration == storage.generation) &&
                pack.originalPreparingStorage == &storage && pack.originalPreparingGeneration == storage.generation &&
                pack.originalInvocation == &invocation && pack.originalInvocationGeneration == invocation.generation &&
                pack.originalInvocationSlot == invocation.slot && !pack.originalInvocationEnded &&
                packComposition.SameFullOriginalLease(pack.lease, invocation.packLease) &&
                !invocation.originalPackLease.Valid();
            assert(exact);
            if (!exact) { runtimeRecreateRequired = true; return; }
            // All staged duties refer to this one genuine owner before moving it.
            for (const auto& service : originalPlayableServiceLifetimeHolders) {
                if (service.engine_ != this || service.invocation_ != &invocation ||
                    service.reference_.invocationGeneration != invocation.generation ||
                    service.packDutyOwner_ != &owner) continue;
                const bool dutyReady = MatchesOriginalPlayableServicePackDuty(service);
                assert(dutyReady);
                if (!dutyReady) { runtimeRecreateRequired = true; return; }
            }
            invocation.originalPackLease = std::move(owner);
            // The native move is the only ownership transfer; no lease is acquired.
            for (auto& service : originalPlayableServiceLifetimeHolders) {
                if (service.engine_ != this || service.invocation_ != &invocation ||
                    service.reference_.invocationGeneration != invocation.generation ||
                    service.packDutyOwner_ != &owner) continue;
                service.packDuty_ = OriginalPlayableServicePackDuty::OriginalInvocationOwns;
                service.packDutyOwner_ = &invocation.originalPackLease;
                service.packDutyDeferredRecord_ = {};
            }
            if (MatchesOriginalDeferredPackRecord(storage.originalDeferredPackRecord)) {
                auto& original = originalDeferredPackHolders[storage.originalDeferredPackRecord.slot];
                original.transferred_ = true;
            }
            storage.originalPackLeaseGeneration = 0;
            storage.originalDeferredPackRecord = {};
        }
        if (invocation.preparingStorage == &storage &&
            invocation.preparingStorageGeneration == storage.generation) {
            invocation.preparingStorage = nullptr;
            invocation.preparingStorageGeneration = 0;
        }
        original.ForgetAfterScratchExit();
    }

    void EndOriginalPreparingInvocationStorage(EnginePlayableStartRow& invocation) noexcept {
        assert(invocation.serviceCount == 0);
        auto* packOwner = &invocation.originalPackLease;
        if (!packOwner->Valid() && invocation.preparingStorage != nullptr &&
            invocation.preparingStorageGeneration == invocation.preparingStorage->generation)
            if (auto* issuedPack = OriginalPreparingPackOwner(*invocation.preparingStorage)) packOwner = issuedPack;
        if (packOwner->Valid()) {
            auto& pack = packComposition.originalLeaseRecords_[packOwner->native_.index_];
            const bool exact = packOwner->receiver_ == this && packComposition.MatchesOriginalLease(packOwner->native_) &&
                pack.originalInvocation == &invocation && pack.originalInvocationGeneration == invocation.generation &&
                pack.originalInvocationSlot == invocation.slot && !pack.originalInvocationEnded;
            assert(exact);
            if (exact) pack.originalInvocationEnded = true;
            else runtimeRecreateRequired = true;
        }
        // End only already-issued associations at this actual final native exit.
        for (auto& storage : actionPlayStorage) {
            auto& original = storage.originalInvocationStorage;
            if (original.engine_ != this || original.attempt_ != &storage ||
                original.attemptGeneration_ != storage.generation || original.invocation_ != &invocation ||
                original.invocationGeneration_ != invocation.generation) continue;
            original.ended_ = true;
            original.blocked_ = {};
        }
    }

    bool ReclaimOriginalPlayableServiceStorage(EnginePlayableServiceRow& row,
        std::uint64_t completedSequence, bool published) noexcept {
        if (!CanReclaimOriginalPlayableServiceStorage(row) ||
            playablePartitions.CommitProofHeld(row.partition, row.identity) ||
            playablePartitions.Published(row.partition, row.identity) != published ||
            (published && !playablePartitions.CanRelease(row.partition, row.identity, completedSequence)))
            return false;
        if (!published && !playablePartitions.RollbackCommit(row.partition, row.identity)) return false;
        auto& original = originalPlayableServiceLifetimeHolders[row.identity.slot];
        original.ClearIdentity();
        original.baseline_.Return();
        // All native gates and last fallible pack/bank work already completed.
        // This producer-only final prefix cannot reenter a new structural borrow.
        return published ? playablePartitions.Release(row.partition, row.identity, completedSequence)
                         : playablePartitions.Cancel(row.partition, row.identity);
    }

    void ReturnOriginalPlayableServiceStorageAfterQuiescence() noexcept {
        for (auto& original : originalPlayableServiceLifetimeHolders) {
            if (!original.Valid()) continue;
            assert(playablePartitions.OriginalStorageReclaimReady(original.baseline_));
            original.ClearIdentity();
            original.baseline_.Return();
        }
    }

    void ReturnPreparedActionDeferredLoansAfterQuiescence() noexcept {
        // Return only this Engine's held preparation loans. Original resource
        // owners retain their backing until rollback or receiver destruction.
        for (auto& scratch : actionDeferredScratch) scratch.pairCommitProof.Release();
    }

    ~Engine() {
        timelineGroupCapturePreparation.Detach();
        timelinePool.DetachImmediateLaneRowStoragePreparation();
        timelinePool.SetImmediateLaneGroupDisposalObserver(nullptr, nullptr);
        InvalidateProjectGenerationReservations();
        // The scheduling-domain transaction spans two members, so declaration
        // order alone cannot make teardown safe. Keep this as an idempotent
        // final guard for constructor rollback and direct internal deletion;
        // normal API teardown performs the same sequence before trace cleanup.
        device.RevokeRenderAdmission();
        voices.DisableRenderWorkers();
        (void)device.StopCallback();
        (void)voices.UnbindRealtimeWorkerDomain();
        device.ReleasePrepared();
        for (auto& revision : logicPacks.pending) revision.dynamicBusTapBacking.Release();
        // Private readers are unread loans, distinct from completed audio fences.
        for (std::uint32_t index = 0; index < immediateEntryAttempts.size(); ++index) {
            auto& attempt = immediateEntryAttempts[index];
            attempt.plainLayout = {};
            // This is the Engine's own original structural output. Real joined
            // teardown does not fabricate a capture/ACK or clear foreign loans.
            attempt.originalLaneLifetime.Return();
            attempt.originalLaneFactoryRoot = 0;
            attempt.originalLaneFactoryAttemptGeneration = 0;
            // Owner teardown is joined and foreign caps must already be returned.
            // This clears only the certificate, never a native count or receipt.
            if (attempt.originalC0Factory != nullptr)
                attempt.originalC0Factory->ClearCertificateAfterNativeDisposition();
            for (auto& source : attempt.plainSources) {
                if (!source.bindingReader.Valid()) continue;
                const bool released = source.bindingReader.Release(0);
                assert(released);
                (void)released;
            }
        }
        // Original caps and tables are still alive after callback quiescence.
        ReturnPreparedActionDeferredLoansAfterQuiescence();
        // Child cancellation needs the original unpublished command reservation.
        for (auto& group : acceptedPendingVariableGroups) {
            const bool returned = CancelPendingVariableGroupChildren(group);
            assert(returned);
            (void)returned;
        }
        for (auto& scratch : actionPlayScratch) {
            scratch.privateInitialVcPageCohort.ReleasePageTransferProofs();
            scratch.privateInitialVcPageCohort.ReleaseLaneTransferProofs();
            scratch.variableControlOwnerContext.serviceCommitProof.Release();
            scratch.playableTreeExecutor.ReleaseCommitProofs();
            for (const auto side : scratch.VoiceOwnerContextSides())
                for (auto& owner : side) owner.ReleaseCommitProofs();
            scratch.variableControlDiscreteDecisionOwnerContext.ticketCommitProof.Release();
            scratch.variableControlContinuousDecisionOwnerContext.ticketCommitProof.Release();
            scratch.variableControlOwnerContext.commitProof.Release();
        }
        preparedVoiceCommandBatchProof.Release();
        if (originalQueueReplacementBatch != nullptr) {
            const bool closed = originalQueueReplacementBatch->Close();
            assert(closed);
            (void)closed;
        }
        for (auto& replacement : actionQueueReplacementScratch) {
            if (!replacement.nativeReplacement.Valid()) continue;
            const auto* holder = OriginalQueueReplacementFor(replacement);
            if (holder != nullptr && holder->phase >= OriginalQueueReplacementHolder::Phase::Selected) continue;
            const bool returned = CancelOriginalQueueReplacementBody(replacement);
            assert(returned);
            (void)returned;
        }
        for (auto& resource : authoredStopResources) {
            resource.nativeControlCommit.Release();
            resource.nativeStopLaneOwner.Return();
        }
        queueInstances.EndOriginalConditionalAssociationsAfterQuiescence();
        for (auto& bank : queueProgramBanks) bank.continuationLane.ReturnAfterQuiescence();
        if (originalQueueReplacementHolders.data() != nullptr)
            for (std::uint32_t i = 0; i < originalQueueReplacementHolders.size(); ++i) {
                auto& holder = originalQueueReplacementHolders[i];
                if (holder.phase == OriginalQueueReplacementHolder::Phase::Empty) continue;
                holder.sourceArmCommit.Release();
                holder.sourceVoiceCommit.Release();
                for (auto& proof : holder.sourceEffectCommits) proof.Release();
                holder.calendar.ReturnOwnStorageAfterQuiescence();
                const bool returned = holder.conditionalReader.Release();
                assert(returned);
                (void)returned;
                holder.selected.Release();
                holder.domain.ReturnOwnStorageAfterQuiescence();
                holder.clock.Release();
                holder.current.Release();
                holder.serviceRetention.Return();
                holder.bankRetention.Release();
                holder.oldBankRetention.Release();
            }
        // Joined shutdown withdraws Engine associations before their native
        // baselines. Every foreign borrower must return before Engine death.
        for (auto& current : originalSingleCurrentHolders) {
            current.Dispose();
            current.retirementHold.Release();
        }
        // Callback and workers are joined; these Capturing groups have no audio uses.
        // Joined owner teardown ends this observer ledger without claiming native disposal.
        for (auto& original : pendingVariableQueueNativeOrigins) original = {};
        for (auto& original : pendingVariableFutureNodes) original = {};
        for (auto& group : preparedPendingVariableGroups) group.futureNodeCount = 0;
        for (auto& group : acceptedPendingVariableGroups) group.futureNodeCount = 0;
        for (auto& group : preparedPendingVariableGroups) group.queueNativeOriginCount = 0;
        for (auto& group : acceptedPendingVariableGroups) group.queueNativeOriginCount = 0;
        for (auto& group : preparedPendingVariableGroups) {
            const bool closed = group.Close(*this);
            assert(closed);
            (void)closed;
        }
        for (auto& group : acceptedPendingVariableGroups) {
            const bool closed = group.Close(*this);
            assert(closed);
            (void)closed;
        }
        for (auto& source : pendingVariableLeafSources) {
            assert(source.group == nullptr);
            ReturnPendingVariableLeafSource(source);
        }
        for (auto& storage : actionPlayStorage) {
            const bool cancelled = storage.initialSelectionBatch.Cancel();
            assert(cancelled);
            (void)cancelled;
        }
        for (auto& ordinary : pendingOrdinarySelections) {
            const bool closed = ClosePendingOrdinarySelection(ordinary);
            assert(closed);
            (void)closed;
        }
        for (auto& original : originalPlayableLifetimeHolders) original.Release();
        // Actual callback/worker join has completed. Return structural own
        // factory outputs before their Source/Body receivers are shut down.
        for (auto& proof : originalSingleOccurrenceCommitProofs) proof.Release();
        compositeCompletionOwners.ReleaseOriginalPreparedLeafCommitProofsForQuiescentOwnerTeardown();
        for (auto& original : originalQueueSourcePreparationHolders) original.ReturnAfterJoinedTerm();
        for (auto& original : originalSinglePreparationHolders) original.Return();
        for (auto& original : originalSingleOccurrenceBaselines) original.Release();
        for (auto& original : originalSingleDecisionBaselines) original.Release();
        // Actual callback/worker join permits own structural Return, never a
        // fabricated leaf completion or a forced foreign Return.
        for (auto& original : originalCompositeCompletionPreparations) original.original_.Release();
        for (auto& original : originalOccurrencePreparations) original.original_.Release();
        // Pool backing outlives the genuine conditional group-loan returns.
        queueInstances.EndOriginalConditionalsForQuiescentOwnerTeardown();
        if (originalQueueConditionalHolders.data() != nullptr)
            for (std::uint32_t index = 0; index < originalQueueConditionalHolders.size(); ++index)
                assert(queueInstances.OriginalConditionalOwnerEmpty(
                    originalQueueConditionalHolders[index].baseline,
                    originalQueueConditionalHolders[index].clock));
        // These are storage baselines, not fabricated audio completion fences.
        // Every external loan must have returned before the owning Engine dies.
        for (std::size_t index = 0; index < originalQueueGroupHolders.size(); ++index)
            originalQueueGroupHolders[index].Release();
        for (auto& holder : originalQueueLifetimeHolders) holder.Release();
        for (auto& holder : originalQueueBankHolders) {
            holder.baseline.Release();
            holder.reservation.Release();
        }
        // Join every offline/real-time worker before Source registry destruction.
        // Disposed Source records can still have a physical installed Slot loan.
        voices.ShutdownOriginalSourceBodiesAfterQuiescence();
        variables.ShutdownReadCutsAfterQuiescence();
        originalSourceSubmissions.ShutdownAfterQuiescence();
        mixer.FinishControlHeaderAckAfterQuiescence();
        for (auto& resource : authoredStopResources) {
            auto& range = resource.originalControlHeader;
            if (range.State() == CommandQueue::OriginalRangeState::Published) {
                const bool completed = commands.CompleteOriginalControlHeaderRange(
                    range, CommandQueue::OriginalControlDisposition::QuiescedDiscard);
                assert(completed);
                (void)completed;
            }
            // Workers and deferred native ACK have already joined/returned.
            // External origins must return before their owning Engine dies.
            const bool released = resource.ReturnOriginalControlHeaderAfterDisposition();
            assert(released);
            (void)released;
        }
        // Joined Source/Body/control exit precedes our service storage Return.
        for (auto& bank : queueProgramBanks) {
            const bool returned = ReleaseQueueProgramMembers(bank);
            assert(returned);
            (void)returned;
            bank.scheduleStorageLoan.Reset();
        }
        ReturnOriginalPlayableServiceStorageAfterQuiescence();
        ShutdownOriginalPackIdentityLeasesAfterQuiescence();
        // Executors borrow scratch tokens, whose sparse backing is destroyed first.
        for (auto& storage : actionPlayStorage) storage.playableTreeExecutor.reset();
    }


    static OriginalDecisionLifetimePin* ResolveOriginalSingleDecisionBaseline(
        void* context, DecisionTable& table, DecisionToken token) noexcept {
        auto* engine = static_cast<Engine*>(context);
        if (engine == nullptr || &table != &engine->decisions || !token.Valid() ||
            token.slot >= engine->originalSingleDecisionBaselines.size()) return nullptr;
        auto& actual = engine->originalSingleDecisionBaselines[token.slot];
        return table.IsOriginalDecisionLifetimePinFor(actual, token) ? &actual : nullptr;
    }
    static OccurrenceStatePool::OriginalSingleCompletionOwnerPin* ResolveOriginalSingleOccurrenceBaseline(
        void* context, OccurrenceStatePool& pool, const OccurrenceToken& token) noexcept {
        auto* engine = static_cast<Engine*>(context);
        if (engine == nullptr || &pool != &engine->timelineOccurrences || token.generation == 0 ||
            token.slot >= engine->originalSingleOccurrenceBaselines.size()) return nullptr;
        auto& actual = engine->originalSingleOccurrenceBaselines[token.slot];
        return pool.IsOriginalSingleCompletionOwnerPinFor(actual, token) ? &actual : nullptr;
    }
    static OccurrenceStatePool::PreparedCompositeCompletionCommitProof* ResolveOriginalCompositeOccurrenceCommit(
        void* context, OccurrenceStatePool& pool, const OccurrenceToken& token) noexcept {
        auto* engine = static_cast<Engine*>(context);
        if (engine == nullptr || &pool != &engine->timelineOccurrences) return nullptr;
        return engine->compositeCompletionOwners.ResolveOriginalPreparedLeafCommit(pool, token);
    }
    static OccurrenceStatePool::PreparedSingleCompletionCommitProof* ResolveOriginalSingleOccurrenceCommit(
        void* context, OccurrenceStatePool& pool, const OccurrenceToken& token) noexcept {
        auto* engine = static_cast<Engine*>(context);
        if (engine == nullptr || &pool != &engine->timelineOccurrences || token.generation == 0 ||
            token.slot >= engine->originalSingleOccurrenceCommitProofs.size()) return nullptr;
        auto& actual = engine->originalSingleOccurrenceCommitProofs[token.slot];
        return pool.IsPreparedSingleCompletionCommitFor(actual, token) ? &actual : nullptr;
    }
    bool PrepareOriginalDecisionReservation(const DecisionReserveRequest& request,
        PreparedDecisionReservation* prepared) noexcept {
        OriginalDecisionLifetimePin actual;
        if (!decisions.PrepareReservation(request, prepared, &actual)) return false;
        const auto token = prepared->Token();
        // A free native row cannot coexist with a valid prior original cap.
        // The successful native factory already checked output and marker limits.
        assert(token.Valid() && token.slot < originalSingleDecisionBaselines.size() &&
            !originalSingleDecisionBaselines[token.slot].Valid());
        originalSingleDecisionBaselines[token.slot] = std::move(actual);
        return true;
    }
    bool PrepareOriginalQueueDecisionReservation(const DecisionReserveRequest& request,
        PreparedDecisionReservation* prepared) noexcept {
        if (!PrepareOriginalDecisionReservation(request, prepared)) return false;
        if (originalQueueDecisionPreparationHookForTest != nullptr)
            originalQueueDecisionPreparationHookForTest(*this, *prepared,
                originalSingleDecisionBaselines[prepared->Token().slot], originalQueueDecisionPreparationContextForTest);
        return true;
    }
    bool PinOriginalSingleCompletionOwner(const OccurrenceToken& token,
        const CompletionOwnerRef& owner) noexcept {
        if (token.generation == 0 || token.slot >= originalSingleOccurrenceBaselines.size()) return false;
        auto& actual = originalSingleOccurrenceBaselines[token.slot];
        if (actual.Valid()) return false;
        return timelineOccurrences.TryPinCompletionOwner(token, owner, &actual);
    }

    class OriginalSinglePreparationHolder {
    public:
        OriginalSinglePreparationHolder() noexcept = default;
        OriginalSinglePreparationHolder(const OriginalSinglePreparationHolder&) = delete;
        OriginalSinglePreparationHolder& operator=(const OriginalSinglePreparationHolder&) = delete;
        OriginalSinglePreparationHolder(OriginalSinglePreparationHolder&&) = delete;
        OriginalSinglePreparationHolder& operator=(OriginalSinglePreparationHolder&&) = delete;
        const PreparedSingleCurrentOriginBundle::SourcePin& OriginalSource() const noexcept { return source_; }
        const PreparedSingleCurrentOriginBundle::BodyPin& OriginalBody() const noexcept { return body_; }
        const PreparedSingleCurrentOriginBundle::LedgerPin& OriginalLedger() const noexcept { return ledgerOriginal_; }
        const PreparedSingleCurrentOriginBundle::LedgerProof& OriginalLedgerPreparation() const noexcept { return ledgerCommit_; }
        TimelineArmToken OriginalArm() const noexcept { return arm_; }
        VnVoiceHandle OriginalSourceIdentity() const noexcept { return sourceIdentity_; }
    private:
        friend struct Engine;
        Engine* receiver_ = nullptr;
        TimelineArmToken arm_{};
        VnVoiceHandle sourceIdentity_ = 0;
        VoiceReleaseObserver physicalRelease_{};
        PreparedSingleCurrentOriginBundle::SourcePin source_;
        PreparedSingleCurrentOriginBundle::BodyPin body_;
        PreparedSingleCurrentOriginBundle::LedgerPin ledgerOriginal_;
        PreparedSingleCurrentOriginBundle::LedgerProof ledgerCommit_;
        OccurrenceToken originalOccurrence_{};
        CompletionMutationBinding originalMutation_{};
        VoiceLedger::PreparedCommitInput originalLedgerInput_{};
        bool singleIssued_ = false;
        bool returned_ = false;
        void Return() noexcept {
            ledgerCommit_.Release();
            ledgerOriginal_.Release();
            body_.Release();
            source_.Release();
            returned_ = true;
        }
    };
    static bool SameOriginalSingleArm(TimelineArmToken a, TimelineArmToken b) noexcept {
        return a.Valid() && a.slot == b.slot && a.generation == b.generation &&
            a.renderSlot == b.renderSlot && a.renderGeneration == b.renderGeneration;
    }
    bool PrepareOriginalSourceRowBacking(std::uint32_t count, bool fullSourceCommandBacking) noexcept {
        if (count > timelineArms.Capacity()) return false;
        // Physical Arm allowances include every dependent column, without issuing an owner.
        for (; originalSourceBackingPrefix < count; ++originalSourceBackingPrefix)
            if (!originalSourceSubmissions.PrepareRecordBacking(originalSourceBackingPrefix) ||
                !originalSinglePreparationHolders.Emplace(originalSourceBackingPrefix) ||
                !originalQueueSourcePreparationHolders.Emplace(originalSourceBackingPrefix)) return false;
        // Full preparation covers old holes as well as the new physical suffix.
        if (fullSourceCommandBacking)
            for (std::uint32_t slot = 0; slot < count; ++slot)
                if (!originalSourceSubmissions.PrepareSlotBacking(slot)) return false;
        return true;
    }
    OriginalSinglePreparationHolder* OriginalSinglePreparationForArm(TimelineArmToken arm) noexcept {
        if (!arm.Valid() || !originalSinglePreparationHolders.IsConstructed(arm.slot)) return nullptr;
        auto& holder = originalSinglePreparationHolders[arm.slot];
        return holder.receiver_ == this && !holder.returned_ && SameOriginalSingleArm(holder.arm_, arm) &&
            originalSourceSubmissions.IsOriginalSourceLifetimePinFor(holder.source_, arm) &&
            voices.IsOriginalSourceBodyLifetimePinFor(holder.body_, arm, holder.sourceIdentity_)
                ? &holder : nullptr;
    }
    // Invoked by the actual original Capture factory, before any published head.
    // This method never issues an origin from a token or a later Source lookup.
    bool CaptureOriginalSingleSourcePreparation(const CommandRecord& originalHeader,
        const TimelineArmTable::PreparedCommitProof& originalArm,
        const VoicePool::PreparedCommitProof& originalVoice,
        std::span<const EffectPoolBank::PreparedCommitProof> originalEffects) noexcept {
        const auto arm = originalArm.OriginalPreparedToken();
        const auto render = originalVoice.OriginalPreparedHandle();
        if (originalArm.Owner() != &timelineArms || originalVoice.Owner() != &voices ||
            !arm.Valid() || arm.slot >= originalSinglePreparationHolders.size() ||
            render == 0 || render != ((static_cast<std::uint64_t>(arm.renderGeneration) << 32) | arm.renderSlot)) return false;
        if (!originalSinglePreparationHolders.Emplace(arm.slot)) return false;
        auto& holder = originalSinglePreparationHolders[arm.slot];
        if (holder.source_.Valid() || holder.body_.Valid() ||
            (holder.receiver_ != nullptr && !holder.returned_)) return false;
        VoiceReleaseObserver physicalRelease{};
        if (!voices.CaptureReleaseObserver(render, &physicalRelease)) return false;
        if (!originalSourceSubmissions.CapturePreparedSource(originalHeader, originalArm,
                originalVoice, originalEffects, &holder.source_, &holder.body_)) return false;
        // Both outputs and their observer now belong to this genuine factory.
        // There is no fallible operation after issuance and before registration.
        holder.receiver_ = this;
        originalSingleSourcePreparationSeen = true;
        holder.arm_ = arm;
        holder.sourceIdentity_ = originalHeader.payload.play.sourceIdentity;
        holder.physicalRelease_ = physicalRelease;
        holder.returned_ = false;
        holder.singleIssued_ = false;
        holder.originalOccurrence_ = {};
        holder.originalMutation_ = {};
        holder.originalLedgerInput_ = {};
        return true;
    }
    bool BorrowOriginalSingleSourcePreparation(const OriginalSinglePreparationHolder& original,
        PreparedSingleCurrentOriginBundle::SourcePin* source,
        PreparedSingleCurrentOriginBundle::BodyPin* body) noexcept {
        if (source == nullptr || body == nullptr || source->Valid() || body->Valid() ||
            OriginalSinglePreparationForArm(original.arm_) != &original) return false;
        PreparedSingleCurrentOriginBundle::SourcePin sourceLoan;
        PreparedSingleCurrentOriginBundle::BodyPin bodyLoan;
        if (!original.source_.Borrow(&sourceLoan) || !original.body_.Borrow(&bodyLoan)) return false;
        *source = std::move(sourceLoan);
        *body = std::move(bodyLoan);
        return true;
    }
    using OriginalQueueSourcePreparationHolder = EngineOriginalQueueSourcePreparation;
    OriginalQueueSourcePreparationHolder* OriginalQueueSourcePreparationForArm(TimelineArmToken arm) noexcept {
        if (!arm.Valid() || !originalQueueSourcePreparationHolders.IsConstructed(arm.slot)) return nullptr;
        auto& holder = originalQueueSourcePreparationHolders[arm.slot];
        return holder.receiver_ == this && !holder.returned_ && SameOriginalSingleArm(holder.arm_, arm) &&
            originalSourceSubmissions.IsOriginalSourceLifetimePinFor(holder.source_, arm) &&
            voices.IsOriginalSourceBodyLifetimePinFor(holder.body_, arm, holder.sourceIdentity_)
                ? &holder : nullptr;
    }
    // Queue Source staging is independent of Single completion authority.
    // The only issuer is the same actual original Source capture factory.
    bool CaptureOriginalQueueSourcePreparation(const CommandRecord& originalHeader,
        const TimelineArmTable::PreparedCommitProof& originalArm,
        const VoicePool::PreparedCommitProof& originalVoice,
        std::span<const EffectPoolBank::PreparedCommitProof> originalEffects,
        bool retainOriginalSingle, EngineOriginalQueueSourcePreparation** issued = nullptr) noexcept {
        const auto arm = originalArm.OriginalPreparedToken();
        const auto render = originalVoice.OriginalPreparedHandle();
        if ((issued != nullptr && *issued != nullptr) ||
            originalArm.Owner() != &timelineArms || originalVoice.Owner() != &voices ||
            !arm.Valid() || arm.slot >= originalQueueSourcePreparationHolders.size() ||
            render == 0 || render != ((static_cast<std::uint64_t>(arm.renderGeneration) << 32) | arm.renderSlot)) return false;
        if (!originalQueueSourcePreparationHolders.Emplace(arm.slot)) return false;
        auto& holder = originalQueueSourcePreparationHolders[arm.slot];
        if (holder.source_.Valid() || holder.body_.Valid() || holder.compositeLedgerCommit_.Valid() ||
            holder.compositeLedgerOriginal_.Valid() ||
            (holder.receiver_ != nullptr && !holder.returned_)) return false;
        VoiceReleaseObserver physicalRelease{};
        if (!voices.CaptureReleaseObserver(render, &physicalRelease)) return false;
        if (retainOriginalSingle) {
            if (!CaptureOriginalSingleSourcePreparation(originalHeader, originalArm, originalVoice, originalEffects)) return false;
            auto* single = OriginalSinglePreparationForArm(arm);
            if (single == nullptr || !BorrowOriginalSingleSourcePreparation(*single, &holder.source_, &holder.body_)) return false;
        } else if (!originalSourceSubmissions.CapturePreparedSource(originalHeader, originalArm,
                originalVoice, originalEffects, &holder.source_, &holder.body_)) return false;
        holder.receiver_ = this;
        originalQueueSourcePreparationSeen = true;
        holder.arm_ = arm;
        holder.sourceIdentity_ = originalHeader.payload.play.sourceIdentity;
        holder.physicalRelease_ = physicalRelease;
        holder.returned_ = false;
        holder.compositeLedgerInput_ = {};
        if (issued != nullptr) *issued = &holder;
        if (originalQueueSourcePreparationHookForTest != nullptr)
            originalQueueSourcePreparationHookForTest(*this, holder, originalQueueSourcePreparationContextForTest);
        return true;
    }
    bool ReturnOriginalQueueSourcePreparationAfterPhysicalRelease(TimelineArmToken arm) noexcept {
        if (!arm.Valid() || !originalQueueSourcePreparationHolders.IsConstructed(arm.slot)) return true;
        auto& holder = originalQueueSourcePreparationHolders[arm.slot];
        if (holder.receiver_ == nullptr || !SameOriginalSingleArm(holder.arm_, arm)) return true;
        if (holder.receiver_ != this) return false;
        if (holder.returned_) return true;
        if (!holder.physicalRelease_.Valid() || !voices.ReleaseObserved(holder.physicalRelease_) ||
            OriginalQueueSourcePreparationForArm(arm) != &holder) return false;
        holder.Return();
        return true;
    }
    void ServiceOriginalQueueSourcePreparations() noexcept {
        if (!originalQueueSourcePreparationSeen) return;
        for (auto& holder : originalQueueSourcePreparationHolders) {
            if (holder.receiver_ == this && !holder.returned_)
                (void)ReturnOriginalQueueSourcePreparationAfterPhysicalRelease(holder.arm_);
            if (holder.receiver_ == this && holder.compositeLedgerOriginal_.Valid())
                (void)ReturnOriginalCompositeLedgerAfterNativeEnd(&holder);
        }
    }
    bool MatchesOriginalCompositeLedgerPreparation(const EnginePreparedTimelineVoiceStart& voice) const noexcept {
        const auto* holder = voice.originalQueueSource;
        if (holder == nullptr || !voice.arm.Valid() || !originalQueueSourcePreparationHolders.IsConstructed(voice.arm.slot) ||
            holder != &originalQueueSourcePreparationHolders[voice.arm.slot] || holder->receiver_ != this ||
            !SameOriginalSingleArm(holder->arm_, voice.arm) || holder->sourceIdentity_ != voice.renderHandle) return false;
        const auto& original = holder->compositeLedgerInput_;
        return voice.ledgerSlot == original.identity.slot && voice.renderHandle == original.renderHandle &&
            voice.ledgerPreparation.identity.slot == original.identity.slot &&
            voice.ledgerPreparation.identity.generation == original.identity.generation &&
            voice.ledgerPreparation.parent.previewHandle == original.parent.previewHandle &&
            voice.ledgerPreparation.parent.internalToken == original.parent.internalToken &&
            voice.ledgerPreparation.renderHandle == original.renderHandle &&
            voice.ledgerPreparation.priority == original.priority &&
            SameCompletionMutationBinding(voice.ledgerPreparation.completionMutation, original.completionMutation) &&
            ledger.IsOriginalCompositeCompletionMutationOwnerFor(holder->compositeLedgerOriginal_, original.identity,
                original.parent, original.renderHandle, original.completionMutation);
    }
    bool HasOriginalCompositeLedgerCommit(const EnginePreparedTimelineVoiceStart& voice) const noexcept {
        return MatchesOriginalCompositeLedgerPreparation(voice) &&
            ledger.MatchesPreparedPlayableCommit(voice.originalQueueSource->compositeLedgerCommit_, voice.ledgerPreparation);
    }
    bool BindOriginalCompositeLedgerPreparation(EnginePreparedTimelineVoiceStart& voice,
        const EngineOriginalCompositeCompletionPreparation* composite, std::uint32_t leafIndex) noexcept {
        if (composite == nullptr || composite->receiver_ != this ||
            !compositeCompletionOwners.IsOriginalLifetimePinFor(composite->original_, composite->token_)) return false;
        KnownScheduleLeafV1 leaf{};
        if (!compositeCompletionOwners.CapturePreparedLeaf(composite->token_, leafIndex, &leaf)) return false;
        const CompletionMutationBinding mutation{leaf.occurrence, leaf.predicate.owner};
        if (voice.originalQueueSource == nullptr)
            return ledger.BindPreparedCompositeCompletionMutation(&voice.ledgerPreparation, mutation);
        auto* holder = voice.originalQueueSource;
        if (OriginalQueueSourcePreparationForArm(voice.arm) != holder || !voice.prepared ||
            holder->sourceIdentity_ != voice.renderHandle || holder->compositeLedgerCommit_.Valid() ||
            holder->compositeLedgerOriginal_.Valid() || !voice.completionProgressBound ||
            voice.progressOccurrence.slot != leaf.occurrence.slot ||
            voice.progressOccurrence.generation != leaf.occurrence.generation ||
            voice.voicePreparation.completionProgress != timelineOccurrences.CompletionProgressOf(leaf.occurrence) ||
            voice.voicePreparation.completionRenderGeneration != leaf.renderGeneration ||
            voice.ledgerPreparation.identity.slot != voice.ledgerSlot ||
            voice.ledgerPreparation.renderHandle != voice.renderHandle) return false;
        if (!ledger.BindPreparedCompositeCompletionMutation(&voice.ledgerPreparation, mutation)) return false;
        VoiceLedger::PreparedCommitProof commit;
        VoiceLedger::OriginalCompositeCompletionMutationOwner original;
        if (!ledger.ProvePreparedPlayableCommit(voice.ledgerPreparation, &commit) ||
            !ledger.IssueOriginalCompositeCompletionMutationOwner(commit, &original)) {
            commit.Release();
            const bool cleared = ledger.ClearCompletionMutationExact(voice.ledgerSlot, mutation);
            assert(cleared);
            if (!cleared) runtimeRecreateRequired = true;
            else voice.ledgerPreparation.completionMutation = {};
            return false;
        }
        holder->compositeLedgerInput_ = voice.ledgerPreparation;
        holder->compositeLedgerCommit_ = std::move(commit);
        holder->compositeLedgerOriginal_ = std::move(original);
        if (originalCompositeLedgerPreparationHookForTest != nullptr)
            originalCompositeLedgerPreparationHookForTest(*this, *holder, originalCompositeLedgerPreparationContextForTest);
        return true;
    }
    bool PrepareOriginalCompositeLeafCompletionCommits(
        const EngineOriginalCompositeCompletionPreparation* original) noexcept {
        if (original == nullptr || original->receiver_ != this || !original->token_.Valid() ||
            original->token_.slot >= originalCompositeCompletionPreparations.size() ||
            original != &originalCompositeCompletionPreparations[original->token_.slot]) return false;
        if (!compositeCompletionOwners.PrepareOriginalLeafCompletionCommits(original->original_, timelineOccurrences))
            return false;
        if (originalCompositeLeafPreparationHookForTest != nullptr)
            originalCompositeLeafPreparationHookForTest(*this, *original, originalCompositeLeafPreparationContextForTest);
        return true;
    }
    bool ReleaseOriginalCompositeLeafPreparationProofs(CompositeCompletionOwnerToken token) noexcept {
        if (!token.Valid()) return true;
        auto* original = ResolveOriginalCompositeBaseline(this, compositeCompletionOwners, token);
        if (original == nullptr) return false;
        CompositeCompletionOwnerInfo actual{};
        if (!compositeCompletionOwners.CapturePrepared(token, &actual))
            return compositeCompletionOwners.IsOriginalLifetimePinned(*original);
        if (actual.leafCount > kMaxKnownScheduleLeaves) return false;
        std::array<OccurrenceStatePool::PreparedCompositeCompletionCommitProof*, kMaxKnownScheduleLeaves> held{};
        for (std::uint32_t index = 0; index < actual.leafCount; ++index) {
            KnownScheduleLeafV1 leaf{};
            if (!compositeCompletionOwners.CaptureOriginalLeaf(*original, index, &leaf)) return false;
            held[index] = compositeCompletionOwners.ResolveOriginalPreparedLeafCommit(timelineOccurrences, leaf.occurrence);
            if (held[index] == nullptr) continue;
            CompletionOwnerRef owner{};
            if (!timelineOccurrences.IsPreparedCompositeCompletionCommitFor(*held[index], leaf.occurrence) ||
                !timelineOccurrences.CaptureCompletionOwner(leaf.occurrence, &owner) ||
                !SameCompletionOwner(owner, leaf.predicate.owner)) return false;
        }
        // Native cancellation returns only its short gates before Source unbind.
        // Exact owner rollback and storage retirement still require native End.
        for (auto* proof : held) if (proof != nullptr) proof->Release();
        return true;
    }
    bool ConsumeOriginalCompositeLedgerCommit(const EnginePreparedTimelineVoiceStart& voice) noexcept {
        if (!HasOriginalCompositeLedgerCommit(voice)) return false;
        VoiceLedger::ConsumePreparedPlayableCommit(std::move(voice.originalQueueSource->compositeLedgerCommit_));
        return true;
    }
    bool ReleaseOriginalQueuePreparationProofs(const EnginePreparedTimelineVoiceStart& voice) noexcept {
        if (voice.originalQueueSource != nullptr) {
            if (!voice.arm.Valid() || !originalQueueSourcePreparationHolders.IsConstructed(voice.arm.slot) ||
                voice.originalQueueSource != &originalQueueSourcePreparationHolders[voice.arm.slot] ||
                voice.originalQueueSource->receiver_ != this ||
                !SameOriginalSingleArm(voice.originalQueueSource->arm_, voice.arm)) return false;
            if (voice.originalQueueSource->compositeLedgerCommit_.Valid()) {
                if (!HasOriginalCompositeLedgerCommit(voice)) return false;
                voice.originalQueueSource->compositeLedgerCommit_.Release();
            }
        }
        return ReleaseOriginalSinglePreparationProofs(voice);
    }
    bool HasOriginalQueueLedgerCommit(const EnginePreparedTimelineVoiceStart& voice) noexcept {
        return HasOriginalSingleLedgerCommit(voice) || HasOriginalCompositeLedgerCommit(voice);
    }
    bool ReturnOriginalCompositeLedgerAfterNativeEnd(EngineOriginalQueueSourcePreparation* holder) noexcept {
        if (holder == nullptr) return true;
        if (!holder->arm_.Valid() || !originalQueueSourcePreparationHolders.IsConstructed(holder->arm_.slot) ||
            holder != &originalQueueSourcePreparationHolders[holder->arm_.slot] || holder->receiver_ != this) return false;
        if (!holder->compositeLedgerOriginal_.Valid()) return true;
        if (holder->compositeLedgerCommit_.Valid() ||
            !ledger.HasOriginalCompositeCompletionNativeEnd(holder->compositeLedgerOriginal_)) return false;
        holder->compositeLedgerOriginal_.Release();
        return true;
    }
    const OriginalDecisionLifetimePin* OriginalSingleDecisionPreparation(
        const PreparedDecisionReservation& original) const noexcept {
        const auto token = original.Token();
        if (!token.Valid() || token.slot >= originalSingleDecisionBaselines.size()) return nullptr;
        const auto& actual = originalSingleDecisionBaselines[token.slot];
        return decisions.MatchesOriginalPreparedDecision(actual, original) ? &actual : nullptr;
    }
    bool ProveOriginalSingleCompletionPreparation(const EnginePreparedTimelineVoiceStart& voice) noexcept {
        auto* holder = OriginalSinglePreparationForArm(voice.arm);
        const auto token = voice.progressOccurrence;
        const auto& mutation = voice.ledgerPreparation.completionMutation;
        if (holder == nullptr || holder->singleIssued_ || holder->ledgerCommit_.Valid() ||
            holder->ledgerOriginal_.Valid() || !voice.prepared || !voice.completionProgressBound ||
            token.generation == 0 || token.slot >= originalSingleOccurrenceBaselines.size() ||
            token.slot >= originalSingleOccurrenceCommitProofs.size() ||
            originalSingleOccurrenceCommitProofs[token.slot].Valid() ||
            mutation.owner.kind != CompletionOwnerKind::SingleQueue || !mutation.owner.singleDecision.Valid() ||
            mutation.owner.composite.Valid() || mutation.occurrence.slot != token.slot ||
            mutation.occurrence.generation != token.generation ||
            voice.ledgerPreparation.identity.slot != voice.ledgerSlot ||
            voice.ledgerPreparation.renderHandle != voice.renderHandle ||
            voice.voicePreparation.renderHandle != voice.renderHandle ||
            voice.renderHandle != ((static_cast<std::uint64_t>(voice.arm.renderGeneration) << 32) | voice.arm.renderSlot)) return false;
        const auto* source = originalSourceSubmissions.Find(voice.arm);
        if (source == nullptr || (source->State() != OriginalSourceSubmissionRegistry::Phase::Preparing &&
            source->State() != OriginalSourceSubmissionRegistry::Phase::QueuePrepared)) return false;
        const auto& originalOccurrence = originalSingleOccurrenceBaselines[token.slot];
        const auto decisionToken = mutation.owner.singleDecision;
        if (decisionToken.slot >= originalSingleDecisionBaselines.size() ||
            !decisions.IsOriginalDecisionLifetimePinFor(originalSingleDecisionBaselines[decisionToken.slot], decisionToken) ||
            !timelineOccurrences.IsOriginalSingleCompletionOwnerPinFor(originalOccurrence, token)) return false;
        const auto epoch = timelineOccurrences.ScheduleEpochOf(token);
        PreparedSingleCurrentOriginBundle::OccurrenceProof occurrenceProof;
        PreparedSingleCurrentOriginBundle::LedgerProof ledgerProof;
        PreparedSingleCurrentOriginBundle::LedgerPin ledgerOriginal;
        if (!timelineOccurrences.ProvePreparedSingleCompletionCommit(originalOccurrence, mutation.owner,
                voice.voicePreparation.completionProgress, voice.voicePreparation.completionRenderGeneration,
                epoch, &occurrenceProof) ||
            !ledger.ProvePreparedSingleCompletionCommit(voice.ledgerPreparation, &ledgerProof) ||
            !ledger.IssueOriginalCompletionMutationOwner(ledgerProof, &ledgerOriginal)) return false;
        // All fallible factories finished. Each output moves to fixed owned
        // storage before any Source command head can become visible.
        holder->originalOccurrence_ = token;
        holder->originalMutation_ = ledgerProof.OriginalInput().completionMutation;
        holder->originalLedgerInput_ = ledgerProof.OriginalInput();
        originalSingleOccurrenceCommitProofs[token.slot] = std::move(occurrenceProof);
        holder->ledgerCommit_ = std::move(ledgerProof);
        holder->ledgerOriginal_ = std::move(ledgerOriginal);
        holder->singleIssued_ = true;
        return true;
    }
    bool MatchesOriginalSingleLedgerPreparation(const OriginalSinglePreparationHolder& holder,
        const EnginePreparedTimelineVoiceStart& voice) const noexcept {
        const auto& logical = holder.originalLedgerInput_;
        return holder.receiver_ == this && !holder.returned_ && holder.singleIssued_ &&
            SameOriginalSingleArm(holder.arm_, voice.arm) &&
            voice.ledgerPreparation.identity.slot == logical.identity.slot &&
            voice.ledgerPreparation.identity.generation == logical.identity.generation &&
            voice.ledgerSlot == logical.identity.slot && voice.renderHandle == logical.renderHandle &&
            voice.ledgerPreparation.renderHandle == logical.renderHandle &&
            voice.ledgerPreparation.parent.previewHandle == logical.parent.previewHandle &&
            voice.ledgerPreparation.parent.internalToken == logical.parent.internalToken &&
            voice.ledgerPreparation.priority == logical.priority &&
            SameCompletionMutationBinding(voice.ledgerPreparation.completionMutation, logical.completionMutation) &&
            ledger.IsOriginalCompletionMutationOwnerFor(holder.ledgerOriginal_, logical.identity, logical.parent,
                logical.renderHandle, logical.completionMutation);
    }
    bool HasOriginalSinglePreparationAssociation(const EnginePreparedTimelineVoiceStart& voice) noexcept {
        auto* holder = OriginalSinglePreparationForArm(voice.arm);
        return holder != nullptr && MatchesOriginalSingleLedgerPreparation(*holder, voice);
    }
    bool HasOriginalSingleLedgerCommit(const EnginePreparedTimelineVoiceStart& voice) noexcept {
        auto* holder = OriginalSinglePreparationForArm(voice.arm);
        return holder != nullptr && MatchesOriginalSingleLedgerPreparation(*holder, voice) &&
            ledger.MatchesPreparedSingleCompletionCommit(holder->ledgerCommit_);
    }
    bool ConsumeOriginalSingleLedgerCommit(const EnginePreparedTimelineVoiceStart& voice) noexcept {
        auto* holder = OriginalSinglePreparationForArm(voice.arm);
        if (holder == nullptr || !MatchesOriginalSingleLedgerPreparation(*holder, voice) ||
            !ledger.MatchesPreparedSingleCompletionCommit(holder->ledgerCommit_)) return false;
        VoiceLedger::ConsumePreparedSingleCompletionCommit(std::move(holder->ledgerCommit_));
        return true;
    }
    bool ReleaseOriginalSinglePreparationProofs(const EnginePreparedTimelineVoiceStart& voice) noexcept {
        if (!voice.arm.Valid() || !originalSinglePreparationHolders.IsConstructed(voice.arm.slot)) return true;
        auto& holder = originalSinglePreparationHolders[voice.arm.slot];
        if (holder.receiver_ == nullptr || !SameOriginalSingleArm(holder.arm_, voice.arm)) return true;
        if (holder.receiver_ != this) return false;
        if (holder.returned_ || !holder.singleIssued_) return true;
        if (!MatchesOriginalSingleLedgerPreparation(holder, voice)) return false;
        const auto token = holder.originalOccurrence_;
        if (token.generation == 0 || token.slot >= originalSingleOccurrenceCommitProofs.size()) return false;
        auto& originalProof = originalSingleOccurrenceCommitProofs[token.slot];
        if (originalProof.Valid() && !timelineOccurrences.IsPreparedSingleCompletionCommitFor(originalProof, token)) return false;
        originalProof.Release();
        holder.ledgerCommit_.Release();
        return true;
    }
    bool OwnsImmediateEntryAttemptStorage(const EngineImmediateEntryAttempt& attempt) const noexcept {
        for (std::uint32_t i = 0; i < immediateEntryAttempts.size(); ++i)
            if (&immediateEntryAttempts[i] == &attempt) return true;
        return false;
    }
    bool ReserveOriginalImmediateEntryLane(EngineImmediateEntryAttempt& attempt,
        ImmediateLaneDemand demand, bool issueOriginal = false) noexcept {
        if (attempt.originalLaneLifetime.HandleObservation().Valid() ||
            attempt.originalLaneFactoryRoot != 0 || attempt.originalLaneFactoryAttemptGeneration != 0 ||
            (attempt.originalC0Factory != nullptr &&
             attempt.originalC0Factory->phase_ != OriginalInitialC0FactoryRecord::Phase::Empty)) return false;
        // Preserve every legacy no-output native precondition and result.
        if (!issueOriginal) return immediateActionLane.Reserve(attempt.root, demand, &attempt.lane);
        if (!OwnsImmediateEntryAttemptStorage(attempt) || !attempt.active ||
            attempt.root == 0 || attempt.generation == 0 || attempt.lane.Valid() ||
            attempt.gate.Observe(attempt.generation) != ImmediateEntryAttemptState::Preparing) return false;
        if (!immediateActionLane.Reserve(attempt.root, demand, &attempt.lane,
                &attempt.originalLaneLifetime)) return false;
        // These immutable receipts are installed only by successful same-call
        // issuance, before the attempt can be exposed or its owner can change.
        attempt.originalLaneFactoryRoot = attempt.root;
        attempt.originalLaneFactoryAttemptGeneration = attempt.generation;
        return true;
    }
    bool ReserveOriginalImmediateEntryLane(EngineImmediateEntryAttempt& attempt,
        std::uint32_t eventCredit, bool issueOriginal = false) noexcept {
        return ReserveOriginalImmediateEntryLane(attempt,
            {eventCredit, eventCredit, eventCredit, eventCredit}, issueOriginal);
    }
    bool BorrowOriginalImmediateEntryLane(const EngineImmediateEntryAttempt& attempt,
        OriginalImmediateLaneAttemptLifetimePin* out) const noexcept {
        if (!OwnsImmediateEntryAttemptStorage(attempt) || !attempt.active ||
            attempt.root == 0 || attempt.generation == 0 ||
            attempt.originalLaneFactoryRoot != attempt.root ||
            attempt.originalLaneFactoryAttemptGeneration != attempt.generation ||
            !immediateActionLane.IsOriginalAttemptLifetimePinFor(
                attempt.originalLaneLifetime, attempt.lane, attempt.root)) return false;
        return attempt.originalLaneLifetime.Borrow(out);
    }
    bool RetireOriginalImmediateEntryLane(EngineImmediateEntryAttempt& attempt,
        std::uint64_t completedAudioSequence) noexcept {
        if (!attempt.originalLaneLifetime.HandleObservation().Valid()) {
            if (attempt.originalLaneFactoryRoot != 0 || attempt.originalLaneFactoryAttemptGeneration != 0 ||
                (attempt.originalC0Factory != nullptr &&
                 attempt.originalC0Factory->phase_ != OriginalInitialC0FactoryRecord::Phase::Empty)) return false;
            return immediateActionLane.Retire(attempt.lane, completedAudioSequence);
        }
        if (!OwnsImmediateEntryAttemptStorage(attempt) || !attempt.active ||
            attempt.root == 0 || attempt.generation == 0 || !attempt.lane.Valid() ||
            attempt.originalLaneFactoryRoot != attempt.root ||
            attempt.originalLaneFactoryAttemptGeneration != attempt.generation) return false;
        // Never downgrade a stale or foreign original to the legacy no-output
        // path. Native Retire preserves this baseline on every false return.
        if (!immediateActionLane.IsOriginalAttemptLifetimePinFor(
                attempt.originalLaneLifetime, attempt.lane, attempt.root)) return false;
        // A consumed C1 no longer needs this producer's replacement association.
        // Return it before Lane retirement, which in turn lets Queue service end
        // the original conditional. External readers retain their own loans.
        if (auto* original = OriginalQueueConditionalForAttempt(attempt))
            (void)original->projection.ReturnAfterApplied(
                immediateActionLane, attempt.lane, decisions, completedAudioSequence);
        if (!immediateActionLane.Retire(attempt.lane, completedAudioSequence,
                &attempt.originalLaneLifetime)) return false;
        attempt.originalLaneFactoryRoot = 0;
        attempt.originalLaneFactoryAttemptGeneration = 0;
        if (attempt.originalC0Factory != nullptr)
            attempt.originalC0Factory->ClearCertificateAfterNativeDisposition();
        return true;
    }
    bool HasInitialSoundQueueCohort(const EngineImmediateEntryAttempt& attempt) const noexcept {
        const auto& receipt = attempt.soundQueueCohort;
        if (receipt.receiver_ != this || !attempt.active || attempt.root == 0 ||
            receipt.root_ != attempt.root || receipt.generation_ != attempt.generation ||
            attempt.initialService != receipt.sound_.service ||
            attempt.initialInvocationGeneration != receipt.sound_.invocationGeneration) return false;
        for (std::uint32_t i = 0; i < immediateEntryAttempts.size(); ++i)
            if (&immediateEntryAttempts[i] == &attempt) return true;
        return false;
    }
    bool MatchesInitialSoundQueueParticipant(const EngineImmediateEntryAttempt& attempt,
        const EnginePlayableServiceRow& row, bool queueOnly = false) const noexcept {
        if (!HasInitialSoundQueueCohort(attempt) || !row.InvocationValid() ||
            row.PendingImmediateCohortRoot() != attempt.root ||
            row.Invocation().actionFiring != attempt.root) return false;
        const auto matches = [&](const InitialSoundQueueCohortReceipt::Participant& original) noexcept {
            return row.identity == original.service &&
                row.invocationGeneration == original.invocationGeneration &&
                row.ownerInstance == original.owner.child && row.ownerEpoch == original.owner.childEpoch &&
                row.rootInstance == original.owner.child &&
                row.partition.slot == original.partition.slot &&
                row.partition.generation == original.partition.generation &&
                playablePartitions.Owns(row.partition, row.identity);
        };
        return matches(attempt.soundQueueCohort.queue_) ||
            (!queueOnly && matches(attempt.soundQueueCohort.sound_));
    }
    bool MatchesInitialSoundQueueFrontierOwner(const EngineImmediateEntryAttempt& attempt,
        const FrontierOwnerIdentity& owner, bool queueOwner) const noexcept {
        return HasInitialSoundQueueCohort(attempt) && owner ==
            (queueOwner ? attempt.soundQueueCohort.queue_.owner : attempt.soundQueueCohort.sound_.owner);
    }
    EnginePlayableServiceRow* ResolveInitialSoundQueueParticipant(
        const EngineImmediateEntryAttempt& attempt) noexcept {
        if (!HasInitialSoundQueueCohort(attempt)) return nullptr;
        const auto& original = attempt.soundQueueCohort.queue_;
        auto* row = ResolvePlayableService(original.service, original.owner.child, original.owner.childEpoch);
        return row != nullptr && MatchesInitialSoundQueueParticipant(attempt, *row, true) ? row : nullptr;
    }
    bool ResetInitialSoundQueueCohort(EngineImmediateEntryAttempt& attempt) noexcept {
        if (attempt.active) return false;
        for (std::uint32_t i = 0; i < immediateEntryAttempts.size(); ++i) {
            if (&immediateEntryAttempts[i] != &attempt) continue;
            attempt.soundQueueCohort.primaries_ = {};
            attempt.soundQueueCohort.sound_ = {};
            attempt.soundQueueCohort.queue_ = {};
            attempt.soundQueueCohort.receiver_ = nullptr;
            attempt.soundQueueCohort.root_ = 0;
            attempt.soundQueueCohort.generation_ = 0;
            return true;
        }
        return false;
    }
    bool RecordInitialSoundQueueCohortFromPreparedFactories(EngineImmediateEntryAttempt& attempt,
        EngineActionPlayScratch& sound, EngineActionPlayScratch& queue) noexcept {
        if (originalQueueConditionalHolders.size() == 0 || attempt.soundQueueCohort.receiver_ != nullptr ||
            !attempt.active || !attempt.lane.Valid() || attempt.root == 0 ||
            attempt.gate.Observe(attempt.generation) != ImmediateEntryAttemptState::Preparing ||
            !sound.prepared || !queue.prepared || !sound.instance.Valid() || !queue.instance.Valid() ||
            sound.instance.Parent() != 0 || queue.instance.Parent() != 0 ||
            sound.instance.Handle() == queue.instance.Handle() || sound.row == nullptr || queue.row == nullptr ||
            sound.variableControl || queue.variableControl || sound.packLeaseOwned || queue.packLeaseOwned ||
            sound.voiceStartCount != 1 || queue.voiceStartCount != 1 || queue.queueSuccessorVoiceStartCount != 1 ||
            sound.row->queueMode || !queue.row->queueMode || !queue.row->queueFirstContinuationLane ||
            !queue.row->queueTimingPending || queue.row->QueuePayload().queueUnplacedGroupCount != 2 ||
            !queue.initialOriginalConditionalVersion.Valid() || queue.initialOriginalConditional == nullptr ||
            sound.row->root.kind != PlayableKindV1::Sound || queue.row->root.kind != PlayableKindV1::Container ||
            attempt.initialService != sound.row->identity ||
            attempt.initialInvocationGeneration != sound.row->invocationGeneration) return false;
        bool ownAttempt = false, ownSound = false, ownQueue = false;
        for (std::uint32_t i = 0; i < immediateEntryAttempts.size(); ++i)
            ownAttempt |= &immediateEntryAttempts[i] == &attempt;
        for (auto& actual : actionPlayScratch) {
            ownSound |= &actual == &sound;
            ownQueue |= &actual == &queue;
        }
        if (!ownAttempt || !ownSound || !ownQueue) return false;
        const auto preparedRowMatches = [&](const EngineActionPlayScratch& actual) noexcept {
            const auto& row = *actual.row;
            return row.actionPrepared && row.pendingImmediateEntry && row.InvocationValid() &&
                row.Invocation().actionFiring == attempt.root && row.PendingImmediateCohortRoot() == attempt.root &&
                row.ownerInstance == actual.instance.Handle() && row.ownerEpoch == actual.instance.ScheduleEpoch() &&
                row.rootInstance == actual.instance.Handle() && playablePartitions.Owns(row.partition, row.identity) &&
                ResolvePlayableService(row.identity, row.ownerInstance, row.ownerEpoch) == &row;
        };
        if (!preparedRowMatches(sound) || !preparedRowMatches(queue) ||
            sound.row->Invocation().worldGeneration != queue.row->Invocation().worldGeneration) return false;
        // The native table verifies the actual original preparation inputs and receiver.
        PlayableInstanceTable::PreparedCommitProof soundProof, queueProof;
        if (!playableInstances.ProvePreparedCommit(sound.instance, &soundProof) ||
            !playableInstances.ProvePreparedCommit(queue.instance, &queueProof)) return false;
        auto& receipt = attempt.soundQueueCohort;
        const auto save = [&](InitialSoundQueueCohortReceipt::Participant& out,
            const EngineActionPlayScratch& actual) noexcept {
            const auto& row = *actual.row;
            out.service = row.identity;
            out.partition = row.partition;
            out.invocationGeneration = row.invocationGeneration;
            // Invocation belongs to the actual first producer; child/service remain distinct.
            out.owner = {actual.instance.Handle(), actual.instance.ScheduleEpoch(), 0, 0,
                sound.instance.Handle(), sound.instance.ScheduleEpoch(), row.identity.slot, row.identity.generation};
        };
        save(receipt.sound_, sound);
        save(receipt.queue_, queue);
        receipt.root_ = attempt.root;
        receipt.generation_ = attempt.generation;
        receipt.receiver_ = this;
        return true;
    }
    bool ReserveInitialSoundQueueCompanionPrimary(EngineImmediateEntryAttempt& attempt,
        bool continuation, std::uint64_t anchor, const Plan03GroupSliceInput& input,
        Plan03GroupSliceIdentity* out) noexcept {
        const bool originalC0 = OriginalInitialC0FactoryRequested(attempt);
        if (!HasInitialSoundQueueCohort(attempt) && !originalC0)
            return ReserveOriginalQueueLaneGroup(attempt.root, anchor, input, timelineArms, out);
        auto& receipt = attempt.soundQueueCohort;
        const auto index = continuation ? 1u : 0u;
        const auto& coordinators = attempt.QueueCoordinators();
        const auto& reservation = continuation ? coordinators.queueContinuationCoordinatorReservation :
            coordinators.initialQueueCoordinatorReservation;
        const auto* expectedOutput = continuation ? &coordinators.queueContinuationPrimaryGroup :
            &coordinators.initialQueuePrimaryGroup;
        const auto& request = reservation.OriginalRequest();
        if (out != expectedOutput || receipt.primaries_[index] != nullptr ||
            attempt.gate.Observe(attempt.generation) != ImmediateEntryAttemptState::Preparing ||
            !decisions.OwnsOpenPreparedReservation(reservation) ||
            input.decision.slot != reservation.Token().slot || input.decision.generation != reservation.Token().generation ||
            request.transport != timelinePool.Epoch() || request.expectedContinuationGeneration != attempt.generation ||
            request.sliceCount != 1u || input.sliceIndex != 0u || input.memberCount != 0u || input.sourceControlCount != 0u)
            return false;
        OriginalQueueFutureGroupRegistration* issued = nullptr;
        if (!ReserveOriginalQueueLaneGroup(attempt.root, anchor, input, timelineArms, out, nullptr, &issued)) return false;
        if (HasInitialSoundQueueCohort(attempt)) receipt.primaries_[index] = issued;
        return !originalC0 || (issued != nullptr && RecordOriginalInitialC0GroupFactoryOutput(
            attempt, continuation ? 2u : 0u, anchor, input, *issued));
    }
    bool DeclareInitialSoundQueueCompanionFromFactoryOutputs(EngineImmediateEntryAttempt& attempt,
        EngineActionPlayScratch& queueScratch, const OriginalQueueFutureGroupRegistration& initialMember,
        const OriginalQueueFutureGroupRegistration& conditionalMember) noexcept {
        if (!HasInitialSoundQueueCohort(attempt) || !attempt.lane.Valid() ||
            attempt.gate.Observe(attempt.generation) != ImmediateEntryAttemptState::Preparing ||
            !queueScratch.prepared || queueScratch.row == nullptr || queueScratch.initialOriginalConditional == nullptr ||
            !MatchesInitialSoundQueueParticipant(attempt, *queueScratch.row, true) ||
            !decisions.OwnsOpenPreparedReservation(attempt.QueueCoordinators().initialQueueCoordinatorReservation) ||
            !decisions.OwnsOpenPreparedReservation(attempt.QueueCoordinators().queueContinuationCoordinatorReservation)) return false;
        bool actualScratch = false;
        for (const auto& original : actionPlayScratch) actualScratch |= &original == &queueScratch;
        if (!actualScratch) return false;
        const auto& receipt = attempt.soundQueueCohort;
        const std::array<const OriginalQueueFutureGroupRegistration*, 4> originals{
            receipt.primaries_[0], &initialMember, receipt.primaries_[1], &conditionalMember};
        for (const auto* original : originals) {
            if (original == nullptr || !original->Valid()) return false;
            const auto identity = original->Identity();
            if (identity.committedRow >= originalQueueGroupHolders.size() ||
                &originalQueueGroupHolders[identity.committedRow] != original ||
                !timelinePool.IsOriginalQueueFutureGroupRegistrationFor(*original, identity, attempt.root)) return false;
        }
        const auto& row = *queueScratch.row;
        if (!(originals[0]->Identity() == attempt.QueueCoordinators().initialQueuePrimaryGroup) ||
            !(originals[2]->Identity() == attempt.QueueCoordinators().queueContinuationPrimaryGroup) ||
            initialMember.Identity().decision.slot != row.initialQueueEntryDecision.slot ||
            initialMember.Identity().decision.generation != row.initialQueueEntryDecision.generation ||
            conditionalMember.Identity().decision.slot != row.QueueFiling().queueDecision.slot ||
            conditionalMember.Identity().decision.generation != row.QueueFiling().queueDecision.generation) return false;
        const auto queue = row.queueInstance;
        const auto version = queueScratch.initialOriginalConditionalVersion;
        if (!queue.Valid() || queue.slot >= queueInstances.Capacity() ||
            ResolveOriginalQueueConditionalBaseline(this, queueInstances, queue) != queueScratch.initialOriginalConditional ||
            !queueInstances.IsOriginalConditionalFutureReservationFor(*queueScratch.initialOriginalConditional,
                originalQueueLifetimeHolders[queue.slot], queue, version, row.QueueFiling().queueDecision, row.QueuePayload().queueCurrentOccurrence))
            return false;
        FrontierQueueInitialIdentity initial{};
        FrontierQueueConditionalIdentity conditional{};
        WorldGeneration world = 0;
        PlayableInstanceHandle queueRoot = 0;
        if (!queueInstances.CapturePreparedOriginalConditionalFamily(*queueScratch.initialOriginalConditional,
                originalQueueLifetimeHolders[queue.slot], queueScratch.queueConditional,
                &initial, &conditional, &world, &queueRoot) ||
            world != row.Invocation().worldGeneration || queueRoot != receipt.queue_.owner.child ||
            attempt.QueueCoordinators().initialQueueCoordinatorReservation.OriginalRequest().world != world ||
            attempt.QueueCoordinators().queueContinuationCoordinatorReservation.OriginalRequest().world != world) return false;
        return timelinePool.DeclareInitialSoundQueueCompanion(originals, immediateActionLane,
            attempt.lane.slot, attempt.lane.generation, attempt.root, attempt.generation, world,
            receipt.sound_.owner, receipt.queue_.owner, initial, conditional);
    }
    struct OriginalSingleCurrentHolder;
    static OccurrenceStatePool::OriginalStoragePin* ResolveOriginalOccurrenceStorageBaseline(
        void* context, OccurrenceStatePool& pool, const OccurrenceToken& token) noexcept {
        auto* engine = static_cast<Engine*>(context);
        if (engine == nullptr || &pool != &engine->timelineOccurrences || token.generation == 0 ||
            token.slot >= engine->originalOccurrencePreparations.size()) return nullptr;
        auto& actual = engine->originalOccurrencePreparations[token.slot];
        return actual.receiver_ == engine && pool.IsOriginalStoragePinFor(actual.original_, token)
            ? &actual.original_ : nullptr;
    }
    bool PrepareOriginalOccurrence(const OccurrenceKey& key, OccurrenceToken* out,
        EngineOriginalOccurrencePreparation** issued = nullptr) noexcept {
        if (out == nullptr || (issued != nullptr && *issued != nullptr)) return false;
        OccurrenceToken token{};
        OccurrenceStatePool::OriginalStoragePin actual;
        if (!timelineOccurrences.PrepareAcquire(key, &token, &actual)) return false;
        assert(token.slot < originalOccurrencePreparations.size());
        auto& holder = originalOccurrencePreparations[token.slot];
        // Native storage is unavailable while any original loan remains.
        if (holder.original_.Valid()) {
            (void)timelineOccurrences.CancelPreparedAcquire(token, nullptr, &actual);
            runtimeRecreateRequired = true;
            return false;
        }
        holder.receiver_ = this;
        holder.token_ = token;
        holder.original_ = std::move(actual);
        *out = token;
        if (issued != nullptr) *issued = &holder;
        if (originalOccurrencePreparationHookForTest != nullptr)
            originalOccurrencePreparationHookForTest(*this, holder, originalOccurrencePreparationContextForTest);
        return true;
    }
    static CompositeCompletionOwnerPool::OriginalLifetimePin* ResolveOriginalCompositeBaseline(
        void* context, CompositeCompletionOwnerPool& pool, CompositeCompletionOwnerToken token) noexcept {
        auto* engine = static_cast<Engine*>(context);
        if (engine == nullptr || &pool != &engine->compositeCompletionOwners ||
            !token.Valid() || token.slot >= engine->originalCompositeCompletionPreparations.size()) return nullptr;
        auto& original = engine->originalCompositeCompletionPreparations[token.slot];
        return original.receiver_ == engine && pool.IsOriginalLifetimePinFor(original.original_, token)
            ? &original.original_ : nullptr;
    }
    CompositeCompletionAcquireStatus PrepareOriginalCompositeCompletion(
        OccurrenceToken composed, std::uint64_t revision, const KnownScheduleLeafV1* leaves,
        std::uint32_t count, OccurrenceStatePool& occurrences, CompositeCompletionOwnerToken* out,
        EngineOriginalCompositeCompletionPreparation** issued = nullptr) noexcept {
        if (&occurrences != &timelineOccurrences || (issued != nullptr && *issued != nullptr))
            return CompositeCompletionAcquireStatus::Invalid;
        CompositeCompletionOwnerPool::OriginalLifetimePin actual;
        const auto status = compositeCompletionOwners.PrepareAcquire(
            composed, revision, leaves, count, occurrences, out, &actual);
        if (status != CompositeCompletionAcquireStatus::Ok) return status;
        assert(out != nullptr && out->slot < originalCompositeCompletionPreparations.size());
        auto& holder = originalCompositeCompletionPreparations[out->slot];
        if (holder.original_.Valid()) {
            // A Free native row cannot have an outstanding same-slot loan.
            (void)compositeCompletionOwners.CancelPrepared(*out);
            runtimeRecreateRequired = true;
            return CompositeCompletionAcquireStatus::Invalid;
        }
        holder.receiver_ = this;
        holder.token_ = *out;
        holder.original_ = std::move(actual);
        if (issued != nullptr) *issued = &holder;
        if (originalCompositePreparationHookForTest != nullptr)
            originalCompositePreparationHookForTest(*this, holder, originalCompositePreparationContextForTest);
        return CompositeCompletionAcquireStatus::Ok;
    }

    bool PrepareOriginalSingleCurrentFromFactoryOutputs(
        const EnginePreparedTimelineVoiceStart& voice,
        const OriginalPlayableServiceLifetimeHolder& originalService,
        const PlayableInstanceTable::PreparedParentLinksProof& playablePreparation,
        const PreparedDecisionReservation& decisionPreparation,
        const TimelineArmTable::PreparedCommitProof& armPreparation,
        const VoicePool::PreparedCommitProof& voicePreparation,
        const CommandQueue::PreparedBatchProof& batch,
        OriginalSingleCurrentHolder** outOriginal = nullptr) noexcept {
        auto* holder = OriginalSinglePreparationForArm(voice.arm);
        const auto* originalDecision = OriginalSingleDecisionPreparation(decisionPreparation);
        const auto token = voice.progressOccurrence;
        if ((outOriginal != nullptr && *outOriginal != nullptr) || holder == nullptr || originalDecision == nullptr ||
            !HasOriginalSingleLedgerCommit(voice) || token.generation == 0 ||
            token.slot >= originalSingleOccurrenceBaselines.size() || token.slot >= originalSingleOccurrenceCommitProofs.size() ||
            originalService.engine_ != this || originalService.service_ == nullptr ||
            !originalService.service_->actionPrepared ||
            playablePartitions.Published(originalService.service_->partition, originalService.service_->identity)) return false;
        const auto& originalOccurrence = originalSingleOccurrenceCommitProofs[token.slot];
        if (!playableInstances.MatchesPreparedOriginalAncestry(playablePreparation,
                originalOccurrence.OriginalArrangingInstance(), originalService.reference_.owner) ||
            !playableInstances.MatchesPreparedOriginalAncestry(playablePreparation,
                originalService.reference_.owner, holder->ledgerCommit_.OriginalInput().parent.internalToken)) return false;
        OriginalPlayableServiceLifetimePin service;
        if (!originalService.Borrow(&service)) return false;
        OriginalSingleCurrentHolder* current = nullptr;
        if (!PrepareOriginalSingleCurrent(playablePreparation, holder->source_, holder->body_,
            *originalDecision, decisionPreparation, originalSingleOccurrenceBaselines[token.slot],
            originalSingleOccurrenceCommitProofs[token.slot], holder->ledgerOriginal_, holder->ledgerCommit_,
            armPreparation, voicePreparation, voice.voicePreparation, batch, voice.arm,
            holder->sourceIdentity_, timelineOccurrences.ScheduleEpochOf(token), &current)) return false;
        current->serviceRetention = std::move(service);
        if (outOriginal != nullptr) *outOriginal = current;
        return true;
    }
    bool ReturnOriginalSingleSourcePreparationAfterPhysicalRelease(TimelineArmToken arm) noexcept {
        if (!arm.Valid() || !originalSinglePreparationHolders.IsConstructed(arm.slot)) return true;
        auto& holder = originalSinglePreparationHolders[arm.slot];
        if (holder.receiver_ == nullptr || !SameOriginalSingleArm(holder.arm_, arm)) return true;
        if (holder.receiver_ != this) return false;
        if (holder.returned_) return true;
        if (!holder.physicalRelease_.Valid() || !voices.ReleaseObserved(holder.physicalRelease_) ||
            OriginalSinglePreparationForArm(arm) != &holder) return false;
        // This returns only the actual Engine-owned structural factory outputs.
        // Independent loans and the real native Arm/audio fences remain intact.
        if (holder.singleIssued_) {
            const auto token = holder.originalOccurrence_;
            if (token.generation == 0 || token.slot >= originalSingleOccurrenceCommitProofs.size()) return false;
            auto& proof = originalSingleOccurrenceCommitProofs[token.slot];
            if (proof.Valid() && !timelineOccurrences.IsPreparedSingleCompletionCommitFor(proof, token)) return false;
            proof.Release();
            holder.ledgerCommit_.Release();
        }
        for (auto& current : originalSingleCurrentHolders) {
            if ((current.state != OriginalSingleCurrentHolder::State::Prepared &&
                 current.state != OriginalSingleCurrentHolder::State::Closing) ||
                !SameOriginalSingleArm(current.originalCurrentSourceArm, arm) ||
                current.originalCurrentSourceIdentity != holder.sourceIdentity_) continue;
            if (!current.bundle.CloseOriginalComponentsAfterPhysicalRelease(voices, arm)) return false;
            current.state = OriginalSingleCurrentHolder::State::Closing;
            current.serviceRetention.Return();
        }
        holder.Return();
        return true;
    }
    void ServiceOriginalSingleSourcePreparations() noexcept {
        if (!originalSingleSourcePreparationSeen) return;
        for (auto& holder : originalSinglePreparationHolders)
            if (holder.receiver_ == this && !holder.returned_)
                (void)ReturnOriginalSingleSourcePreparationAfterPhysicalRelease(holder.arm_);
    }

    struct OriginalSingleCurrentHolder {
        enum class State : std::uint8_t { Empty, Prepared, Closing, Ending };
        PreparedSingleCurrentOriginBundle bundle;
        OriginalPlayableServiceLifetimePin serviceRetention;
        PlayableInstanceTable::OriginalLifetimeRetirementHold retirementHold;
        State state = State::Empty;
        PlayableInstanceTable* originalTable = nullptr;
        PlayableInstanceHandle originalRoot = 0;
        PlayableInstanceHandle originalArranging = 0;
        // These identities come from the genuine joined issuer and are never
        // refreshed from a Queue row after it selects a new future Decision.
        DecisionToken originalCurrentSingleDecision{};
        OccurrenceToken originalCurrentOccurrence{};
        CompletionOwnerRef originalCurrentCompletionOwner{};
        TimelineArmToken originalCurrentSourceArm{};
        VnVoiceHandle originalCurrentSourceIdentity = 0;
        VoiceReleaseObserver originalCurrentPhysicalRelease{};
        void Dispose() noexcept {
            if (state == State::Empty || state == State::Ending) return;
            state = State::Ending;
            bundle.Release();
            serviceRetention.Return();
        }
        bool ClearInstallAfterNativeRetirement(Engine& engine) noexcept {
            const auto slot = static_cast<std::uint32_t>(originalArranging);
            if (state != State::Ending || bundle.Valid() || retirementHold.Valid() || originalTable != &engine.playableInstances ||
                originalArranging == 0 || slot >= engine.originalPlayableLifetimeHolders.size() ||
                engine.playableInstances.IsOriginalLifetimePinFor(
                    engine.originalPlayableLifetimeHolders[slot], originalArranging) ||
                engine.playableInstances.Classify(originalArranging).live) return false;
            // The original factory receipt and the real native retirement have
            // completed. Clear only this exact old installation, never a reused
            // render slot or a successor that belongs to another Source.
            if (!originalCurrentPhysicalRelease.Valid() ||
                !engine.voices.ReleaseObserved(originalCurrentPhysicalRelease)) return false;
            (void)engine.ClearOriginalTimelineVoiceInstallIdentity(originalCurrentSourceIdentity,
                originalCurrentPhysicalRelease, originalCurrentSourceArm);
            return true;
        }
        bool ResetAfterNativeRetirement(Engine& engine) noexcept {
            if (!ClearInstallAfterNativeRetirement(engine)) return false;
            assert(engine.originalSingleCurrentCount != 0);
            --engine.originalSingleCurrentCount;
            state = State::Empty;
            originalTable = nullptr; originalRoot = 0; originalArranging = 0;
            originalCurrentSingleDecision = {}; originalCurrentOccurrence = {};
            originalCurrentCompletionOwner = {}; originalCurrentSourceArm = {};
            originalCurrentSourceIdentity = 0; originalCurrentPhysicalRelease = {};
            return true;
        }
    };
    static PlayableInstanceTable::OriginalLifetimePin* ResolveOriginalPlayableBaseline(
        void* opaque, PlayableInstanceTable& table, PlayableInstanceHandle handle) noexcept {
        auto* engine = static_cast<Engine*>(opaque);
        if (engine == nullptr || &engine->playableInstances != &table || handle == 0) return nullptr;
        const auto slot = static_cast<std::uint32_t>(handle);
        if (slot >= engine->originalPlayableLifetimeHolders.size()) return nullptr;
        auto& cap = engine->originalPlayableLifetimeHolders[slot];
        return table.IsOriginalLifetimePinFor(cap, handle) ? &cap : nullptr;
    }
    static void DisposeOriginalPlayableAssociation(void* opaque, PlayableInstanceTable& table,
        PlayableInstanceHandle handle) noexcept {
        auto* engine = static_cast<Engine*>(opaque);
        if (engine == nullptr || &engine->playableInstances != &table ||
            ResolveOriginalPlayableBaseline(opaque, table, handle) == nullptr) return;
        for (auto& current : engine->originalSingleCurrentHolders)
            if ((current.state == OriginalSingleCurrentHolder::State::Prepared ||
                 current.state == OriginalSingleCurrentHolder::State::Closing) &&
                current.bundle.MatchesOriginalPlayable(table, handle)) current.Dispose();
        for (auto& ordinary : engine->pendingOrdinarySelections)
            if (ordinary.engine == engine && ordinary.root.OriginalHandle() == handle)
                (void)engine->ClosePendingOrdinarySelection(ordinary);
        for (auto& node : engine->pendingVariableFutureNodes)
            if (node.group != nullptr && node.handle == handle) node.disposed = true;
        for (auto& group : engine->acceptedPendingVariableGroups) {
            if (!group.accepted || group.engine != engine) continue;
            for (std::uint32_t index = 0; index < group.topology_.nodeCount; ++index) {
                auto& node = group.topology_.nodes[index];
                if (node.handle != handle) continue;
                node.nativeDisposition = true;
                node.loan.Release();
            }
            if (group.root.OriginalHandle() == handle)
                (void)group.Close(*engine);
        }
    }
    bool IssueOriginalPlayableLifetimes(
        const PlayableInstanceTable::PreparedParentLinksProof& original,
        bool needsCurrentHolders = true) noexcept {
        return PrepareOriginalPlayableStorage(needsCurrentHolders) &&
            playableInstances.IssueOriginalPreparedLifetimeBaselines(original,
                {originalPlayableLifetimeHolders.data(), originalPlayableLifetimeHolders.size()});
    }
    bool IssueOriginalPlayableTreeLifetimes(PlayableTreeExecutor& tree,
        bool needsCurrentHolders = true) noexcept {
        const auto* original = tree.OriginalPreparedParentLinks(playableInstances);
        if (original == nullptr || !IssueOriginalPlayableLifetimes(*original, needsCurrentHolders)) return false;
        if (originalPlayableTreePreparationHookForTest != nullptr)
            originalPlayableTreePreparationHookForTest(*this, tree, originalPlayableTreePreparationContextForTest);
        return true;
    }

    // The original producer passes the actual native held preparations.
    // No later row/Queue token lookup issues Current authority.
    bool PrepareOriginalSingleCurrent(
        const PlayableInstanceTable::PreparedParentLinksProof& playablePreparation,
        const PreparedSingleCurrentOriginBundle::SourcePin& source,
        const PreparedSingleCurrentOriginBundle::BodyPin& body,
        const OriginalDecisionLifetimePin& decision, const PreparedDecisionReservation& decisionPreparation,
        const PreparedSingleCurrentOriginBundle::OccurrencePin& occurrence,
        const PreparedSingleCurrentOriginBundle::OccurrenceProof& occurrencePreparation,
        const PreparedSingleCurrentOriginBundle::LedgerPin& logicalVoice,
        const PreparedSingleCurrentOriginBundle::LedgerProof& ledgerPreparation,
        const TimelineArmTable::PreparedCommitProof& armPreparation,
        const VoicePool::PreparedCommitProof& voicePreparation, const VoicePool::PreparedCommitInput& physical,
        const CommandQueue::PreparedBatchProof& batch, TimelineArmToken arm, VnVoiceHandle sourceIdentity,
        std::uint64_t scheduleEpoch, OriginalSingleCurrentHolder** outOriginal = nullptr) noexcept {
        if ((outOriginal != nullptr && *outOriginal != nullptr) ||
            !ledger.MatchesPreparedSingleCompletionCommit(ledgerPreparation)) return false;
        const auto root = ledgerPreparation.OriginalInput().parent.internalToken;
        const auto arranging = occurrencePreparation.OriginalArrangingInstance();
        auto* rootOriginal = ResolveOriginalPlayableBaseline(this, playableInstances, root);
        auto* arrangingOriginal = ResolveOriginalPlayableBaseline(this, playableInstances, arranging);
        const auto slot = static_cast<std::uint32_t>(arranging);
        if (rootOriginal == nullptr || arrangingOriginal == nullptr || slot >= originalSingleCurrentHolders.size()) return false;
        auto& holder = originalSingleCurrentHolders[slot];
        if (holder.state == OriginalSingleCurrentHolder::State::Ending && !holder.ResetAfterNativeRetirement(*this)) return false;
        if (holder.state != OriginalSingleCurrentHolder::State::Empty || holder.bundle.Valid()) return false;
        PreparedSingleCurrentOriginBundle prepared;
        if (!PreparedSingleCurrentOriginBundle::PrepareWithOriginalPlayableLifetimes(
                originalSourceSubmissions, voices, timelineArms, decisions, timelineOccurrences, ledger,
                playableInstances, playablePreparation, *rootOriginal, *arrangingOriginal,
                source, body, decision, decisionPreparation, occurrence, occurrencePreparation,
                logicalVoice, ledgerPreparation, armPreparation, voicePreparation, physical, batch,
                arm, sourceIdentity, scheduleEpoch, &prepared)) return false;
        holder.bundle = std::move(prepared);
        holder.originalTable = &playableInstances;
        holder.originalRoot = root;
        holder.originalArranging = arranging;
        holder.originalCurrentSingleDecision = holder.bundle.OriginalCurrentSingleDecision();
        holder.originalCurrentOccurrence = holder.bundle.OriginalCurrentOccurrence();
        holder.originalCurrentCompletionOwner = holder.bundle.OriginalCurrentCompletionOwner();
        holder.originalCurrentSourceArm = holder.bundle.OriginalCurrentSourceArm();
        holder.originalCurrentSourceIdentity = holder.bundle.OriginalCurrentSourceIdentity();
        holder.originalCurrentPhysicalRelease = physical.originalRelease;
        holder.state = OriginalSingleCurrentHolder::State::Prepared;
        ++originalSingleCurrentCount;
        if (outOriginal != nullptr) *outOriginal = &holder;
        return true;
    }
    void ServiceOriginalSingleCurrentEndings() noexcept {
        if (originalSingleCurrentCount == 0) return;
        for (auto& current : originalSingleCurrentHolders) {
            if ((current.state != OriginalSingleCurrentHolder::State::Prepared &&
                 current.state != OriginalSingleCurrentHolder::State::Closing) ||
                current.originalTable != &playableInstances || current.retirementHold.Valid() ||
                !current.bundle.OriginalPhysicalReleaseObserved() || !current.bundle.HasOriginalPlayableRetention()) continue;
            auto* actual = ResolveOriginalPlayableBaseline(this, playableInstances, current.originalArranging);
            if (actual != nullptr)
                (void)playableInstances.BeginOriginalLifetimeRetirement(*actual, &current.retirementHold);
        }
    }
    void ReleaseOriginalSingleRetirementHold(PlayableInstanceHandle actualEnding) noexcept {
        for (auto& current : originalSingleCurrentHolders)
            if (current.state == OriginalSingleCurrentHolder::State::Ending &&
                current.originalTable == &playableInstances &&
                playableInstances.IsOriginalLifetimeRetirementHoldFor(current.retirementHold, actualEnding))
                current.retirementHold.Release();
    }
    void ServiceOriginalSingleRetiredInstallations() noexcept {
        if (originalSingleCurrentCount == 0) return;
        for (auto& current : originalSingleCurrentHolders)
            if (current.state == OriginalSingleCurrentHolder::State::Ending)
                (void)current.ClearInstallAfterNativeRetirement(*this);
    }
    void ServiceOriginalPlayableLifetimes() noexcept {
        // Retained original requests survive callers that release their API
        // bookkeeping. Every retry still traverses the true native end path.
        bool progress = playableInstances.HasRequestedOriginalLifetimeDispositions();
        while (progress) {
            progress = false;
            for (auto& original : originalPlayableLifetimeHolders)
                if (playableInstances.HasRequestedOriginalLifetimeDisposition(original) &&
                    playableInstances.TryFinishOriginalLifetimeDisposition(original)) progress = true;
        }
        ServiceOriginalSingleRetiredInstallations();
    }
    bool BorrowOriginalSingleCurrent(const OriginalSingleCurrentHolder& original,
        PreparedSingleCurrentOriginBundle* out) const noexcept {
        if (original.state != OriginalSingleCurrentHolder::State::Prepared ||
            original.originalTable != &playableInstances || original.originalArranging == 0) return false;
        const auto slot = static_cast<std::uint32_t>(original.originalArranging);
        return slot < originalSingleCurrentHolders.size() && &originalSingleCurrentHolders[slot] == &original &&
            original.bundle.HasOriginalPlayableRetention() && original.bundle.Borrow(out);
    }

    bool BorrowOriginalSingleCurrentService(const OriginalSingleCurrentHolder& original,
        OriginalPlayableServiceLifetimePin* out) const noexcept {
        if (original.state != OriginalSingleCurrentHolder::State::Prepared ||
            original.originalTable != &playableInstances || !original.bundle.Valid() ||
            !original.bundle.HasOriginalPlayableRetention() || original.originalArranging == 0) return false;
        const auto slot = static_cast<std::uint32_t>(original.originalArranging);
        return slot < originalSingleCurrentHolders.size() && &originalSingleCurrentHolders[slot] == &original &&
            original.serviceRetention.Borrow(out);
    }

    // Explicit disabled producer routing; no capacity is added or bypassed.
    bool originalInitialSingleQueueOriginProducerEnabled = false;
    std::uint64_t nextOriginalInitialC0FactoryMarker = 0;
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE) && !defined(HARKEN_RT_QUALIFICATION)
    void (*originalInitialC0PreparedHookForTest)(Engine&, EngineImmediateEntryAttempt&, void*) noexcept = nullptr;
    void* originalInitialC0PreparedContextForTest = nullptr;
#endif

    bool BeginOriginalInitialC0Factory(EngineImmediateEntryAttempt& attempt) noexcept {
        if (!OwnsImmediateEntryAttemptStorage(attempt) || !attempt.active ||
            (attempt.originalC0Factory != nullptr &&
             attempt.originalC0Factory->phase_ != OriginalInitialC0FactoryRecord::Phase::Empty) ||
            attempt.gate.Observe(attempt.generation) != ImmediateEntryAttemptState::Preparing ||
            attempt.originalLaneFactoryRoot != attempt.root ||
            attempt.originalLaneFactoryAttemptGeneration != attempt.generation ||
            !immediateActionLane.IsOriginalAttemptLifetimePinFor(attempt.originalLaneLifetime, attempt.lane, attempt.root)) return false;
        if (attempt.originalC0Factory == nullptr) {
            if (memguard::IsAudioTagged()) return false;
            try {
                detail::LogicAllocCheckpoint(detail::LogicAllocSite::ImmediateEntryStorage);
                attempt.originalC0Factory = std::make_unique<OriginalInitialC0FactoryRecord>();
            } catch (...) { return false; }
        }
        auto& record = *attempt.originalC0Factory;
        record.receiver_ = this; record.root_ = attempt.root; record.attemptGeneration_ = attempt.generation;
        record.lane_ = attempt.lane; record.phase_ = OriginalInitialC0FactoryRecord::Phase::Staging;
        return true;
    }
    bool OriginalInitialC0FactoryRequested(const EngineImmediateEntryAttempt& attempt) const noexcept {
        if (attempt.originalC0Factory == nullptr) return false;
        const auto& record = *attempt.originalC0Factory;
        return OwnsImmediateEntryAttemptStorage(attempt) && attempt.active && record.receiver_ == this &&
            record.root_ == attempt.root && record.attemptGeneration_ == attempt.generation &&
            record.lane_.slot == attempt.lane.slot && record.lane_.generation == attempt.lane.generation &&
            record.phase_ != OriginalInitialC0FactoryRecord::Phase::Empty &&
            attempt.originalLaneFactoryRoot == attempt.root &&
            attempt.originalLaneFactoryAttemptGeneration == attempt.generation &&
            immediateActionLane.IsOriginalAttemptLifetimePinFor(attempt.originalLaneLifetime, attempt.lane, attempt.root);
    }
private:
    bool RecordOriginalInitialC0GroupFactoryOutput(EngineImmediateEntryAttempt& attempt,
        std::uint32_t role, std::uint64_t anchor, const Plan03GroupSliceInput& input,
        OriginalQueueFutureGroupRegistration& actual) noexcept {
        if (attempt.originalC0Factory == nullptr) return false;
        auto& record = *attempt.originalC0Factory;
        if (role >= 4 || !OriginalInitialC0FactoryRequested(attempt) ||
            record.phase_ != OriginalInitialC0FactoryRecord::Phase::Staging || record.originals_[role] != nullptr ||
            attempt.gate.Observe(attempt.generation) != ImmediateEntryAttemptState::Preparing ||
            !timelinePool.IsOrdinaryOriginalQueueFutureGroupRegistrationFor(actual, actual.Identity(), attempt.root) ||
            !actual.MatchesOriginalScope(attempt.root, anchor, input, timelineArms)) return false;
        const auto identity = actual.Identity();
        if (identity.committedRow >= originalQueueGroupHolders.size() ||
            &originalQueueGroupHolders[identity.committedRow] != &actual) return false;
        for (const auto& old : record.groups_) if (old.Exact() && old == identity) return false;
        if (role != 0 && anchor != record.provisionalAnchor_) return false;
        record.originals_[role] = &actual; record.groups_[role] = identity;
        record.reservedInputs_[role] = input;
        if (role == 0) record.provisionalAnchor_ = anchor;
        return true;
    }

public:
    bool ReserveOriginalInitialC0MemberGroup(EngineImmediateEntryAttempt& attempt,
        bool conditional, std::uint64_t anchor, const Plan03GroupSliceInput& input,
        Plan03GroupSliceIdentity* out, OriginalQueueFutureGroupRegistration** issued) noexcept {
        const auto role = conditional ? 3u : 1u;
        if (attempt.originalC0Factory == nullptr) return false;
        auto& record = *attempt.originalC0Factory;
        if (out == nullptr || (issued != nullptr && *issued != nullptr) ||
            !OriginalInitialC0FactoryRequested(attempt) ||
            record.phase_ != OriginalInitialC0FactoryRecord::Phase::Staging ||
            record.originals_[role] != nullptr || record.originals_[0] == nullptr ||
            anchor != record.provisionalAnchor_ ||
            attempt.gate.Observe(attempt.generation) != ImmediateEntryAttemptState::Preparing) return false;
        OriginalQueueFutureGroupRegistration* actual = nullptr;
        if (!ReserveOriginalQueueLaneGroup(attempt.root, anchor, input, timelineArms, out, nullptr, &actual)) return false;
        // A failure retains the actual native Engine baseline and full output.
        // The caller still cancels the same original cohort before native Retire.
        if (actual == nullptr || !RecordOriginalInitialC0GroupFactoryOutput(attempt, role, anchor, input, *actual)) return false;
        if (issued != nullptr) *issued = actual;
        return true;
    }

    // All inputs still belong to the actual held preparation before Head.
    // Freezing this record borrows no old conditional/clock and no redundant B.
    // This is only the producer's storage-shape predicate. Native issuance still
    // requires the complete held Source, Single, Queue, bank and group proofs.
    bool MatchesOriginalInitialSingleQueueCurrentGeometry(
        const EngineActionPlayScratch& scratch) noexcept {
        if (!scratch.prepared || scratch.row == nullptr || scratch.voiceStartCount != 1 ||
            !HasOriginalSingleLedgerCommit(scratch.voiceStarts[0])) return false;
        const auto& row = *scratch.row;
        if (row.QueuePayload().queueCurrentScheduleLeafCount == 1) return true;
        const auto& voice = scratch.voiceStarts[0];
        if (row.QueuePayload().queueCurrentScheduleLeafCount != 0 || row.scheduleLeafCount != 0 ||
            row.zeroWorkMarker || row.hasZeroWorkCompletion || !voice.prepared || !voice.arm.Valid() ||
            voice.renderHandle == 0 || row.renderHandle != voice.renderHandle ||
            !SameOriginalSingleArm(row.arm, voice.arm) ||
            !SameOriginalSingleArm(row.schedule.armToken, voice.arm) ||
            !row.schedule.occurrence.present ||
            row.schedule.occurrence.token.slot != voice.progressOccurrence.slot ||
            row.schedule.occurrence.token.generation != voice.progressOccurrence.generation ||
            row.QueuePayload().queueCurrentProgramBank == nullptr || row.QueuePayload().queueCurrentProgramBank->programCount != 1)
            return false;
        const auto& frozen = row.QueuePayload().queueCurrentProgramBank->schedules[0];
        return SameOriginalSingleArm(frozen.armToken, voice.arm) &&
            frozen.eventCount == 1 && frozen.events != nullptr &&
            SameOriginalSingleArm(frozen.events[0].armToken, voice.arm);
    }

    bool PrepareOriginalInitialSingleQueueOriginFromFactoryOutputs(
        EngineImmediateEntryAttempt& attempt, EngineActionPlayScratch& scratch,
        const EnginePreparedTimelineVoiceStart& voice, OriginalSingleCurrentHolder& current,
        const PlayableInstanceTable::PreparedParentLinksProof& links,
        const PreparedDecisionReservation& decisionPreparation,
        const TimelineArmTable::PreparedCommitProof& armPreparation,
        const VoicePool::PreparedCommitProof& voicePreparation,
        const CommandQueue::PreparedBatchProof& batch,
        const LogicCommitCoordinator::PreparedIssue& actualIssue,
        const DecisionTable::PreparedTicketCommitProof& actualCurrentTicket) noexcept {
        const auto sequence = actualIssue.Sequence();
        if (attempt.originalC0Factory == nullptr) return false;
        auto& record = *attempt.originalC0Factory;
        if (!OriginalInitialC0FactoryRequested(attempt) ||
            record.phase_ != OriginalInitialC0FactoryRecord::Phase::Staging || record.marker_ != 0 ||
            nextOriginalInitialC0FactoryMarker == UINT64_MAX ||
            !logicCommits.IsOriginalPreparedIssueFor(actualIssue) || sequence == 0 ||
            !actualCurrentTicket.Associated(&decisions, &decisionPreparation) ||
            actualCurrentTicket.Sequence() != sequence ||
            actualCurrentTicket.Token().slot != current.originalCurrentSingleDecision.slot ||
            actualCurrentTicket.Token().generation != current.originalCurrentSingleDecision.generation ||
            attempt.gate.Observe(attempt.generation) != ImmediateEntryAttemptState::Preparing ||
            !scratch.prepared || scratch.row == nullptr || scratch.variableControl || scratch.voiceStartCount != 1 ||
            &scratch.voiceStarts[0] != &voice || scratch.initialOriginalConditional == nullptr ||
            !scratch.initialOriginalConditionalVersion.Valid() || !scratch.queueConditional.Valid() ||
            !voice.prepared || !voice.armPreparation.pendingImmediateStart ||
            voice.armPreparation.parentGroup.Valid() || voice.armPreparation.memberIndex != UINT16_MAX ||
            current.state != OriginalSingleCurrentHolder::State::Prepared ||
            current.originalTable != &playableInstances || !current.bundle.HasOriginalPlayableRetention() ||
            !SameOriginalSingleArm(current.originalCurrentSourceArm, voice.arm)) return false;
        bool actualScratch = false;
        for (const auto& original : actionPlayScratch) actualScratch |= &original == &scratch;
        const auto currentSlot = static_cast<std::uint32_t>(current.originalArranging);
        if (!actualScratch || currentSlot >= originalSingleCurrentHolders.size() ||
            &originalSingleCurrentHolders[currentSlot] != &current) return false;
        const auto& row = *scratch.row;
        if (!row.queueFirstContinuationLane || !row.queueTimingPending || !row.pendingImmediateEntry ||
            !row.actionPrepared || !row.InvocationValid() || row.Invocation().actionFiring != attempt.root ||
            row.PendingImmediateCohortRoot() != attempt.root || row.ownerInstance != scratch.instance.Handle() ||
            row.ownerEpoch != scratch.instance.ScheduleEpoch() || row.rootInstance != scratch.instance.Handle() ||
            ResolvePlayableService(row.identity, row.ownerInstance, row.ownerEpoch) != &row ||
            !playablePartitions.Owns(row.partition, row.identity) || row.QueuePayload().queueUnplacedGroupCount != 2 ||
            !MatchesOriginalInitialSingleQueueCurrentGeometry(scratch) || row.QueuePayload().queueInitialCurrentDelayedCount != 0 ||
            row.QueuePayload().queueCurrentProgramBank == nullptr || row.QueuePayload().queueCurrentProgramBank->programCount != 1 ||
            row.queueInstance.slot >= queueInstances.Capacity() ||
            current.originalRoot != scratch.instance.Handle() ||
            !SameCompletionOwner(current.originalCurrentCompletionOwner, voice.ledgerPreparation.completionMutation.owner) ||
            current.originalCurrentOccurrence.slot != row.QueuePayload().queueCurrentOccurrence.slot ||
            current.originalCurrentOccurrence.generation != row.QueuePayload().queueCurrentOccurrence.generation ||
            !timelinePool.Epoch()) return false;
        const bool mixed = HasInitialSoundQueueCohort(attempt);
        if (mixed ? !MatchesInitialSoundQueueParticipant(attempt, row, true) :
            (attempt.initialService != row.identity || attempt.initialInvocationGeneration != row.invocationGeneration)) return false;
        const auto occurrence = voice.progressOccurrence;
        auto* preparation = OriginalSinglePreparationForArm(voice.arm);
        if (preparation == nullptr || occurrence.generation == 0 ||
            occurrence.slot >= originalSingleOccurrenceCommitProofs.size() ||
            !current.bundle.MatchesPrepared(links, decisionPreparation,
                originalSingleOccurrenceCommitProofs[occurrence.slot], preparation->ledgerCommit_,
                armPreparation, voicePreparation, voice.voicePreparation, batch)) return false;
        if (!decisions.OwnsOpenPreparedReservation(attempt.QueueCoordinators().initialQueueCoordinatorReservation) ||
            !decisions.OwnsOpenPreparedReservation(attempt.QueueCoordinators().queueContinuationCoordinatorReservation)) return false;
        const auto& c0 = attempt.QueueCoordinators().initialQueueCoordinatorReservation.OriginalRequest();
        const auto& c1 = attempt.QueueCoordinators().queueContinuationCoordinatorReservation.OriginalRequest();
        if (c0.world != row.Invocation().worldGeneration || c1.world != c0.world ||
            c0.transport != timelinePool.Epoch() || c1.transport != c0.transport ||
            c0.expectedContinuationGeneration != attempt.generation || c1.expectedContinuationGeneration != attempt.generation ||
            c0.sliceCount != 1 || c1.sliceCount != 1 || sequence == 0) return false;
        for (std::uint32_t role = 0; role < 4; ++role) {
            auto* original = record.originals_[role];
            if (original == nullptr || !timelinePool.IsOrdinaryOriginalQueueFutureGroupRegistrationFor(*original, record.groups_[role], attempt.root) ||
                !original->MatchesOriginalScope(attempt.root, record.provisionalAnchor_, record.reservedInputs_[role], timelineArms)) return false;
        }
        for (std::uint32_t role = 1; role < 4; ++role)
            if (!timelinePool.IsOriginalQueueFutureGroupFamily(*record.originals_[0], *record.originals_[role])) return false;
        if (!(record.groups_[0] == attempt.QueueCoordinators().initialQueuePrimaryGroup) ||
            !(record.groups_[2] == attempt.QueueCoordinators().queueContinuationPrimaryGroup) ||
            record.reservedInputs_[0].memberCount != 0 || record.reservedInputs_[0].sourceControlCount != 0 ||
            record.reservedInputs_[1].memberCount != 0 || record.reservedInputs_[1].sourceControlCount != 0 ||
            record.reservedInputs_[2].memberCount != 0 || record.reservedInputs_[2].sourceControlCount != 0 ||
            (record.groups_[0].decision.slot != attempt.QueueCoordinators().initialQueueCoordinatorReservation.Token().slot || record.groups_[0].decision.generation != attempt.QueueCoordinators().initialQueueCoordinatorReservation.Token().generation) ||
            (record.groups_[2].decision.slot != attempt.QueueCoordinators().queueContinuationCoordinatorReservation.Token().slot || record.groups_[2].decision.generation != attempt.QueueCoordinators().queueContinuationCoordinatorReservation.Token().generation) ||
            (record.groups_[1].decision.slot != row.initialQueueEntryDecision.slot || record.groups_[1].decision.generation != row.initialQueueEntryDecision.generation) ||
            (record.groups_[3].decision.slot != current.originalCurrentSingleDecision.slot || record.groups_[3].decision.generation != current.originalCurrentSingleDecision.generation)) return false;
        const auto queue = row.queueInstance;
        const auto& parent = originalQueueLifetimeHolders[queue.slot];
        FrontierQueueInitialIdentity initial{};
        FrontierQueueConditionalIdentity conditional{};
        WorldGeneration world = 0;
        PlayableInstanceHandle root = 0;
        KnownCompletionPredicateV1 predicate{};
        CompletionOwnerRef owner{};
        if (ResolveOriginalQueueConditionalBaseline(this, queueInstances, queue) != scratch.initialOriginalConditional ||
            !queueInstances.IsOriginalConditionalFutureReservationFor(*scratch.initialOriginalConditional,
                parent, queue, scratch.initialOriginalConditionalVersion, current.originalCurrentSingleDecision,
                current.originalCurrentOccurrence) ||
            !queueInstances.CapturePreparedOriginalConditionalFamily(*scratch.initialOriginalConditional, parent,
                scratch.queueConditional, &initial, &conditional, &world, &root) ||
            !queueInstances.CapturePreparedOriginalConditionalCurrent(*scratch.initialOriginalConditional, parent,
                scratch.queueConditional, &predicate, &owner) || root != current.originalRoot || world != c0.world ||
            !SameCompletionOwner(owner, current.originalCurrentCompletionOwner) ||
            predicate.renderGeneration != voice.arm.renderGeneration ||
            !SameCompletionOwner(predicate.owner, owner)) return false;
        auto& bank = *row.QueuePayload().queueCurrentProgramBank;
        auto* bankOriginal = OriginalQueueBankHolderFor(&bank);
        const auto& program = bank.schedules[0];
        if (bankOriginal == nullptr || !bankOriginal->baseline.Valid() ||
            bankOriginal->baseline.Generation() != bank.Generation() ||
            !SameOriginalSingleArm(program.armToken, voice.arm) || program.events == nullptr || program.eventCount != 1 ||
            program.events[0].frameOffsetFromEntry < 0 || program.runtimeProgram.programGeneration == 0 ||
            program.runtimeProgram.program == nullptr ||
            row.pendingImmediateTimeDomain != ImmediateLaneTimeDomain::RelativeToAnchor ||
            row.pendingImmediateFixedEntryFrame != 0) return false;
        // Commit exactly once after every fallible native authentication.
        record.marker_ = ++nextOriginalInitialC0FactoryMarker;
        record.queue_ = queue; record.initial_ = initial; record.conditional_ = conditional; record.issueSequence_ = sequence;
        record.world_ = world; record.transport_ = c0.transport; record.queueRoot_ = root;
        record.service_ = row.identity; record.partition_ = row.partition;
        record.invocationGeneration_ = row.invocationGeneration;
        record.queueOwner_ = mixed ? attempt.soundQueueCohort.queue_.owner : FrontierOwnerIdentity{
            row.ownerInstance, row.ownerEpoch, 0, 0, row.ownerInstance, row.ownerEpoch, row.identity.slot, row.identity.generation};
        record.admissionOwner_ = mixed ? attempt.soundQueueCohort.sound_.owner : record.queueOwner_;
        record.current_ = &current.bundle; record.bank_ = &bank; record.bankGeneration_ = bank.Generation();
        record.sourceArm_ = voice.arm; record.sourceIdentity_ = current.originalCurrentSourceIdentity;
        record.currentDecision_ = current.originalCurrentSingleDecision;
        record.currentOccurrence_ = current.originalCurrentOccurrence; record.completionOwner_ = owner;
        record.predicate_ = predicate; record.sourceRuntimeProgram_ = program.runtimeProgram;
        // The true aggregate program and physical Arm program have independent
        // generations. Each remains authenticated by its own original issuer.
        record.sourceProgramGeneration_ = voice.armPreparation.programGeneration;
        record.sourceTarget_ = voice.armPreparation.target; record.sourceDomain_ = row.pendingImmediateTimeDomain;
        record.sourceOffset_ = static_cast<std::uint64_t>(program.events[0].frameOffsetFromEntry);
        record.sourceFixedBase_ = row.pendingImmediateFixedEntryFrame;
        // Do not retain mutable scratch member-array pointers after issuance.
        record.reservedInputs_ = {};
        record.phase_ = OriginalInitialC0FactoryRecord::Phase::Prepared;
        return true;
    }

    bool BorrowOriginalPreparedInitialSingleQueueOrigin(const EngineImmediateEntryAttempt& attempt,
        OriginalPreparedInitialSingleQueueOrigin* out) noexcept {
        if (attempt.originalC0Factory == nullptr) return false;
        const auto& record = *attempt.originalC0Factory;
        if (out == nullptr || out->record_ != nullptr || !OriginalInitialC0FactoryRequested(attempt) ||
            (record.phase_ != OriginalInitialC0FactoryRecord::Phase::Prepared &&
             record.phase_ != OriginalInitialC0FactoryRecord::Phase::Bound) || record.marker_ == 0 || record.current_ == nullptr ||
            !record.current_->Valid() || !record.current_->HasOriginalPlayableRetention() ||
            record.bank_ == nullptr || record.bank_->Generation() != record.bankGeneration_) return false;
        OriginalSingleCurrentHolder* current = nullptr;
        for (auto& original : originalSingleCurrentHolders)
            if (&original.bundle == record.current_) current = &original;
        auto* bank = OriginalQueueBankHolderFor(record.bank_);
        if (current == nullptr || current->state != OriginalSingleCurrentHolder::State::Prepared ||
            current->originalRoot != record.queueRoot_ ||
            !SameOriginalSingleArm(current->originalCurrentSourceArm, record.sourceArm_) ||
            current->originalCurrentSourceIdentity != record.sourceIdentity_ || bank == nullptr ||
            !bank->baseline.Valid() || bank->baseline.Generation() != record.bankGeneration_) return false;
        for (std::uint32_t role = 0; role < 2; ++role)
            if (record.originals_[role] == nullptr ||
                !timelinePool.IsOrdinaryOriginalQueueFutureGroupRegistrationFor(*record.originals_[role], record.groups_[role], record.root_)) return false;
        OriginalPreparedInitialSingleQueueOrigin loan;
        if (!BorrowOriginalImmediateEntryLane(attempt, &loan.lane_) ||
            !record.originals_[0]->BorrowLifetime(&loan.groups_[0]) ||
            !record.originals_[1]->BorrowLifetime(&loan.groups_[1]) ||
            !originalQueueLifetimeHolders[record.queue_.slot].Borrow(&loan.parent_) ||
            !bank->baseline.Borrow(&loan.bank_) || !BorrowOriginalSingleCurrent(*current, &loan.current_)) return false;
        loan.record_ = &record; loan.receiver_ = this; loan.marker_ = record.marker_;
        loan.root_ = record.root_; loan.generation_ = record.attemptGeneration_; loan.laneIdentity_ = record.lane_;
        loan.laneReceiver_ = &immediateActionLane; loan.pool_ = &timelinePool; loan.queueReceiver_ = &queueInstances;
        *out = std::move(loan);
        return true;
    }

    // The original producer alone once-binds its enduring certificate. A
    // structural borrower cannot call this using a captured Entry/anchor DTO.
    bool BindOriginalInitialC0SourceOnce(EngineImmediateEntryAttempt& attempt,
        PrivateOriginalQueueGroupClockProof& proof) noexcept {
        if (attempt.originalC0Factory == nullptr) return false;
        auto& record = *attempt.originalC0Factory;
        if (!OriginalInitialC0FactoryRequested(attempt) || record.marker_ == 0 || record.current_ == nullptr ||
            record.phase_ != OriginalInitialC0FactoryRecord::Phase::Prepared ||
            record.originals_[0] == nullptr || record.originals_[1] == nullptr ||
            !record.current_->Valid() || !record.current_->HasOriginalPlayableRetention() ||
            record.current_->sources_ != &originalSourceSubmissions || record.current_->voices_ != &voices ||
            record.current_->arms_ != &timelineArms || record.current_->decisions_ != &decisions ||
            record.current_->occurrences_ != &timelineOccurrences || record.current_->logicalVoices_ != &ledger ||
            record.current_->instances_ != &playableInstances ||
            !SameOriginalSingleArm(record.current_->arm_, record.sourceArm_) ||
            record.current_->sourceIdentity_ != record.sourceIdentity_ ||
            record.current_->OriginalCurrentSingleDecision().slot != record.currentDecision_.slot ||
            record.current_->OriginalCurrentSingleDecision().generation != record.currentDecision_.generation ||
            record.current_->OriginalCurrentOccurrence().slot != record.currentOccurrence_.slot ||
            record.current_->OriginalCurrentOccurrence().generation != record.currentOccurrence_.generation ||
            !SameCompletionOwner(record.current_->OriginalCurrentCompletionOwner(), record.completionOwner_) ||
            !record.current_->ObserveInstalled() ||
            record.bank_ == nullptr || record.bank_->Generation() != record.bankGeneration_ ||
            !timelinePool.IsPrivateOriginalQueueGroupClockProofFor(proof, *record.originals_[0], *record.originals_[1])) return false;
        OriginalQueueFutureGroupLifetimePin primary, initial;
        std::array<std::uint64_t, 3> frames{};
        const FrontierGroupBinding* primaryBinding = nullptr;
        const FrontierGroupBinding* initialBinding = nullptr;
        TimelineArmObservation source{};
        if (!record.originals_[0]->BorrowLifetime(&primary) || !record.originals_[1]->BorrowLifetime(&initial) ||
            !timelinePool.CapturePrivateOriginalQueueC0Source(proof, primary, initial, timelineArms, decisions,
                record.sourceArm_, &frames, &primaryBinding, &initialBinding) ||
            primaryBinding == nullptr || initialBinding == nullptr ||
            !(primaryBinding->operation.owner == record.admissionOwner_) ||
            !(initialBinding->operation.owner == record.queueOwner_) || !(initialBinding->queueInitial == record.initial_) ||
            primaryBinding->operation.world != record.world_ || primaryBinding->operation.transport != record.transport_ ||
            !timelineArms.Observe(record.sourceArm_, &source) || source.armedCompletionSequence == 0 ||
            frames[0] > UINT64_MAX - record.sourceOffset_ || frames[2] != frames[0] + record.sourceOffset_) return false;
        if (record.bank_->programCount != 1) return false;
        const auto& sourceProgram = record.bank_->schedules[0];
        if (!SameOriginalSingleArm(sourceProgram.armToken, record.sourceArm_) ||
            sourceProgram.runtimeProgram.program != record.sourceRuntimeProgram_.program ||
            sourceProgram.runtimeProgram.programGeneration != record.sourceRuntimeProgram_.programGeneration ||
            sourceProgram.events == nullptr || sourceProgram.eventCount != 1 ||
            sourceProgram.events[0].frameOffsetFromEntry < 0 ||
            static_cast<std::uint64_t>(sourceProgram.events[0].frameOffsetFromEntry) != record.sourceOffset_)
            return false;
        const auto* originalArm = timelineArms.RowAt(record.sourceArm_.slot);
        if (originalArm == nullptr || originalArm->generation != record.sourceArm_.generation ||
            originalArm->renderSlot != record.sourceArm_.renderSlot ||
            originalArm->renderGeneration != record.sourceArm_.renderGeneration ||
            originalArm->programGeneration != record.sourceProgramGeneration_ ||
            !(originalArm->target == record.sourceTarget_) || originalArm->parentGroup.Valid() ||
            originalArm->memberIndex != UINT16_MAX || !originalArm->pendingImmediateStart ||
            !originalArm->startBound || originalArm->startFrame != frames[2] ||
            originalArm->frontierAdmission != primaryBinding ||
            primaryBinding->entry.attempt != record.attemptGeneration_ ||
            primaryBinding->operation.order.logicCommitSequence != record.issueSequence_ ||
            initialBinding->operation.order.logicCommitSequence != record.issueSequence_ ||
            record.sourceDomain_ != ImmediateLaneTimeDomain::RelativeToAnchor || record.sourceFixedBase_ != 0)
            return false;
        record.binding_.entry = primaryBinding->entry;
        record.binding_.anchor = frames[0]; record.binding_.initial = frames[1];
        record.binding_.source = frames[2]; record.binding_.ack = source.armedCompletionSequence;
        record.phase_ = OriginalInitialC0FactoryRecord::Phase::Bound;
        return true;
    }

    bool BindOriginalInitialC0SourceFromActualPlacement(EngineImmediateEntryAttempt& attempt,
        PrivateImmediateLanePlacementProof* placement) noexcept {
        if (attempt.originalC0Factory == nullptr) return false;
        auto& record = *attempt.originalC0Factory;
        if (placement == nullptr || !OriginalInitialC0FactoryRequested(attempt) ||
            record.phase_ != OriginalInitialC0FactoryRecord::Phase::Prepared ||
            record.originals_[0] == nullptr || record.originals_[1] == nullptr ||
            !immediateActionLane.IsPrivatePlacementProofFor(*placement, attempt.lane, attempt.root))
            return false;
        PrivateOriginalQueueGroupClockProof clock;
        if (!timelinePool.ProvePrivateOriginalQueueGroupClock(*record.originals_[0],
                *record.originals_[1], placement, &clock)) return false;
        const bool bound = BindOriginalInitialC0SourceOnce(attempt, clock);
        // Hand the same native placement back before releasing its private gate.
        // A successful once-binding is retained even if a later factory refuses.
        const bool returned = clock.ReturnPlacement(placement);
        assert(returned);
        return bound && returned;
    }

    OriginalQueueBankHolder* OriginalQueueBankHolderFor(
        const EngineQueueProgramBank* bank) noexcept {
        if (bank == nullptr ||
            queueProgramBanks.size() == 0) return nullptr;
        const auto index = queueProgramBanks.IndexOf(*bank);
        if (index == UINT32_MAX || !originalQueueBankHolders.IsConstructed(index) ||
            &queueProgramBanks[index] != bank) return nullptr;
        return &originalQueueBankHolders[index];
    }

    static bool QueueProgramBankReusable(const EngineQueueProgramBank& bank) noexcept {
        return !bank.occupied && !bank.HasOriginalLifetimeOwners() && bank.Generation() != UINT64_MAX;
    }

    bool PrepareQueueProgramBankBacking(std::uint32_t count,
        StableSlotStorage<EngineQueueProgramBank>::Preparation* out) noexcept {
        return queueProgramBanks.PrepareBacking(count, 0, QueueProgramBankReusable,
            [this](std::uint32_t slot) noexcept { return PrepareQueueProgramBankSlot(slot); }, out);
    }

    bool PrepareQueueProgramBankSlot(std::uint32_t slot) noexcept {
        return originalQueueBankHolders.Emplace(slot) && queueProgramBanks.Emplace(slot);
    }

    bool PrepareQueueInstanceHolders(std::uint32_t count) noexcept {
        const auto workspaces = std::min(count, config.maxPlayableInstances);
        return initialQueueTimingMembersScratch.ConstructThrough(workspaces) &&
            initialQueueTransitionTimingProofsScratch.ConstructThrough(workspaces) &&
            initialQueueConditionalTimingProofsScratch.ConstructThrough(workspaces) &&
            initialQueueCurrentCompositeProofsScratch.ConstructThrough(workspaces) &&
            initialQueueSuccessorCompositeProofsScratch.ConstructThrough(workspaces) &&
            actionQueueReplacementScratch.ConstructThrough(workspaces) &&
            originalQueueLifetimeHolders.ConstructThrough(count) &&
            originalQueueFactoryLifetimeHolders.ConstructThrough(count) &&
            (queueInstances.originalConditionalCapacity_ == 0 ||
                (originalQueueConditionalHolders.ConstructThrough(count) &&
                 originalQueueReplacementHolders.ConstructThrough(count)));
    }

    EngineQueueProgramBank* ReserveOriginalQueueBank() noexcept {
        for (std::uint32_t index = 0; index < queueProgramBanks.size(); ++index) {
            if (!queueProgramBanks.IsConstructed(index)) {
                if (queueProgramBanks.PreparedUseActive()) continue;
                if (!PrepareQueueProgramBankSlot(index)) return nullptr;
            }
            auto& bank = queueProgramBanks[index];
            if (!QueueProgramBankReusable(bank)) continue;
            if (!queueProgramBanks.ProtectBackingForAcquire(index, QueueProgramBankReusable,
                    [this](std::uint32_t slot) noexcept { return PrepareQueueProgramBankSlot(slot); })) return nullptr;
            auto& original = originalQueueBankHolders[index];
            if (!bank.TryReserve(&original.reservation)) continue;
            if (bank.IssueLifetimePin(original.reservation, &original.baseline)) return &bank;
            original.reservation.Release();
            const bool reset = bank.TryResetStorage();
            assert(reset);
            (void)reset;
        }
        return nullptr;
    }

    bool OriginalQueueBankHasExternalOwners(const EngineQueueProgramBank& bank) noexcept {
        auto* original = OriginalQueueBankHolderFor(&bank);
        return original == nullptr ||
            bank.HasOriginalOwnersOtherThan(original->reservation, original->baseline);
    }

    bool ResetOriginalQueueBank(EngineQueueProgramBank& bank) noexcept {
        if (bank.supersededRoot != 0 && playableInstances.Classify(bank.supersededRoot).live)
            return false;
        auto* original = OriginalQueueBankHolderFor(&bank);
        if (original == nullptr || bank.HasOriginalOwnersOtherThan(original->reservation, original->baseline))
            return false;
        return ReleaseQueueProgramMembers(bank) &&
            bank.TryResetOriginalStorage(original->reservation, original->baseline);
    }

    bool CancelOriginalQueueBank(EngineQueueProgramBank*& alias) noexcept {
        if (alias == nullptr) return true;
        if (OriginalQueueBankHolderFor(alias) == nullptr) return false;
        // A published bank retains its baseline until the real bank service fence.
        if (!alias->published && !ResetOriginalQueueBank(*alias)) return false;
        alias = nullptr;
        return true;
    }

    struct OriginalQueueConditionalHolder {
        QueueInstanceTable::OriginalConditionalFutureReservation baseline;
        QueueInstanceTable::OriginalConditionalLaneClock clock;
        OriginalImmediateLaneConditionalProjection projection;
        EngineOwnedQueueFutureFiling initialFuture{};
        EngineQueueProgramBank::OriginalLifetimePin initialFutureBank;
        bool initialFutureCaptured = false;
        // Same-factory associations; the clock retains both native group rows.
        const OriginalQueueFutureGroupRegistration* initialGroup = nullptr;
        const OriginalQueueFutureGroupRegistration* conditionalGroup = nullptr;
        const OriginalSingleCurrentHolder* current = nullptr;
        EnginePlayableServiceReference service{};
        std::uint64_t invocationGeneration = 0;
    };

    // Stable storage survives Action scratch reuse and retains the real Current
    // until the selected Queue future and its native readers have finished.
    struct OriginalQueueReplacementHolder {
        enum class Phase : std::uint8_t { Empty, Preparing, Frozen, Selected, Published, Retiring };
        Phase phase = Phase::Empty;
        MonitorControlCause resetCause{};
        EngineActionQueueReplacementScratch* preparation = nullptr;
        EnginePlayableServiceReference service{};
        QueueInstanceToken queue{};
        PreparedSingleCurrentOriginBundle current;
        OriginalPlayableServiceLifetimePin serviceRetention;
        QueueInstanceTable::OriginalConditionalLaneClock clock;
        PrivateSelectedQueueFutureDomain domain;
        PrivateSelectedQueueFutureCalendar calendar;
        PrivateSelectedQueueFutureSpanPool::Owner selected;
        PrivateSelectedQueueFutureSpanPool::NativeConditionalRead conditionalReader;
        PreparedDecisionReservation coordinator;
        DecisionTable::PreparedTicketCommitProof coordinatorTicket, memberTicket, sourceTicket;
        std::array<DecisionToken, 2> decisions{};
        std::array<Plan03GroupSliceIdentity, 2> groups{};
        ArmGroupToken coordinatorArms{};
        FrontierEntryReceiptRef entry{};
        std::uint64_t relativeFrame = 0;
        std::uint64_t sequence = 0;
        bool ticketsConsumed = false;
        bool executionPrepared = false;
        TimelineArmTable::PreparedCommitProof sourceArmCommit;
        VoicePool::PreparedCommitProof sourceVoiceCommit;
        std::array<EffectPoolBank::PreparedCommitProof, kTimelineMaxVoiceEffects> sourceEffectCommits;
        EngineQueueProgramBank::OriginalLifetimePin bankRetention;
        EngineQueueProgramBank::OriginalLifetimePin oldBankRetention;
        EngineOwnedQueueFutureFiling oldFuture{};
        DecisionToken oldSourceDecision{};
        OccurrenceToken oldSourceOccurrence{};
        const EngineImmediateEntryAttempt* originalAttempt = nullptr;
        std::uint64_t originalAttemptGeneration = 0;
        bool spanClockBound = false, clockBound = false, conditionalBound = false, oldSourceAborted = false;
        bool oldLedgerCleared = false, oldOccurrenceCleared = false;
        bool sourceClockBound = false, sourceArmPublished = false, calendarPinned = false;
        std::uint32_t frontierBoundCount = 0;
        bool physicalPrepared = false;
    };

    OriginalQueueReplacementHolder* OriginalQueueReplacementFor(
        const EngineActionQueueReplacementScratch& scratch) noexcept {
        const auto queue = scratch.nativeReplacement;
        if (!queue.Valid() || queue.slot >= originalQueueReplacementHolders.size()) return nullptr;
        auto& holder = originalQueueReplacementHolders[queue.slot];
        return holder.phase != OriginalQueueReplacementHolder::Phase::Empty &&
            holder.preparation == &scratch && holder.queue.slot == queue.slot &&
            holder.queue.generation == queue.generation ? &holder : nullptr;
    }

    bool BeginOriginalQueueReplacement(EngineActionQueueReplacementScratch& scratch,
        EnginePlayableServiceRow& row, const PreparedQueueReplacementOrder& order) noexcept {
        if (scratch.nativeReplacement.Valid() || !order.Valid() ||
            row.queueInstance.slot >= originalQueueReplacementHolders.size() || originalQueueReplacementSpans == nullptr)
            return false;
        const auto* original = OriginalQueueConditionalForService(row);
        if (original == nullptr || !original->projection.Valid() ||
            original->initialGroup == nullptr || original->conditionalGroup == nullptr ||
            !queueInstances.IsCurrentOriginalConditionalFutureReservation(original->baseline)) return false;
        bool actualScratch = false;
        for (const auto& candidate : actionQueueReplacementScratch) actualScratch |= &candidate == &scratch;
        if (!actualScratch) return false;
        auto& holder = originalQueueReplacementHolders[row.queueInstance.slot];
        if (holder.phase != OriginalQueueReplacementHolder::Phase::Empty) return false;
        const EngineImmediateEntryAttempt* originalAttempt = nullptr;
        for (std::uint32_t i = 0; i < immediateEntryAttempts.size(); ++i)
            if (OriginalQueueConditionalForAttempt(immediateEntryAttempts[i]) == original) {
                originalAttempt = &immediateEntryAttempts[i]; break;
            }
        OriginalImmediateLaneAttemptLifetimePin lane;
        if (originalAttempt == nullptr || !BorrowOriginalImmediateEntryLane(*originalAttempt, &lane)) return false;
        holder.phase = OriginalQueueReplacementHolder::Phase::Preparing;
        ++originalQueueReplacementCount;
        holder.preparation = &scratch;
        holder.service = original->service;
        holder.queue = row.queueInstance;
        holder.sequence = order.Sequence();
        scratch.nativeReplacement = row.queueInstance;
        scratch.row = &row;
        std::array<std::uint64_t, 2> relative{};
        if (!BorrowOriginalSingleCurrent(*original->current, &holder.current) ||
            !BorrowOriginalSingleCurrentService(*original->current, &holder.serviceRetention) ||
            !original->clock.Borrow(&holder.clock) ||
            !timelinePool.CaptureOriginalQueueGroupRelativeFrames(*original->initialGroup,
                *original->conditionalGroup, &relative) ||
            !timelinePool.PreparePrivateSelectedQueueFutureDomain(*original->initialGroup,
                *original->conditionalGroup, lane, order, immediateActionLane, &holder.domain)) return false;
        holder.relativeFrame = relative[1];
        holder.oldFuture = original->initialFuture;
        holder.oldSourceDecision = row.queueSuccessorDecision;
        holder.oldSourceOccurrence = original->initialFuture.queueSuccessorOccurrence;
        holder.originalAttempt = originalAttempt;
        holder.originalAttemptGeneration = originalAttempt->generation;
        if (original->initialFutureBank.Valid() &&
            !original->initialFutureBank.Borrow(&holder.oldBankRetention)) return false;
        return true;
    }

    bool FreezeOriginalQueueReplacement(EngineActionQueueReplacementScratch& scratch,
        const QueueInstanceTable::PreparedCatalogResetScope& matched,
        const PreparedQueueReplacementOrder& order) noexcept {
        auto* held = OriginalQueueReplacementFor(scratch);
        if (held == nullptr || held->phase != OriginalQueueReplacementHolder::Phase::Preparing ||
            !held->domain.Valid() || !order.Valid() || held->sequence != order.Sequence() ||
            scratch.row == nullptr || scratch.sourceControlCount != 0 ||
            (!scratch.sameProgramKey && scratch.successorLeafCount != 1)) return false;
        auto& holder = *held;
        const auto* original = OriginalQueueConditionalForService(*scratch.row);
        if (original == nullptr || original->current == nullptr) return false;
        const auto& current = *original->current;
        const auto* currentDecision = ResolveOriginalSingleDecisionBaseline(this,
            decisions, current.originalCurrentSingleDecision);
        const auto* currentOccurrence = ResolveOriginalSingleOccurrenceBaseline(this,
            timelineOccurrences, current.originalCurrentOccurrence);
        const auto* member = OriginalSingleDecisionPreparation(scratch.decision);
        if (currentDecision == nullptr || currentOccurrence == nullptr || member == nullptr ||
            !decisions.ProvePreparedReservationCommit(scratch.decision,
                scratch.decision.OriginalRequest(), order.Sequence(), &holder.memberTicket) ||
            !PrepareOriginalQueueDecisionReservation(scratch.decision.OriginalRequest(), &holder.coordinator) ||
            !decisions.ProvePreparedReservationCommit(holder.coordinator,
                holder.coordinator.OriginalRequest(), order.Sequence(), &holder.coordinatorTicket)) return false;
        const auto* coordinator = OriginalSingleDecisionPreparation(holder.coordinator);
        if (coordinator == nullptr) return false;
        holder.decisions = {holder.coordinator.Token(), scratch.decision.Token()};
        const OriginalDecisionLifetimePin* sourceDecision = nullptr;
        const OccurrenceStatePool::OriginalSingleCompletionOwnerPin* successor = nullptr;
        const OccurrenceStatePool::PreparedSingleCompletionCommitProof* successorProof = nullptr;
        if (!scratch.sameProgramKey) {
            sourceDecision = OriginalSingleDecisionPreparation(scratch.successorDecision);
            successor = ResolveOriginalSingleOccurrenceBaseline(this, timelineOccurrences, scratch.successorOccurrence);
            successorProof = ResolveOriginalSingleOccurrenceCommit(this, timelineOccurrences, scratch.successorOccurrence);
            if (sourceDecision == nullptr || successor == nullptr || successorProof == nullptr ||
                !decisions.ProvePreparedReservationCommit(scratch.successorDecision,
                    scratch.successorDecision.OriginalRequest(), order.Sequence(), &holder.sourceTicket)) return false;
        }
        for (std::uint32_t i = 0; i < holder.groups.size(); ++i) {
            Plan03GroupSliceInput input{};
            input.decision = holder.decisions[i];
            input.executeFrame = holder.relativeFrame;
            if (i == 1 && !scratch.sameProgramKey) {
                input.members = &scratch.successorStarts[0].arm;
                input.memberCount = 1;
            }
            PreparedPrivateImmediateLaneGroup issued;
            PrivateImmediateLaneGroupTransferProof transfer;
            if (!holder.domain.ReserveGroup(0, input, timelineArms, &holder.groups[i], &issued)) return false;
            if (timelinePool.ProvePrivateImmediateLaneGroupTransfer(issued,
                    holder.domain.RootObservation(), 0, input, timelineArms, &transfer) &&
                ConsumeOriginalQueueLaneGroup(std::move(transfer)) == holder.groups[i]) continue;
            transfer.Release();
            if (!timelinePool.CancelPrivateImmediateLaneGroup(&issued, &holder.groups[i]))
                runtimeRecreateRequired = true;
            return false;
        }
        const auto& parent = originalQueueLifetimeHolders[holder.queue.slot];
        const auto& primaryGroup = originalQueueGroupHolders[holder.groups[0].committedRow];
        const auto& memberGroup = originalQueueGroupHolders[holder.groups[1].committedRow];
        bool prepared = false;
        if (scratch.sameProgramKey) {
            prepared = originalQueueReplacementSpans->PrepareRetainedCurrent(queueInstances, matched,
                parent, original->baseline, holder.clock, order, *currentDecision, *currentOccurrence,
                decisions, scratch.decision, *member, holder.memberTicket, holder.coordinator, *coordinator,
                holder.coordinatorTicket, timelineOccurrences, timelinePool, primaryGroup, memberGroup,
                &holder.selected, &holder.domain);
        } else {
            const PrivateSelectedQueueFutureSpanPool::SourceCompletionFactory source{
                scratch.successorDecision, *sourceDecision, holder.sourceTicket};
            prepared = originalQueueReplacementSpans->Prepare(queueInstances, matched, parent,
                original->baseline, holder.clock, order, *currentDecision, *currentOccurrence, decisions,
                scratch.decision, *member, holder.memberTicket, holder.coordinator, *coordinator,
                holder.coordinatorTicket, timelineOccurrences, *successor, *successorProof, timelinePool,
                primaryGroup, memberGroup, &holder.selected, &holder.domain, &source);
        }
        if (!prepared || !originalQueueReplacementSpans->FreezeUnbound(holder.selected) ||
            !holder.selected.BorrowNativeConditional(&holder.conditionalReader)) return false;
        holder.phase = OriginalQueueReplacementHolder::Phase::Frozen;
        return true;
    }

    bool PrepareOriginalQueueReplacementExecution(EngineActionQueueReplacementScratch& scratch) noexcept {
        auto* held = OriginalQueueReplacementFor(scratch);
        if (held == nullptr || held->phase != OriginalQueueReplacementHolder::Phase::Frozen ||
            held->ticketsConsumed || held->executionPrepared || !scratch.armGroup.Valid() ||
            !held->coordinatorTicket.Valid() || !held->memberTicket.Valid()) return false;
        auto& holder = *held;
        auto* service = ResolvePlayableService(holder.service);
        if (service != scratch.row || service == nullptr || !holder.current.Valid()) return false;
        const auto child = playableInstances.Classify(service->ownerInstance);
        const auto root = playableInstances.Classify(service->rootInstance);
        const auto parent = playableInstances.Classify(child.parent);
        if (!child.live || child.scheduleEpoch != service->ownerEpoch || !root.live ||
            (child.parent != 0 && !parent.live)) return false;
        const FrontierOwnerIdentity identity{service->ownerInstance, service->ownerEpoch,
            child.parent, child.parent == 0 ? 0 : parent.scheduleEpoch, service->rootInstance,
            root.scheduleEpoch, service->identity.slot, service->identity.generation};
        if (!identity.Valid() || !armGroups.Reserve(holder.decisions[0], nullptr, 0, &holder.coordinatorArms))
            return false;
        scratch.nativeMemberDecision = holder.decisions[1];
        scratch.nativeSourceDecision = scratch.successorDecision.Token();
        DecisionTable::ConsumePreparedReservationCommit(std::move(holder.memberTicket));
        DecisionTable::ConsumePreparedReservationCommit(std::move(holder.coordinatorTicket));
        if (holder.sourceTicket.Valid())
            DecisionTable::ConsumePreparedReservationCommit(std::move(holder.sourceTicket));
        holder.ticketsConsumed = true;
        const auto* controller = decisions.PrepareFrontierControlProgram(holder.decisions[0],
            holder.domain.RootObservation());
        if (controller == nullptr) return false;
        holder.entry = {holder.decisions[0], holder.domain.RootObservation(), identity};
        std::array<ImmediateLaneEvent, 2> events{};
        auto& entry = events[0];
        entry.relativeFrame = holder.relativeFrame;
        entry.event.program = {controller->generation, controller};
        entry.event.subeventPhase = 10;
        entry.event.logicCommitSequence = holder.sequence;
        entry.event.ownerInstanceHandle = identity.child;
        entry.event.ownerInstanceScheduleEpoch = identity.childEpoch;
        entry.event.frontier.kind = FrontierOperationKind::Entry;
        entry.event.frontier.entry = holder.entry;
        entry.event.frontier.owner = identity;
        entry.event.frontier.group = holder.groups[0];
        entry.event.frontier.world = service->Invocation().worldGeneration;
        entry.event.frontier.transport = timelinePool.Epoch();
        entry.event.frontier.operationEventCount = 1;
        entry.event.frontier.entryEventOrdinal = 0;
        ImmediateLaneMemberManifest member{};
        if (!scratch.sameProgramKey) {
            if (scratch.successorLeafCount != 1 || scratch.programBank == nullptr ||
                scratch.programBank->programCount != 1 || scratch.programBank->eventCount != 1 ||
                !HasOriginalSingleLedgerCommit(scratch.successorStarts[0])) return false;
            const auto& source = scratch.programBank->schedules[0];
            if (source.runtimeProgram.program == nullptr || source.runtimeProgram.program->payloadCount != 1 ||
                source.runtimeProgram.program->payloads == nullptr || source.eventCount != 1 ||
                source.events == nullptr || source.events[0].payload != 0 ||
                source.runtimeProgram.program->payloads[0].kind != TimelineEventKind::Start ||
                !SameOriginalSingleArm(source.armToken, scratch.successorStarts[0].arm)) return false;
            auto& start = events[1];
            start = entry;
            start.futureGroupedStart = true;
            start.armProgramGeneration = source.runtimeProgram.programGeneration;
            start.event.program = source.runtimeProgram;
            start.event.armToken = scratch.successorStarts[0].arm;
            start.event.subeventPhase = 20;
            start.event.frontier.kind = FrontierOperationKind::Dependent;
            start.event.frontier.group = holder.groups[1];
            start.event.frontier.entryEventOrdinal = UINT32_MAX;
            member.group = holder.groups[1];
            member.arm = start.event.armToken;
            member.eventOrdinal = 1;
            member.memberIndex = 0;
            member.future = true;
            member.relativeFrame = holder.relativeFrame;
            member.programGeneration = source.runtimeProgram.programGeneration;
            member.target = source.runtimeProgram.program->payloads[0].target;
            member.ownerInstance = identity.child;
            member.ownerEpoch = identity.childEpoch;
        }
        if (!holder.calendar.Prepare(*originalQueueReplacementSpans, holder.selected, holder.domain,
                events.data(), scratch.sameProgramKey ? 1u : 2u,
                scratch.sameProgramKey ? nullptr : &member, timelineArms)) return false;
        for (std::size_t i = 0; i < holder.decisions.size(); ++i) {
            DecisionParticipantRef participant{};
            participant.kind = i == 0 ? DecisionParticipantKind::Subdecision : DecisionParticipantKind::QueueInstance;
            participant.slot = i == 0 ? holder.decisions[1].slot : holder.queue.slot;
            participant.generation = i == 0 ? holder.decisions[1].generation : holder.queue.generation;
            DecisionCommitPayload payload{};
            payload.pages = {static_cast<std::uint32_t>(holder.groups[i].committedRow),
                holder.groups[i].rowClaimGeneration};
            payload.armGroup = i == 0 ? holder.coordinatorArms : scratch.armGroup;
            payload.participants = &participant;
            payload.participantCount = 1;
            if (!decisions.Commit(holder.decisions[i], payload)) return false;
        }
        holder.executionPrepared = true;
        return true;
    }

    bool SealOriginalQueueReplacementSource(EngineActionQueueReplacementScratch& scratch) noexcept {
        auto* held = OriginalQueueReplacementFor(scratch);
        if (held == nullptr || held->phase != OriginalQueueReplacementHolder::Phase::Frozen ||
            !held->executionPrepared || held->physicalPrepared) return false;
        auto& holder = *held;
        if (scratch.sameProgramKey) {
            holder.physicalPrepared = true;
            return true;
        }
        if (scratch.successorLeafCount != 1 || scratch.programBank == nullptr) return false;
        if (forceTimelineVoiceCommitRollbackForTest) {
            forceTimelineVoiceCommitRollbackForTest = false;
            return false;
        }
        auto& source = scratch.successorStarts[0];
        auto* bank = OriginalQueueBankHolderFor(scratch.programBank);
        if (!source.prepared || !HasOriginalSingleLedgerCommit(source) ||
            ResolveOriginalSingleOccurrenceCommit(this, timelineOccurrences, scratch.successorOccurrence) == nullptr ||
            source.effectCount > holder.sourceEffectCommits.size() || bank == nullptr ||
            !bank->baseline.Borrow(&holder.bankRetention) ||
            !voices.ProvePreparedCommit(source.voicePreparation, &holder.sourceVoiceCommit) ||
            !timelineArms.ProvePreparedCommit(source.armPreparation, &holder.sourceArmCommit)) return false;
        for (std::uint32_t i = 0; i < source.effectCount; ++i)
            if (!source.effectOwnershipHeld[i] ||
                !effectBank.ProvePreparedCommit(source.effects[i], source.effectIdentities[i],
                    &holder.sourceEffectCommits[i])) return false;
        holder.physicalPrepared = true;
        return true;
    }

    bool AttachOriginalQueueReplacementBatch(std::uint32_t count) noexcept {
        if (originalQueueReplacementBatch == nullptr || count > actionQueueReplacementScratch.size()) return false;
        std::uint32_t nativeCount = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            auto& scratch = actionQueueReplacementScratch[i];
            if (!scratch.nativeReplacement.Valid()) continue;
            auto* holder = OriginalQueueReplacementFor(scratch);
            auto* original = OriginalQueueConditionalForService(*scratch.row);
            if (holder == nullptr || original == nullptr ||
                holder->phase != OriginalQueueReplacementHolder::Phase::Frozen ||
                !holder->physicalPrepared || !holder->executionPrepared ||
                !originalQueueReplacementBatch->Attach(original->projection,
                    *originalQueueReplacementSpans, holder->selected)) return false;
            ++nativeCount;
        }
        if (nativeCount == 0 || nativeCount != originalQueueReplacementBatch->Scope().MatchedCount()) return false;
        for (std::uint32_t i = 0; i < count; ++i) {
            auto& scratch = actionQueueReplacementScratch[i];
            if (!scratch.nativeReplacement.Valid()) continue;
            auto* original = OriginalQueueConditionalForService(*scratch.row);
            if (original == nullptr || !originalQueueReplacementBatch->PrepareQueueSelection(
                    original->projection, scratch.resetExecuteFrame, scratch.resetExecutionOrdinal)) return false;
        }
        return true;
    }

    bool BindOriginalQueueReplacementCalendar(OriginalQueueReplacementHolder& holder) noexcept {
        if (holder.phase != OriginalQueueReplacementHolder::Phase::Selected ||
            !holder.executionPrepared || !holder.selected.Valid()) return false;
        auto* row = ResolvePlayableService(holder.service);
        const auto* original = row == nullptr ? nullptr : OriginalQueueConditionalForService(*row);
        PrivateSelectedQueueFutureSpanPool::ConditionalBacking selected{};
        QueueInstanceTable::OriginalConditionalInitialClockBinding clock{};
        Plan03PageGroupView prerequisite{};
        TimelineTransactionProgramRef controller{};
        QueueInstanceSnapshot queue{};
        if (original == nullptr || original->initialGroup == nullptr ||
            !holder.clock.CaptureInitialBinding(&clock) ||
            !holder.conditionalReader.Capture(&selected) ||
            !queueInstances.Capture(holder.queue, &queue) ||
            queue.pendingVersion != selected.version.version ||
            !timelinePool.ResolvePlan03GroupSliceProducer(original->initialGroup->Identity(), &prerequisite) ||
            prerequisite.frontier == nullptr || !prerequisite.frontier->Valid() ||
            prerequisite.frontier->role != FrontierGroupRole::EntryMember ||
            !decisions.CaptureFrontierControlProgram(holder.decisions[0], &controller) ||
            controller.program == nullptr || clock.anchorFrame > UINT64_MAX - holder.relativeFrame)
            return false;
        const auto executeFrame = clock.anchorFrame + holder.relativeFrame;
        if (!holder.spanClockBound) {
            if (!originalQueueReplacementSpans->BindOriginalFirstS(holder.selected, holder.clock)) return false;
            holder.spanClockBound = true;
        }
        if (!holder.clockBound) {
            // Placement may retry after capacity returns; the span's once-binding remains valid.
            if (!holder.calendar.BindOriginalFirstS(holder.clock)) return false;
            holder.clockBound = true;
        }
        for (std::uint32_t i = holder.frontierBoundCount; i < 2; ++i) {
            FrontierGroupBinding binding{};
            binding.role = i == 0 ? FrontierGroupRole::Coordinator : FrontierGroupRole::EntryMember;
            binding.entry = holder.entry;
            binding.operation.kind = i == 0 ? FrontierOperationKind::Entry : FrontierOperationKind::Dependent;
            binding.operation.owner = holder.entry.owner;
            binding.operation.order.executeFrame = executeFrame;
            binding.operation.order.logicCommitSequence = holder.sequence;
            binding.operation.order.subeventPhase = 10;
            binding.operation.group = holder.groups[i];
            binding.operation.transport = timelinePool.Epoch();
            binding.operation.world = row->Invocation().worldGeneration;
            binding.primary = holder.groups[0];
            if (i == 0) {
                binding.prerequisite = prerequisite.frontier->entry;
                binding.prerequisiteMember = original->initialGroup->Identity();
                FrontierActivationDescriptor descriptor{};
                descriptor.entry = binding.entry;
                descriptor.primary = binding.primary;
                descriptor.order = binding.operation.order;
                descriptor.worldGeneration = binding.operation.world;
                descriptor.worldSemanticFingerprint = queue.programSemanticFingerprint;
                descriptor.expectedMemberCount = descriptor.expectedEntryEventCount = 1;
                descriptor.programGeneration = controller.programGeneration;
                if (!decisions.PrepareFrontierActivation(holder.decisions[0], descriptor, timelinePool)) return false;
            } else {
                binding.queueConditional = {holder.queue.slot, holder.queue.generation,
                    queue.logicMirror.version, selected.version.version, selected.current, selected.successor};
            }
            if (!timelinePool.BindPlan03GroupFrontier(holder.groups[i], binding)) return false;
            ++holder.frontierBoundCount;
        }
        if (!holder.conditionalBound) {
            Plan03PageGroupView member{};
            if (!timelinePool.ResolvePlan03GroupSliceProducer(holder.groups[1], &member) || member.frontier == nullptr ||
                !queueInstances.BindFrontierConditional(holder.queue, *member.frontier, timelinePool) ||
                !decisions.BindFrontierDecision(holder.decisions[1], *member.frontier, timelinePool)) return false;
            holder.conditionalBound = true;
        }
        return true;
    }

    bool RetireOriginalQueueReplacementCalendar(OriginalQueueReplacementHolder& holder,
        std::uint64_t completed) noexcept {
        if (holder.phase < OriginalQueueReplacementHolder::Phase::Selected) return false;
        if (holder.phase != OriginalQueueReplacementHolder::Phase::Retiring) {
            QueueInstanceTable::OriginalConditionalLaneClockState state{};
            if (!holder.clock.CaptureState(&state) || state != QueueInstanceTable::OriginalConditionalLaneClockState::Retired ||
                !holder.conditionalReader.Release()) return false;
            const bool returned = holder.phase == OriginalQueueReplacementHolder::Phase::Selected
                ? holder.calendar.Abort(completed) : holder.calendar.RetireSelected(completed);
            if (!returned) return false;
            holder.phase = OriginalQueueReplacementHolder::Phase::Retiring;
        }
        while (holder.frontierBoundCount != 0) {
            if (!timelinePool.DetachPlan03GroupFrontier(holder.groups[holder.frontierBoundCount - 1], completed))
                return false;
            --holder.frontierBoundCount;
        }
        if (holder.conditionalBound) {
            if (!decisions.ReleaseFrontierDecisionBinding(holder.decisions[1])) return false;
            holder.conditionalBound = false;
        }
        holder.selected.Release();
        if (holder.domain.Valid() && (!holder.domain.CancelGroups(completed,
                originalQueueGroupHolders.data(), originalQueueGroupHolders.size()) || !holder.domain.Return())) return false;
        if (holder.decisions[0].Valid()) {
            (void)decisions.Invalidate(holder.decisions[0]);
            if (!decisions.Retire(holder.decisions[0], completed) || !decisions.ReclaimRetired(holder.decisions[0]))
                return false;
            holder.decisions[0] = {};
        }
        if (holder.coordinatorArms.Valid()) {
            if (!armGroups.BeginRetire(holder.coordinatorArms) || !armGroups.Reclaim(holder.coordinatorArms, completed))
                return false;
            holder.coordinatorArms = {};
        }
        // The selected calendar and its readers are gone. Return the original
        // clock/group and Current loans before awaiting their native disposal.
        holder.clock.Release();
        holder.current.Release();
        if (!holder.oldSourceAborted || holder.oldSourceDecision.Valid() ||
            holder.oldSourceOccurrence.generation != 0) return false;
        ArmGroupSnapshot oldSourceGroup{};
        if (holder.oldFuture.queueArmGroup.Valid() &&
            armGroups.Capture(holder.oldFuture.queueArmGroup, &oldSourceGroup)) return false;
        holder.serviceRetention.Return();
        holder.bankRetention.Release();
        holder.oldBankRetention.Release();
        holder.preparation = nullptr;
        holder.phase = OriginalQueueReplacementHolder::Phase::Empty;
        assert(originalQueueReplacementCount != 0);
        --originalQueueReplacementCount;
        holder.service = {};
        holder.queue = {};
        holder.decisions = {};
        holder.groups = {};
        holder.entry = {};
        holder.oldFuture = EngineOwnedQueueFutureFiling{};
        holder.oldSourceDecision = {};
        holder.oldSourceOccurrence = {};
        holder.relativeFrame = holder.sequence = 0;
        holder.ticketsConsumed = holder.executionPrepared = holder.physicalPrepared = false;
        holder.spanClockBound = holder.clockBound = holder.oldSourceAborted = false;
        holder.oldLedgerCleared = holder.oldOccurrenceCleared = false;
        holder.sourceClockBound = holder.sourceArmPublished = holder.calendarPinned = false;
        holder.originalAttempt = nullptr;
        holder.originalAttemptGeneration = 0;
        return true;
    }

    bool HasSelectedOriginalQueueReplacement(const EngineImmediateEntryAttempt& attempt) const noexcept {
        const auto queue = attempt.queueContinuationQueue;
        if (!OwnsImmediateEntryAttemptStorage(attempt) || !attempt.active || !queue.Valid() ||
            queue.slot >= originalQueueReplacementHolders.size()) return false;
        const auto& holder = originalQueueReplacementHolders[queue.slot];
        return holder.phase >= OriginalQueueReplacementHolder::Phase::Selected &&
            holder.queue.generation == queue.generation && holder.originalAttempt == &attempt &&
            holder.originalAttemptGeneration == attempt.generation && holder.service.token == attempt.initialService;
    }

    bool CancelOriginalQueueReplacementBody(EngineActionQueueReplacementScratch& scratch) noexcept {
        if (!scratch.nativeReplacement.Valid()) return true;
        auto* held = OriginalQueueReplacementFor(scratch);
        if (held == nullptr || (held->phase != OriginalQueueReplacementHolder::Phase::Preparing &&
                held->phase != OriginalQueueReplacementHolder::Phase::Frozen)) return false;
        auto& holder = *held;
        holder.sourceArmCommit.Release();
        holder.sourceVoiceCommit.Release();
        for (auto& proof : holder.sourceEffectCommits) proof.Release();
        holder.physicalPrepared = false;
        if (holder.ticketsConsumed && holder.selected.Valid() &&
            !originalQueueReplacementSpans->RollbackUnselectedDecisionCommits(holder.selected, holder.domain))
            return false;
        if (!holder.calendar.Abort(mixer.BlocksRendered()) || !holder.conditionalReader.Release()) return false;
        holder.selected.Release();
        holder.sourceTicket.Release();
        holder.memberTicket.Release();
        holder.coordinatorTicket.Release();
        if (holder.domain.Valid() && (!holder.domain.CancelGroups(mixer.BlocksRendered(),
                originalQueueGroupHolders.data(), originalQueueGroupHolders.size()) || !holder.domain.Return())) return false;
        if (holder.coordinator.Valid() && !decisions.CancelPreparedReservation(&holder.coordinator)) return false;
        if (holder.ticketsConsumed && holder.decisions[0].Valid()) {
            if (!decisions.ReclaimCancelled(holder.decisions[0]) &&
                (!decisions.Cancel(holder.decisions[0]) ||
                    !decisions.ReclaimCancelled(holder.decisions[0]))) return false;
            holder.decisions[0] = {};
        }
        if (holder.coordinatorArms.Valid() && !armGroups.CancelBeforePublication(holder.coordinatorArms)) return false;
        holder.coordinatorArms = {};
        holder.entry = {};
        holder.ticketsConsumed = holder.executionPrepared = false;
        holder.clock.Release();
        holder.bankRetention.Release();
        holder.oldBankRetention.Release();
        holder.oldFuture = EngineOwnedQueueFutureFiling{};
        holder.oldSourceDecision = {};
        holder.oldSourceOccurrence = {};
        holder.originalAttempt = nullptr;
        holder.originalAttemptGeneration = 0;
        holder.current.Release();
        holder.serviceRetention.Return();
        holder.preparation = nullptr;
        holder.service = {};
        holder.queue = {};
        holder.decisions = {};
        holder.groups = {};
        holder.relativeFrame = holder.sequence = 0;
        holder.phase = OriginalQueueReplacementHolder::Phase::Empty;
        assert(originalQueueReplacementCount != 0);
        --originalQueueReplacementCount;
        scratch.nativeReplacement = {};
        return true;
    }

    const OriginalQueueConditionalHolder* OriginalQueueConditionalForAttempt(
        const EngineImmediateEntryAttempt& attempt) const noexcept {
        const auto queue = attempt.queueContinuationQueue;
        if (!OwnsImmediateEntryAttemptStorage(attempt) || !attempt.active ||
            !queue.Valid() || queue.slot >= originalQueueConditionalHolders.size()) return nullptr;
        const auto& holder = originalQueueConditionalHolders[queue.slot];
        const auto original = holder.baseline.Token();
        return original.slot == queue.slot && original.generation == queue.generation &&
            holder.service.token == attempt.initialService ? &holder : nullptr;
    }
    OriginalQueueConditionalHolder* OriginalQueueConditionalForAttempt(
        const EngineImmediateEntryAttempt& attempt) noexcept {
        return const_cast<OriginalQueueConditionalHolder*>(
            static_cast<const Engine&>(*this).OriginalQueueConditionalForAttempt(attempt));
    }

    static QueueInstanceTable::OriginalConditionalFutureReservation*
    ResolveOriginalQueueConditionalBaseline(void* opaque, QueueInstanceTable& table,
        QueueInstanceToken queue) noexcept {
        auto* engine = static_cast<Engine*>(opaque);
        if (engine == nullptr || &engine->queueInstances != &table || !queue.Valid() ||
            queue.slot >= engine->originalQueueConditionalHolders.size())
            return nullptr;
        auto& cap = engine->originalQueueConditionalHolders[queue.slot].baseline;
        const auto token = cap.Token();
        return token.slot == queue.slot && token.generation == queue.generation ? &cap : nullptr;
    }

    static void EndOriginalQueueConditionalAssociation(void* opaque, QueueInstanceTable& table,
        QueueInstanceTable::OriginalConditionalFutureReservation& actual) noexcept {
        auto* engine = static_cast<Engine*>(opaque);
        if (engine == nullptr || &engine->queueInstances != &table) return;
        if (!table.IsEndedOriginalConditional(actual)) return;
        const auto queue = actual.Token();
        if (ResolveOriginalQueueConditionalBaseline(opaque, table, queue) != &actual) return;
        auto& holder = engine->originalQueueConditionalHolders[queue.slot];
        const auto& parent = engine->originalQueueLifetimeHolders[queue.slot];
        if (!table.IsOriginalLifetimePinFor(parent, queue)) return;
        // Native End authenticated the full original family after clearing its
        // pending alias. Only this Engine's baseline and clock are returned.
        if (!table.IsOriginalConditionalClockEmpty(holder.clock) &&
            !table.IsOriginalConditionalLaneClockFor(holder.clock, actual, parent)) {
            assert(false);
            return;
        }
        holder.projection.Return();
        holder.initialFutureBank.Release();
        holder.initialFuture = EngineOwnedQueueFutureFiling{};
        holder.initialFutureCaptured = false;
        holder.clock.Release();
        holder.baseline.Release();
        holder.initialGroup = nullptr;
        holder.conditionalGroup = nullptr;
        holder.current = nullptr;
        holder.service = {};
        holder.invocationGeneration = 0;
    }

    bool PrepareOriginalQueueConditional(QueueInstanceToken queue,
        const ConditionalQueuePrepareRequest& request, PreparedQueueConditionalV1* out,
        QueueInstanceTable::OriginalConditionalFutureReservation** issued = nullptr) noexcept {
        if (out == nullptr || (issued != nullptr && *issued != nullptr) ||
            !queue.Valid() || queue.slot >= originalQueueConditionalHolders.size() ||
            queue.slot >= originalQueueLifetimeHolders.size())
            return false;
        auto& holder = originalQueueConditionalHolders[queue.slot];
        const auto& parent = originalQueueLifetimeHolders[queue.slot];
        if (!queueInstances.OriginalConditionalOwnerEmpty(holder.baseline, holder.clock) ||
            !queueInstances.IsOriginalLifetimePinFor(parent, queue)) return false;
        // No post-prepare fallible tail: the real factory writes the stable cap.
        if (!queueInstances.PrepareConditional(queue, request, out, parent, &holder.baseline)) return false;
        if (issued != nullptr) *issued = &holder.baseline;
        return true;
    }

    bool PrepareIssuedOriginalQueueConditionalClock(
        QueueInstanceTable::OriginalConditionalFutureReservation& issued,
        const OriginalQueueFutureGroupRegistration& originalC0,
        const OriginalQueueFutureGroupRegistration& originalC1) noexcept {
        const auto queue = issued.Token();
        if (ResolveOriginalQueueConditionalBaseline(this, queueInstances, queue) != &issued ||
            !queueInstances.IsAssociatedOriginalConditional(issued)) return false;
        auto& holder = originalQueueConditionalHolders[queue.slot];
        if (!queueInstances.PrepareOriginalConditionalLaneClock(issued,
            originalQueueLifetimeHolders[queue.slot], timelinePool, originalC0, originalC1,
            &holder.clock)) return false;
        holder.initialGroup = &originalC0;
        holder.conditionalGroup = &originalC1;
        return true;
    }

    bool AssociateOriginalQueueCurrentFromFactoryOutputs(const EngineActionPlayScratch& scratch,
        const OriginalSingleCurrentHolder& current, EngineImmediateEntryAttempt& attempt) noexcept {
        if (scratch.row == nullptr || scratch.initialOriginalConditional == nullptr ||
            !scratch.prepared || scratch.voiceStartCount != 1 || scratch.variableControl ||
            !scratch.row->queueFirstContinuationLane || !scratch.row->queueTimingPending ||
            current.state != OriginalSingleCurrentHolder::State::Prepared ||
            current.originalTable != &playableInstances || !current.bundle.Valid()) return false;
        const auto& row = *scratch.row;
        const auto queue = scratch.initialOriginalConditional->Token();
        if (!queue.Valid() || queue.slot >= queueInstances.Capacity() ||
            ResolveOriginalQueueConditionalBaseline(this, queueInstances, queue) !=
                scratch.initialOriginalConditional || !row.InvocationValid() ||
            !SameOriginalSingleArm(current.originalCurrentSourceArm, scratch.voiceStarts[0].arm) ||
            current.originalCurrentSourceIdentity != scratch.voiceStarts[0].renderHandle ||
            current.originalRoot != row.rootInstance ||
            !queueInstances.IsOriginalConditionalFutureReservationFor(
                *scratch.initialOriginalConditional, originalQueueLifetimeHolders[queue.slot], queue,
                scratch.initialOriginalConditionalVersion, current.originalCurrentSingleDecision,
                current.originalCurrentOccurrence)) return false;
        bool actualScratch = false;
        for (const auto& prepared : actionPlayScratch) actualScratch |= &prepared == &scratch;
        const auto slot = static_cast<std::uint32_t>(current.originalArranging);
        if (!actualScratch || slot >= originalSingleCurrentHolders.size() ||
            &originalSingleCurrentHolders[slot] != &current) return false;
        auto& holder = originalQueueConditionalHolders[queue.slot];
        if (holder.current != nullptr || holder.initialGroup == nullptr ||
            holder.conditionalGroup == nullptr || !holder.clock.Valid()) return false;
        if (row.identity == attempt.initialService && row.QueueFiling().queueTransitionGroupArmCount <= 1u &&
            row.QueueFiling().queueSourceControlArmCount == 0u && !row.QueueFiling().queueTransition.Valid()) {
            if (!OwnsImmediateEntryAttemptStorage(attempt) || !attempt.active ||
                attempt.root != scratch.initialOriginalGroupRoot ||
                attempt.generation != scratch.initialOriginalGroupAttemptGeneration ||
                attempt.queueContinuationService != row.identity ||
                !attempt.QueueCoordinators().queueContinuationPrimaryGroup.Exact() ||
                attempt.QueueCoordinators().queueContinuationPrimaryGroup.committedRow >= originalQueueGroupHolders.size())
                return false;
            const auto& primary = originalQueueGroupHolders[
                attempt.QueueCoordinators().queueContinuationPrimaryGroup.committedRow];
            OriginalImmediateLaneAttemptLifetimePin lane;
            if (!timelinePool.IsOriginalQueueFutureGroupRegistrationFor(primary,
                    attempt.QueueCoordinators().queueContinuationPrimaryGroup) ||
                !BorrowOriginalImmediateEntryLane(attempt, &lane) ||
                !timelinePool.BindImmediateLaneGroupOwner(primary.Identity(), immediateActionLane,
                    attempt.root, attempt.lane.slot, attempt.lane.generation) ||
                !timelinePool.BindImmediateLaneGroupOwner(holder.conditionalGroup->Identity(),
                    immediateActionLane, attempt.root, attempt.lane.slot, attempt.lane.generation) ||
                !OriginalImmediateLaneConditionalProjection::PrepareUnbound(queueInstances,
                    originalQueueLifetimeHolders[queue.slot], holder.baseline, holder.clock,
                    timelinePool, primary, *holder.conditionalGroup, lane, &holder.projection))
                return false;
        }
        if (holder.projection.Valid()) {
            auto* bank = OriginalQueueBankHolderFor(row.QueueFiling().queueSuccessorProgramBank);
            if (holder.initialFutureCaptured || holder.initialFutureBank.Valid() ||
                (row.QueueFiling().queueSuccessorProgramBank != nullptr &&
                 (bank == nullptr || !bank->baseline.Borrow(&holder.initialFutureBank))))
                return false;
            // This copy is issued beside the genuine Current, conditional and
            // group factories. Reset cannot reconstruct it from selected aliases.
            holder.initialFuture = row.QueueFiling();
            holder.initialFuture.queueConditional = scratch.initialOriginalConditionalVersion;
            holder.initialFutureCaptured = true;
        }
        holder.current = &current;
        holder.service = {row.identity, row.ownerInstance, row.ownerEpoch, row.invocationGeneration};
        holder.invocationGeneration = row.invocationGeneration;
        return true;
    }

    const OriginalQueueConditionalHolder* OriginalQueueConditionalForService(
        const EnginePlayableServiceRow& row) const noexcept {
        const auto queue = row.queueInstance;
        if (!row.InvocationValid() || !queue.Valid() ||
            queue.slot >= originalQueueConditionalHolders.size())
            return nullptr;
        const auto& holder = originalQueueConditionalHolders[queue.slot];
        const auto original = holder.baseline.Token();
        if (!holder.initialFutureCaptured || holder.current == nullptr || !holder.baseline.Valid() ||
            original.slot != queue.slot || original.generation != queue.generation ||
            holder.service.token != row.identity || holder.service.owner != row.ownerInstance ||
            holder.service.epoch != row.ownerEpoch ||
            holder.invocationGeneration != row.invocationGeneration ||
            holder.service.invocationGeneration != row.invocationGeneration || row.quarantined ||
            row.identity.slot >= playableServices.size() || &playableServices[row.identity.slot] != &row ||
            !playablePartitions.Owns(row.partition, row.identity)) return nullptr;
        const auto* bank = holder.initialFuture.queueSuccessorProgramBank;
        if (bank != nullptr && (!holder.initialFutureBank.Valid() ||
                holder.initialFutureBank.Generation() != bank->Generation())) return nullptr;
        return &holder;
    }

    const OriginalQueueConditionalHolder* OriginalQueueInitialFilingFor(
        const EnginePlayableServiceRow& row) const noexcept {
        return row.queueTimingPending ? OriginalQueueConditionalForService(row) : nullptr;
    }

    const EngineQueueFutureFilingLayout& InitialQueueFilingFuture(
        const EnginePlayableServiceRow& row) const noexcept {
        const auto* holder = OriginalQueueInitialFilingFor(row);
        return holder == nullptr ? row.QueueFiling()
                                 : holder->initialFuture;
    }
    EngineQueueFutureFilingLayout& InitialQueueFilingFuture(EnginePlayableServiceRow& row) noexcept {
        return const_cast<EngineQueueFutureFilingLayout&>(
            static_cast<const Engine&>(*this).InitialQueueFilingFuture(row));
    }

    bool BindInitialQueueFilingConditional(const EnginePlayableServiceRow& row,
        const FrontierGroupBinding& binding) noexcept {
        const auto* original = OriginalQueueInitialFilingFor(row);
        return original == nullptr
            ? queueInstances.BindFrontierConditional(row.queueInstance, binding, timelinePool)
            : queueInstances.BindRetainedOriginalFrontierConditional(original->baseline,
                originalQueueLifetimeHolders[row.queueInstance.slot], binding, timelinePool);
    }

    bool BindInitialOriginalQueueConditional(
        const QueueInstanceTable::OriginalConditionalFutureReservation& original,
        const QueueInstanceTable::OriginalLifetimePin& parent,
        const FrontierGroupBinding& binding) noexcept {
        return queueInstances.BindRetainedOriginalFrontierConditional(original, parent, binding, timelinePool);
    }

    bool IsOriginalQueueFilingSelected(const EnginePlayableServiceRow& row) const noexcept {
        const auto* holder = OriginalQueueInitialFilingFor(row);
        if (holder == nullptr) return true;
        const auto& original = holder->initialFuture;
        return row.QueueFiling().queueDecision.slot == original.queueDecision.slot &&
            row.QueueFiling().queueDecision.generation == original.queueDecision.generation &&
            row.QueueFiling().queueConditional.version == original.queueConditional.version &&
            row.QueueFiling().queueSuccessorProgramBank == original.queueSuccessorProgramBank;
    }

    // Reflect original filing into the public alias only while it still selects
    // that same original family. A selected replacement keeps its own data.
    void ReflectOriginalQueueFiling(EnginePlayableServiceRow& row) noexcept {
        const auto* holder = OriginalQueueInitialFilingFor(row);
        if (holder == nullptr) return;
        const auto& original = holder->initialFuture;
        if (row.QueueFiling().queueDecision.slot == original.queueDecision.slot &&
            row.QueueFiling().queueDecision.generation == original.queueDecision.generation &&
            row.QueueFiling().queueConditional.version == original.queueConditional.version &&
            row.QueueFiling().queueSuccessorProgramBank == original.queueSuccessorProgramBank)
            row.QueueFiling() = original;
    }

    bool OriginalQueueOwnsUnpublishedGroup(const EngineImmediateEntryAttempt& attempt,
        const Plan03GroupSliceIdentity& group) const noexcept {
        const auto* holder = OriginalQueueConditionalForAttempt(attempt);
        return holder != nullptr &&
            holder->projection.OwnsUnpublishedGroup(immediateActionLane, attempt.lane, group);
    }

    bool HasOriginalQueueClockPlacement(const EngineImmediateEntryAttempt& attempt) noexcept {
        const auto* holder = OriginalQueueConditionalForAttempt(attempt);
        if (holder == nullptr || holder->current == nullptr) return false;
        const auto* row = ResolvePlayableService(holder->service);
        return row != nullptr && row->InvocationValid() &&
            row->invocationGeneration == holder->invocationGeneration &&
            row->PendingImmediateCohortRoot() == attempt.root && row->queueTimingPending;
    }

    bool BindOriginalQueueClocksFromActualPlacement(EngineImmediateEntryAttempt& attempt,
        PrivateImmediateLanePlacementProof* placement) noexcept {
        if (placement == nullptr || !immediateActionLane.IsPrivatePlacementProofFor(
                *placement, attempt.lane, attempt.root)) return false;
        auto* original = OriginalQueueConditionalForAttempt(attempt);
        if (original != nullptr && original->current != nullptr) {
            auto& holder = *original;
            const auto* row = ResolvePlayableService(holder.service);
            if (row == nullptr || !row->InvocationValid() ||
                row->invocationGeneration != holder.invocationGeneration ||
                row->PendingImmediateCohortRoot() != attempt.root || !row->queueTimingPending) return true;
            if (holder.initialGroup == nullptr || holder.conditionalGroup == nullptr ||
                holder.current->state != OriginalSingleCurrentHolder::State::Prepared ||
                !holder.current->bundle.Valid()) return false;
            PrivateOriginalQueueGroupClockProof proof;
            if (!timelinePool.ProvePrivateOriginalQueueGroupClock(*holder.initialGroup,
                    *holder.conditionalGroup, placement, &proof)) return false;
            const bool bound = BindInitialOriginalSingleQueueClock(holder.baseline,
                originalQueueLifetimeHolders[attempt.queueContinuationQueue.slot], holder.clock,
                proof, holder.current->bundle);
            const bool returned = proof.ReturnPlacement(placement);
            if (!bound || !returned ||
                (holder.projection.Valid() && !holder.projection.BindPrepared())) return false;
        }
        return true;
    }

    bool AbortIssuedOriginalQueueConditional(PreparedQueueConditionalV1& prepared,
        PreparedQueueInstanceV1* acquired,
        QueueInstanceTable::OriginalConditionalFutureReservation*& issued,
        QueueInstanceToken originalQueue, ConditionalQueueVersionToken issuedVersion, DecisionToken decision,
        OccurrenceToken current) noexcept {
        if (issued == nullptr) return true;
        if (!originalQueue.Valid() || originalQueue.slot >= originalQueueConditionalHolders.size() ||
            &originalQueueConditionalHolders[originalQueue.slot].baseline != issued) return false;
        // Native End may already have returned this Engine's own cap. An alias
        // to a new episode is rejected by the original full version check.
        if (!issued->Valid()) { issued = nullptr; return true; }
        const auto queue = issued->Token();
        if (queue.slot != originalQueue.slot || queue.generation != originalQueue.generation ||
            ResolveOriginalQueueConditionalBaseline(this, queueInstances, queue) != issued ||
            issued->Version().version != issuedVersion.version ||
            !queueInstances.IsOriginalConditionalFutureReservationFor(*issued,
                originalQueueLifetimeHolders[queue.slot], queue, issuedVersion, decision, current))
            return false;
        bool ended = false;
        if (prepared.Valid()) {
            if (prepared.Version().version != issuedVersion.version) return false;
            ended = queueInstances.CancelPreparedConditional(&prepared);
        } else if (acquired != nullptr && acquired->Valid()) {
            ended = queueInstances.RollbackInitializingConditional(*acquired,
                queue, issuedVersion, decision);
        } else {
            ended = queueInstances.RetireConditionalForAbort(queue, issuedVersion, decision);
        }
        if (!ended) return false;
        // The authentic native End callback, not this alias, returned own loans.
        issued = nullptr;
        return true;
    }

    bool PrepareOriginalQueueConditionalClock(QueueInstanceToken queue,
        const OriginalQueueFutureGroupRegistration& originalC0,
        const OriginalQueueFutureGroupRegistration& originalC1) noexcept {
        auto* baseline = ResolveOriginalQueueConditionalBaseline(this, queueInstances, queue);
        if (baseline == nullptr || !queueInstances.IsAssociatedOriginalConditional(*baseline)) return false;
        auto& holder = originalQueueConditionalHolders[queue.slot];
        return queueInstances.PrepareOriginalConditionalLaneClock(*baseline,
            originalQueueLifetimeHolders[queue.slot], timelinePool, originalC0, originalC1, &holder.clock);
    }

    bool BorrowOriginalQueueConditional(QueueInstanceToken queue,
        QueueInstanceTable::OriginalConditionalFutureReservation* out) noexcept {
        auto* cap = ResolveOriginalQueueConditionalBaseline(this, queueInstances, queue);
        return cap != nullptr && queueInstances.IsAssociatedOriginalConditional(*cap) && cap->Borrow(out);
    }

    bool BorrowOriginalQueueConditionalClock(QueueInstanceToken queue,
        QueueInstanceTable::OriginalConditionalLaneClock* out) noexcept {
        auto* cap = ResolveOriginalQueueConditionalBaseline(this, queueInstances, queue);
        return cap != nullptr && queueInstances.IsAssociatedOriginalConditional(*cap) &&
            originalQueueConditionalHolders[queue.slot].clock.Borrow(out);
    }

    MonitorLifecycleFacts CaptureMonitorOwner(PlayableInstanceHandle owner) const noexcept {
        MonitorLifecycleFacts facts{};
        if (!playableInstances.CaptureMonitorFacts(owner, &facts)) {
            facts.identityObservationIncomplete = owner != 0;
            return facts;
        }
        facts.parentPlayable = owner;
        facts.resourceOwner = owner;
        return facts;
    }

    bool PrepareOriginalQueueAcquire(const QueueInstanceCreateRequest& create,
        PreparedQueueInstanceV1* prepared,
        const OriginalPlayableServiceLifetimeHolder* service = nullptr,
        EngineActionPlayScratch* checkpointOwner = nullptr) noexcept {
        if (service != nullptr && (service->engine_ != this || service->service_ == nullptr ||
                !service->service_->actionPrepared || !MatchesOriginalPlayableServicePackDuty(*service) ||
                playablePartitions.Published(service->service_->partition, service->service_->identity) ||
                create.world != service->invocation_->worldGeneration ||
                create.projectId != service->invocation_->projectId)) return false;
        QueueCheckpointTable::BackingPreparation checkpoint;
        EngineFrontierAttemptToken borrower{};
        if (create.resumePosition) {
            if (checkpointOwner != nullptr) {
                borrower = NativeBackingTokenFor(*checkpointOwner);
                if (!borrower.Valid() || !checkpointOwner->queueRolesBorrowed ||
                    !checkpointOwner->queueCheckpointBacking.Split(1, 0, &checkpoint)) return false;
            } else if (!queueCheckpoints.PrepareBacking(1, &checkpoint)) return false;
        }
        const auto returnCheckpoint = [&]() noexcept {
            if (!borrower.Valid()) return;
            const bool returned = checkpoint.ReturnTo(&checkpointOwner->queueCheckpointBacking);
            assert(returned);
            (void)returned;
        };
        QueueInstanceTable::OriginalLifetimePin original;
        auto observedCreate = create;
        observedCreate.monitorIdentity = CaptureMonitorOwner(create.rootInstance);
        if (!queueInstances.PrepareAcquire(observedCreate, prepared, &original)) {
            returnCheckpoint();
            return false;
        }
        const auto token = original.Token();
        if (token.slot >= originalQueueLifetimeHolders.size() ||
            originalQueueLifetimeHolders[token.slot].Valid()) {
            const bool cancelled = queueInstances.CancelPreparedAcquire(prepared, &original);
            assert(cancelled);
            returnCheckpoint();
            runtimeRecreateRequired = true;
            return false;
        }
        originalQueueLifetimeHolders[token.slot] = std::move(original);
        auto& factory = originalQueueFactoryLifetimeHolders[token.slot];
        factory.engine_ = this;
        factory.preparation_ = prepared;
        factory.service_ = service;
        factory.queue_ = token;
        factory.create_ = create;
        factory.checkpointBacking_ = std::move(checkpoint);
        factory.checkpointBorrower_ = borrower;
        factory.state_ = OriginalQueueFactoryLifetimeHolder::State::Acquired;
        return true;
    }

    bool PrepareOriginalQueueCheckpointReturn(const OriginalQueueFactoryLifetimeHolder& factory,
        QueueCheckpointTable::BackingPreparation** destination) noexcept;
    bool FinishOriginalQueueCheckpointReturn(OriginalQueueFactoryLifetimeHolder& factory,
        QueueCheckpointTable::BackingPreparation* destination) noexcept;

    bool StoreOriginalQueueCheckpoint(QueueInstanceToken token, const QueueCheckpointKey& key,
        const QueueCheckpointImage& image) noexcept {
        if (!token.Valid() || token.slot >= originalQueueFactoryLifetimeHolders.size() ||
            token.slot >= originalQueueLifetimeHolders.size()) return false;
        auto& factory = originalQueueFactoryLifetimeHolders[token.slot];
        const auto& create = factory.create_;
        if (factory.engine_ != this || factory.queue_.slot != token.slot ||
            factory.queue_.generation != token.generation || !factory.checkpointBacking_.Valid() ||
            !queueInstances.IsOriginalLifetimePinFor(originalQueueLifetimeHolders[token.slot], token) ||
            key.world != create.world || key.containerStableId != create.containerStableId ||
            key.programSemanticFingerprint != create.programSemanticFingerprint || key.resetEpoch != create.resetEpoch ||
            key.emitterSlot != create.emitterSlot || key.emitterGeneration != create.emitterGeneration) return false;
        const auto use = factory.checkpointBacking_.Use();
        return queueCheckpoints.Store(key, image);
    }

    bool JoinOriginalQueueFactoryLifetime(const PreparedQueueInstanceV1& preparation,
        const PlayableInstanceTable::PreparedParentLinksProof& links) noexcept {
        const auto token = preparation.Token();
        if (!token.Valid() || token.slot >= originalQueueFactoryLifetimeHolders.size()) return false;
        auto& factory = originalQueueFactoryLifetimeHolders[token.slot];
        const auto* service = factory.service_;
        if (factory.engine_ != this || factory.preparation_ != &preparation ||
                factory.state_ != OriginalQueueFactoryLifetimeHolder::State::Acquired ||
                factory.queue_.slot != token.slot || factory.queue_.generation != token.generation ||
                service == nullptr || service->service_ == nullptr || !service->service_->actionPrepared ||
                !MatchesOriginalPlayableServicePackDuty(*service) ||
                playablePartitions.Published(service->service_->partition, service->service_->identity) ||
                !queueInstances.IsOriginalLifetimePinFor(originalQueueLifetimeHolders[token.slot], token))
            return false;
        const auto queueRoot = factory.create_.rootInstance;
        if ((!playableInstances.MatchesPreparedOriginalAncestry(links, queueRoot, service->reference_.owner) &&
                !playableInstances.MatchesPreparedOriginalAncestry(links, service->reference_.owner, queueRoot)) ||
                ResolveOriginalPlayableBaseline(this, playableInstances, queueRoot) == nullptr) return false;
        factory.state_ = OriginalQueueFactoryLifetimeHolder::State::Joined;
        if (originalQueueFactoryLifetimeHookForTest != nullptr)
            originalQueueFactoryLifetimeHookForTest(*this, factory, originalQueueFactoryLifetimeContextForTest);
        return true;
    }

    bool BorrowOriginalQueueFactoryLifetime(const OriginalQueueFactoryLifetimeHolder& factory,
        OriginalQueueFactoryLifetimePin* out) noexcept {
        const auto token = factory.queue_;
        if (out == nullptr || out->engine_ != nullptr || factory.engine_ != this ||
                factory.state_ != OriginalQueueFactoryLifetimeHolder::State::Joined || !token.Valid() ||
                token.slot >= originalQueueFactoryLifetimeHolders.size() ||
                &originalQueueFactoryLifetimeHolders[token.slot] != &factory || factory.service_ == nullptr ||
                !queueInstances.IsOriginalLifetimePinFor(originalQueueLifetimeHolders[token.slot], token))
            return false;
        const auto* root = ResolveOriginalPlayableBaseline(this, playableInstances, factory.create_.rootInstance);
        OriginalQueueFactoryLifetimePin loan;
        if (root == nullptr || !originalQueueLifetimeHolders[token.slot].Borrow(&loan.queue_) ||
                !factory.service_->Borrow(&loan.service_) || !root->Borrow(&loan.root_)) return false;
        loan.engine_ = this;
        loan.create_ = factory.create_;
        *out = std::move(loan);
        return true;
    }

    bool CloneOriginalQueueFactoryLifetime(const OriginalQueueFactoryLifetimePin& original,
        OriginalQueueFactoryLifetimePin* out) noexcept {
        if (out == nullptr || out->engine_ != nullptr || original.engine_ != this || !original.Valid()) return false;
        OriginalQueueFactoryLifetimePin loan;
        if (!original.queue_.Borrow(&loan.queue_) || !original.service_.Borrow(&loan.service_) ||
                !original.root_.Borrow(&loan.root_)) return false;
        loan.engine_ = this;
        loan.create_ = original.create_;
        *out = std::move(loan);
        return true;
    }

    void CloseOriginalQueueFactoryLifetime(QueueInstanceToken token) noexcept {
        if (!token.Valid() || token.slot >= originalQueueFactoryLifetimeHolders.size()) return;
        auto& factory = originalQueueFactoryLifetimeHolders[token.slot];
        if (factory.engine_ == this && factory.queue_.slot == token.slot &&
                factory.queue_.generation == token.generation)
            factory.state_ = OriginalQueueFactoryLifetimeHolder::State::Closing;
    }

    bool CancelOriginalQueueAcquire(PreparedQueueInstanceV1* prepared) noexcept {
        if (prepared == nullptr) return false;
        const auto token = prepared->Token();
        if (!token.Valid() || token.slot >= originalQueueLifetimeHolders.size()) return false;
        auto& original = originalQueueLifetimeHolders[token.slot];
        auto& factory = originalQueueFactoryLifetimeHolders[token.slot];
        QueueCheckpointTable::BackingPreparation* destination = nullptr;
        if (!PrepareOriginalQueueCheckpointReturn(factory, &destination)) return false;
        CloseOriginalQueueFactoryLifetime(token);
        if (!queueInstances.CancelPreparedAcquire(prepared, original.Valid() ? &original : nullptr)) return false;
        return FinishOriginalQueueCheckpointReturn(factory, destination);
    }

    bool ReleaseOriginalQueue(QueueInstanceToken token) noexcept {
        if (!token.Valid() || token.slot >= originalQueueLifetimeHolders.size()) return false;
        auto& original = originalQueueLifetimeHolders[token.slot];
        auto& factory = originalQueueFactoryLifetimeHolders[token.slot];
        QueueCheckpointTable::BackingPreparation* destination = nullptr;
        if (!PrepareOriginalQueueCheckpointReturn(factory, &destination)) return false;
        CloseOriginalQueueFactoryLifetime(token);
        if (!queueInstances.Release(token, original.Valid() ? &original : nullptr)) return false;
        return FinishOriginalQueueCheckpointReturn(factory, destination);
    }

    // Bind private initial Single geometry from the original factory outputs.
    // Diagnostic C0 output selection does not supply the Current or its clock.
    bool BindInitialOriginalSingleQueueClock(
        const QueueInstanceTable::OriginalConditionalFutureReservation& original,
        const QueueInstanceTable::OriginalLifetimePin& parentOriginal,
        const QueueInstanceTable::OriginalConditionalLaneClock& clock,
        const PrivateOriginalQueueGroupClockProof& privateClock,
        const PreparedSingleCurrentOriginBundle& current) noexcept {
        if (current.sources_ != &originalSourceSubmissions || current.voices_ != &voices ||
            current.arms_ != &timelineArms || current.decisions_ != &decisions ||
            current.occurrences_ != &timelineOccurrences || current.logicalVoices_ != &ledger ||
            current.instances_ != &playableInstances || !current.ObserveInstalled() ||
            !queueInstances.IsOriginalConditionalLaneClockFor(clock, original, parentOriginal)) return false;
        const auto root = current.logical_.parent.internalToken;
        const auto parent = playableInstances.Classify(root);
        const auto arranging = playableInstances.Classify(current.arrangingInstance_);
        if (!parent.live || parent.lifecycle != PlayableLifecycle::Active || parent.scheduleEpoch == 0 ||
            !arranging.live || arranging.lifecycle != PlayableLifecycle::Active) return false;
        // The genuine Bundle authenticated the prepared ancestry. Recheck the
        // same receiver's live full-generation chain before binding its Current.
        auto child = current.arrangingInstance_;
        bool sameRoot = child == root;
        for (std::uint32_t depth = 0; !sameRoot && depth < playableInstances.Capacity(); ++depth) {
            const auto node = playableInstances.Classify(child);
            if (!node.live || node.lifecycle != PlayableLifecycle::Active || node.parent == 0) return false;
            child = node.parent;
            sameRoot = child == root;
        }
        if (!sameRoot) return false;
        TimelineArmObservation installed{};
        if (!timelineArms.Observe(current.arm_, &installed) || installed.armedCompletionSequence == 0 ||
            installed.startedBlockSequence != 0 || installed.logicalCompletionSequence != 0 ||
            installed.tombstoneAudioSequence != 0 ||
            (installed.state != ArmState::Armed && installed.state != ArmState::Published)) return false;
        return queueInstances.BindPrivateInitialOriginalConditionalClock(original, parentOriginal, clock,
            timelinePool, privateClock, timelineArms, decisions, current.arm_, root, parent.scheduleEpoch,
            current.logical_.completionMutation.occurrence, current.logical_.completionMutation.owner,
            installed.armedCompletionSequence);
    }

    bool BorrowOriginalQueueLifetime(const EnginePlayableServiceReference& service,
        QueueInstanceTable::OriginalLifetimePin* out) noexcept {
        auto* row = ResolvePlayableService(service);
        if (row == nullptr || !row->InvocationValid() ||
            !playablePartitions.Owns(row->partition, row->identity) ||
            !row->queueInstance.Valid() || row->queueInstance.slot >= queueInstances.Capacity())
            return false;
        const auto& original = originalQueueLifetimeHolders[row->queueInstance.slot];
        return queueInstances.IsOriginalLifetimePinFor(original, row->queueInstance) &&
            original.Borrow(out);
    }

    bool BorrowOriginalQueueCurrentBank(const EnginePlayableServiceReference& service,
        EngineQueueProgramBank::OriginalLifetimePin* out) noexcept {
        auto* row = ResolvePlayableService(service);
        if (row == nullptr || !row->InvocationValid() || !row->HasFeatureArrays() ||
            !playablePartitions.Owns(row->partition, row->identity)) return false;
        auto* original = OriginalQueueBankHolderFor(row->QueuePayload().queueCurrentProgramBank);
        return original != nullptr && original->baseline.Borrow(out);
    }

    bool BorrowOriginalQueueSuccessorBank(const EnginePlayableServiceReference& service,
        EngineQueueProgramBank::OriginalLifetimePin* out) noexcept {
        auto* row = ResolvePlayableService(service);
        if (row == nullptr || !row->InvocationValid() || !row->HasFeatureArrays() ||
            !playablePartitions.Owns(row->partition, row->identity)) return false;
        auto* original = OriginalQueueBankHolderFor(row->QueueFiling().queueSuccessorProgramBank);
        return original != nullptr && original->baseline.Borrow(out);
    }

    Plan03GroupSliceIdentity ConsumeOriginalQueueLaneGroup(
        PrivateImmediateLaneGroupTransferProof&& proof,
        OriginalQueueFutureGroupRegistration** issued = nullptr) noexcept {
        const auto identity = proof.Identity();
        if ((issued != nullptr && *issued != nullptr) || !identity.Exact() || identity.committedRow >= originalQueueGroupHolders.size() ||
            originalQueueGroupHolders.data() == nullptr) return {};
        auto& original = originalQueueGroupHolders[identity.committedRow];
        if (!timelinePool.ConsumePrivateImmediateLaneGroupTransfer(std::move(proof), &original))
            return {};
        assert(timelinePool.IsOriginalQueueFutureGroupRegistrationFor(original, identity));
        if (issued != nullptr) *issued = &original;
        return identity;
    }

    bool ReserveOriginalQueueLaneGroup(std::uint64_t cohortRoot,
        std::uint64_t anchor, const Plan03GroupSliceInput& input,
        const TimelineArmTable& arms, Plan03GroupSliceIdentity* out,
        PreparedPrivateImmediateLaneGroup* callerOriginal = nullptr,
        OriginalQueueFutureGroupRegistration** issued = nullptr) noexcept {
        if (issued != nullptr && (*issued != nullptr || callerOriginal != nullptr)) return false;
        if (callerOriginal != nullptr)
            return timelinePool.ReserveImmediateLaneGroup(cohortRoot, anchor, input, arms,
                out, callerOriginal);
        if (out == nullptr || out->Valid()) return false;
        Plan03GroupSliceIdentity identity{};
        PreparedPrivateImmediateLaneGroup original;
        if (!timelinePool.ReserveImmediateLaneGroup(cohortRoot, anchor, input, arms,
                &identity, &original)) return false;
        PrivateImmediateLaneGroupTransferProof proof;
        if (timelinePool.ProvePrivateImmediateLaneGroupTransfer(original, cohortRoot,
                anchor, input, arms, &proof)) {
            const auto consumed = ConsumeOriginalQueueLaneGroup(std::move(proof), issued);
            if (consumed.Exact()) { *out = consumed; return true; }
        }
        proof.Release();
        const bool cancelled = timelinePool.CancelPrivateImmediateLaneGroup(&original, &identity);
        assert(cancelled);
        if (!cancelled) runtimeRecreateRequired = true;
        return false;
    }

    bool CancelOriginalQueueLaneGroup(std::uint64_t cohortRoot,
        const Plan03GroupSliceIdentity& identity) noexcept {
        if (identity.committedRow >= originalQueueGroupHolders.size()) return false;
        auto& original = originalQueueGroupHolders[identity.committedRow];
        return timelinePool.CancelPrivateImmediateLaneGroup(cohortRoot, identity,
            original.Valid() ? &original : nullptr);
    }

    bool CancelOriginalQueueLaneGroups(std::uint64_t cohortRoot,
        std::uint64_t completedAudioSequence) noexcept {
        return timelinePool.CancelImmediateLaneGroups(cohortRoot, completedAudioSequence,
            originalQueueGroupHolders.data(), originalQueueGroupHolders.size());
    }

    bool BorrowOriginalQueueFutureGroup(const EnginePlayableServiceReference& service,
        const Plan03GroupSliceIdentity& identity,
        OriginalQueueFutureGroupLifetimePin* out) noexcept {
        auto* row = ResolvePlayableService(service);
        if (row == nullptr || !row->InvocationValid() ||
            !playablePartitions.Owns(row->partition, row->identity) ||
            identity.committedRow >= originalQueueGroupHolders.size()) return false;
        const auto sameDecision = [](DecisionToken left, DecisionToken right) noexcept {
            return left.Valid() && right.Valid() && left.slot == right.slot &&
                left.generation == right.generation;
        };
        if (!sameDecision(identity.decision, row->initialQueueEntryDecision) &&
            (!row->HasFeatureArrays() || !sameDecision(identity.decision, row->QueueFiling().queueDecision)) &&
            !sameDecision(identity.decision, row->queueArrivalDecision)) return false;
        const auto& original = originalQueueGroupHolders[identity.committedRow];
        return timelinePool.IsOriginalQueueFutureGroupRegistrationFor(original, identity,
                row->Invocation().actionFiring) && original.BorrowLifetime(out);
    }

    const EngineConfig config;
    MonitorRequestSource monitorRequests{AllocateMonitorEngineInstance()};
    const PlayableStorageLayout playableStorage;
    ResampleTables tables;
    MultirateResampleTables multirateTables;
    BiquadTrigTable biquadTrig;
    PCMRegistry registry;
    // Logic-pack ledger (section 19): applied entries + dependency edges
    // per loaded v2 pack, keyed by the pack handle. Declared before
    // packs only for grouping; construction is independent. Logic thread
    // only.
    PackLogicStore logicPacks;
    PackComposition packComposition;
    PackTable packs; // registry entries of loaded packs reference the
                          // pack buffers; both live for the engine's life,
                          // and teardown never dereferences sample data
    StreamManager streams; // owns the stream rings voices point into, and
                           // the streaming thread; declared before voices
                           // so it outlives every context holding a ring
                           // pointer, and its thread joins only after the
                           // device callback (declared last) has stopped
    EffectPoolBank effectBank; // declared before voices: the pool the voice
                               // apply path resolves instances from
    DecisionTable decisions;
    TransitionInstancePool transitionInstances;
    ArmGroupTable armGroups;
    TimelineArmTable timelineArms;
    std::vector<EngineInitialFrontierArmPin> initialFrontierArmPins;
    std::vector<TimelineArmTable::BoundArmPublicationInput> initialRootArmPublicationInputs;
    std::vector<TimelineArmTable::Row*> initialRootArmPublicationRows;
    std::vector<DecisionTable::FrontierPublicationProof> initialRootActivationPublicationProofs;
    // Original bank storage outlives every genuine Engine-held loan.
    EngineQueueScheduleStorage queueScheduleStorage;
    StableSlotStorage<EngineQueueProgramBank> queueProgramBanks;
    // Original Queue storage outlives every genuine Engine-held loan.
    QueueInstanceTable queueInstances;
    // Bus-topology plan pool: logic constructs plans, the audio thread consumes
    // and never frees them (retired through the pool's own queue). Declared
    // before buses because the bus tree installs plans from it and retires
    // superseded ones into it.
    TopologyPlanPool topologyPlans;
    TopologyPlanPool preparedTopologyPlans;
    std::shared_ptr<ProjectGenerationReservationDomainV1>
        projectGenerationReservationDomain =
            std::make_shared<ProjectGenerationReservationDomainV1>(
                ProjectGenerationReservationDomainV1{this, 1, true});
    std::vector<PreparedOwnerClaimV1> preparedOwnerClaims;
    std::vector<std::uint32_t> preparedPreviewOwnerSlots;
    std::vector<ProjectGenerationInstallAckIdentityV1>
        projectGenerationInstallAckSideTable;
    ProjectGenerationInstallAckRingV1 projectGenerationInstallAckRing;
    DynamicRouteSystem dynamicRoutes;
    // Logic-side caller-buffer capture scratch. Sized once at Init so route
    // capture can preserve its canonical public ordering without allocating.
    std::vector<VnEffectDynamicRouteInfo> dynamicRouteCaptureScratch;
    // Logic-side fixed scratch for the combined signal/control influence DAG.
    // Wiring and route preflights reuse it; no public mutation allocates a walk.
    std::vector<std::uint8_t> influenceVisited;
    std::vector<std::uint32_t> influenceStack;
    SourceTerminalRecords sourceTerminals;
    OccurrenceStatePool timelineOccurrences;
    VoicePool voices;
    StableRowStorage<EngineTimelineVoiceInstallIdentity> timelineVoiceInstallIdentities;
    BusTree buses;
    LogicCommitCoordinator logicCommits;
    ActionCommitBundle actionCommitBundle;
    TransportEpochOwner transportEpochOwner;
    ScheduledTransactionPool timelinePool;
    QueueCheckpointTable queueCheckpoints;
    // Fixed, producer-only storage. Copy/reset of service or scratch rows cannot
    // move these native loans. Destruction precedes Pool, Queue table, and banks.
    StableSlotStorage<OriginalQueueBankHolder> originalQueueBankHolders;
    StableRowStorage<QueueInstanceTable::OriginalLifetimePin> originalQueueLifetimeHolders;
    StableRowStorage<OriginalQueueFactoryLifetimeHolder> originalQueueFactoryLifetimeHolders;
    void (*originalQueueFactoryLifetimeHookForTest)(Engine&,
        const OriginalQueueFactoryLifetimeHolder&, void*) = nullptr;
    void* originalQueueFactoryLifetimeContextForTest = nullptr;
    StableRowStorage<OriginalQueueConditionalHolder> originalQueueConditionalHolders;
    std::unique_ptr<PrivateSelectedQueueFutureSpanPool> originalQueueReplacementSpans;
    StableRowStorage<OriginalQueueReplacementHolder> originalQueueReplacementHolders;
    std::unique_ptr<OriginalConditionalReplacementBatch> originalQueueReplacementBatch;
    std::uint32_t originalQueueReplacementAbandonCount = 0;
    std::uint32_t originalQueueReplacementCount = 0;
    StableRowStorage<OriginalQueueFutureGroupRegistration> originalQueueGroupHolders;
    std::size_t originalQueueGroupCapacity = 0;
    // Stop cancellation holders must return before their native table dies.
    PlayableInstanceTable playableInstances;
    // Lane fence references must be destroyed before their original backing.
    std::vector<EngineAuthoredStopRoot> authoredStopRoots;
    std::uint32_t authoredStopRootPrefixCount = 0;
    StableRowStorage<EngineAuthoredStopResource> authoredStopResources;
    ImmediateActionLane immediateActionLane;
    VariableSliceTable variableSlices;
    ParamSystem params;
    VariableServiceV1 variables;
    std::vector<EngineActionFiringPlanRow> actionFiringPlanScratch;
    std::vector<ActionFiringOwnerStamp> actionFiringOwnerReleaseScratch;
    OriginalPackIdentityLease originalActionFiringPreparation;
    std::vector<EngineOriginalActionFiringPackHolder> originalActionFiringPackHolders;
    std::vector<EngineActionStepPlanRow> actionStepPlanScratch;
    std::vector<PreparedVariableWriteToken> actionVariableWriteScratch;
    std::vector<PreparedSelectionResetV1> actionSelectionResetScratch;
    std::vector<PendingSelectionBatchCommitV1> actionFixedSelectionCommitInputs;
    std::vector<PreparedQueueResetSetV1> actionQueueResetScratch;
    std::vector<PreparedQueueCheckpointResetV1>
        actionQueueCheckpointResetScratch;
    // The first Stop/Reset lease waits here until actual root capture allocates its record.
    // A failed native Return retains the same cap until joined shutdown.
    EngineOriginalPackIdentityLease originalDeferredTargetPreparation;
    std::vector<EngineActionDeferredScratch> actionDeferredScratch;
    std::vector<EngineOriginalDeferredPackHolder> originalDeferredPackHolders;
    // Stable backing. Native tables outlive every genuine cap and bundle.
    StableRowStorage<PlayableInstanceTable::OriginalLifetimePin> originalPlayableLifetimeHolders;
    StableRowStorage<OriginalSingleCurrentHolder> originalSingleCurrentHolders;
    std::uint32_t originalSingleCurrentCount = 0;
    void (*originalPlayableTreePreparationHookForTest)(Engine&, PlayableTreeExecutor&, void*) = nullptr;
    void* originalPlayableTreePreparationContextForTest = nullptr;
    ContainerPlaybackTable containerPlayback;
    VariableControlInstanceTable variableControlInstances;
    // Logic-side identity layer; prepared owner proofs borrow its actual rows.
    VoiceLedger ledger;
    EnginePlayablePartitionPool playablePartitions;
    EngineFrontierOwnerLedger::Limits actionPlayOwnerLimits{};
    StableSlotStorage<EngineActionPlayStorage> actionPlayStorage;
    StableSlotStorage<EngineActionPlayScratch> actionPlayScratch;
    // One default-state allocation only; never a native reservation or owner.
    EngineActionPlayScratch::NativeVoiceSide ordinarySingleVoiceBacking;
    std::vector<EngineFrontierAttemptToken> actionPlayAttemptTokens;
    std::vector<OriginalPendingVariableGroupHolder*> initialPendingVariableOwnerScratch;
    std::vector<EngineInitialQueueEntryDecisionOwner>
        initialQueueEntryDecisionOwnersScratch;
    std::vector<DecisionParticipantRef>
        initialQueueCoordinatorParticipantsScratch;
    std::uint32_t initialQueueEntryDecisionOwnerCount = 0;
    std::unique_ptr<VariableControlRetargetTreeScratch> variableControlRetargetScratch;
    // Serial logic-side service preparation, separate from Action start scratch.
    bool servicePreparedScheduleLeavesInUse = false;
    std::vector<EngineActionStopScratch> actionStopScratch;
    std::vector<std::uint64_t> actionPlayFrameScratch;
    std::vector<ImmediateLaneEvent> pendingImmediateLaneEventsScratch;
    std::vector<EngineImmediateLaneDemandQuantum> initialLaneDemandEventQuantaScratch;
    std::vector<EngineImmediateLaneDemandQuantum> initialLaneDemandGroupQuantaScratch;
    StableRowStorage<EngineInitialQueueTimingMember>
        initialQueueTimingMembersScratch;
    StableRowStorage<TransitionInstancePool::PrivateTimingProof>
        initialQueueTransitionTimingProofsScratch;
    StableRowStorage<QueueInstanceTable::PrivateConditionalTimingProof>
        initialQueueConditionalTimingProofsScratch;
    StableRowStorage<CompositeCompletionOwnerPool::PrivateReanchorProof>
        initialQueueCurrentCompositeProofsScratch;
    StableRowStorage<CompositeCompletionOwnerPool::PrivateReanchorProof>
        initialQueueSuccessorCompositeProofsScratch;
    StableRowStorage<EngineInitialQueueTimingProgram>
        initialQueueTimingProgramsScratch;
    StableRowStorage<EngineInitialQueueTimingEvent>
        initialQueueTimingEventsScratch;
    StableRowStorage<Plan03GroupSliceInput>
        initialQueueTimingGroupsScratch;
    StableRowStorage<Plan03ImmediateLaneGroupDescriptor>
        initialQueueLaneGroupsScratch;
    StableRowStorage<EngineImmediateEntryAttempt> immediateEntryAttempts;
    std::uint32_t activeImmediateEntryAttemptCount = 0;
    void SetImmediateEntryAttemptActive(EngineImmediateEntryAttempt& attempt,
                                        bool active) noexcept {
        if (attempt.active == active) return;
        if (active) {
            ++activeImmediateEntryAttemptCount;
        } else {
            assert(activeImmediateEntryAttemptCount != 0);
            --activeImmediateEntryAttemptCount;
        }
        attempt.active = active;
    }
    StableRowStorage<EngineQueueContinuationRecord> queueContinuationRecords;
    StableRowStorage<EngineQueueFutureRecord> queueFutureRecords;
    void (*voiceCommandBatchPublishHookForTest)(Engine&, void*) = nullptr;
    void* voiceCommandBatchPublishHookContextForTest = nullptr;
    void (*immediateEntryAfterPublishHookForTest)(Engine&, void*) = nullptr;
    void* immediateEntryAfterPublishContextForTest = nullptr;
    StableRowStorage<EngineActionQueueReplacementScratch>
        actionQueueReplacementScratch;
    // Tokens and descriptor pins must be destroyed before their owning tables.
    std::vector<EngineDecisionRetirement> decisionRetirements;
    std::vector<EnginePlayableStartRow> playableStarts;
    StableRowStorage<EnginePlayableServiceRow> playableServices;
    StableSlotStorage<EngineMemberProgramRecord> memberProgramRecords;
    std::uint32_t retiredMemberProgramCount = 0;
    StableSlotStorage<EnginePlayableMemberRecord> playableMemberRecords;

    EnginePlayableServiceRow* MemberRelationOwner(std::size_t logicalSlot) noexcept {
        if (logicalSlot >= playableStorage.memberCount || playableStorage.memberStride == 0) return nullptr;
        const auto partition = static_cast<std::uint32_t>(logicalSlot / playableStorage.memberStride);
        const auto token = playablePartitions.CaptureOwner(partition);
        if (!token.Valid() || token.slot >= playableServices.size()) return nullptr;
        auto& row = playableServices[token.slot];
        return row.identity == token && row.partition.slot == partition ? &row : nullptr;
    }
    std::uint32_t* MemberRelationLink(EnginePlayableServiceRow& row, std::uint32_t offset) noexcept {
        auto* link = &row.memberRelationHead;
        while (*link != UINT32_MAX && memberRelations[*link].logicalOffset < offset)
            link = &memberRelations[*link].next;
        return link;
    }
    EnginePlayableMemberReference ReadPlayableMemberReference(std::size_t logicalSlot) const noexcept {
        auto& engine = *const_cast<Engine*>(this);
        auto* row = engine.MemberRelationOwner(logicalSlot);
        if (row == nullptr) return {};
        const auto offset = static_cast<std::uint32_t>(logicalSlot % playableStorage.memberStride);
        const auto link = *engine.MemberRelationLink(*row, offset);
        if (link == UINT32_MAX || memberRelations[link].logicalOffset != offset) return {};
        return ExpandMemberRelation(memberRelations[link]);
    }
    EnginePlayableMemberReference ExpandMemberRelation(const EnginePlayableMemberRelation& node) const noexcept {
        EnginePlayableMemberReference reference{};
        reference.service.token = node.service;
        reference.memberOffset = node.memberOffset;
        reference.leaf = node.leaf;
        reference.leafEpoch = node.leafEpoch;
        reference.authoredChildOrdinal = node.authoredChildOrdinal;
        // A relation remains a reader even after its leaf stops resolving.
        if (node.service.slot < playableServices.size()) {
            const auto& target = playableServices[node.service.slot];
            if (target.identity == node.service) {
                reference.service.owner = target.ownerInstance;
                reference.service.epoch = target.ownerEpoch;
                reference.service.invocationGeneration = target.invocationGeneration;
                reference.partitionGeneration = target.partition.generation;
            }
        }
        return reference;
    }
    void InitializeMemberRelations(std::uint32_t previous) noexcept {
        for (auto index = previous; index < memberRelations.ConstructedCount(); ++index) {
            memberRelations[index].next = freeMemberRelation;
            freeMemberRelation = index;
        }
    }
    bool PrepareMemberRelationBacking(std::uint32_t count, StableStoragePreparation* out) noexcept {
        const auto previous = memberRelations.ConstructedCount();
        const bool prepared = memberRelations.PrepareBacking(
            std::min(count, memberRelations.Capacity()), occupiedMemberRelations, out,
            [&](std::uint32_t required) noexcept {
                return required <= previous || AllowNativeBackingAllocation();
            });
        InitializeMemberRelations(previous);
        return prepared;
    }
    bool PrepareMemberRelationWrite(std::uint32_t count, StableStoragePreparation* out) noexcept {
        // An enclosing native preparation already covers its publication writes.
        return memberRelations.PreparedUseActive() || PrepareMemberRelationBacking(count, out);
    }
    void RemoveMemberRelation(EnginePlayableServiceRow& row, std::uint32_t* link) noexcept {
        const auto index = *link;
        auto& node = memberRelations[index];
        *link = node.next;
        node = {};
        node.next = freeMemberRelation;
        freeMemberRelation = index;
        --row.memberRelationCount;
        --occupiedMemberRelations;
    }
    void ClearPlayableMemberRelations(EnginePlayableServiceRow& row) noexcept {
        while (row.memberRelationHead != UINT32_MAX) RemoveMemberRelation(row, &row.memberRelationHead);
    }
    void ClearPlayableMemberReferences(std::size_t base, std::size_t count) noexcept {
        if (count == 0 || base >= playableStorage.memberCount) return;
        const auto end = base + std::min(count, playableStorage.memberCount - base);
        while (base < end) {
            const auto partitionBase = base - base % playableStorage.memberStride;
            const auto limit = std::min(end, partitionBase + playableStorage.memberStride);
            if (auto* row = MemberRelationOwner(base)) {
                auto* link = MemberRelationLink(*row, static_cast<std::uint32_t>(base - partitionBase));
                while (*link != UINT32_MAX && memberRelations[*link].logicalOffset < limit - partitionBase)
                    RemoveMemberRelation(*row, link);
            }
            base = limit;
        }
    }
    bool SetPlayableMemberReference(std::size_t logicalSlot,
        const EnginePlayableMemberReference& reference) noexcept {
        auto* row = MemberRelationOwner(logicalSlot);
        if (row == nullptr) return !reference.service.token.Valid();
        const auto offset = static_cast<std::uint32_t>(logicalSlot % playableStorage.memberStride);
        auto* link = MemberRelationLink(*row, offset);
        const bool existing = *link != UINT32_MAX && memberRelations[*link].logicalOffset == offset;
        if (!reference.service.token.Valid()) {
            if (existing) RemoveMemberRelation(*row, link);
            return true;
        }
        if (reference.service.token.slot >= playableServices.size()) return false;
        const auto& target = playableServices[reference.service.token.slot];
        if (target.identity != reference.service.token || target.ownerInstance != reference.service.owner ||
            target.ownerEpoch != reference.service.epoch || target.invocationGeneration != reference.service.invocationGeneration ||
            target.partition.generation != reference.partitionGeneration) return false;
        if (!existing) {
            if (occupiedMemberRelations == memberRelations.Capacity()) return false;
            const auto previous = memberRelations.ConstructedCount();
            if (!memberRelations.ConstructThrough(occupiedMemberRelations + 1u,
                    [&](std::uint32_t required) noexcept {
                        return required <= previous || AllowNativeBackingAllocation();
                    })) return false;
            InitializeMemberRelations(previous);
            if (freeMemberRelation == UINT32_MAX) return false;
            const auto index = freeMemberRelation;
            freeMemberRelation = memberRelations[index].next;
            memberRelations[index].next = *link;
            memberRelations[index].logicalOffset = offset;
            *link = index;
            ++row->memberRelationCount;
            ++occupiedMemberRelations;
        }
        auto& node = memberRelations[*link];
        node.service = reference.service.token;
        node.leaf = reference.leaf;
        node.leafEpoch = reference.leafEpoch;
        node.memberOffset = reference.memberOffset;
        node.authoredChildOrdinal = reference.authoredChildOrdinal;
        return true;
    }
    void SetPlayableMemberOrdinal(std::size_t logicalSlot, std::uint32_t ordinal) noexcept {
        auto* row = MemberRelationOwner(logicalSlot);
        if (row == nullptr) return;
        const auto offset = static_cast<std::uint32_t>(logicalSlot % playableStorage.memberStride);
        const auto link = *MemberRelationLink(*row, offset);
        if (link != UINT32_MAX && memberRelations[link].logicalOffset == offset)
            memberRelations[link].authoredChildOrdinal = ordinal;
    }
    void MovePlayableMemberReference(std::size_t from, std::size_t to) noexcept {
        if (from == to) return;
        auto* source = MemberRelationOwner(from);
        auto* target = MemberRelationOwner(to);
        assert(source != nullptr && target != nullptr);
        if (source == nullptr || target == nullptr) { runtimeRecreateRequired = true; return; }
        ClearPlayableMemberReferences(to, 1);
        const auto sourceOffset = static_cast<std::uint32_t>(from % playableStorage.memberStride);
        auto* sourceLink = MemberRelationLink(*source, sourceOffset);
        if (*sourceLink == UINT32_MAX || memberRelations[*sourceLink].logicalOffset != sourceOffset) return;
        const auto index = *sourceLink;
        auto& node = memberRelations[index];
        *sourceLink = node.next;
        --source->memberRelationCount;
        node.logicalOffset = static_cast<std::uint32_t>(to % playableStorage.memberStride);
        auto* targetLink = MemberRelationLink(*target, node.logicalOffset);
        node.next = *targetLink;
        *targetLink = index;
        ++target->memberRelationCount;
    }
    template<class Visitor>
    void VisitPlayableMemberReferences(EnginePlayableServiceRow& row, Visitor&& visitor) noexcept {
        const auto base = playableStorage.MemberBase(row.partition.slot);
        auto index = row.memberRelationHead;
        while (index != UINT32_MAX) {
            const auto& node = memberRelations[index];
            const auto slot = base + node.logicalOffset;
            index = node.next;
            visitor(slot, ExpandMemberRelation(node));
        }
    }

    bool PreparePlayableMemberDirectory(EnginePlayableServiceRow& row) noexcept {
        if (!row.HasFeatureArrays() &&
            (playableServices.PreparedUseActive() || !row.PrepareFeatureArrays())) return false;
        if (row.members.size() == playableStorage.memberStride) return true;
        if (memguard::IsAudioTagged() || playableMemberRecords.PreparedUseActive()) return false;
        try { row.members.resize(playableStorage.memberStride); }
        catch (...) { return false; }
        return true;
    }
    static bool PlayableMemberReusable(const EnginePlayableMemberRecord& record) noexcept {
        return !record.occupied && record.generation != UINT64_MAX;
    }
    bool PreparePlayableMemberBacking(std::uint32_t count,
        StableSlotStorage<EnginePlayableMemberRecord>::Preparation* out) noexcept {
        return playableMemberRecords.PrepareBacking(count, 0, PlayableMemberReusable,
            [this](std::uint32_t slot) noexcept { return playableMemberRecords.Emplace(slot); }, out);
    }
    EnginePlayableMemberRecord* FindPlayableMember(EnginePlayableMemberToken token) noexcept {
        if (!token.Valid() || !playableMemberRecords.IsConstructed(token.slot)) return nullptr;
        auto& record = playableMemberRecords[token.slot];
        return record.occupied && record.generation == token.generation ? &record : nullptr;
    }
    EnginePlayableMemberRecord* FindPlayableMember(const EnginePlayableServiceRow& row,
        std::uint32_t offset) noexcept {
        if (offset >= row.members.size() || !row.InvocationValid() || row.quarantined ||
            !playablePartitions.Owns(row.partition, row.identity)) return nullptr;
        const auto token = row.members[offset];
        auto* record = FindPlayableMember(token);
        return record != nullptr && record->primaryOwned && record->service == row.identity &&
            record->partitionGeneration == row.partition.generation &&
            record->physicalSlot == playableStorage.MemberBase(row.partition.slot) + offset ? record : nullptr;
    }
    EnginePlayableMemberRecord* FindPlayableMember(std::size_t physicalSlot) noexcept {
        if (physicalSlot >= playableStorage.memberCount || playableStorage.memberStride == 0) return nullptr;
        const auto partition = static_cast<std::uint32_t>(physicalSlot / playableStorage.memberStride);
        const auto owner = playablePartitions.CaptureOwner(partition);
        if (!owner.Valid() || owner.slot >= playableServices.size()) return nullptr;
        auto& row = playableServices[owner.slot];
        if (row.identity != owner || row.partition.slot != partition) return nullptr;
        return FindPlayableMember(row, static_cast<std::uint32_t>(physicalSlot % playableStorage.memberStride));
    }
    const EnginePlayableMemberRecord& ReadPlayableMember(std::size_t physicalSlot) const noexcept {
        const auto* record = const_cast<Engine*>(this)->FindPlayableMember(physicalSlot);
        static const EnginePlayableMemberRecord empty{};
        return record == nullptr ? empty : *record;
    }
    EnginePlayableMemberRecord& MutablePlayableMember(std::size_t physicalSlot) noexcept {
        auto* record = FindPlayableMember(physicalSlot);
        assert(record != nullptr);
        return *record;
    }
    EnginePlayableMemberToken AcquirePlayableMemberRecord() noexcept {
        for (std::uint32_t slot = 0; slot < playableMemberRecords.size(); ++slot) {
            if (!playableMemberRecords.IsConstructed(slot)) {
                if (playableMemberRecords.PreparedUseActive()) continue;
                if (!playableMemberRecords.Emplace(slot)) return {};
            }
            auto& record = playableMemberRecords[slot];
            if (!PlayableMemberReusable(record)) continue;
            if (!playableMemberRecords.ProtectBackingForAcquire(slot, PlayableMemberReusable,
                    [this](std::uint32_t next) noexcept { return playableMemberRecords.Emplace(next); })) return {};
            record.occupied = true;
            ++record.generation;
            return {slot, record.generation};
        }
        return {};
    }
    static void RecyclePlayableMember(EnginePlayableMemberRecord& record) noexcept {
        if (record.primaryOwned || record.programOwners != 0) return;
        const auto generation = record.generation;
        record = {};
        record.generation = generation;
    }
    bool PreparePlayableMemberBinding(EnginePlayableServiceRow& row, std::uint32_t offset,
        EnginePlayableMemberToken token) noexcept {
        if (offset >= playableStorage.memberStride || !row.InvocationValid() || row.quarantined ||
            !playablePartitions.Owns(row.partition, row.identity)) return false;
        auto* record = FindPlayableMember(token);
        if (record == nullptr || !PreparePlayableMemberDirectory(row)) return false;
        if (row.members[offset] == token) return FindPlayableMember(row, offset) == record;
        if (record->primaryOwned) return false;
        if (row.members[offset].Valid() && FindPlayableMember(row, offset) == nullptr) return false;
        return offset >= row.memberPrograms.size() || !row.memberPrograms[offset].Valid() ||
            FindMemberProgram(row, offset) != nullptr;
    }
    bool BindPlayableMember(EnginePlayableServiceRow& row, std::uint32_t offset,
        EnginePlayableMemberToken token) noexcept {
        if (!PreparePlayableMemberBinding(row, offset, token)) return false;
        if (row.members[offset] == token) return true;
        if (offset < row.memberPrograms.size() && row.memberPrograms[offset].Valid()) {
            auto* program = FindMemberProgram(row, offset);
            program->directoryOwned = false;
            ++retiredMemberProgramCount;
            row.memberPrograms[offset] = {};
        }
        if (!ReleasePlayableMember(row, offset)) return false;
        auto* record = FindPlayableMember(token);
        record->service = row.identity;
        record->partitionGeneration = row.partition.generation;
        record->physicalSlot = static_cast<std::uint32_t>(playableStorage.MemberBase(row.partition.slot) + offset);
        record->primaryOwned = true;
        row.members[offset] = token;
        RefreshPlayableMemberOnset(record->physicalSlot);
        return true;
    }
    EnginePlayableMemberRecord* AcquirePlayableMember(EnginePlayableServiceRow& row,
        std::uint32_t offset) noexcept {
        if (offset >= playableStorage.memberStride || !row.InvocationValid() || row.quarantined ||
            !playablePartitions.Owns(row.partition, row.identity) ||
            !PreparePlayableMemberDirectory(row)) return nullptr;
        if (row.members[offset].Valid()) return FindPlayableMember(row, offset);
        const auto token = AcquirePlayableMemberRecord();
        auto* record = FindPlayableMember(token);
        if (record == nullptr) return nullptr;
        if (BindPlayableMember(row, offset, token)) return record;
        RecyclePlayableMember(*record);
        return nullptr;
    }
    bool ReleasePlayableMember(EnginePlayableServiceRow& row, std::uint32_t offset) noexcept {
        if (offset >= row.members.size()) return false;
        if (!row.members[offset].Valid()) return true;
        if (offset < row.memberPrograms.size() && row.memberPrograms[offset].Valid()) return false;
        auto* record = FindPlayableMember(row, offset);
        if (record == nullptr) return false;
        const auto reference = ReadPlayableMemberReference(record->physicalSlot);
        if (reference.service.token == row.identity &&
            reference.service.owner == row.ownerInstance && reference.service.epoch == row.ownerEpoch &&
            reference.service.invocationGeneration == row.invocationGeneration &&
            reference.partitionGeneration == row.partition.generation && reference.memberOffset == offset &&
            reference.leaf == record->completion.leafInstance &&
            reference.leafEpoch == record->completion.leafEpoch)
            ClearPlayableMemberReferences(record->physicalSlot, 1);
        RemovePlayableMemberOnset(record->physicalSlot);
        record->primaryOwned = false;
        record->physicalSlot = UINT32_MAX;
        row.members[offset] = {};
        RecyclePlayableMember(*record);
        return true;
    }

    EnginePlayableMemberRecord* FindQueueProgramMember(const EngineQueueProgramBank* bank,
        std::uint32_t index) noexcept {
        return bank != nullptr && bank->occupied && index < bank->members.size()
            ? FindPlayableMember(bank->members[index]) : nullptr;
    }
    EnginePlayableMemberRecord* FindQueueSuccessorMember(const EngineQueueFutureFilingLayout& future,
        std::uint32_t offset) noexcept {
        return offset < future.queueSuccessorPrograms.count
            ? FindQueueProgramMember(future.queueSuccessorProgramBank, future.queueSuccessorPrograms.first + offset)
            : nullptr;
    }
    EnginePlayableMemberRecord* FindQueueSuccessorMember(const EnginePlayableServiceRow& row,
        std::uint32_t offset) noexcept {
        return row.HasFeatureArrays() ? FindQueueSuccessorMember(row.QueueFiling(), offset) : nullptr;
    }
    bool RetainQueueProgramMember(EngineQueueProgramBank& bank, std::uint32_t index,
        EnginePlayableMemberToken token) noexcept {
        if (!bank.occupied || bank.published || index >= bank.members.size()) return false;
        auto* record = FindPlayableMember(token);
        if (record == nullptr) return false;
        if (bank.members[index].Valid()) return bank.members[index] == token;
        if (record->programOwners == UINT32_MAX) return false;
        ++record->programOwners;
        bank.members[index] = token;
        return true;
    }
    EnginePlayableMemberRecord* AcquireQueueProgramMember(EngineQueueProgramBank& bank,
        std::uint32_t index) noexcept {
        if (!bank.occupied || bank.published || index >= bank.members.size()) return nullptr;
        if (bank.members[index].Valid()) return FindQueueProgramMember(&bank, index);
        const auto token = AcquirePlayableMemberRecord();
        auto* record = FindPlayableMember(token);
        if (record == nullptr) return nullptr;
        if (RetainQueueProgramMember(bank, index, token)) return record;
        RecyclePlayableMember(*record);
        return nullptr;
    }
    bool ReleaseQueueProgramMembers(EngineQueueProgramBank& bank) noexcept {
        for (const auto token : bank.members) {
            if (!token.Valid()) continue;
            const auto* record = FindPlayableMember(token);
            if (record == nullptr || record->programOwners == 0) return false;
        }
        for (auto& token : bank.members) {
            if (!token.Valid()) continue;
            auto* record = FindPlayableMember(token);
            --record->programOwners;
            token = {};
            RecyclePlayableMember(*record);
        }
        return true;
    }

    bool PrepareMemberProgramDirectory(EnginePlayableServiceRow& row) noexcept {
        if (row.memberPrograms.size() == playableStorage.initialProgramStride) return true;
        if (memguard::IsAudioTagged() || memberProgramRecords.PreparedUseActive()) return false;
        try { row.memberPrograms.resize(playableStorage.initialProgramStride); }
        catch (...) { return false; }
        return true;
    }
    static bool MemberProgramReusable(const EngineMemberProgramRecord& record) noexcept {
        return !record.occupied && record.generation != UINT64_MAX;
    }
    bool PrepareMemberProgramBacking(std::uint32_t count,
        StableSlotStorage<EngineMemberProgramRecord>::Preparation* out) noexcept {
        return memberProgramRecords.PrepareBacking(count, 0, MemberProgramReusable,
            [this](std::uint32_t slot) noexcept { return memberProgramRecords.Emplace(slot); }, out);
    }
    EngineMemberProgramRecord* FindMemberProgram(const EnginePlayableServiceRow& row,
        std::uint32_t offset) noexcept {
        if (offset >= row.memberPrograms.size()) return nullptr;
        const auto token = row.memberPrograms[offset];
        if (!token.Valid() || !memberProgramRecords.IsConstructed(token.slot)) return nullptr;
        auto& record = memberProgramRecords[token.slot];
        const auto& reference = record.member;
        return record.occupied && record.directoryOwned && record.generation == token.generation &&
            offset < row.members.size() && record.memberToken == row.members[offset] &&
            reference.service.token == row.identity && reference.service.owner == row.ownerInstance &&
            reference.service.epoch == row.ownerEpoch &&
            reference.service.invocationGeneration == row.invocationGeneration &&
            reference.partitionGeneration == row.partition.generation && reference.memberOffset == offset
            ? &record : nullptr;
    }
    EngineMemberProgramRecord* AcquireMemberProgram(EnginePlayableServiceRow& row,
        const EnginePlayableMemberReference& reference) noexcept {
        EnginePlayableServiceRow* owner = nullptr;
        std::size_t physicalSlot = 0;
        if (!ResolvePlayableMember(reference, &owner, &physicalSlot) || owner != &row ||
            reference.memberOffset >= playableStorage.initialProgramStride ||
            !PrepareMemberProgramDirectory(row)) return nullptr;
        auto& token = row.memberPrograms[reference.memberOffset];
        if (token.Valid()) {
            auto* record = FindMemberProgram(row, reference.memberOffset);
            return record != nullptr && record->member.leaf == reference.leaf &&
                record->member.leafEpoch == reference.leafEpoch ? record : nullptr;
        }
        auto* member = FindPlayableMember(row, reference.memberOffset);
        if (member == nullptr || member->programOwners == UINT32_MAX) return nullptr;
        for (std::uint32_t slot = 0; slot < memberProgramRecords.size(); ++slot) {
            if (!memberProgramRecords.IsConstructed(slot)) {
                if (memberProgramRecords.PreparedUseActive()) continue;
                if (!memberProgramRecords.Emplace(slot)) return nullptr;
            }
            auto& record = memberProgramRecords[slot];
            if (!MemberProgramReusable(record)) continue;
            if (!memberProgramRecords.ProtectBackingForAcquire(slot, MemberProgramReusable,
                    [this](std::uint32_t next) noexcept { return memberProgramRecords.Emplace(next); })) return nullptr;
            record.member = reference;
            record.memberToken = row.members[reference.memberOffset];
            ++member->programOwners;
            record.directoryOwned = true;
            record.occupied = true;
            ++record.generation;
            token = {slot, record.generation};
            return &record;
        }
        return nullptr;
    }
    EngineMemberProgramRecord* ResolveMemberProgram(const EnginePlayableMemberReference& reference) noexcept {
        EnginePlayableServiceRow* owner = nullptr;
        std::size_t physicalSlot = 0;
        if (!ResolvePlayableMember(reference, &owner, &physicalSlot)) return nullptr;
        auto* record = FindMemberProgram(*owner, reference.memberOffset);
        return record != nullptr && record->member.leaf == reference.leaf &&
            record->member.leafEpoch == reference.leafEpoch ? record : nullptr;
    }
    const EngineMemberProgramRecord* ResolveMemberProgram(const EnginePlayableMemberReference& reference) const noexcept {
        return const_cast<Engine*>(this)->ResolveMemberProgram(reference);
    }
    bool MemberProgramHasReaders(const EngineMemberProgramRecord& record) const noexcept {
        return !record.readFence.ReadCompleted(mixer.BlocksRendered()) ||
            timelinePool.HasProgramReference(record.schedule.runtimeProgram) ||
            immediateActionLane.HasProgramReference(record.schedule.runtimeProgram) ||
            timelinePlanner.HasProgramReference(record.schedule.runtimeProgram);
    }
    bool RecycleMemberProgram(EngineMemberProgramRecord& record) noexcept {
        auto* member = FindPlayableMember(record.memberToken);
        if (member == nullptr || member->programOwners == 0) return false;
        if (!record.directoryOwned) {
            if (retiredMemberProgramCount == 0) return false;
            --retiredMemberProgramCount;
        }
        record.payloads = {};
        record.events = {};
        record.runtime = {};
        record.schedule = {};
        record.member = {};
        record.memberToken = {};
        record.readFence.ResetAfterSafeRetirement();
        record.directoryOwned = false;
        record.occupied = false;
        --member->programOwners;
        RecyclePlayableMember(*member);
        return true;
    }
    void ServiceRetiredMemberPrograms() noexcept {
        if (retiredMemberProgramCount == 0) return;
        for (auto& record : memberProgramRecords) {
            if (!record.occupied || record.directoryOwned || MemberProgramHasReaders(record)) continue;
            const auto* member = FindPlayableMember(record.memberToken);
            if (member == nullptr) {
                runtimeRecreateRequired = true;
                continue;
            }
            if (member->release.Valid() && !voices.ReleaseObserved(member->release)) continue;
            if (!RecycleMemberProgram(record)) runtimeRecreateRequired = true;
        }
    }
    bool RetiredMemberProgramsHoldService(const EnginePlayableServiceRow& row) noexcept {
        if (retiredMemberProgramCount == 0) return false;
        for (const auto& record : memberProgramRecords) {
            if (!record.occupied || record.directoryOwned) continue;
            auto* owner = ResolvePlayableService(record.member.service);
            for (std::size_t depth = 0;; ++depth) {
                if (owner == nullptr || depth == playableServices.size()) {
                    runtimeRecreateRequired = true;
                    return true;
                }
                if (owner == &row) return true;
                if (!owner->parentService.Valid()) break;
                owner = ResolvePlayableService(owner->parentService);
            }
        }
        return false;
    }
    bool ReleaseMemberProgram(EnginePlayableServiceRow& row, std::uint32_t offset) noexcept {
        if (offset >= row.memberPrograms.size()) return false;
        if (!row.memberPrograms[offset].Valid()) return true;
        auto* record = FindMemberProgram(row, offset);
        if (record == nullptr || MemberProgramHasReaders(*record) || !RecycleMemberProgram(*record)) return false;
        row.memberPrograms[offset] = {};
        return true;
    }
    static bool PlayableServiceReusable(const EnginePlayableServiceRow& row) noexcept {
        return row.phase == EnginePlayableStartPhase::Free && !row.actionPrepared &&
            !row.partition.Valid() && !row.quarantined;
    }
    bool InitializePlayableServiceRows(std::uint32_t previous) noexcept {
        for (auto slot = previous; slot < playableServices.size(); ++slot) {
            auto& row = playableServices[slot];
            row.identity = {slot, 1};
            row.invocation = &playableStarts[slot];
            row.invocationGeneration = playableStarts[slot].generation;
        }
        return !playableServices.HasPreparedBacking() || PreparePlayableFeatureBacking();
    }
    bool PrepareVariableControlRetargetStorage() noexcept {
        if (variableControlRetargetScratch != nullptr) return true;
        if (memguard::IsAudioTagged() || playableServices.PreparedUseActive()) return false;
        try {
            detail::LogicAllocCheckpoint(detail::LogicAllocSite::VariableControlRetargetStorage);
            variableControlRetargetScratch =
                std::make_unique<VariableControlRetargetTreeScratch>(config.maxPlayableInstances);
        } catch (...) { return false; }
        return true;
    }
    VariableControlRetargetTreeScratch& VariableControlRetargetScratch() noexcept {
        assert(variableControlRetargetScratch != nullptr);
        return *variableControlRetargetScratch;
    }
    bool PrepareTimelineStartStorage(bool complete = true) noexcept {
        auto& scratch = timelineStartScratch;
        const auto starts = complete ? scratch.starts.Capacity() : 1u;
        if (scratch.starts.size() >= starts && (!complete ||
            (scratch.transactions.size() == scratch.transactions.Capacity() &&
             scratch.extraCommands.size() == scratch.extraCommands.Capacity() &&
             scratch.queueEvents.size() == scratch.queueEvents.Capacity()))) return true;
        if (memguard::IsAudioTagged() || playableServices.PreparedUseActive()) return false;
        return scratch.starts.ConstructThrough(starts) && (!complete ||
            (scratch.transactions.ConstructThrough(scratch.transactions.Capacity()) &&
             scratch.extraCommands.ConstructThrough(scratch.extraCommands.Capacity()) &&
             scratch.queueEvents.ConstructThrough(scratch.queueEvents.Capacity())));
    }
    bool PrepareOriginalSingleCurrentStorage() noexcept {
        if (originalSingleCurrentHolders.size() == originalSingleCurrentHolders.Capacity()) return true;
        if (memguard::IsAudioTagged() || playableServices.PreparedUseActive()) return false;
        return originalSingleCurrentHolders.ConstructThrough(originalSingleCurrentHolders.Capacity());
    }
    bool PreparePlayableFeatureBacking() noexcept {
        if (!PrepareOriginalSingleCurrentStorage() || !PreparePlayablePublicationStorage() ||
            !PrepareTimelineStartStorage() || !PrepareVariableControlRetargetStorage()) return false;
        for (auto& row : playableServices) {
            if (row.HasFeatureArrays()) continue;
            if (playableServices.PreparedUseActive() || !row.PrepareFeatureArrays()) return false;
        }
        return true;
    }
    bool PrepareOriginalPlayableStorage(bool needsCurrentHolders = true) noexcept {
        return playableInstances.PrepareStorage() &&
            originalPlayableLifetimeHolders.ConstructThrough(originalPlayableLifetimeHolders.Capacity()) &&
            (!needsCurrentHolders || PrepareOriginalSingleCurrentStorage());
    }
    bool PreparePlayablePublicationStorage(bool needsContainerState = true) noexcept {
        const auto arms = timelineArms.Capacity();
        const auto events = immediateActionLane.EventsPerAttempt();
        if ((!needsContainerState ||
             (initialFrontierArmPins.size() == arms && initialRootArmPublicationInputs.size() == arms)) &&
            pendingImmediateLaneEventsScratch.size() == events) return true;
        if (memguard::IsAudioTagged() || playableServices.PreparedUseActive()) return false;
        try {
            if (needsContainerState) {
                initialFrontierArmPins.resize(arms);
                initialRootArmPublicationInputs.resize(arms);
            }
            pendingImmediateLaneEventsScratch.resize(events);
        } catch (...) { return false; }
        return true;
    }
    bool PrepareVoiceBindingCatalogStorage() noexcept {
        if (voiceBindingCatalogScratch.capacity() >= config.maxBindingAttachments) return true;
        if (memguard::IsAudioTagged()) return false;
        try { voiceBindingCatalogScratch.reserve(config.maxBindingAttachments); }
        catch (...) { return false; }
        return true;
    }
    bool PreparePlayableStateStorage(bool needsContainerState = true) noexcept {
        return PrepareTimelineStartStorage(needsContainerState) &&
            PrepareOriginalPlayableStorage(needsContainerState) &&
            (!needsContainerState ||
                (initialFrontierEvents.ConstructThrough(initialFrontierEvents.Capacity()) &&
                 initialFrontierRequests.ConstructThrough(initialFrontierRequests.Capacity()) &&
                 pendingVariableLeafSources.ConstructThrough(pendingVariableLeafSources.Capacity()) &&
                 pendingVariableFutureSources.ConstructThrough(pendingVariableFutureSources.Capacity()) &&
                 pendingVariableFutureNodes.ConstructThrough(pendingVariableFutureNodes.Capacity()))) &&
            spatial.PrepareVoiceStorage(config.voiceCapacity.Render()) &&
            PreparePlayablePublicationStorage(needsContainerState);
    }
    bool PreparePlayableServiceDependencies(std::uint32_t required,
                                            bool requireQueueTiming = true) noexcept {
        if (!originalPlayableServiceLifetimeHolders.ConstructThrough(required)) return false;
        if (!requireQueueTiming) return true;
        // One service contributes one selected future. Its source and successor
        // programs contain at most Start/Envelope or Envelope/Stop events.
        const auto programs = std::min<std::uint64_t>(initialQueueTimingProgramsScratch.Capacity(),
            static_cast<std::uint64_t>(required) * EngineQueueProgramBankBody::kProgramCapacity);
        const auto events = std::min<std::uint64_t>(initialQueueTimingEventsScratch.Capacity(),
            programs * EngineQueueProgramBankBody::kEventsPerProgram);
        const auto groups = std::min<std::uint64_t>(initialQueueTimingGroupsScratch.Capacity(),
            static_cast<std::uint64_t>(required) *
                std::tuple_size_v<decltype(EngineQueueServicePayload::queueUnplacedGroupFrames)>);
        return initialQueueTimingProgramsScratch.ConstructThrough(static_cast<std::uint32_t>(programs)) &&
            initialQueueTimingEventsScratch.ConstructThrough(static_cast<std::uint32_t>(events)) &&
            initialQueueTimingGroupsScratch.ConstructThrough(static_cast<std::uint32_t>(groups));
    }
    EnginePlayableServiceRow* PrepareFreePlayableService(bool requireQueueTiming = true) noexcept {
        const auto free = std::find_if(playableServices.begin(), playableServices.end(), PlayableServiceReusable);
        const auto slot = free == playableServices.end() ? playableServices.size() : free->identity.slot;
        if (slot >= playableServices.Capacity()) return nullptr;
        const auto previous = playableServices.size();
        if (!playableServices.ConstructThrough(slot + 1u,
                [this, requireQueueTiming](std::uint32_t required) noexcept {
                    return PreparePlayableServiceDependencies(required,
                        requireQueueTiming || playableServices.HasPreparedBacking());
                })) return nullptr;
        if (!InitializePlayableServiceRows(previous)) return nullptr;
        return &playableServices[slot];
    }
    bool PreparePlayableServiceBacking(std::uint32_t count, StableStoragePreparation* out) noexcept {
        std::uint32_t occupiedPrefix = 0;
        for (const auto& row : playableServices)
            if (!PlayableServiceReusable(row)) occupiedPrefix = row.identity.slot + 1u;
        const auto previous = playableServices.size();
        if (!playableServices.PrepareBacking(count, occupiedPrefix, out,
                [this](std::uint32_t required) noexcept {
                    return PreparePlayableServiceDependencies(required);
                })) return false;
        if (!InitializePlayableServiceRows(previous) || !PreparePlayableFeatureBacking()) {
            out->Release();
            return false;
        }
        for (auto& row : playableServices)
            if (!PrepareMemberProgramDirectory(row) || !PreparePlayableMemberDirectory(row)) return false;
        return true;
    }
    struct PlayableServiceWorkFamily {};
    using PlayableServiceWorkIdentity = RuntimeIndexIdentity<PlayableServiceWorkFamily>;
    RuntimeActivityIndex<PlayableServiceWorkFamily> playableServiceWork;

    PlayableServiceWorkIdentity CapturePlayableServiceWorkIdentity(std::uint32_t slot) const noexcept {
        if (slot >= playableServices.size()) return {};
        const auto& row = playableServices[slot];
        return row.partition.Valid()
            ? PlayableServiceWorkIdentity{row.identity.generation, slot, 0}
            : PlayableServiceWorkIdentity{};
    }
    void SetPlayableServiceOccupied(EnginePlayableServiceRow& row, bool occupied) noexcept {
        assert(row.identity.slot < playableServices.size() &&
            &playableServices[row.identity.slot] == &row);
        const auto identity = CapturePlayableServiceWorkIdentity(row.identity.slot);
        const bool recorded = playableServiceWork.SetMembership(identity, RuntimeActivitySet::Active,
            occupied, [&](PlayableServiceWorkIdentity current) noexcept {
                return current == CapturePlayableServiceWorkIdentity(current.slot);
            });
        assert(recorded);
        if (!recorded) {
            runtimeRecreateRequired = true;
            return;
        }
        if (occupied) LinkPlayableServiceCohort(row);
        else UnlinkPlayableServiceCohort(row);
    }
    EnginePlayableServiceRow* NextOccupiedPlayableService(std::uint32_t* cursor) noexcept {
        const auto identity = playableServiceWork.Next(RuntimeActivitySet::Active, cursor,
            [&](std::uint32_t slot) noexcept { return CapturePlayableServiceWorkIdentity(slot); });
        return identity.Valid() ? &playableServices[identity.slot] : nullptr;
    }
    void SetPlayableServiceCohortRoot(EnginePlayableServiceRow& row,
        ActionFiringHandle root) noexcept {
        assert(row.identity.slot < playableServices.size() &&
            &playableServices[row.identity.slot] == &row);
        if (row.pendingImmediateCohortRoot_ == root) return;
        const bool occupied = row.cohortLinked_;
        UnlinkPlayableServiceCohort(row);
        row.pendingImmediateCohortRoot_ = root;
        if (occupied) LinkPlayableServiceCohort(row);
    }
    struct PlayableCohortCursor {
        std::uint32_t nextSlot = 0;
        EnginePlayableServiceToken previous{};
    };
    EnginePlayableServiceRow* NextOccupiedPlayableCohortService(
        ActionFiringHandle root, PlayableCohortCursor* cursor) noexcept {
        // Exhaustion is sticky, even if a callback later inserts another row.
        if (cursor == nullptr || cursor->nextSlot >= playableServices.Capacity()) return nullptr;
        auto slot = playableCohortBuckets_[PlayableCohortBucket(root)].head;
        if (cursor->previous.Valid() && cursor->previous.slot < playableServices.size()) {
            const auto& previous = playableServices[cursor->previous.slot];
            if (previous.identity == cursor->previous && previous.cohortLinked_ &&
                previous.partition.Valid() && previous.pendingImmediateCohortRoot_ == root)
                slot = previous.cohortNext_;
        }
        // Follow current links, never a successor saved before native service callbacks.
        while (slot != UINT32_MAX) {
            auto& row = playableServices[slot];
            if (slot >= cursor->nextSlot && row.cohortLinked_ &&
                row.pendingImmediateCohortRoot_ == root &&
                CapturePlayableServiceWorkIdentity(slot).Valid()) {
                cursor->nextSlot = slot + 1u;
                cursor->previous = row.identity;
                return &row;
            }
            slot = row.cohortNext_;
        }
        cursor->nextSlot = playableServices.Capacity();
        return nullptr;
    }
private:
    static constexpr std::size_t kPlayableCohortBucketCount = 256u;
    static std::size_t PlayableCohortBucket(ActionFiringHandle root) noexcept {
        return static_cast<std::uint32_t>(root) & (kPlayableCohortBucketCount - 1u);
    }
    struct PlayableCohortBucketLinks {
        std::uint32_t head = UINT32_MAX;
        std::uint32_t tail = UINT32_MAX;
    };
    std::array<PlayableCohortBucketLinks, kPlayableCohortBucketCount> playableCohortBuckets_{};
    void LinkPlayableServiceCohort(EnginePlayableServiceRow& row) noexcept {
        if (row.cohortLinked_) return;
        const auto slot = row.identity.slot;
        auto& bucket = playableCohortBuckets_[PlayableCohortBucket(row.pendingImmediateCohortRoot_)];
        // New higher slots, including ordinary root-zero services, append without a scan.
        if (bucket.tail != UINT32_MAX && bucket.tail < slot) {
            row.cohortPrevious_ = bucket.tail;
            row.cohortNext_ = UINT32_MAX;
            playableServices[bucket.tail].cohortNext_ = slot;
            bucket.tail = slot;
            row.cohortLinked_ = true;
            return;
        }
        auto* next = &bucket.head;
        auto previous = UINT32_MAX;
        while (*next != UINT32_MAX && *next < slot) {
            previous = *next;
            next = &playableServices[previous].cohortNext_;
        }
        row.cohortPrevious_ = previous;
        row.cohortNext_ = *next;
        if (*next != UINT32_MAX) playableServices[*next].cohortPrevious_ = slot;
        else bucket.tail = slot;
        *next = slot;
        row.cohortLinked_ = true;
    }
    void UnlinkPlayableServiceCohort(EnginePlayableServiceRow& row) noexcept {
        if (!row.cohortLinked_) return;
        auto& bucket = playableCohortBuckets_[PlayableCohortBucket(row.pendingImmediateCohortRoot_)];
        if (row.cohortPrevious_ == UINT32_MAX)
            bucket.head = row.cohortNext_;
        else playableServices[row.cohortPrevious_].cohortNext_ = row.cohortNext_;
        if (row.cohortNext_ != UINT32_MAX)
            playableServices[row.cohortNext_].cohortPrevious_ = row.cohortPrevious_;
        else bucket.tail = row.cohortPrevious_;
        row.cohortPrevious_ = UINT32_MAX;
        row.cohortNext_ = UINT32_MAX;
        row.cohortLinked_ = false;
    }
public:
    // Production Current borrowing remains closed while this seed is collecting.
    StableRowStorage<OriginalPlayableServiceLifetimeHolder> originalPlayableServiceLifetimeHolders;
    StableRowStorage<OriginalPendingVariableQueueNativeOrigin> pendingVariableQueueNativeOrigins;
    StableSlotStorage<OriginalPendingOrdinarySelectionHolder> pendingOrdinarySelections;
    StableSlotStorage<OriginalPendingVariableGroupHolder> preparedPendingVariableGroups;
    StableSlotStorage<OriginalPendingVariableGroupHolder> acceptedPendingVariableGroups;
    bool pendingVariableGroupReplacementActive = false;
    bool deferRegisteredPendingVariableSelectionReplacementForTest = false;
    bool deferPendingImmediateEntriesForTest = false;
    StableRowStorage<OriginalPendingVariableLeafSource> pendingVariableLeafSources;
    std::uint32_t pendingVariableLeafSourceCount = 0;
    StableRowStorage<OriginalPendingVariableFutureSource> pendingVariableFutureSources;
    bool PreparePendingVariableQueueOriginBacking(std::uint32_t count,
        StableStoragePreparation* out) noexcept {
        auto occupied = pendingVariableQueueNativeOrigins.ConstructedCount();
        while (occupied != 0 && pendingVariableQueueNativeOrigins[occupied - 1u].Empty()) --occupied;
        return pendingVariableQueueNativeOrigins.PrepareBacking(
            std::min(count, pendingVariableQueueNativeOrigins.Capacity()), occupied, out);
    }
    StableRowStorage<OriginalPendingVariableFutureNode> pendingVariableFutureNodes;
    void (*pendingVariableGroupPlacementHookForTest)(Engine&, EngineActionPlayScratch&,
        EngineImmediateEntryAttempt&, const PrivateImmediateLanePlacementProof&,
        VariableReadCutReader, void*) noexcept = nullptr;
    void* pendingVariableGroupPlacementContextForTest = nullptr;
    void (*pendingVariableGroupPoliciesPreparedHookForTest)(Engine&, EngineActionPlayScratch&, void*) noexcept = nullptr;
    void (*pendingVariableGroupVoicesPreparedHookForTest)(Engine&, EngineActionPlayScratch&, void*) noexcept = nullptr;
    void* pendingVariableGroupVoicesPreparedContextForTest = nullptr;
    void* pendingVariableGroupPoliciesPreparedContextForTest = nullptr;
    std::uint64_t nextOriginalPendingVariableProducerMarker = 0;
    void (*originalPlayableServiceReservedHookForTest)(Engine&,
        const OriginalPlayableServiceLifetimeHolder&, void*) = nullptr;
    void* originalPlayableServiceReservedContextForTest = nullptr;
    EngineFrontierExpansionScratch frontierExpansion;

    EnginePlayableServiceRow* ResolvePlayableService(
        EnginePlayableServiceToken token, PlayableInstanceHandle owner,
        std::uint64_t epoch) noexcept {
        if (!token.Valid() || token.slot >= playableServices.size()) return nullptr;
        auto& row = playableServices[token.slot];
        if (row.identity != token || row.quarantined || !row.InvocationValid() ||
            row.ownerInstance != owner || row.ownerEpoch != epoch ||
            !playablePartitions.Owns(row.partition, token)) return nullptr;
        return &row;
    }

    EnginePlayableServiceRow* ResolveSourceResourceOwner(
        const SourceTerminalResourceOwner& owner) noexcept {
        auto* row = ResolvePlayableService({owner.serviceSlot, owner.serviceGeneration},
            owner.instance, owner.epoch);
        if (row == nullptr || row->Invocation().slot != owner.invocationSlot ||
            row->invocationGeneration != owner.invocationGeneration) return nullptr;
        return row;
    }

    bool CapturePendingQueueSourceIdentity(const EnginePlayableServiceRow& member,
        const SourceTerminalRecord& source, EngineTimelineVoiceInstallIdentity* out) noexcept {
        if (out == nullptr || !member.queueTimingPending || !member.InvocationValid() ||
            member.PendingImmediateCohortRoot() == 0 ||
            member.PendingImmediateCohortRoot() != member.Invocation().actionFiring ||
            !source.occupied || source.controller != member.rootInstance ||
            sourceTerminals.At(playableInstances.SourceRetirementSlot(source.leaf)) != &source ||
            ResolveSourceResourceOwner(source.resourceOwner) != &member ||
            playableInstances.Classify(source.leaf).scheduleEpoch != source.acceptedEpoch) return false;
        EngineTimelineVoiceInstallIdentity install{};
        if (!CaptureTimelineVoiceInstallIdentity(source.render, &install) ||
            install.releaseObserver.slot != source.release.slot ||
            install.releaseObserver.targetSequence != source.release.targetSequence ||
            install.startArm.renderSlot != static_cast<std::uint32_t>(source.render) ||
            install.startArm.renderGeneration != static_cast<std::uint32_t>(source.render >> 32)) return false;
        *out = install;
        return true;
    }

    EnginePlayableServiceRow* ResolvePlayableService(
        const EnginePlayableServiceReference& reference) noexcept {
        if (!reference.Valid()) return nullptr;
        auto* row = ResolvePlayableService(reference.token, reference.owner, reference.epoch);
        return row != nullptr && row->invocationGeneration == reference.invocationGeneration
            ? row : nullptr;
    }

    bool ResolvePlayableMember(const EnginePlayableMemberReference& reference,
                               EnginePlayableServiceRow** outOwner,
                               std::size_t* outSlot) noexcept {
        if (outOwner == nullptr || outSlot == nullptr) return false;
        auto* row = ResolvePlayableService(reference.service);
        if (row == nullptr || row->partition.generation != reference.partitionGeneration ||
            reference.memberOffset >= playableStorage.memberStride ||
            reference.leaf == 0 || reference.leafEpoch == 0) return false;
        const auto slot = playableStorage.MemberBase(row->partition.slot) + reference.memberOffset;
        const auto& member = ReadPlayableMember(slot).completion;
        if (member.leafInstance != reference.leaf || member.leafEpoch != reference.leafEpoch)
            return false;
        *outOwner = row;
        *outSlot = slot;
        return true;
    }

    bool ResolvePlayableMemberSlot(std::size_t logicalSlot, std::size_t* physicalSlot) noexcept {
        if (physicalSlot == nullptr || logicalSlot >= playableStorage.memberCount) return false;
        const auto& reference = ReadPlayableMemberReference(logicalSlot);
        if (!reference.service.token.Valid()) {
            *physicalSlot = logicalSlot;
            return true;
        }
        EnginePlayableServiceRow* owner = nullptr;
        return ResolvePlayableMember(reference, &owner, physicalSlot);
    }

    bool ResolvePlayableMemberSlot(std::size_t logicalSlot, std::size_t* physicalSlot) const noexcept {
        return const_cast<Engine*>(this)->ResolvePlayableMemberSlot(logicalSlot, physicalSlot);
    }

    EnginePlayableMemberRecord* FindLogicalPlayableMember(std::size_t logicalSlot) noexcept {
        std::size_t physicalSlot = 0;
        return ResolvePlayableMemberSlot(logicalSlot, &physicalSlot) ? FindPlayableMember(physicalSlot) : nullptr;
    }

    const EnginePlayableMemberRecord& ReadLogicalPlayableMember(std::size_t logicalSlot) const noexcept {
        const auto* record = const_cast<Engine*>(this)->FindLogicalPlayableMember(logicalSlot);
        static const EnginePlayableMemberRecord empty{};
        return record == nullptr ? empty : *record;
    }

    EnginePlayableMemberRecord& MutableLogicalPlayableMember(std::size_t logicalSlot) noexcept {
        auto* record = FindLogicalPlayableMember(logicalSlot);
        assert(record != nullptr);
        if (record == nullptr) std::terminate();
        return *record;
    }

    const CapturedTimelineVoiceInstall* ResolvePlayableServiceVoice(
        const EnginePlayableServiceRow& row, std::uint32_t index = 0) const noexcept {
        if (row.usesCapturedScheduleVoices) {
            if (row.usesCapturedVoice || row.capturedScheduleVoices.size() != row.scheduleLeafCount ||
                index >= row.capturedScheduleVoices.size()) return nullptr;
            const auto& voice = row.capturedScheduleVoices[index];
            return voice.Valid() ? &voice : nullptr;
        }
        if (row.usesCapturedVoice)
            return index == 0 && row.capturedVoice.Valid() ? &row.capturedVoice : nullptr;
        if (row.HasFeatureArrays() && index < row.QueuePayload().queueCurrentPrograms.count) {
            const auto* member = const_cast<Engine*>(this)->FindQueueProgramMember(
                row.QueuePayload().queueCurrentProgramBank, row.QueuePayload().queueCurrentPrograms.first + index);
            if (member != nullptr && member->voice.Valid() &&
                (row.scheduleLeafCount > 1 || SameOriginalSingleArm(member->arm, row.arm)))
                return &member->voice;
        }
        if (row.queueMode && index == 0) {
            const auto* member = const_cast<Engine*>(this)->FindPlayableMember(row, 0);
            if (member != nullptr && member->voice.Valid() && SameOriginalSingleArm(member->arm, row.arm))
                return &member->voice;
        }
        if (index >= row.scheduleLeafCount) return nullptr;
        std::size_t offset = index;
        if (row.variableControlIndependentFrontier && index >= row.variableControlDiscreteMemberCount) {
            offset = static_cast<std::size_t>(row.variableControlContinuousMemberOffset) +
                index - row.variableControlDiscreteMemberCount;
        }
        if (offset > (std::numeric_limits<std::size_t>::max)() - row.scheduleStorageBase) return nullptr;
        std::size_t slot = 0;
        if (!ResolvePlayableMemberSlot(row.scheduleStorageBase + offset, &slot) ||
            slot >= playableStorage.memberCount || !ReadPlayableMember(slot).voice.Valid()) return nullptr;
        return &ReadPlayableMember(slot).voice;
    }

    template <std::size_t Capacity>
    bool ResolvePlayableMemberSlots(std::size_t logicalBase, std::size_t count,
                                   std::array<std::size_t, Capacity>& physicalSlots) noexcept {
        if (count > Capacity || logicalBase > playableStorage.memberCount ||
            count > playableStorage.memberCount - logicalBase) return false;
        for (std::size_t index = 0; index < count; ++index) {
            if (!ResolvePlayableMemberSlot(logicalBase + index, &physicalSlots[index])) return false;
        }
        return true;
    }

    bool CapturePlayableMemberReference(EnginePlayableServiceRow& service,
                                       std::size_t logicalSlot,
                                       EnginePlayableMemberReference* output) noexcept {
        if (output == nullptr || !playablePartitions.Owns(service.partition, service.identity) ||
            logicalSlot >= playableStorage.memberCount) return false;
        if (ReadPlayableMemberReference(logicalSlot).service.token.Valid()) {
            EnginePlayableServiceRow* owner = nullptr;
            std::size_t physicalSlot = 0;
            if (!ResolvePlayableMember(ReadPlayableMemberReference(logicalSlot), &owner, &physicalSlot))
                return false;
            *output = ReadPlayableMemberReference(logicalSlot);
            return true;
        }
        const auto base = playableStorage.MemberBase(service.partition.slot);
        if (logicalSlot < base || logicalSlot - base >= playableStorage.memberStride) return false;
        const auto& member = ReadPlayableMember(logicalSlot).completion;
        if (member.leafInstance == 0 || member.leafEpoch == 0) return false;
        *output = {{service.identity, service.ownerInstance, service.ownerEpoch, service.invocationGeneration},
            service.partition.generation, static_cast<std::uint32_t>(logicalSlot - base),
            member.leafInstance, member.leafEpoch};
        return true;
    }
    StableRowStorage<Plan03PageGroupView> timelineGroupCaptureScratch;
    ScheduledTransactionPool::PendingEventStorageRegistration timelineGroupCapturePreparation;
    std::uint32_t preparedPageGroupCaptures = 0;
    std::uint32_t preparedLaneGroupCaptures = 0;
    bool PrepareTimelineGroupCaptureStorage(std::uint64_t pageGroups, std::uint64_t laneGroups) noexcept {
        pageGroups = std::max<std::uint64_t>(pageGroups, preparedPageGroupCaptures);
        laneGroups = std::max<std::uint64_t>(laneGroups, preparedLaneGroupCaptures);
        const auto capacity = timelineGroupCaptureScratch.Capacity();
        if (pageGroups > capacity || laneGroups > capacity - pageGroups ||
            !timelineGroupCaptureScratch.ConstructThrough(static_cast<std::uint32_t>(pageGroups + laneGroups)))
            return false;
        preparedPageGroupCaptures = static_cast<std::uint32_t>(pageGroups);
        preparedLaneGroupCaptures = static_cast<std::uint32_t>(laneGroups);
        return true;
    }
    std::vector<TimelineTransactionProgramRef> publishedFrontierProgramRefsScratch;
    StableRowStorage<ScheduledEventInput, std::size_t> initialFrontierEvents;
    StableRowStorage<TransactionRequest, std::size_t> initialFrontierRequests;
    bool initialFrontierWorkspaceOccupied = false;
    // Serial producer leaves delegate before acquiring this exclusive scratch.
    EngineTimelineStartScratch timelineStartScratch{};
    std::vector<EngineOrdinaryRetiringSource> ordinaryRetiringSources;
    std::uint32_t ordinaryRetiringSourceCount = 0;
    std::vector<EngineOrdinaryRetiringLeaf> ordinaryRetiringLeaves;
    struct MemberOnsetFamily {};
    using MemberOnsetIdentity = RuntimeIndexIdentity<MemberOnsetFamily>;
    RuntimeActivityIndex<MemberOnsetFamily> memberOnsets;

    MemberOnsetIdentity PlayableMemberOnsetIdentity(std::uint32_t slot) const noexcept {
        const auto& member = ReadPlayableMember(slot).completion;
        return member.leafInstance != 0
            ? MemberOnsetIdentity{member.leafEpoch, slot, member.source.generation}
            : MemberOnsetIdentity{};
    }
    void RefreshPlayableMemberOnset(std::size_t slot) noexcept {
        const auto identity = PlayableMemberOnsetIdentity(static_cast<std::uint32_t>(slot));
        if (!identity.Valid()) return;
        const auto& member = ReadPlayableMember(slot).completion;
        const bool pending = member.source.Valid() && !member.resourcesRetired &&
            member.sourceStartedBlockSequence == 0;
        const bool recorded = memberOnsets.SetMembership(identity, RuntimeActivitySet::Active,
            pending, [&](MemberOnsetIdentity current) noexcept {
                return current == PlayableMemberOnsetIdentity(current.slot);
            });
        assert(recorded);
        if (!recorded) runtimeRecreateRequired = true;
    }
    void RemovePlayableMemberOnset(std::size_t slot) noexcept {
        const auto previous = PlayableMemberOnsetIdentity(static_cast<std::uint32_t>(slot));
        if (previous.Valid()) {
            const bool removed = memberOnsets.SetMembership(previous, RuntimeActivitySet::Active,
                false, [&](MemberOnsetIdentity current) noexcept {
                    return current == PlayableMemberOnsetIdentity(current.slot);
                });
            assert(removed);
            if (!removed) runtimeRecreateRequired = true;
        }
    }
    void SetPlayableMemberCompletion(std::size_t slot, VariableControlMemberCompletion value) noexcept {
        RemovePlayableMemberOnset(slot);
        auto* record = FindPlayableMember(slot);
        if (record == nullptr) {
            assert(value.leafInstance == 0);
            if (value.leafInstance != 0) runtimeRecreateRequired = true;
            return;
        }
        record->completion = value;
        record->monitorSelection = {};
        RefreshPlayableMemberOnset(slot);
    }
    StableRowStorage<EngineControlProgramRecord> variableControlRetargetPrograms;
    bool VariableControlRetargetProgramHasReaders(const EngineControlProgramRecord& record) const noexcept {
        const auto program = record.schedule.runtimeProgram;
        return record.reserved || timelinePool.HasProgramReference(program) || timelinePlanner.HasProgramReference(program);
    }
    bool VariableControlRetargetProgramsHaveReaders(EnginePlayableServiceToken service) const noexcept {
        for (const auto& record : variableControlRetargetPrograms)
            if (record.service == service && VariableControlRetargetProgramHasReaders(record)) return true;
        return false;
    }
    EngineControlProgramRecord* AcquireVariableControlRetargetProgram(EnginePlayableServiceToken service) noexcept {
        if (!service.Valid()) return nullptr;
        for (auto& record : variableControlRetargetPrograms) {
            if (VariableControlRetargetProgramHasReaders(record)) continue;
            record = {};
            record.service = service;
            record.reserved = true;
            return &record;
        }
        if (!variableControlRetargetPrograms.EmplaceBack()) return nullptr;
        auto& record = variableControlRetargetPrograms[variableControlRetargetPrograms.size() - 1u];
        record.service = service;
        record.reserved = true;
        return &record;
    }
    std::vector<SelectionChildV1> queueSelectionChildScratch;
    std::vector<std::uint32_t> queueRemainingScratch;
    std::vector<std::uint32_t> queuePermutationScratch;
    std::vector<EffectiveQueueState> queueProjectionScratch;
    std::uint32_t queueProjectionCount = 0;
    bool queueProjectionInUse = false;
    SelectionMemoryTableV1 selectionMemory;
    ResolvedPlayablePropertyCutStorageV1 playablePropertyCutScratch;
    // Public binding system: definition registry + attachment pool.
    // Declared after params — the
    // instance layer lives there and evaluates through views into this
    // table's fixed storage. Logic thread only; the audio thread never
    // learns bindings exist.
    BindingTable bindings;
    std::vector<PackVoiceBinding> voiceBindingCatalogScratch;
    // Bus-scope combine storage (section 8), LIVE from M6 S5 on: pack
    // apply writes the authored slot, BUS attachments evaluate into the
    // Variable slot each update, the tick's bus flush publishes changed
    // reductions through kOpcodeSetBusVolume inside the section-10
    // transaction, and Vn_SetBusVolume retargets the API-direct slot
    // (publishing the reduced value in the same call — with no other
    // slots that reduction IS the caller's value, so the no-binding path
    // stays bit-identical to M5). Logic thread only.
    BusTargetStore busTargets;
    // Effect-parameter binding resolution store (VN_TARGET_SCOPE_*_EFFECT_PARAM):
    // per installed (object, fx slot, parameter) base tracking and per-domain
    // combine, keyed by the packed locator. Seeded from the values an effect
    // install applies, retargeted by a direct Vn_Set*EffectParam, folded from
    // binding contributions each update, and cleared on clear / voice-end. A
    // parameter with no binding never enters the emit path, so the direct-set
    // behavior stays byte-identical. Logic thread only.
    EffectParamTargetStore effectParamTargets;
    // Probe detector rows and ballistics: the single owner of the meter
    // ring drain protocol. Probe binding rows live in the shared
    // BindingTable pools; meterProbes is the stateless orchestration
    // over both owners.
    MeterSystem meterDetectors;
    MeterProbeSystem meterProbes;
    // Meter block ring (section 11.2): the declared audio->logic peak
    // transport. Declared before mixer (the mixer holds a reference and
    // appends one record per rendered block while armed).
    MeterBlockRing meterRing;
    Spatializer spatial;
    // Update reclaim scratch. Collect can insert one published binding
    // (64), one pending, one park and one mutually exclusive ledger voice
    // per Logical voice. Each published retain slot then adds at most one
    // lease-named key. The ring is plans * 64, not one map per lease.
    std::vector<Spatializer::RetainedProfileKey> retainedProfileKeyScratch;
    // Logic-side emitter objects: shared position/orientation and per-emitter
    // local Variable overrides. Attached voices follow an emitter's pose and
    // resolve Variable reads at emitter scope; the audio thread never sees it.
    EmitterTable emitters;
    MonitorEmitterRegistry monitorEmitters;
    ActionFiringTable actionFirings;
    ActionSeekTable actionSeeks;
    StreamSeekHandoffTable streamSeekHandoffs;
    ActionDeferredRecordTable actionDeferredRecords;
    ActionDeferredPairState lastDeferredPairFault{};
    VnResult lastDeferredPairFaultLeaseResult = VN_OK;
    bool hasDeferredPairFault = false;
    ActionCooldownTable actionCooldowns;
    // The engine's single deterministic PRNG stream. Container weighted draws
    // and Action probability/jitter draws consume it in execution order; a
    // failed container resolution restores its state so it advances only on a
    // successful pick. Seeded from config.randomSeed (0 selects the documented
    // default constant).
    SplitMix64 rng;
    CommandQueue commands;
    void AcceptSourceCommandPublications() noexcept {
        if (!runtimeRecreateRequired) commands.AcceptSourcePublications();
    }
    OriginalSourceSubmissionRegistry originalSourceSubmissions;
    // Fixed actual Arm factory slots; every Source/Body receiver outlives them.
    StableSlotStorage<OriginalSinglePreparationHolder> originalSinglePreparationHolders;
    StableSlotStorage<OriginalQueueSourcePreparationHolder> originalQueueSourcePreparationHolders;
    bool originalSingleSourcePreparationSeen = false;
    bool originalQueueSourcePreparationSeen = false;
    std::uint32_t originalSourceBackingPrefix = 0;
    void (*originalQueueSourcePreparationHookForTest)(Engine&, const OriginalQueueSourcePreparationHolder&, void*) = nullptr;
    void* originalQueueSourcePreparationContextForTest = nullptr;
    void (*originalCompositeLedgerPreparationHookForTest)(Engine&, const OriginalQueueSourcePreparationHolder&, void*) = nullptr;
    void* originalCompositeLedgerPreparationContextForTest = nullptr;
    bool forceTimelineInheritedStopPreparationRefusalForTest = false;
    // Each late native fence keeps its own table slot, independent of Arm reuse.
    // Native Decision lifetime outputs cover actual Queue Current and Future
    // reservations. Completion-kind and ordinary Single equality stay strict.
    std::vector<OriginalDecisionLifetimePin> originalSingleDecisionBaselines;
    void (*originalQueueDecisionPreparationHookForTest)(Engine&, const PreparedDecisionReservation&,
        const OriginalDecisionLifetimePin&, void*) = nullptr;
    void* originalQueueDecisionPreparationContextForTest = nullptr;
    std::vector<OccurrenceStatePool::OriginalSingleCompletionOwnerPin> originalSingleOccurrenceBaselines;
    std::vector<OccurrenceStatePool::PreparedSingleCompletionCommitProof> originalSingleOccurrenceCommitProofs;
    CommandQueue::PreparedBatchProof preparedVoiceCommandBatchProof;
    std::vector<CommandRecord> actionPlayCommandScratch;
    std::vector<CommandRecord> actionSeekCommandScratch;
    std::vector<EngineActionSeekPreparation> actionSeekPreparationScratch;
    std::vector<CommandRecord> playLedBatchScratch;
    // Transactional property publication: one tick's resolved property
    // changes cross to the audio thread
    // whole or not at all, and a deferred batch reserves its admission
    // against every other command producer until it lands. Batch capacity
    // is the command queue's (rounded) capacity: HasRoom(N) can never
    // admit a batch larger than the queue, and the section-10.4 capacity
    // contract refuses wirings whose worst-case tick exceeds it, so no
    // legal publication can outgrow this buffer. Declared after commands
    // (construction reads the rounded capacity). The deferral counter
    // lives inside as DeferredCount(): engine-internal until the
    // section-12 snapshot append (paramPublicationDeferredCount) surfaces
    // it. Logic thread only.
    PropertyPublisher propertyPublisher;
    std::vector<std::uint32_t> spatialPropertyGroupIndices;
    std::vector<std::uint8_t> toneSectionCommandsPublished;
    CommandQueue::BatchReservation timelineBatchReservation{};
    std::vector<std::uint32_t> toneSectionPublishedRampMs;
    // Alternates scope priority when one fixed batch cannot serve both.
    bool propertyBusFirst = false;
    NotificationQueue notifications;
    // Logic-thread authority for Render admission and exact victim-generation
    // claims. It has no audio-thread or render-context contact surface.
    VoiceManager voiceManager;
    // Logic-originated notifications (parking/return/virtual completions),
    // delivered after the translated audio-origin notifications each Vn_Update
    // so the caller observes one merged, deterministically ordered stream. Sized to
    // the ledger capacity; drop-oldest with a counter, mirroring the audio
    // notification queue. No feature emits into it in this engine version.
    NotificationQueue logicNotifications;
    LogRing logRing; // declared before mixer: the mixer holds a reference
    // Logic-originated log records (container resolution outcomes), kept
    // separate from the audio-produced logRing so the audio thread stays the
    // sole producer of that ring. Produced and drained on the externally
    // synchronized logic thread (raw voice start/Vn_Update), then formatted through the
    // same FormatLogRecord path and delivered after the audio records.
    LogRing logicLogRing;
    // Per-slot source-cursor mirror: the audio thread publishes each rendering
    // voice's source read cursor here every block so the logic thread has a
    // coherent timeline for audibility parking. Declared before mixer (the
    // mixer holds a reference and publishes into it). One row per configured
    // voice slot.
    SourceCursorMirror sourceCursors;
    CompositeCompletionOwnerPool compositeCompletionOwners;
    // Same native capacity; receiver outlives all own factory output holders.
    std::vector<EngineOriginalCompositeCompletionPreparation> originalCompositeCompletionPreparations;
    void (*originalCompositePreparationHookForTest)(Engine&, const EngineOriginalCompositeCompletionPreparation&, void*) = nullptr;
    void* originalCompositePreparationContextForTest = nullptr;
    void (*originalCompositeLeafPreparationHookForTest)(Engine&, const EngineOriginalCompositeCompletionPreparation&, void*) = nullptr;
    void* originalCompositeLeafPreparationContextForTest = nullptr;
    // Same capacity as actual native Occurrence storage; never grown at runtime.
    std::vector<EngineOriginalOccurrencePreparation> originalOccurrencePreparations;
    void (*originalOccurrencePreparationHookForTest)(Engine&, const EngineOriginalOccurrencePreparation&, void*) = nullptr;
    void* originalOccurrencePreparationContextForTest = nullptr;
    DeferredIntentPool timelineDeferred;
    ProgramGenerationRegistry timelinePrograms;
    std::array<std::unique_ptr<EngineTimelineProgramSet>, 2>
        timelineProgramSets;
    TimelinePlanner timelinePlanner;
    VoiceTimelineSink timelineSink;
    TimelineExecutor timelineExecutor;
    MixerLoop mixer;
    NullBackend nullBackend;
    std::unique_ptr<TimelineServiceCoordinator> timelineService;
    // Logic-thread lifecycle state. The Engine and all render/content state
    // remain intact while the real device is absent and can be rebuilt.
    bool suspended = false;
    // Declared last on purpose: destruction runs in reverse order, so the
    // device callback stops before any render state it drives is torn
    // down. Idle in null mode; Vn_Init starts it for VN_DEVICE_REAL.
    DeviceBackend device;

    // Resolved parameter commits consumed as non-finite at the wire
    // boundary (poisoned math never retries). Logic thread only.
    std::uint64_t paramDropCount = 0;

    // Gather commits refused by a full publication batch buffer: one
    // tick's dirty set exceeded the command queue's capacity (reachable
    // through spatializer-owned contributions, which sit outside the
    // binding-instance bound). Publication degrades to capacity-sized
    // chunks across ticks and converges; this counts the refused stages.
    // Logic thread only.
    std::uint64_t paramBatchOverflowCount = 0;

    // Voices ended by steal policy. Counted when the steal commits the victim
    // to stopping, so it stays exact even if the advisory STOLEN notification is
    // dropped. Logic thread only.
    std::uint64_t voiceStolenCount = 0;
    std::uint64_t nextStreamParkRequestSequence = 1;
    bool currentWorldReplacementAdmissionClosed = false;
    bool runtimeTransportResetActive = false;
    std::uint64_t runtimeTransportResetFrame = 0;
    bool runtimeRecreateRequired = false;
    bool forceActionPlayCommitFailureForTest = false;
    bool forceImmediateLaneReservationFailureForTest = false;
    std::uint32_t forceImmediateLaneBindLeafFailureForTest = UINT32_MAX;
    std::uint32_t forceImmediateQueueArmPublishFailureAtForTest = UINT32_MAX;
    std::uint32_t forceImmediateQueuePostC0PublishFailureForTest = 0;
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
    std::uint32_t forcePrivateVcRootPublicationFailureAtForTest = UINT32_MAX;
    bool forcePrivateVcRootSideInvalidationRetryForTest = false;
#endif
    void (*immediateQueuePostC0PublishHookForTest)(
        Engine&, EngineImmediateEntryAttempt&, void*) = nullptr;
    void* immediateQueuePostC0PublishContextForTest = nullptr;
    std::uint32_t forceImmediateLaneFutureEventFaultForTest = 0;
    std::uint32_t forceInitialQueuePrivateRootReaderYieldAfterForTest = UINT32_MAX;
    std::uint64_t forceInitialQueuePrivateRootReaderYieldSequenceForTest = 0;
    std::uint32_t forceInitialQueuePrivateStopPushFailureAtForTest = UINT32_MAX;
    std::uint64_t forceInitialQueuePrivateStopRetrySequenceForTest = 0;
    void (*initialQueuePartialPublishAbortHookForTest)(
        Engine&, EngineImmediateEntryAttempt&, void*) = nullptr;
    void* initialQueuePartialPublishAbortContextForTest = nullptr;
    void (*immediateLaneReservedHookForTest)(Engine&, void*) = nullptr;
    void* immediateLaneReservedContextForTest = nullptr;
    bool forceActionPlayPostTransferCommitFailureForTest = false;
    bool forceOrdinaryStartPostRemoveVerificationFailureForTest = false;
    bool holdOrdinaryDeferredPageCancellationForTest = false;
    bool forceRetainedPagePublicationPressureForTest = false;
    std::uint32_t forceInitialQueueEntryTransferFailureAfterForTest = 0;
    bool forceActionTimelineVoiceOwnerCommitFailureForTest = false;
    bool forceActionTimelineVoiceOwnerRollbackFailureForTest = false;
    void (*timelineVoicePreparedHookForTest)(Engine&, const EnginePreparedTimelineVoiceStart&, void*) = nullptr;
    void (*servicePartitionProvedHookForTest)(Engine&, EngineActionPlayScratch&, void*) = nullptr;
    void* servicePartitionProvedContextForTest = nullptr;
    void (*privateInitialVcLaneTransferHookForTest)(Engine&, EngineActionPlayScratch&,
        std::uint32_t, void*) = nullptr;
    void* privateInitialVcLaneTransferContextForTest = nullptr;
    void (*privateInitialVcPageTransferHookForTest)(Engine&, EngineActionPlayScratch&,
        std::uint32_t, void*) = nullptr;
    void* privateInitialVcPageTransferContextForTest = nullptr;
    void (*privateInitialVcPageBatchHookForTest)(Engine&, EngineActionPlayScratch&,
        std::uint32_t, void*) = nullptr;
    void* privateInitialVcPageBatchContextForTest = nullptr;
    void* timelineVoicePreparedContextForTest = nullptr;
    bool forceTimelineVoiceCommitRollbackForTest = false;
    bool forceTimelineVoiceCommitRollbackUnbindFailureForTest = false;
    bool forceActionFinalOwnerCommitFailureForTest = false;
    bool forceActionVariableControlBeforeMemberFailureForTest = false;
    bool forceActionVariableControlPostLeaseTransferFailureForTest = false;
    bool forceDeferredFrontierPostHelperFailureForTest = false;
    bool forceDeferredFrontierChildPublishFailureForTest = false;
    bool forcePublishedDeferredEntryPostcheckFailureOnceForTest = false;
    EnginePlayableServiceToken deferredFrontierRejectedChildForTest{};
    void (*publishedDeferredSettlementHookForTest)(
        Engine&, std::uint32_t, bool, std::uint32_t, std::uint32_t,
        void*) = nullptr;
    void* publishedDeferredSettlementContextForTest = nullptr;
    bool forceActionVariableControlDeferredCleanupPendingOnceForTest = false;
    bool forceActionVariableControlDeferredRetryResetFailureOnceForTest = false;
    std::uint32_t actionFinalOwnerCommitFailureSkipForTest = 0;
    std::uint32_t actionTimelineVoiceOwnerRollbackAttemptsForTest = 0;
    std::uint32_t actionQueueInstanceOwnerCancelAttemptsForTest = 0;
    bool forceDirectPlayablePostFailureForTest = false;
    bool forcePreparedScheduleAllocationFailureForTest = false;
    std::int32_t nativeBackingAllocationFailureCountdownForTest = -1;
    std::uint64_t nativeBackingAllocationCountForTest = 0;
    bool forceContainerLoopPostFailureForTest = false;
    void (*queueArrivalCancelObservationHookForTest)(Engine&, TimelineArmToken) = nullptr;
    void (*armGroupTombstoneObservationHookForTest)(
        Engine&, DecisionToken, ArmGroupToken, TimelineArmToken,
        std::uint16_t, std::uint64_t, void*) = nullptr;
    void* armGroupTombstoneObservationContextForTest = nullptr;
    void (*queueContinuationFinalOnsetHookForTest)(Engine&, PlayableInstanceHandle) = nullptr;
    bool (*queueNaturalEndRetirementHookForTest)(Engine&, EnginePlayableServiceRow&) = nullptr;
    void (*cancellationPrescanHookForTest)(Engine&, void*) = nullptr;
    void* cancellationPrescanContextForTest = nullptr;
    void (*actionAdmissionRecheckHookForTest)(Engine&, void*) = nullptr;
    void* actionAdmissionRecheckContextForTest = nullptr;
    void (*selectionInputHookForTest)(Engine&, const SelectionPlayRequestV1&, void*) = nullptr;
    void (*playableRetirementHookForTest)(const EnginePlayableServiceRow&, PlayableInstanceHandle,
        PlayableTerminalKind, void*) = nullptr;
    void* playableRetirementContextForTest = nullptr;
    void (*queueInputHookForTest)(const ContainerProgramViewV3&, const RandomQueueSeedContext&,
                                  const EffectiveQueueState&, void*) = nullptr;
    void* selectionInputContextForTest = nullptr;
    TimelinePhysicalAdmissionFault timelinePhysicalAdmissionFault =
        TimelinePhysicalAdmissionFault::None;

    // Instance binds that failed at a voice-birth site (onset, immediate
    // start, or parked return) AFTER acceptance reserved their capacity
    // and slots. Structurally unreachable — acceptance refuses everything
    // the bind could refuse — so a nonzero count marks a bookkeeping bug;
    // each hit also logs the instance-pool exhaustion record. Logic
    // thread only.
    std::uint64_t bindingInstanceOnsetFailCount = 0;

    // Instance-pool refusals (the snapshot's bindingInstanceRefusedCount):
    // incremented exactly where the instance-pool exhaustion record is
    // logged — the per-voice apply verb's ceiling/table refusals, a play
    // refused at acceptance for an over-ceiling matched attachment set,
    // and the defensive onset-bind failure above — so counter and log
    // records stay 1:1. Logic thread only.
    std::uint64_t bindingInstanceRefusedCount = 0;

    // Attribution capture truncations (section 12): capture calls whose
    // caller buffer was smaller than the row set. Each also logs
    // kLogCodeCaptureTruncated; requiredCount reported the full need, so
    // the call itself still succeeded. Logic thread only.
    std::uint64_t captureTruncationCount = 0;

    // Stream slots promised to steal-deferred stream starts that have not yet
    // claimed one. A pending stream voice holds no physical stream slot, so
    // without a logic-side reservation two steals could both observe the same
    // free slot and overbook it — the first pending start would take it and the
    // second would spin unbounded. Both the steal preflight and the normal
    // stream-admission path treat a stream slot as available only when the free
    // count exceeds this reservation, so promised capacity is never handed out
    // twice. Incremented when a stream steal is admitted, decremented when the
    // deferred start claims a slot or the pending start is cancelled. Logic
    // thread only.
    std::uint32_t pendingStreamReservations = 0;

    // Emitter creations refused because every emitter slot was live. Surfaces
    // in the snapshot as noEmitterCount. Logic thread only.
    std::uint64_t noEmitterCount = 0;

    // Per-render-slot origin playable id (0 = none): the id the play named
    // as a registered Sound. Written at play, reset on slot reuse. Logic
    // thread only.
    StableRowStorage<VnSoundId> voiceOriginPlayable;

    // Per-render-slot Play facts retained for audibility parking (see
    // VoicePlayFacts), written at play. Logic thread only.
    StableRowStorage<VoicePlayFacts> voicePlayFacts;
    StableRowStorage<EnginePlaybackPathStorage> playbackPaths;

    // Audibility-parking state (all logic thread only). Each per-ledger-slot
    // record owns its replay state and virtual clock, so voices parked in
    // different updates do not share an entry-time assumption. Counters surface
    // in the snapshot; parkedVoiceCount is the live ledger.ParkedCount().
    StableSlotStorage<ParkRecord> parkRecords;
    // Bounded scratch for one logic-side audibility pass; cursors are captured
    // anew and parked identities are rebuilt on every pass.
    std::vector<SourceCursorMirror::Row> audibilityCursorRows;
    std::vector<std::uint32_t> audibilityParkedSlots;
    std::vector<VnVoiceHandle> audibilityParkedHandles;
    std::uint64_t parkedHighWater = 0;
    std::uint64_t voiceParkedCount = 0;    // cumulative park transitions
    std::uint64_t voiceReturnedCount = 0;  // cumulative return transitions
    std::uint64_t returnRefusedCount = 0;  // returns refused by capacity/steal
    std::uint64_t parkDeferredCount = 0;   // parks deferred by a stale cursor

    // Logic-side audio frame clock refreshed from the published snapshot.
    std::uint64_t serviceFrameClock = 0;
    // The public snapshot retains the legacy meter counters at zero; private
    // MeterProbe observability lives on MeterProbeSystem.
    // Probe slots stay generation-pinned while neutral publication, command
    // settlement, and the authored ramp's audio fence are outstanding.
    std::uint64_t meterProbeWithdrawalCount = 0;

    // Hot-reload observability (logic thread only). Counts are cumulative;
    // the live pinned-generation total is derived at capture time from the
    // three generation owners (PCM registry, pack table, stream manager),
    // while the high water is maintained at each successful replace — the
    // only moment the total can rise.
    std::uint64_t hotReloadCount = 0;
    std::uint64_t hotReloadRefusedCount = 0;
    std::uint64_t pinnedGenerationHighWater = 0;

    // Per-ledger-slot staging for steal-deferred starts (see
    // PendingStartRecord). Logic thread only.
    StableSlotStorage<PendingStartRecord> pendingStarts;

    // Session-trace state (all logic thread only; the audio thread has no
    // structural connection to any of it). `trace` is the live recorder or
    // null. A pre-start stateful call is accepted as origin only while every
    // such call belongs to the current-world/private-probe image; all other
    // mutation closes the gate. traceFailedSticky is the snapshot flag set by
    // a mid-session write failure
    // and cleared by the next accepted Vn_TraceStart. traceOrigin keeps the
    // caller vocabulary and the resolved device format as semantic values;
    // process-local callback pointers are nulled before it is stored.
    std::unique_ptr<TraceRecorder> trace;
    void CompleteVariableOperation() noexcept;
    void* variableOutcomeSinkContext = nullptr;
    void (*variableOutcomeSink)(void*, const VariableOutcomeRecord&) noexcept = nullptr;
    std::uint32_t variableOperationDepth = 0;
    std::uint64_t traceSessionEpoch = 0;
    bool statefulCallSeen = false;
    bool traceOriginWorldProbeOnly = true;
    bool traceFailedSticky = false;
    RuntimeProjectGenerationTokenV1 lastPairedProjectGenerationToken{};
    std::uint64_t lastPairedPublishedBusFingerprint = 0;
    std::uint64_t lastPairedCompileTargetProfileFingerprint = 0;
    std::uint32_t lastPairedPublicationVintage = 0;
    BusAdmissionVerdict lastPreviewBusAdmission = BusAdmissionVerdict::NotAuthored;
    // A user callback exception poisons the session; the host must call
    // Vn_Term explicitly before any further stateful operation.
    bool callbackFatalSticky = false;
    std::uint32_t callbackDispatchDepth = 0;
    TraceSessionOrigin traceOrigin{};

    // Vn_Update self-measurement (logic thread only): whole-call CPU in
    // microseconds for the last pump and the peak since init, merged into
    // snapshots at capture time. cpuTicksPerSecond is the cached QPC rate
    // the conversions divide by.
    std::uint32_t updateCpuLastUs = 0;
    std::uint32_t updateCpuPeakUs = 0;
    std::uint64_t cpuTicksPerSecond = 0;

    // The last coherent audio-published payload: Vn_CaptureSnapshot falls
    // back to it when every bounded capture attempt collides with the
    // writer. Logic thread only.
    SnapshotPayload lastSnapshotPayload;

    // Logic-side mirror of the effect type installed in each voice slot,
    // indexed [voice slot index][chain slot]. Reset for a slot at raw voice start,
    // written at Vn_SetVoiceEffect / cleared at Vn_ClearVoiceEffect, and read
    // at Vn_SetVoiceEffectParam so the value range is validated against the
    // installed type. A stale voice handle is gated by generation before this
    // is consulted, and reuse resets the entry, so no generational field is
    // needed here. Logic thread only.
    StableRowStorage<std::array<std::uint32_t, kMaxFxSlotsPerVoice>>
        effectSlotTypes;
    StableRowStorage<std::array<EffectInstanceRef, kMaxFxSlotsPerVoice>>
        voiceEffectInstanceRefs;

    // One voice slot's committed effect parameters and bypass. The installed
    // type lives in effectSlotTypes; the values here complete the picture so
    // audibility parking can reconstruct a voice's effect chain on return.
    struct VoiceEffectSlotConfig {
        bool bypass = false;
        std::array<float, kMaxEffectParamCount> params{};
    };
    // Logic-side mirror of each voice slot's effect parameters/bypass, indexed
    // [voice slot index][chain slot]. Reset for a slot at each start, written at
    // Vn_SetVoiceEffect / Vn_SetVoiceEffectParam / Vn_SetVoiceEffectBypass, and
    // captured at park. Logic thread only.
    StableRowStorage<std::array<VoiceEffectSlotConfig, kMaxFxSlotsPerVoice>>
        effectSlotConfig;

    // Logic-side mirror of each render slot's voice sends, indexed [voice slot
    // index][send slot]. Reset for a slot at each start, written at
    // Vn_SetVoiceSend / Vn_ClearVoiceSend / Vn_SetVoiceSendLevel, captured at
    // park, and staged for a pending start. Logic thread only.
    StableRowStorage<std::array<VoiceSendConfig, kMaxVoiceSendSlots>>
        voiceSendConfig;
    VoicePool::SlotStorageRegistration logicVoiceStorageRegistration;

    bool PrepareRetainedProfileKeyStorage() noexcept {
        const std::uint64_t count = kMaxAttenuationProfiles +
            3ull * config.voiceCapacity.Logical() + spatial.RetainStorageCapacity();
        if (count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) return false;
        try { retainedProfileKeyScratch.reserve(static_cast<std::size_t>(count)); }
        catch (...) { return false; }
        return true;
    }
    bool PrepareLogicVoiceStorage(std::uint32_t count) noexcept {
        if (!params.PrepareVoiceStorage(count) || !spatial.PrepareVoiceStorage(count) ||
            !voiceOriginPlayable.ConstructThrough(count) || !voicePlayFacts.ConstructThrough(count) ||
            !playbackPaths.ConstructThrough(count) || !timelineVoiceInstallIdentities.ConstructThrough(count) ||
            !voiceEffectInstanceRefs.ConstructThrough(count) || !effectSlotConfig.ConstructThrough(count) ||
            !voiceSendConfig.ConstructThrough(count)) return false;
        const auto previous = effectSlotTypes.size();
        if (!effectSlotTypes.ConstructThrough(count)) return false;
        for (auto index = previous; index < effectSlotTypes.size(); ++index)
            effectSlotTypes[index].fill(kEffectTypeNone);
        return true;
    }

    // Bus->bus send-edge membership, the logic source of truth the topology
    // planner sorts. Row-major by from-bus: busSendAdj[from*busSendSlotsPerBus
    // + s] is a target dense index or kInvalidBusIndex. Sized once at init.
    // Logic thread only.
    std::vector<std::uint32_t> busSendAdj;
    // Provenance imprints for the live Bus slot and each exact Bus Send slot.
    // A zero cookie denotes a caller-created object. Sized once at init and
    // indexed in lockstep with BusTree dense indices / busSendAdj.
    std::vector<VnPackHandle> busOwnerCookies;
    struct TopologyRetirementV1 {
        std::uint32_t generation = 0;
        std::uint64_t revision = 0;
        std::uint64_t ownerId = 0;
        VnPackHandle cookie = 0;
    };
    std::vector<TopologyRetirementV1> busRetirements;
    std::vector<VnPackHandle> busSendOwnerCookies;
    std::vector<std::uint64_t> busTopologyOwnerIds;
    std::vector<std::uint64_t> busSendTopologyOwnerIds;
    std::vector<float> busSendLevelDb;
    std::vector<BusTopologyOwnerRecord> busTopologyOwners;
    std::vector<PreviewMasterHolderV1> previewMasterHolders;

    struct CurrentWorldBusAdmissionRowV1 {
        WorldGeneration generation = 0;
        BusAdmissionClaimKeyV1 key{};
        BusAdmissionClaimState state = BusAdmissionClaimState::Closed;
        bool occupied = false;
    };
    std::vector<CurrentWorldBusAdmissionRowV1> currentWorldBusAdmission;

    struct MasterClaimStateV1 {
        std::uint64_t fingerprint = 0;
        std::uint8_t packHeld = 0;
        std::uint8_t previewHeld = 0;
        std::uint8_t directHeld = 0;
    };
    MasterClaimStateV1 masterClaim{};
    bool canonicalPackSetBusy = false;
    struct CanonicalPackSetTombstoneV1 {
        bool hasTombstone = false;
        std::uint64_t setRevision = 0;
        std::uint64_t unloadRevision = 0;
        std::uint64_t semanticKey = 0;
        std::uint64_t activationGeneration = 0;
        VnPackHandle initHandle = 0;
        std::uint64_t busContentGeneration = 0;
        std::uint64_t publishedBusFingerprint = 0;
        std::uint64_t compileTargetProfileFingerprint = 0;
    };
    CanonicalPackSetTombstoneV1 packSetTombstone{};

    // Logic-side authoritative per-bus output fold config (the multichannel
    // amendment): busOutputLayout[i] is bus i's presented output layout (the
    // master's is the device layout; a value equal to the mix layout means no
    // fold), and busDownmixCoeffs[i*64..] its dst x src matrix (coeffs[r*8+c] =
    // gain src c -> dst r). Sized to maxBuses at init. FillPlan copies these
    // into every published plan, delivering Vn_SetBusOutputLayout /
    // Vn_SetBusDownmixMatrix / Vn_ResetBusDownmixMatrix to the audio thread
    // through the existing plan-swap command (zero new opcode). Logic thread
    // only.
    std::vector<std::uint32_t> busOutputLayout;
    std::vector<float> busDownmixCoeffs;

    // Logic-side mirror of the effect type installed in each bus slot, indexed
    // [dense bus index][chain slot]. Written at Vn_SetBusEffect, cleared at
    // Vn_ClearBusEffect, and read at Vn_SetBusEffectParam so the value range is
    // validated against the installed type — the voice effectSlotTypes analogue.
    // Buses have no park/return, so the EffectParamTargetStore is their full
    // parameter-value mirror. Per-slot bypass also lives here so Monitor can
    // copy the last committed target when an empty slot later installs an
    // Effect. Logic thread only.
    std::vector<std::array<std::uint32_t, kMaxFxSlotsPerBus>> busEffectSlotTypes;
    std::vector<std::array<EffectInstanceRef, kMaxFxSlotsPerBus>>
        busEffectInstanceRefs;
    std::vector<std::array<VnPackHandle, kMaxFxSlotsPerBus>>
        busEffectOwnerCookies;
    std::vector<std::array<std::uint8_t, kMaxFxSlotsPerBus>> busEffectSlotBypass;
    std::vector<std::uint32_t> busFaderAfterSlots;

    // Per-bus mute/solo state, indexed by dense bus index. Both are logic-side
    // audibility state composed at the bus-volume publish point (the wire value
    // interpose): they never touch the BusTargetStore combine slots, so the
    // authored volume base and Variable contributions survive a mute/solo
    // round trip untouched. busSoloActiveCount caches the soloed-bus total so
    // the no-solo fast path costs one integer compare. Logic thread only.
    std::vector<std::uint8_t> busMuted;
    std::vector<std::uint8_t> busSoloed;
    std::uint32_t busSoloActiveCount = 0;

    // Bus-graph observability + refusal counters (logic thread only).
    // topologyPlanCount counts plan publishes (a level change publishes
    // nothing, so it stays static across send-level ramps — §9.7 pins this).
    // topologyRefusedCount counts structural changes refused by a full plan
    // pool or command admission. noSendCount counts send connections refused by
    // a full per-bus/per-voice slot table. sendSlotHighWater is the most send
    // slots (bus edges + voice sends) tracked live at once.
    std::uint64_t topologyPlanCount = 0;
    std::uint64_t topologyRefusedCount = 0;
    std::uint64_t noSendCount = 0;
    std::uint64_t busSendActive = 0;   // active bus->bus edges
    std::uint64_t voiceSendActive = 0; // active voice->bus send slots
    std::uint64_t sendSlotHighWater = 0;
    std::uint64_t acceptedBusTopologyRevision = 0;
    VnResult lastTopologyRefusalResult = VN_OK;
    std::uint32_t lastTopologyRefusalCategory = 0;
    std::uint64_t lastTopologyRefusalOwner = 0;
    VnBusId lastTopologyRefusalBusId{};
    VnBusId lastTopologyRefusalRelatedBusId{};
};

inline bool Engine::OriginalPlayableServiceLifetimeHolder::Borrow(
    OriginalPlayableServiceLifetimePin* out) const noexcept {
    return engine_ != nullptr && engine_->BorrowOriginalPlayableServiceLifetime(*this, out);
}

inline bool Engine::OriginalQueueFactoryLifetimePin::Valid() const noexcept {
    return engine_ != nullptr && service_.Valid() && queue_.Valid() &&
        engine_->playableInstances.IsOriginalLifetimePinFor(root_, create_.rootInstance);
}
inline bool Engine::OriginalQueueFactoryLifetimePin::Borrow(OriginalQueueFactoryLifetimePin* out) const noexcept {
    return engine_ != nullptr && engine_->CloneOriginalQueueFactoryLifetime(*this, out);
}
inline bool Engine::OriginalQueueFactoryLifetimeHolder::Borrow(OriginalQueueFactoryLifetimePin* out) const noexcept {
    return engine_ != nullptr && engine_->BorrowOriginalQueueFactoryLifetime(*this, out);
}

inline bool Engine::OriginalPlayableServiceLifetimePin::Valid() const noexcept {
    return engine_ != nullptr && engine_->MatchesOriginalPlayableServiceLifetime(*this);
}

inline bool Engine::OriginalPlayableServiceLifetimePin::Borrow(OriginalPlayableServiceLifetimePin* out) const noexcept {
    return out != this && Valid() && engine_->BorrowOriginalPlayableServiceLifetime(*original_, out);
}

inline VnResult AcquireMasterClaimKind(Engine& engine, std::uint8_t kind,
                                      std::uint64_t fingerprint) {
    auto& claim = engine.masterClaim;
    std::uint8_t* bit = nullptr;
    if (kind == 1) {
        bit = &claim.packHeld;
    } else if (kind == 2) {
        bit = &claim.previewHeld;
    } else if (kind == 3) {
        bit = &claim.directHeld;
    } else {
        return VN_INVALID_ARG;
    }
    if (claim.packHeld == 0 && claim.previewHeld == 0 && claim.directHeld == 0) {
        claim.fingerprint = fingerprint;
        *bit = 1;
        return VN_OK;
    }
    if (claim.fingerprint != fingerprint) {
        return VN_IN_USE;
    }
    *bit = 1;
    return VN_OK;
}

inline void ReleaseMasterClaimKind(Engine& engine, std::uint8_t kind) noexcept {
    auto& claim = engine.masterClaim;
    if (kind == 1) {
        claim.packHeld = 0;
    } else if (kind == 2) {
        claim.previewHeld = 0;
    } else if (kind == 3) {
        claim.directHeld = 0;
    }
    if (claim.packHeld == 0 && claim.previewHeld == 0 && claim.directHeld == 0) {
        claim.fingerprint = 0;
    }
}

namespace detail {

inline bool ActionConsumerCanReachImmediateQuantum(
    std::uint64_t target, std::uint64_t frontier, std::uint64_t rendered,
    std::uint32_t pageCount, std::size_t effectiveCapacity,
    std::uint32_t maxSlicesPerPage, std::size_t preparedSliceRows) noexcept {
    if (target < frontier || target < rendered) return false;
    if (frontier >= rendered) return true;
    const auto lag = rendered - frontier;
    return lag <= pageCount && lag < target - rendered &&
        (effectiveCapacity / 2 >= maxSlicesPerPage || effectiveCapacity >= preparedSliceRows);
}

// The live engine instance, or nullptr outside Init/Term. For host and test
// code only; never reachable through the public header.
Engine* ActiveEngine();
bool ResetPlayableServiceDeferredForTests(Engine& engine, EnginePlayableServiceRow& row) noexcept;
bool CanResetActionPlayScratchForTests(EngineActionPlayScratch& scratch) noexcept;
bool ProvePrivateInitialVcLaneTransferForTests(Engine&, EngineActionPlayScratch&) noexcept;
bool ProvePrivateInitialVcPageTransferForTests(Engine&, EngineActionPlayScratch&) noexcept;
bool ProvePrivateInitialVcPageBatchTransferForTests(Engine&, EngineActionPlayScratch&) noexcept;
bool CommitPreparedVariableControlOwnerForTests(
    EngineActionPlayScratch::PreparedVariableControlOwnerContext* context) noexcept;
#if !defined(HARKEN_RT_QUALIFICATION)
// Internal construction-only limits; zero restores the resolved default.
// Tests can reduce either bound before Init without mutating a live config.
void SetPlayableCapacitiesForTests(std::uint32_t playable, std::uint32_t modes) noexcept;
// kind: 1=pack, 2=preview, 3=direct. Same-F is idempotent; different F is VN_IN_USE.
VnResult AcquireMasterClaimForTests(std::uint8_t kind, std::uint64_t fingerprint);
void ReleaseMasterClaimForTests(std::uint8_t kind) noexcept;
#endif
bool CancelPreparedQueueInstanceOwnerForTests(
    EngineActionPlayScratch::PreparedQueueInstanceOwnerContext* context)
    noexcept;
bool CancelPreparedTimelineVoiceOwnerForTests(
    EngineActionPlayScratch::PreparedTimelineVoiceOwnerContext* context)
    noexcept;
bool CancelPreparedOccurrenceOwnerForTests(
    EngineActionPlayScratch::PreparedOccurrenceOwnerContext* context) noexcept;

} // namespace detail

class VariableOperationScope {
public:
    explicit VariableOperationScope(Engine* engine, bool hold = true) noexcept
        : engine_(engine), activeEngine_(engine != nullptr && detail::ActiveEngine() == engine), hold_(hold) {
        if (engine_ != nullptr && hold_) ++engine_->variableOperationDepth;
    }
    ~VariableOperationScope() noexcept;
    VariableOperationScope(const VariableOperationScope&) = delete;
    VariableOperationScope& operator=(const VariableOperationScope&) = delete;
private:
    Engine* engine_;
    bool activeEngine_;
    bool hold_;
};

} // namespace harken

#endif // HARKEN_API_ENGINE_H
