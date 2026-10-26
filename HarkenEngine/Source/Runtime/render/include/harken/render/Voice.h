// Per-voice render state and the Voice Render Cascade result protocol.
//
// The cascade is a fixed function sequence over the per-voice POD
// VoiceRenderContext:
//
//   source decode -> resample -> insert effect -> tone filter -> gain
//
// executed once per block in a single forward pass — no pull protocol, no
// node objects, no per-stage buffer ownership (scratch comes from the mixer
// loop's per-block arena).
#ifndef HARKEN_RENDER_VOICE_H
#define HARKEN_RENDER_VOICE_H

#include "harken/core/BlockArena.h"
#include "harken/core/ChannelLayout.h"
#include "harken/core/CompletionProgress.h"
#include "harken/core/SourceTerminalRecord.h"
#include "harken/core/PcmView.h"
#include "harken/core/StreamRing.h"
#include "harken/core/TimelineProgram.h"
#include "harken/core/TimelineExecutionOrder.h"
#include "harken/core/FrontierActivation.h"
#include "harken/core/DecisionToken.h"
#include "harken/core/VariableControlRuntime.h"
#include "harken/core/VariableWeightControl.h"
#include "harken/core/CurrentSelection.h"
#include "harken/dsp/Biquad.h"
#include "harken/dsp/GainRamp.h"
#include "harken/dsp/Resampler.h"
#include "harken/render/InsertEffect.h"
#include "harken/render/SpatialMatrix.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>

namespace harken {

// Lifecycle fact of the stage result protocol. Orthogonal to data
// availability: framesProduced says how much data this block carries,
// SourcePhase says where the voice is in its life. "Produced 0 while
// Active" is the starvation condition (accounting exists from M1; it
// becomes reachable with M2 streaming).
enum class SourcePhase : std::uint8_t {
    Active = 0,   // real source material still flowing
    Draining = 1, // source finished; effect and tone tails still sounding
    Done = 2      // nothing further; the voice slot can be released
};

enum class PostEffectTailContribution : std::uint32_t {
    Lowpass = 0,
    Highpass = 1,
    Shelf = 2,
    HighShelf = 3,
};

inline constexpr std::uint32_t kNoTailRearmPosition =
    std::numeric_limits<std::uint32_t>::max();

constexpr std::uint32_t TailContributionPositionAfterEffects(
    std::uint32_t fxSlotCount, PostEffectTailContribution contribution) {
    return fxSlotCount + static_cast<std::uint32_t>(contribution);
}

// The two orthogonal facts a block render reports.
struct StageResult {
    std::uint32_t framesProduced = 0; // 0..blockFrames
    SourcePhase phase = SourcePhase::Done;
};

// One fixed-duration transition for a scalar spatial fold parameter. Progress
// identifies the next frame to render; retargeting therefore starts from the
// value heard on the preceding frame, not the next value of the old trajectory.
struct SpatialScalarTransition {
    float start = 0.0f;
    float target = 0.0f;
    std::uint32_t totalFrames = 1;
    std::uint32_t progress = 1;
    float inverseSpan = 1.0f;

    bool Settled() const { return progress >= totalFrames; }

    float Mix() const {
        if (Settled() || progress + 1 >= totalFrames) {
            return 1.0f;
        }
        return static_cast<float>(progress) * inverseSpan;
    }

    float Value() const {
        const float mix = Mix();
        if (mix <= 0.0f) {
            return start;
        }
        if (mix >= 1.0f) {
            return target;
        }
        return start + (target - start) * mix;
    }

    void SnapTo(float value) {
        start = value;
        target = value;
        totalFrames = 1;
        progress = 1;
        inverseSpan = 1.0f;
    }

    void Retarget(float value, std::uint32_t requestedFrames) {
        const float live = LastRenderedValue();
        if (live == value || requestedFrames <= 1) {
            SnapTo(value);
            return;
        }
        start = live;
        target = value;
        totalFrames = requestedFrames;
        progress = 0;
        inverseSpan = 1.0f / static_cast<float>(requestedFrames - 1);
    }

    void AdvanceFrame() {
        if (Settled()) {
            return;
        }
        ++progress;
        if (progress >= totalFrames) {
            start = target;
        }
    }

private:
    float LastRenderedValue() const {
        if (Settled()) {
            return target;
        }
        if (progress == 0) {
            return start;
        }
        const float mix =
            static_cast<float>(progress - 1) * inverseSpan;
        return start + (target - start) * mix;
    }
};

// One voice->bus send slot's audio-thread state. The send contribution is the
// voice's post-pan/post-width stereo block (the identical block that feeds its
// dry bus) scaled by this level, added to the target bus's input — so the
// spatial image travels with the send. Membership is per-voice (no cycles: a
// voice receives nothing); the level ramps with the same block-boundary
// discipline as a bus gain. An inactive slot (targetBusIndex == kInvalidBusIndex
// sentinel, mirrored by active) contributes nothing.
struct VoiceSendSlot {
    bool active = false;
    std::uint32_t targetBusIndex = 0; // dense bus index of the send target
    std::uint32_t targetBusGeneration = 0;
    GainRamp gain;
    bool ramping = false;
    float rampStartGain = 1.0f;
    float rampEndGain = 1.0f;
    std::uint64_t rampBlocksTotal = 0;
    std::uint64_t rampBlocksDone = 0;
};

struct VoiceTimelineEvent {
    struct StopPayload {
        AuthoredStopOperationIdentity authoredStopOperation{};
        MonitorControlCause controlCause{};
    };
    struct WeightPayload {
        WeightSegmentV1 variableWeight{};
        VariableWeightControlIdentity variableWeightOwner{};
    };

    VoiceTimelineEvent() noexcept = default;
    explicit VoiceTimelineEvent(TimelineEventKind kind) noexcept : kind_(kind) {
        if (kind == TimelineEventKind::Stop) std::construct_at(&payload_.stop);
        else if (kind == TimelineEventKind::VariableWeight) std::construct_at(&payload_.weight);
    }

    TimelineEventKind Kind() const noexcept { return kind_; }
    StopPayload& Stop() noexcept {
        assert(kind_ == TimelineEventKind::Stop);
        return payload_.stop;
    }
    const StopPayload& Stop() const noexcept {
        assert(kind_ == TimelineEventKind::Stop);
        return payload_.stop;
    }
    WeightPayload& Weight() noexcept {
        assert(kind_ == TimelineEventKind::VariableWeight);
        return payload_.weight;
    }
    const WeightPayload& Weight() const noexcept {
        assert(kind_ == TimelineEventKind::VariableWeight);
        return payload_.weight;
    }

private:
    // A kind transition copies a complete event, including its active payload.
    TimelineEventKind kind_ = TimelineEventKind::Start;

public:
    std::uint16_t offset = 0;
    TimelineEnvelopeDirection envelopeDirection =
        TimelineEnvelopeDirection::FadeIn;
    bool authoredFadeOutCandidate = false;
    bool authoredRootStop = false;
    bool firstOutputDeferred = false;
    std::uint64_t frame = 0;
    std::uint64_t endFrame = 0;
    std::uint64_t value = 0;
    const CurveLUT* envelopeCurve = nullptr;
    TimelineExecutionOrder order{};
    FrozenFrontierEventProof frontierProof{};

private:
    union Payload {
        std::uint8_t empty;
        StopPayload stop;
        WeightPayload weight;
        Payload() noexcept : empty{} {}
    } payload_{};
};

static_assert(std::is_trivially_copyable_v<VoiceTimelineEvent>);
static_assert(std::is_trivially_copy_assignable_v<VoiceTimelineEvent>);
static_assert(std::is_trivially_destructible_v<VoiceTimelineEvent>);

struct VoiceEventSpan {
    std::uint32_t renderGeneration = 0;
    std::uint64_t quantumIndex = 0;
    std::uint32_t eventCount = 0;
    VoiceTimelineEvent events[kTimelineMaxCombinedEventsPerVoiceQuantum]{};
};

struct VoiceRenderContext {
    // Keep authoritative block eligibility state together; all owners and checks remain unchanged.
    StreamRing* streamRing = nullptr;
    std::uint32_t voiceSendActiveCount = 0;
    std::uint32_t fxSlotCount = 0;
    std::uint32_t variableWeightControlCount = 0;
    std::uint32_t tailRemaining = 0;
    std::uint32_t tailRearmPosition = kNoTailRearmPosition;
    SourcePhase phase = SourcePhase::Done;
    bool dynamicVoiceTapActive = false;
    bool streamParkActive = false;
    bool lpfEnabled = false;
    bool hpfEnabled = false;
    bool shelfEnabled = false;
    bool highShelfEnabled = false;
    bool variableWeightActive = false;
    bool spatialMatrixDirty = true;
    bool fading = false;
    bool tailStarted = false;
    bool timelineStopPending = false;
    bool timelineFallbackStopActive = false;
    bool authoredRootStopApplied = false;

    SourceTerminalRecord* sourceTerminal = nullptr;
    SourceTerminalToken sourceTerminalToken{};
    std::uint32_t sourceTerminalStreamGeneration = 0;
    SourceTerminalKind stagedSourceTerminal = SourceTerminalKind::None;
    SourceTerminalKind stagedPlaybackEnd = SourceTerminalKind::None;
    MonitorControlCause monitorAdoptedControlCause{};
    bool monitorControlAdopted = false;
    bool monitorTimelineControlAdopted = false;
    bool monitorStoppedBeforeCascade = false;
    std::uint32_t stagedSourceTerminalOffset = 0;
    std::uint32_t stagedPlaybackEndOffset = 0;
    const BiquadTrigTable* biquadTrig = nullptr;
    std::uint32_t engineSampleRate = 0;
    // Stable public identity for effect-private Voice taps. A render slot can
    // be released and reused, and a parked voice can return in another slot,
    // without changing this route-facing identity.
    VnVoiceHandle dynamicSourceIdentity = 0;
    // Source: immutable registered PCM plus the read cursor.
    PCMView pcm;
    std::uint64_t srcCursor = 0;
    std::uint64_t sourceActivationStartFrame = 0;
    std::uint64_t authoredSourceDeadlineFrame = UINT64_MAX;
    // Pending immediate starts derive finite duration from the admitted Start.
    std::uint64_t pendingAuthoredDurationFrames = UINT64_MAX;
    // The initial variable-control weights are prepared before the lane's
    // final anchor is known. The audio owner shifts them at the first Start.
    std::uint64_t pendingImmediateWeightBaseFrame = 0;
    bool pendingImmediateWeightRebase = false;
    std::uint32_t pendingImmediateWeightCount = 0;
    std::uint32_t sourceActivationLoopsRemaining = 1;

    // Looping. The loop region is [loopStartFrame, loopEndFrame) in source
    // frames, resolved on the logic side at play time (pack loop points when
    // loopable, else the whole file); the audio thread never looks it up.
    // The intro [0, loopStartFrame) plays once, then the region repeats:
    // loopInfinite loops until stopped, otherwise loopPassesRemaining counts
    // the region traversals still to do (including the one in progress). On
    // the last traversal the read continues past loopEndFrame through the
    // outro [loopEndFrame, pcm.frames) and ends normally. A single-pass
    // voice (loopPassesRemaining == 1, not infinite) never wraps and reads
    // the whole file exactly like a non-looping voice.
    std::uint64_t loopStartFrame = 0;
    std::uint64_t loopEndFrame = 0;
    bool hasExitMarker = false;
    std::uint64_t exitMarkerSourceFrame = 0;
    bool loopInfinite = false;
    std::uint32_t loopPassesRemaining = 1;
    std::uint32_t completedLoopPasses = 0;

    // Streamed source. When streamRing is non-null the source decode stage
    // feeds the resampler from the ring instead of from pcm (pcm then only
    // carries channelCount and sampleRate for the cascade; frames stays 0
    // and no loop fields apply). The pointer is wired on the logic side
    // before the play command publishes it, and cleared on release after
    // the audio thread stores Retired into the ring's lifecycle word.
    std::uint64_t streamRingToken = 0;

    std::uint64_t streamActivationStartFrame = 0;
    std::uint64_t streamFeedActivationStartFrame = 0;
    std::uint64_t streamFrameCount = 0;
    std::uint64_t streamLoopStartFrame = 0;
    std::uint64_t streamLoopEndFrame = 0;
    std::uint32_t streamActivationLoopsRemaining = 1;
    std::uint32_t streamFeedActivationLoopsRemaining = 1;

    VnVoiceHandle streamParkPublicHandle = 0;
    std::uint64_t streamParkRequestSequence = 0;

    // Prebuffer gate. The voice renders silence and reports Preparing to
    // the snapshot until the ring holds prebufferTargetFrames (or the
    // source already ended — a short file can never reach the target).
    // Once open the gate never closes; a later underrun is starvation,
    // not re-preparation.
    bool streamGateOpen = false;
    std::uint32_t prebufferTargetFrames = 0;

    // Starvation facts, audio-thread-owned. starvedThisBlock is a data
    // fact of the last rendered block (gate open, source live, ring could
    // not fill the block); the cooldown spaces STARVED notifications exactly
    // starvationCooldownBlocks apart under continuous starvation.
    bool starvedThisBlock = false;
    std::uint32_t starveCooldownRemaining = 0;

    // Dense index of the bus this voice mixes into (0 = master). Resolved on
    // the logic side at play time and migrated to the surviving parent when a
    // bus-destroy cascade tombstones its current route.
    std::uint32_t busIndex = 0;
    std::uint32_t busGeneration = 0;

    // Voice->bus send slots: a fixed-capacity array processed after the dry
    // fold. voiceSendActiveCount is the number of currently active slots; when
    // it is 0 the fold is bit-identical to the pre-send path (no tap captured,
    // no extra arena use), which preserves the no-send invariant.
    VoiceSendSlot voiceSends[kMaxVoiceSendSlots];

    ResamplerView resampler;

    // Insert-effect chain: a fixed-capacity slot array processed in
    // ascending order, with fxSlotCount slots active (the resolved
    // fxSlotsPerVoice). An empty slot (process == nullptr) is skipped at
    // zero cost — bit-identical to a passthrough. Per slot, fxBypassMix is
    // the settled dry/wet blend (0 = fully wet/active, 1 = fully bypassed/
    // dry); a bypass toggle sets fxBypassTarget and arms fxBypassRamping,
    // which crossfades across exactly one block. An occupied slot always
    // runs its process (so a bypassed effect's tail keeps ringing and is
    // never truncated); bypass only governs how much of the wet result the
    // blend keeps.
    InsertEffect fxSlots[kMaxFxSlotsPerVoice];
    float fxBypassMix[kMaxFxSlotsPerVoice] = {};
    float fxBypassTarget[kMaxFxSlotsPerVoice] = {};
    bool fxBypassRamping[kMaxFxSlotsPerVoice] = {};
    bool compressorParamTransitionThisBlock[kMaxFxSlotsPerVoice] = {};
    bool compressorBypassTransitionThisBlock[kMaxFxSlotsPerVoice] = {};

    // Identity of the pooled instance in each occupied slot (type ==
    // kEffectTypeNone when empty). The audio thread sets this when it installs
    // an effect and reads it to name the instance on the retire queue when the
    // slot gives it up (voice end, clear, or in-place replace), so the logic
    // thread returns exactly the right instance to its pool.
    EffectInstanceRef fxSlotEffectRef[kMaxFxSlotsPerVoice];

    // Tone stage. Disabled filters are exact bypasses so an untouched voice
    // is bit-transparent through this stage; enabling happens when a cutoff
    // is applied (control APIs land in a later segment; tests drive these
    // fields directly today).
    BiquadCoeffs lpfCoeffs;
    BiquadCoeffs hpfCoeffs;
    std::uint32_t lpfTailFrames = 0;
    std::uint32_t hpfTailFrames = 0;
    BiquadState lpfState[kMaxChannels];
    BiquadState hpfState[kMaxChannels];
    BiquadTransitionState<kMaxChannels> lpfTransition;
    BiquadTransitionState<kMaxChannels> hpfTransition;
    BiquadParameterGlide lpfGlide;
    BiquadParameterGlide hpfGlide;

    // Low and high shelves complete the fixed post-filter Tone Strip before
    // gain. A 0 dB shelf is disabled — an exact bypass, like the filters.
    BiquadCoeffs shelfCoeffs;
    std::uint32_t shelfTailFrames = 0;
    BiquadState shelfState[kMaxChannels];
    BiquadTransitionState<kMaxChannels> shelfTransition;
    BiquadParameterGlide shelfGlide;

    BiquadCoeffs highShelfCoeffs;
    std::uint32_t highShelfTailFrames = 0;
    BiquadState highShelfState[kMaxChannels];
    BiquadTransitionState<kMaxChannels> highShelfTransition;
    BiquadParameterGlide highShelfGlide;

    // Combined-ratio factor for runtime pitch updates: source rate over
    // engine rate, fixed at voice start. A resolved caller ratio arriving
    // by command multiplies this before it reaches the resampler.
    double rateScale = 1.0;

    // Gain stage: authored voice volume, frame-accurate OutputGain.
    // N = ceil(rampMs * rate / 1000) produced samples; 0 snaps.
    OutputGain gain;
    OutputGain outputGain;
    WeightSegmentV1 variableWeightSegment{};
    struct OwnedVariableWeight {
        VariableWeightControlIdentity owner{};
        WeightSegmentV1 segment{};
        bool active = false;
        // Existing entries keep their owner and position until the voice is reset.
        std::uint32_t initialRebaseGeneration = 0;
        double inverseSpan = 0.0;
    };
    std::span<OwnedVariableWeight> variableWeightControls{};

    bool ApplyVariableWeightControl(VariableWeightControlIdentity owner,
        const WeightSegmentV1& segment) noexcept {
        float checked = 0.0f;
        if (!owner.Valid() || !ResolveWeightSegment(segment, segment.startFrame, &checked)) return false;
        for (std::uint32_t index = 0; index < variableWeightControlCount; ++index) {
            auto& binding = variableWeightControls[index];
            if (binding.owner != owner) continue;
            if (!binding.active || segment.segmentGeneration <= binding.segment.segmentGeneration) return false;
            binding.segment = segment;
            binding.active = true;
            binding.inverseSpan = 1.0 / static_cast<double>(segment.endFrame - segment.startFrame - 1u);
            return true;
        }
        if (variableWeightControlCount == variableWeightControls.size()) return false;
        variableWeightControls[variableWeightControlCount++] = {owner, segment, true, 0,
            1.0 / static_cast<double>(segment.endFrame - segment.startFrame - 1u)};
        return true;
    }

    // Emitter position, stored per voice. All acoustic geometry is
    // computed on the logic side from its own mirror; this copy exists
    // for future render-rate consumers and observability.
    float positionX = 0.0f;
    float positionY = 0.0f;
    float positionZ = 0.0f;

    // Mono and stereo fold through one matrix after source-channel processing.
    // spatialized is fixed at play. Logic publishes pan2d without geometry and
    // pan with geometry; the legacy front pair remains observable metadata.
    // Pan, width and Mix changes retarget the same complete gain image at the
    // first actual output frame. Wider 2D sources retain role-based mapping.
    bool spatialized = false;
    float pan = 0.0f;   // -1 full left .. +1 full right
    float panGain0 = 1.0f;
    float panGain1 = 0.0f;
    std::uint32_t panChannel0 = 0;
    std::uint32_t panChannel1 = kInvalidPanChannel;
    float pan2d = 0.0f;
    bool spatialMatrixHasRendered = false;
    SpatialMatrixTransition spatialMatrix;
    float width = 0.0f; // 0 point source .. 1 full image
    float spatializationMix = 1.0f; // 0 authored 2D image .. 1 point-source image

    // Wider 2D sources retain their front-channel balance transitions.
    SpatialScalarTransition balanceLeftTransition{1.0f, 1.0f, 1, 1, 1.0f};
    SpatialScalarTransition balanceRightTransition{1.0f, 1.0f, 1, 1, 1.0f};

    // Fade-stop: linear ramp to zero over fadeTotalFrames, then truncate.
    std::uint32_t fadeTotalFrames = 0;
    std::uint32_t fadeDoneFrames = 0;
    std::uint32_t fadeStartOffsetInBlock = 0;

    // Effect and tone tail drain bookkeeping. tailRemaining is the serial
    // aggregate still to sound; it is recomputed every drain block from the
    // occupied effects and enabled tone sections, so clearing or replacing a
    // section mid-drain shortens it instead of freezing the value captured
    // when drain began. tailElapsed is the running count of frames already
    // drained since the source ended, so a recomputed aggregate resumes from
    // the right position rather than restarting. A state-preserving update
    // records the earliest serial contribution whose suffix must be rearmed
    // at the next aggregate computation. tailStarted marks that drain began.
    std::uint32_t tailElapsed = 0;
    // True when the suffix at tailRearmPosition (as it stood before any
    // update still pending in this rearm batch was applied) was already
    // reporting an infinite tail. Distinguishes "this batch's suffix was
    // already unbounded" from "this batch just made it unbounded" so
    // ApplyPendingTailRearm only restarts tailElapsed for the latter.
    bool tailRearmSuffixWasInfinite = false;

    // Stage taps: peak absolute sample of the current block at each
    // cascade stage boundary, max across channels, linear amplitude. In
    // this cascade source decode and resample fuse into one forward pass,
    // so they form a single observable boundary (postResample). The
    // fields always exist so snapshot layout is build-invariant; they
    // stay 0 when tap measurement is compiled out.
    float tapPostResample = 0.0f;
    float tapPostFx = 0.0f;
    float tapPostFilter = 0.0f;
    float tapPostGain = 0.0f;

    // Sample-accurate start: frames skipped at the front of the first
    // rendered block. Rendering begins directly at this write position.
    std::uint32_t startOffsetFrames = 0;
    bool firstBlock = true;
    bool timelineGated = false;
    std::uint32_t timelineArmSlot = UINT32_MAX;
    std::uint32_t timelineArmGeneration = 0;
    ParentAudioGroupRef timelineParentGroup{};
    std::uint16_t timelineGroupMemberIndex = UINT16_MAX;
    struct TimelineEnvelopeState {
        bool active = false;
        bool fadeOut = false;
        std::uint64_t startFrame = 0;
        std::uint64_t endFrame = 0;
        bool useCurve = false;
        CurveLUT curve{};
    };
    TimelineEnvelopeState timelineEnvelopes[2]{};
    bool timelineFadeOutCoveredSample = false;
    bool timelineStopAuthored = false;
    std::uint32_t timelineStopOffset = 0;
    std::uint32_t timelineStopDeclickFrames = 0;
    std::uint64_t timelineStopFrame = UINT64_MAX;
    std::uint64_t timelineFallbackStopFrame = UINT64_MAX;
    std::uint64_t authoredRootStopFrame = UINT64_MAX;
    std::uint32_t authoredRootStopEvent = UINT32_MAX;
    VoiceTimelineEvent authoredRootStopControl{TimelineEventKind::Stop};

    StageResult lastResult; // most recent block's protocol facts
    CompletionProgressRow* completionProgress = nullptr;
    OccurrenceToken completionOccurrence{};
    std::uint32_t completionRenderGeneration = 0;
    std::uint64_t playbackFactsRevision = 0;

    // Handoff between the cascade's three phases when the mixer splits them for
    // job-parallel rendering (base spec §9 per-voice independence; M5 §8.2).
    // PrepareVoiceCascade (serial) reserves the arena scratch and resolves the
    // block offset; RunVoiceCascade — which may run on a render worker — fills
    // the scratch and records how many frames it produced; FoldVoiceIntoBuses
    // (serial, fixed slot order) sums the scratch into the shared bus planes.
    // In serial rendering the three run back-to-back for one voice
    // (RenderVoiceBlock), reproducing the pre-split single-pass arithmetic.
    struct CascadeScratch {
        ResamplerUnitySpan borrowed{};
        std::uint32_t plainPcmFeedFrames = 0;
        bool plainPcmCandidate = false;
        float* scratch[kMaxChannels] = {}; // per-source-channel cascade output
        float* dry[kMaxChannels] = {};     // blend planes (only when needBlend)
        // Send-scatter tap (mix-layout wide): the post-pan/post-width image each
        // active send scatters a scaled copy of. Reserved in the serial pre-pass
        // alongside scratch/dry — right after this voice's dry, before the next
        // voice's scratch — so the arena allocation sequence is identical to the
        // serial per-voice pass and thus independent of the worker count. The
        // cascade and fold phases never touch the arena.
        float* tap[kMaxChannels] = {};
        bool tapReady = false; // sends active AND every tap plane reserved
        bool runnable = false; // pre-pass verdict: run this block
        bool needBlend = false; // a slot is bypassed/crossfading
        std::uint32_t offset = 0;   // frames skipped at block front
        std::uint32_t produced = 0; // frames the cascade produced
    };
    CascadeScratch cascade;
    std::uint32_t logicalSourceOutputFramesThisBlock = 0;
    std::uint32_t monitorRenderSlot = 0;
    std::uint32_t monitorRenderGeneration = 0;
    float* monitorPreChain[kMaxChannels] = {};
    std::uint32_t monitorPreChainFrames = 0;
    // Keep frequently read block state ahead of the large resident event array.
    VoiceEventSpan timelineEvents{};
};

// True if any tail contribution at a position in [lo, hi) is currently
// reporting an infinite tail. Reads only ctx's already-public per-section
// fields, so a caller can snapshot "was this range already unbounded"
// immediately before mutating one of the sections in it — the section being
// changed still reads its pre-update value (BiquadTransitionTailFrames only
// reflects a retarget once RequestBiquadTransition has run), and every other
// section in the range reads its genuinely current value.
inline bool AnyTailContributionInfiniteInRange(const VoiceRenderContext& ctx,
                                               std::uint32_t lo,
                                               std::uint32_t hi) {
    for (std::uint32_t s = 0; s < ctx.fxSlotCount; ++s) {
        if (s < lo || s >= hi) {
            continue;
        }
        const InsertEffect& fx = ctx.fxSlots[s];
        if (fx.process != nullptr && fx.tailFrames != nullptr &&
            fx.tailFrames(fx.state) == kInfiniteTail) {
            return true;
        }
    }
    const std::uint32_t lpfPos = TailContributionPositionAfterEffects(
        ctx.fxSlotCount, PostEffectTailContribution::Lowpass);
    if (lpfPos >= lo && lpfPos < hi &&
        BiquadSectionTailFrames(ctx.lpfTransition, ctx.lpfGlide,
                                ctx.lpfEnabled,
                                ctx.lpfTailFrames) == kInfiniteTail) {
        return true;
    }
    const std::uint32_t hpfPos = TailContributionPositionAfterEffects(
        ctx.fxSlotCount, PostEffectTailContribution::Highpass);
    if (hpfPos >= lo && hpfPos < hi &&
        BiquadSectionTailFrames(ctx.hpfTransition, ctx.hpfGlide,
                                ctx.hpfEnabled,
                                ctx.hpfTailFrames) == kInfiniteTail) {
        return true;
    }
    const std::uint32_t shelfPos = TailContributionPositionAfterEffects(
        ctx.fxSlotCount, PostEffectTailContribution::Shelf);
    if (shelfPos >= lo && shelfPos < hi &&
        BiquadSectionTailFrames(ctx.shelfTransition, ctx.shelfGlide,
                                ctx.shelfEnabled,
                                ctx.shelfTailFrames) == kInfiniteTail) {
        return true;
    }
    const std::uint32_t highShelfPos = TailContributionPositionAfterEffects(
        ctx.fxSlotCount, PostEffectTailContribution::HighShelf);
    if (highShelfPos >= lo && highShelfPos < hi &&
        BiquadSectionTailFrames(
            ctx.highShelfTransition, ctx.highShelfGlide,
            ctx.highShelfEnabled,
            ctx.highShelfTailFrames) == kInfiniteTail) {
        return true;
    }
    return false;
}

// Records a pending suffix rearm at `position`, widening the earliest queued
// position when a narrower one arrives. tailRearmSuffixWasInfinite tracks
// whether the suffix at the (possibly widening) position was already
// infinite before this rearm batch's updates — monotonic OR over each widen
// step, so a later, unrelated section going from finite to infinite within
// the same batch is never masked by an earlier, already-infinite one: only
// the newly covered band [position, previous) is (re)checked on a widen: the
// band already covered by a prior, narrower position keeps its earlier
// verdict, since sections in it may have been mutated since that snapshot.
// Must be called before the caller applies its own coefficient/tail update,
// so the section about to change still reports its pre-update contribution.
inline void QueueVoiceTailSuffixRearm(VoiceRenderContext& ctx,
                                      std::uint32_t position) {
    if (!ctx.tailStarted) {
        return;
    }
    const std::uint32_t previous = ctx.tailRearmPosition;
    if (previous == kNoTailRearmPosition) {
        const std::uint32_t end =
            ctx.fxSlotCount + 4u; // past HighShelf
        ctx.tailRearmSuffixWasInfinite =
            AnyTailContributionInfiniteInRange(ctx, position, end);
    } else if (position < previous) {
        ctx.tailRearmSuffixWasInfinite =
            ctx.tailRearmSuffixWasInfinite ||
            AnyTailContributionInfiniteInRange(ctx, position, previous);
    }
    ctx.tailRearmPosition = std::min(previous, position);
}

// Renders one block of the cascade and mixes the result into the planar
// accumulator of the voice's bus, then scatters a scaled copy of the same
// post-pan/post-width block into each active send target. `busPlanes` is the
// full bus-major plane base (bus i's channels start at busPlanes[i *
// mixChannels]); the voice folds into its own bus and reads its send targets'
// planes from the same base. `mixChannels` is the mix layout's channel count
// and `mixLayout` its VnChannelLayout — the widened fold-in pans / maps the
// cascade output (rendered in source channels) into these channels. They
// default to stereo, at which the fold-in takes the frozen two-channel law
// bit-for-bit (the regression floor), so an older two-argument call renders
// exactly as M4 did. Returns the protocol facts for this block; a Done phase
// means the caller releases the voice slot. Scratch comes from `arena`; on
// arena exhaustion the voice contributes nothing this block and the caller's
// degradation counter increments.
StageResult RenderVoiceBlock(VoiceRenderContext& ctx, BlockArena& arena,
                             float* const* busPlanes, std::uint32_t blockFrames,
                             std::uint32_t mixChannels = kOutputChannels,
                             std::uint32_t mixLayout = VN_CHANNEL_LAYOUT_STEREO,
                             std::uint64_t sourceFrameBegin = 0,
                             output_gain_detail::MonoStereoMixKernel monoStereoMix =
                                 &output_gain_detail::MixMonoToStereoScaled);

// The three phases RenderVoiceBlock is composed of, exposed so the mixer can
// run the cascade phase across a worker pool while keeping the fold serial.
//
// Phase 1 (serial): reserve arena scratch for one voice's cascade — the source
// scratch, the blend planes, and (when the voice has active sends) the
// mix-layout-wide send tap — and resolve the block offset. Blend and tap planes
// may share storage because Run completes before Fold overwrites the tap image.
// This is the only phase that touches the arena, and it runs in slot order, so
// the allocation sequence (and hence tight-arena degradation) is identical whether
// rendering is serial or fanned across workers. Returns true when the cascade
// should run this block; false when the voice contributes nothing — a streamed
// voice still prebuffering, or a scratch/blend shortfall — in which case
// ctx.lastResult carries the {0, phase} protocol facts exactly as the pre-split
// cascade returned. A send-tap shortfall does NOT decline the voice: it only
// drops this block's sends (ctx.cascade.tapReady stays false), matching the
// pre-split fold. Records its verdict and buffers in ctx.cascade for the phases
// below. `mixChannels` sizes the send tap.
std::uint64_t VoiceCascadeScratchBytes(
    const VoiceRenderContext& ctx, std::uint32_t blockFrames,
    std::uint32_t mixChannels) noexcept;

bool PrepareVoiceCascade(VoiceRenderContext& ctx, BlockArena& arena,
                         std::uint32_t blockFrames, std::uint32_t mixChannels);

// Phase 2 (worker-safe): run the per-voice cascade — source decode / resample,
// insert-effect chain, tone filter, gain, fade — into the scratch reserved by
// PrepareVoiceCascade. Reads and writes only ctx and its own scratch, so it is
// safe to run concurrently with other voices' cascades on a render worker.
// Records the produced frame count in ctx.cascade and the protocol facts in
// ctx.lastResult. Call only when PrepareVoiceCascade returned true.
void RunVoiceCascade(VoiceRenderContext& ctx, std::uint32_t blockFrames);

// Phase 3 (serial, fixed slot order): fold the completed cascade's scratch into
// the voice's bus accumulator and scatter its sends into the tap reserved by
// PrepareVoiceCascade. The float reduction into shared bus planes is
// order-dependent, so this runs single-threaded in slot order regardless of
// which worker produced the cascade — the determinism invariant (§8.2). Touches
// no arena (all scratch was reserved in phase 1). Call only when
// PrepareVoiceCascade returned true.
void FoldVoiceIntoBuses(VoiceRenderContext& ctx, float* const* busPlanes,
                        std::uint32_t blockFrames, std::uint32_t mixChannels,
                        std::uint32_t mixLayout,
                        std::uint64_t sourceFrameBegin = 0,
                             output_gain_detail::MonoStereoMixKernel monoStereoMix =
                                 &output_gain_detail::MixMonoToStereoScaled);

} // namespace harken

#endif // HARKEN_RENDER_VOICE_H
