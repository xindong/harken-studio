// Forward transport (logic -> audio): a single-producer single-consumer ring
// of fixed-size 64-byte POD command records. The producer is the externally
// synchronized C API (logic thread); the consumer is the mixer loop, which
// drains every pending command at the start of each rendered block.
//
// Payloads carry only values, IDs, and handles — never pointers into
// logic-owned mutable memory. Sample data crosses the boundary exclusively
// through the PCM registry's immutable views.
//
// A full queue refuses the push (the API surfaces VN_BACKPRESSURE and the
// caller retries after a pump); a refusal is counted so the condition stays
// observable. Storage is allocated once at engine init; push and pop touch
// no heap.
#ifndef HARKEN_CORE_COMMAND_QUEUE_H
#define HARKEN_CORE_COMMAND_QUEUE_H

#include <harken/Harken.h>

#include "harken/dsp/Biquad.h"
#include "harken/core/TimelineArmTable.h"
#include "harken/core/OriginalCatalogStageLoan.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

namespace harken {
class MixerLoop;
struct Engine;
namespace detail { struct CommandQueueSubmissionTestAccess; struct OriginalControlOriginTestAccess; }

// Command opcodes. The audio thread's command vocabulary is deliberately
// small and frozen (narrow waist): future features translate down to these
// primitives on the logic side instead of growing this set per feature.
inline constexpr std::uint16_t kOpcodePlay = 1;
inline constexpr std::uint16_t kOpcodeStop = 2;
inline constexpr std::uint16_t kOpcodeSetBusVolume = 3;
inline constexpr std::uint16_t kOpcodeSetVoiceVolume = 4;
inline constexpr std::uint16_t kOpcodeSetVoicePitch = 5;
inline constexpr std::uint16_t kOpcodeSetVoiceFilter = 6;
inline constexpr std::uint16_t kOpcodeSetVoicePosition = 7;
inline constexpr std::uint16_t kOpcodeSetVoicePan = 8;
inline constexpr std::uint16_t kOpcodeSetVoiceSpatialScalar = 9;
// Numeric/source compatibility alias for the pre-mix width command.
inline constexpr std::uint16_t kOpcodeSetVoiceWidth =
    kOpcodeSetVoiceSpatialScalar;
inline constexpr std::uint16_t kOpcodeSetVoiceLowShelf = 10;
inline constexpr std::uint16_t kOpcodeSetVoiceShelf =
    kOpcodeSetVoiceLowShelf;
inline constexpr std::uint16_t kOpcodeSetVoiceEffectBypass = 11;
inline constexpr std::uint16_t kOpcodeSetVoiceEffect = 12;
inline constexpr std::uint16_t kOpcodeClearVoiceEffect = 13;
inline constexpr std::uint16_t kOpcodeSetVoiceEffectParam = 14;
// Bus-graph opcodes. A structural change (bus create, send connect/disconnect)
// compiles to exactly one plan-swap record; send levels and voice sends are
// ordinary audio-thread state driven by their own commands and are NOT plan
// state.
inline constexpr std::uint16_t kOpcodeSetBusTopologyPlan = 15;
inline constexpr std::uint16_t kOpcodeSetBusSendLevel = 16;
inline constexpr std::uint16_t kOpcodeSetVoiceSend = 17;
inline constexpr std::uint16_t kOpcodeClearVoiceSend = 18;
inline constexpr std::uint16_t kOpcodeSetVoiceSendLevel = 19;
// Bus insert-effect + fader-point opcodes. Bus effect chains reuse the voice
// chain mechanics exactly (same pool, same instance handoff); the fader point
// splits a bus's chain around its BusVolume. All are ordinary per-bus
// audio-thread state, driven by their own commands (not topology-plan state).
inline constexpr std::uint16_t kOpcodeSetBusEffect = 20;
inline constexpr std::uint16_t kOpcodeClearBusEffect = 21;
inline constexpr std::uint16_t kOpcodeSetBusEffectParam = 22;
inline constexpr std::uint16_t kOpcodeSetBusEffectBypass = 23;
inline constexpr std::uint16_t kOpcodeSetBusFaderPoint = 24;
inline constexpr std::uint16_t kOpcodeSetBusFilter = 25;
inline constexpr std::uint16_t kOpcodeSeekVoice = 26;

// Highest opcode in the frozen audio-thread vocabulary. Every feature compiles
// down to opcodes in [kOpcodePlay, kMaxKnownOpcode]; nothing pushes a code
// above this. The command queue records which opcodes it has ever admitted (a
// bit per opcode), so a composed scene can prove it stayed within the vocabulary
// — the acceptance command-vocabulary check.
inline constexpr std::uint16_t kMaxKnownOpcode = kOpcodeSeekVoice;

// SetVoiceFilter selects which tone-stage section it retunes. One opcode
// covers both sections so the audio-thread vocabulary stays minimal.
inline constexpr std::uint32_t kFilterKindLowpass = 0;
inline constexpr std::uint32_t kFilterKindHighpass = 1;
inline constexpr std::uint32_t kShelfKindLow = 0;
inline constexpr std::uint32_t kShelfKindHigh = 1;

inline constexpr std::uint32_t kSpatialScalarKindWidth = 0;
inline constexpr std::uint32_t kSpatialScalarKindSpatializationMix = 1;
inline constexpr std::uint32_t kEffectActionBypass = 0;
inline constexpr std::uint32_t kEffectActionReset = 1;
inline constexpr std::uint32_t kStopModeOrdinary = 0;
inline constexpr std::uint32_t kStopModeStreamPark = 1;

inline constexpr std::uint8_t kCommandVersion = 7;
inline constexpr std::uint8_t kPlayLedBatchVersion = 1;

enum class PlayLedBatchKind : std::uint8_t {
    None = 0,
    TimelineArm = 1,
    DeferredStart = 2,
    EmitterStop = 3,
    ParkReturn = 4,
    TimelineControlArm = 5,
};

struct PlayLedBatchHeader {
    std::uint8_t batchVersion;
    PlayLedBatchKind batchKind;
    std::uint16_t reserved;
    std::uint32_t batchCommandCount;
    std::uint64_t armToken;
};

static_assert(sizeof(PlayLedBatchHeader) == 16,
              "play-led batch headers have a fixed wire layout");
static_assert(std::is_trivial_v<PlayLedBatchHeader> &&
                  std::is_standard_layout_v<PlayLedBatchHeader>,
              "play-led batch headers are trivial wire values");

inline constexpr std::uint8_t kCommandFlagPreparedCompressor = 1u << 0;
inline constexpr std::uint8_t kCommandFlagVariableWeightSegment = 1u << 1;
inline constexpr std::uint8_t kCommandKnownFlagsMask =
    kCommandFlagPreparedCompressor | kCommandFlagVariableWeightSegment;
static_assert(kCommandKnownFlagsMask == 0x03u,
              "command flag bits 0 and 1 are assigned internal flags");

struct PreparedBiquadCoeffs {
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
};

inline PreparedBiquadCoeffs PrepareBiquadCoeffs(const BiquadCoeffs& coeffs) {
    return {coeffs.b0, coeffs.b1, coeffs.b2, coeffs.a1, coeffs.a2};
}

inline BiquadCoeffs RestoreBiquadCoeffs(
    const PreparedBiquadCoeffs& prepared) {
    return {prepared.b0, prepared.b1, prepared.b2, prepared.a1, prepared.a2};
}

static_assert(sizeof(PreparedBiquadCoeffs) == 20,
              "prepared biquad coefficients are five floats");
static_assert(std::is_trivially_copyable_v<PreparedBiquadCoeffs>,
              "prepared biquad coefficients cross threads by value");

// Fixed 64-byte POD command record. The 8-byte header is followed by a
// 56-byte payload union sized by `raw`.
struct CommandRecord {
    std::uint16_t opcode;
    std::uint8_t version;
    std::uint8_t flags;
    std::uint32_t sequence; // producer-side monotonic, for diagnostics

    union Payload {
        struct Play {
            PlayLedBatchHeader batch;
            // The slot was claimed and the generation minted on the logic
            // side; applying this command activates rendering.
            VnVoiceHandle handle;
            // Stable public identity used by effect-private Voice taps. The
            // render handle may change when a parked voice returns, while the
            // public handle remains the authored route source identity.
            VnVoiceHandle sourceIdentity;
            // Source frame the read cursor begins at (0 = the file start, the
            // original behavior). A parked voice returns at its virtual offset
            // by carrying it here; a PCM voice seeds its read cursor to it when
            // the command applies. Streams seek to it at activation (logic
            // side), so this field is consumed only on the PCM path.
            std::uint64_t sourceStartFrame;
        } play;
        struct Stop {
            PlayLedBatchHeader batch;
            VnVoiceHandle handle;
            std::uint32_t fadeMs;
            // Fade length in frames, taking precedence over fadeMs when
            // nonzero. The public Vn_Stop contract is milliseconds, but a
            // steal declick must be bounded to exactly one block regardless of
            // block size — a sub-millisecond block would round a 1 ms fade to
            // more than one block. A frames-direct path expresses that bound
            // without distorting the public millisecond contract.
            std::uint32_t fadeFrames;
            std::uint32_t mode;
            VnVoiceHandle publicHandle;
            std::uint64_t parkRequestSequence;
        } stop;
        struct SeekVoice {
            VnVoiceHandle handle;
            VnActionSeekTicket ticket;
            // Absolute source frame or signed relative source-frame delta,
            // selected by mode. The audio thread resolves relative movement
            // during whole-batch preflight.
            std::int64_t frameValue;
            std::uint32_t mode;
            std::uint32_t batchOrdinal;
            std::uint32_t batchCommandCount;
            std::uint32_t reserved0;
            std::uint64_t streamHandoffToken;
        } seekVoice;
        struct SetBusVolume {
            float volumeDb; // converted to linear when the command applies
            std::uint32_t busIndex; // dense index resolved on the logic side
            std::uint32_t rampMs;
            std::uint32_t busGeneration;
            std::uint32_t outputOnly;
        } setBusVolume;
        struct SetVoiceVolume {
            VnVoiceHandle handle;
            float volumeDb; // resolved on the logic side (combine model)
            std::uint32_t rampMs;
            std::uint32_t outputOnly;
        } setVoiceVolume;
        struct SetVoiceVariableWeight {
            VnVoiceHandle handle;
            std::uint64_t startFrame;
            std::uint64_t endFrame;
            float startWeight;
            float endWeight;
            std::uint32_t segmentGeneration;
            std::uint32_t decisionSlot;
            std::uint32_t decisionGeneration;
            std::uint32_t bankSlot;
            std::uint32_t bankGeneration;
        } setVoiceVariableWeight;
        struct SetVoicePitch {
            VnVoiceHandle handle;
            double ratio; // resolved playback-rate multiplier
            std::uint32_t rampMs;
        } setVoicePitch;
        struct SetVoiceFilter {
            VnVoiceHandle handle;
            float cutoffHz;          // meaningful only when enable != 0
            float q;
            std::uint32_t filterKind; // kFilterKind*
            std::uint32_t enable;     // 0 = exact bypass
            PreparedBiquadCoeffs coeffs;
            std::uint32_t tailFrames;
            std::uint32_t transitionFrames;
        } setVoiceFilter;
        struct SetVoicePosition {
            VnVoiceHandle handle;
            float x;
            float y;
            float z;
        } setVoicePosition;
        struct SetVoicePan {
            VnVoiceHandle handle;
            float pan; // resolved on the logic side; -1 (left) .. +1 (right)
            // Exact pair for the fixed session mix layout. The audio thread
            // applies these values directly and never evaluates trigonometry.
            float gain0;
            float gain1;
            std::uint32_t channel0;
            std::uint32_t channel1;
            float pan2d; // same reduction with the spatial contribution neutralized
            float width;
            std::uint32_t hasWidth; // one live publication may carry both targets
        } setVoicePan;
        struct SetVoiceSpatialScalar {
            VnVoiceHandle handle;
            union {
                float value;
                float width; // source compatibility for the width alias
            };
            std::uint32_t kind; // kSpatialScalarKind*
        } setVoiceSpatialScalar;
        // Source-layout alias retained for existing width producers/tests. The
        // value and kind fields are intentionally at the same offsets.
        using SetVoiceWidth = SetVoiceSpatialScalar;
        SetVoiceWidth setVoiceWidth;
        struct SetVoiceShelf {
            VnVoiceHandle handle;
            float frequencyHz;
            float q;
            float gainDb;
            std::uint32_t shelfKind; // kShelfKind*
            std::uint32_t enable;    // 0 = exact bypass
            PreparedBiquadCoeffs coeffs;
            std::uint32_t tailFrames;
            std::uint32_t transitionFrames;
        } setVoiceShelf;
        using SetVoiceLowShelf = SetVoiceShelf;
        SetVoiceLowShelf setVoiceLowShelf;
        struct SetVoiceEffectBypass {
            VnVoiceHandle handle;
            std::uint32_t slot;   // chain index, validated on the logic side
            std::uint32_t bypass; // 0 = run the slot, 1 = bypass it
            std::uint32_t action; // kEffectAction*
            std::uint32_t expectedEffectType;
        } setVoiceEffectBypass;
        struct SetVoiceEffect {
            // The instance was acquired and configured on the logic side; the
            // audio thread resolves (effectType, poolIndex) to the pooled
            // state through the effect bank and installs it into the slot,
            // retiring whatever the slot held. Only ids and indices cross the
            // boundary — no pointer into logic-owned mutable memory.
            VnVoiceHandle handle;
            std::uint32_t slot;
            std::uint32_t effectType; // VnEffectType
            std::uint32_t poolIndex;  // instance index within the type's pool
            std::uint64_t poolGeneration;
        } setVoiceEffect;
        struct ClearVoiceEffect {
            VnVoiceHandle handle;
            std::uint32_t slot;
        } clearVoiceEffect;
        struct SetVoiceEffectParam {
            VnVoiceHandle handle;
            std::uint32_t slot;
            std::uint32_t paramIndex; // per-effect-type private index
            float value;              // validated on the logic side
            std::uint32_t effectType; // stale different-type guard
            PreparedBiquadCoeffs coeffs;
            std::uint32_t tailFrames;
            std::uint32_t transitionFrames;
        } setVoiceEffectParam;
        struct SetBusTopologyPlan {
            // The plan was built and published on the logic side; the audio
            // thread installs it (retiring whatever plan it held) and, for a
            // connect, snaps the new edge's initial level in the same atomic
            // apply so the edge sounds correct on its first block. A create or
            // disconnect leaves connectFrom == kInvalidBusIndex (no edge to
            // level; membership comes from the plan alone).
            std::uint32_t planIndex;
            std::uint32_t dynamicRoutePlanIndex;
            std::uint32_t connectFrom;  // kInvalidBusIndex except on connect
            std::uint32_t connectTo;    // connect target dense index
            float connectLevelDb;       // connect edge initial level
            std::uint32_t planFlags;
            std::uint32_t connectFromGeneration;
            std::uint32_t connectToGeneration;
            std::uint64_t acceptedGraphRevision;
            std::uint32_t batchCommandCount;
        } setBusTopologyPlan;
        struct SetBusSendLevel {
            // A per-edge level change (NOT a structural change): republishes no
            // plan. Ramp discipline matches the bus-volume convention.
            std::uint32_t fromBus; // dense index resolved on the logic side
            std::uint32_t toBus;   // dense index resolved on the logic side
            float levelDb;
            std::uint32_t rampMs;
            std::uint32_t fromGeneration;
            std::uint32_t toGeneration;
        } setBusSendLevel;
        struct SetVoiceSend {
            VnVoiceHandle handle;
            std::uint32_t sendSlot;     // per-voice send slot index
            std::uint32_t targetBusIndex; // dense index; kInvalidBusIndex clears
            std::uint32_t targetBusGeneration;
            float levelDb;
        } setVoiceSend;
        struct ClearVoiceSend {
            VnVoiceHandle handle;
            std::uint32_t sendSlot;
        } clearVoiceSend;
        struct SetVoiceSendLevel {
            VnVoiceHandle handle;
            std::uint32_t sendSlot;
            float levelDb;
            std::uint32_t rampMs;
        } setVoiceSendLevel;
        struct SetBusEffect {
            // Mirror of SetVoiceEffect for a bus chain: the instance was
            // acquired and configured on the logic side; the audio thread
            // resolves (effectType, poolIndex) through the same effect bank and
            // installs it into the bus slot, retiring whatever the slot held.
            std::uint32_t busIndex; // dense index resolved on the logic side
            std::uint32_t slot;
            std::uint32_t effectType; // VnEffectType
            std::uint32_t poolIndex;  // instance index within the type's pool
            std::uint64_t poolGeneration;
            std::uint32_t topologyPlanIndex;
            std::uint32_t dynamicRoutePlanIndex;
            std::uint32_t busGeneration;
        } setBusEffect;
        struct ClearBusEffect {
            std::uint32_t busIndex;
            std::uint32_t slot;
            std::uint32_t topologyPlanIndex;
            std::uint32_t dynamicRoutePlanIndex;
            std::uint32_t busGeneration;
        } clearBusEffect;
        struct SetBusEffectParam {
            std::uint32_t busIndex;
            std::uint32_t slot;
            std::uint32_t paramIndex; // per-effect-type private index
            float value;              // validated on the logic side
            std::uint32_t effectType; // stale different-type guard
            PreparedBiquadCoeffs coeffs;
            std::uint32_t tailFrames;
            std::uint32_t transitionFrames;
            std::uint32_t busGeneration;
        } setBusEffectParam;
        struct SetCompressorEffectParam {
            // The opcode selects Voice or Bus interpretation. A voice stores
            // its full generational handle; a bus stores its dense index
            // zero-extended into the same host lane.
            std::uint64_t host;
            std::uint32_t slot;
            std::uint32_t effectType;
            std::uint32_t transitionFrames;
            float thresholdDb;
            float reciprocalRatio;
            float attackCoeff;
            float releaseCoeff;
            float kneeDb;
            float makeupLinear;
            float lowerKneeLinear;
            float upperKneeLinear;
            float hardKneeScale;
        } setCompressorEffectParam;
        struct SetBusEffectBypass {
            std::uint32_t busIndex;
            std::uint32_t slot;
            std::uint32_t bypass; // 0 = run the slot, 1 = bypass it
            std::uint32_t action; // kEffectAction*
            std::uint32_t expectedEffectType;
            std::uint32_t busGeneration;
        } setBusEffectBypass;
        struct SetBusFaderPoint {
            // The chain split: fx slots [0, faderAfterSlot) run pre-fader
            // (before BusVolume), the rest post-fader. Ordinary per-bus state,
            // applied at the block boundary; validated (<= fxSlotsPerBus) on
            // the logic side.
            std::uint32_t busIndex;
            std::uint32_t faderAfterSlot;
            std::uint32_t busGeneration;
        } setBusFaderPoint;
        struct SetBusFilter {
            std::uint32_t busIndex;
            std::uint32_t busGeneration;
            std::uint32_t filterKind; // kFilterKind*
            std::uint32_t enable;     // 0 = exact bypass
            float cutoffHz;
            float q;
            std::uint32_t outputOnly; // 0 = bus mix, 1 = Direct/output only
            PreparedBiquadCoeffs coeffs;
            std::uint32_t tailFrames;
            std::uint32_t transitionFrames;
        } setBusFilter;
        unsigned char raw[56];
    } payload;
};

static_assert(sizeof(CommandRecord) == 64,
              "command records are fixed 64-byte POD");
static_assert(sizeof(CommandRecord::Payload::SetBusTopologyPlan) <= 56,
              "SetBusTopologyPlan must fit the frozen 56-byte payload");
static_assert(sizeof(CommandRecord::Payload::SetBusFilter) <= 56,
              "SetBusFilter must fit the frozen 56-byte payload");
static_assert(sizeof(CommandRecord::Payload::Play) <= 56,
              "play-led batch metadata must fit the fixed command payload");
static_assert(sizeof(CommandRecord::Payload::Stop) <= 56,
              "stop-led batch metadata must fit the fixed command payload");
static_assert(sizeof(CommandRecord::Payload::SeekVoice) <= 56,
              "seek commands must fit the fixed command payload");
static_assert(sizeof(CommandRecord::Payload::SetVoiceVariableWeight) == 56,
              "variable weight segment fills the fixed command payload");
static_assert(std::is_trivially_copyable_v<
                  CommandRecord::Payload::SetVoiceVariableWeight>,
              "variable weight segments cross threads by value");
static_assert(std::is_trivially_copyable_v<CommandRecord::Payload::Play>,
              "play-led commands cross threads by value");
static_assert(
    sizeof(CommandRecord::Payload::SetCompressorEffectParam) == 56,
    "prepared compressor payload fills the fixed command payload");
static_assert(
    std::is_trivially_copyable_v<
        CommandRecord::Payload::SetCompressorEffectParam>,
    "prepared compressor payload crosses threads by value");
static_assert(std::is_trivially_copyable_v<CommandRecord>,
              "command records cross threads by memcpy semantics");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "queue indices must be lock-free atomics");

class CommandQueue {
    friend struct detail::CommandQueueSubmissionTestAccess;
    friend struct Engine;
    friend class MixerLoop;
    friend struct detail::OriginalControlOriginTestAccess;
public:
    struct BatchReservation {
    private:
        friend class CommandQueue;
        CommandQueue* owner = nullptr;
        std::uint64_t id = 0;
        std::uint64_t head = 0;
        std::uint32_t count = 0;
    };

    enum class OriginalRangeState : std::uint8_t {
        Empty, Prepared, Published, UnpublishedCancelled, Completed
    };
    enum class OriginalRangeKind : std::uint8_t { Source, ControlHeader };
    enum class OriginalControlDisposition : std::uint8_t {
        None, Settled, Cancelled, Invalid, QuiescedDiscard
    };

    class OriginalBatchRange;
    class OriginalControlMutationActor;
    // The real original range and its CommandQueue must outlive every loan.
    // Retention is structural only, including after actual disposition.
    class OriginalControlOriginPin {
    public:
        OriginalControlOriginPin() = default;
        OriginalControlOriginPin(const OriginalControlOriginPin&) = delete;
        OriginalControlOriginPin& operator=(const OriginalControlOriginPin&) = delete;
        OriginalControlOriginPin(OriginalControlOriginPin&& other) noexcept;
        OriginalControlOriginPin& operator=(OriginalControlOriginPin&& other) noexcept;
        ~OriginalControlOriginPin() { Release(); }
        bool Valid() const noexcept;
        bool Borrow(OriginalControlOriginPin* out) const noexcept;
        void Release() noexcept;
    private:
        friend class CommandQueue;
        friend class OriginalControlMutationActor;
        CommandQueue* owner_ = nullptr;
        OriginalBatchRange* range_ = nullptr;
        std::uint64_t origin_ = 0;
        std::uint64_t head_ = 0;
        TimelineArmToken token_{};
    };
    // Only the native front-range factory issues a root mutation actor.
    // An actor keeps its authentic range/typed row, never a physical Voice.
    class OriginalControlMutationActor {
    public:
        OriginalControlMutationActor() = default;
        OriginalControlMutationActor(const OriginalControlMutationActor&) = delete;
        OriginalControlMutationActor& operator=(const OriginalControlMutationActor&) = delete;
        OriginalControlMutationActor(OriginalControlMutationActor&& other) noexcept;
        OriginalControlMutationActor& operator=(OriginalControlMutationActor&& other) noexcept;
        ~OriginalControlMutationActor() { Release(); }
        bool Valid() const noexcept;
        bool Borrow(OriginalControlMutationActor* out) const noexcept;
        bool BeforeFirstWrite() noexcept;
        void Release() noexcept;
    private:
        friend class CommandQueue;
        OriginalControlOriginPin pin_;
        OriginalCatalogOldSiblingLoan catalogSibling_;
        bool catalogFallback_ = false;
        bool counted_ = false;
        bool native_ = false;
        bool wrote_ = false;
    };

    // Stable client storage and this queue must outlive the accepted range.
    class OriginalBatchRange {
    public:
        OriginalBatchRange() = default;
        OriginalBatchRange(const OriginalBatchRange&) = delete;
        OriginalBatchRange& operator=(const OriginalBatchRange&) = delete;
        ~OriginalBatchRange() {
            assert(State() != OriginalRangeState::Prepared &&
                   State() != OriginalRangeState::Published &&
                   controlOriginPins_.load(std::memory_order_acquire) == 0 &&
                   !controlNativeActorHeld_.load(std::memory_order_acquire));
        }
        OriginalRangeState State() const noexcept {
            return state_.load(std::memory_order_acquire);
        }
        const CommandQueue* Owner() const noexcept { return owner_; }
        const void* Client() const noexcept { return client_; }
        std::uint32_t Count() const noexcept { return count_; }
        OriginalRangeKind Kind() const noexcept { return kind_; }
        const TimelineArmTable::OriginalControlHeaderRowOwner* ControlOwner() const noexcept {
            return kind_ == OriginalRangeKind::ControlHeader ? &controlOwner_ : nullptr;
        }
        bool MatchesOriginalControlHeader(const CommandRecord& record) const noexcept {
            return kind_ == OriginalRangeKind::ControlHeader &&
                std::memcmp(&record, &controlRecord_, sizeof(record)) == 0;
        }
        OriginalControlDisposition ControlDisposition() const noexcept {
            return State() == OriginalRangeState::Completed ? controlDisposition_ :
                OriginalControlDisposition::None;
        }
        bool ResetAfterDisposition() noexcept {
            const auto state = State();
            if (state == OriginalRangeState::Prepared ||
                state == OriginalRangeState::Published ||
                controlOriginPins_.load(std::memory_order_acquire) != 0 ||
                controlNativeActorHeld_.load(std::memory_order_acquire)) return false;
            assert(!catalogOrigin_.Valid());
            controlOriginWord_.store(0, std::memory_order_relaxed);
            controlOwner_.Release();
            kind_ = OriginalRangeKind::Source;
            controlRecord_ = {};
            controlDisposition_ = OriginalControlDisposition::None;
            owner_ = nullptr;
            client_ = nullptr;
            origin_ = 0;
            head_ = 0;
            count_ = 0;
            state_.store(OriginalRangeState::Empty, std::memory_order_release);
            return true;
        }
    private:
        friend class CommandQueue;
        friend class OriginalSourceSubmissionRegistry;
        friend struct detail::OriginalControlOriginTestAccess;
        OriginalCatalogStageBaselineLoan catalogOrigin_;
        std::atomic<std::uint64_t> controlOriginWord_{0};
        std::atomic<std::uint32_t> controlOriginPins_{0};
        std::atomic<bool> controlNativeActorHeld_{false};
        CommandQueue* owner_ = nullptr;
        const void* client_ = nullptr;
        std::uint64_t origin_ = 0;
        std::uint64_t head_ = 0;
        std::uint32_t count_ = 0;
        OriginalRangeKind kind_ = OriginalRangeKind::Source;
        TimelineArmTable::OriginalControlHeaderRowOwner controlOwner_;
        CommandRecord controlRecord_{};
        OriginalControlDisposition controlDisposition_ = OriginalControlDisposition::None;
        std::atomic<OriginalRangeState> state_{OriginalRangeState::Empty};
    };

    // The reservation and any original-range attachments have stable storage.
    // Issued by the actual freeze; only native abort or publication settles it.
    // Stable caller storage must outlive the frozen preparation.
    class PreparedBatchDispositionReceipt {
    public:
        PreparedBatchDispositionReceipt() = default;
        PreparedBatchDispositionReceipt(const PreparedBatchDispositionReceipt&) = delete;
        PreparedBatchDispositionReceipt& operator=(const PreparedBatchDispositionReceipt&) = delete;
        ~PreparedBatchDispositionReceipt() {
            if (state_ == State::Prepared) {
                const bool cancelled = owner_->AbortPreparedDispositionReceipt(*this);
                assert(cancelled);
                (void)cancelled;
            }
        }
        bool Issued() const noexcept { return owner_ != nullptr; }
        bool CancelledUnpublishedFrom(const CommandQueue& owner) const noexcept {
            return owner_ == &owner && origin_ != 0 && reservationId_ != 0 &&
                count_ != 0 && state_ == State::CancelledUnpublished;
        }
        bool PublishedFrom(const CommandQueue& owner) const noexcept {
            return owner_ == &owner && origin_ != 0 && reservationId_ != 0 &&
                count_ != 0 && state_ == State::Published;
        }
    private:
        friend class CommandQueue;
        enum class State : std::uint8_t { Unissued, Prepared, CancelledUnpublished, Published };
        CommandQueue* owner_ = nullptr;
        const BatchReservation* reservation_ = nullptr;
        std::uint64_t reservationId_ = 0;
        std::uint64_t head_ = 0;
        std::uint64_t origin_ = 0;
        std::uint32_t count_ = 0;
        State state_ = State::Unissued;
    };

    class PreparedBatchProof {
    public:
        PreparedBatchProof() = default;
        PreparedBatchProof(const PreparedBatchProof&) = delete;
        PreparedBatchProof& operator=(const PreparedBatchProof&) = delete;
        PreparedBatchProof(PreparedBatchProof&& other) noexcept { *this = std::move(other); }
        PreparedBatchProof& operator=(PreparedBatchProof&& other) noexcept {
            if (this != &other) {
                Release();
                owner_ = std::exchange(other.owner_, nullptr);
                reservation_ = std::exchange(other.reservation_, nullptr);
                origin_ = std::exchange(other.origin_, 0);
            }
            return *this;
        }
        ~PreparedBatchProof() { Release(); }
        bool Valid() const noexcept {
            return owner_ != nullptr && owner_->FrozenProofMatches(*this);
        }
        void Release() noexcept {
            if (owner_ != nullptr) owner_->AbortPreparedBatch(*this);
            owner_ = nullptr;
            reservation_ = nullptr;
            origin_ = 0;
        }
    private:
        friend class CommandQueue;
        CommandQueue* owner_ = nullptr;
        BatchReservation* reservation_ = nullptr;
        std::uint64_t origin_ = 0;
    };

    // Producer-local immutable publication bytes, valid until the next freeze.
    class PublishedBatchView {
    public:
        std::uint32_t Count() const noexcept { return count_; }
    private:
        friend class CommandQueue;
        const CommandQueue* owner_ = nullptr;
        std::uint64_t origin_ = 0;
        std::uint32_t count_ = 0;
    };

    // Depth rounds up to a power of two so index masking replaces modulo.
    explicit CommandQueue(std::uint32_t depth,
        const TimelineArmTable* controlReceiver = nullptr) : m_controlReceiver(controlReceiver) {
        std::uint32_t cap = 1;
        while (cap < depth) {
            cap <<= 1;
        }
        m_slots.resize(cap);
        m_publicationScratch.resize(cap);
        m_originalRanges.resize(cap, nullptr);
        m_preparedOriginalRanges.resize(cap, nullptr);
        m_mask = cap - 1;
    }

    ~CommandQueue() { assert(m_liveFrozenOrigin == 0); }

    std::uint32_t Capacity() const {
        return static_cast<std::uint32_t>(m_slots.size());
    }

    // Producer side. `full` lets a caller refuse before committing side
    // effects that would be hard to roll back (a raw voice start claims its
    // voice slot only after the queue admits the command).
    bool Full() const {
        return m_head.load(std::memory_order_relaxed) -
                   m_tail.load(std::memory_order_acquire) >=
               m_slots.size();
    }

    // Whether at least `count` records can be pushed right now. Lets a caller
    // that must emit several commands atomically (a deferred start replays a
    // play plus its staged setters) verify admission for the whole batch
    // before issuing any of it.
    bool HasRoom(std::uint32_t count) const {
        const std::uint64_t used = m_head.load(std::memory_order_relaxed) -
                                   m_tail.load(std::memory_order_acquire);
        return used + count <= m_slots.size();
    }

    // Producer capacity available right now, for admission arithmetic that
    // subtracts a reservation (the property publication discipline) rather
    // than asking a yes/no question. Same memory-order pairing as HasRoom.
    std::uint32_t FreeSlots() const {
        const std::uint64_t used = m_head.load(std::memory_order_relaxed) -
                                   m_tail.load(std::memory_order_acquire);
        return static_cast<std::uint32_t>(m_slots.size() - used);
    }

    // Claims producer admission without publishing slots or changing any
    // queue observation. Only one producer reservation may be live.
    bool ReserveBatch(std::uint32_t count, BatchReservation* out) {
        if (m_reservationLive) {
            return false;
        }
        if (count == 0) {
            return true;
        }
        if (out == nullptr) {
            return false;
        }
        const std::uint64_t head = m_head.load(std::memory_order_relaxed);
        const std::uint64_t tail = m_tail.load(std::memory_order_acquire);
        const std::uint64_t used = head - tail;
        if (count > m_slots.size() - used) {
            ++m_backpressure;
            return false;
        }
        ++m_nextReservationId;
        if (m_nextReservationId == 0) {
            ++m_nextReservationId;
        }
        out->owner = this;
        out->id = m_nextReservationId;
        out->head = head;
        out->count = count;
        m_liveReservationId = m_nextReservationId;
        m_reservationLive = true;
        return true;
    }

    // Fills a previously admitted batch, then publishes the whole range with
    // one release-store. No capacity check or allocation remains in commit.
    bool CommitReservedBatch(BatchReservation* reservation,
                             const CommandRecord* records) noexcept {
        if (m_liveFrozenOrigin != 0 || !ReservationMatches(reservation) || records == nullptr) {
            return false;
        }
        CommitPreparedBatch(reservation, records);
        return true;
    }

    // Exact prepared commit for a reservation returned by ReserveBatch.
    // The caller has already frozen the owner, head and count; this path
    // performs only the bounded copy and one release publication.
    void CommitPreparedBatch(BatchReservation* reservation,
                             const CommandRecord* records) noexcept {
        if (m_liveFrozenOrigin != 0) {
            assert(false && "frozen batches require their genuine consume proof");
            return;
        }
        const std::uint64_t head = reservation->head;
        const std::uint32_t count = reservation->count;
        for (std::uint32_t i = 0; i < count; ++i) {
            CommandRecord record = records[i];
            assert(record.opcode >= kOpcodePlay &&
                   record.opcode <= kMaxKnownOpcode &&
                   "command opcode outside the frozen audio-thread vocabulary");
            record.version = kCommandVersion;
            record.sequence = m_sequence++;
            if (record.opcode < 32u) {
                m_opcodeSeen |= (1u << record.opcode);
            }
            if (record.opcode > kMaxKnownOpcode) {
                m_opcodeOverflowSeen = true;
            }
            m_slots[(head + i) & m_mask] = record;
            m_originalRanges[(head + i) & m_mask] = nullptr;
        }
        m_head.store(head + count, std::memory_order_release);
        ClearReservation(reservation);
    }

    bool ProvePreparedBatch(BatchReservation* reservation,
        const CommandRecord* records, std::uint32_t count,
        PreparedBatchProof* out,
        PreparedBatchDispositionReceipt* receipt = nullptr) noexcept {
        if (out == nullptr || out->owner_ != nullptr || records == nullptr ||
            (receipt != nullptr && receipt->Issued()) || m_frozenDispositionReceipt != nullptr ||
            m_liveFrozenOrigin != 0 || m_nextFrozenOrigin == UINT64_MAX ||
            !ReservationMatches(reservation) || count != reservation->count)
            return false;
        for (std::uint32_t i = 0; i < count; ++i)
            if (records[i].opcode < kOpcodePlay || records[i].opcode > kMaxKnownOpcode)
                return false;
        const auto head = reservation->head;
        m_lastPublishedFrozenOrigin = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            auto record = records[i];
            record.version = kCommandVersion;
            record.sequence = m_sequence + i;
            m_slots[(head + i) & m_mask] = record;
            m_publicationScratch[i] = record;
            m_originalRanges[(head + i) & m_mask] = nullptr;
        }
        m_liveFrozenOrigin = ++m_nextFrozenOrigin;
        m_frozenReservation = reservation;
        m_frozenReservationId = reservation->id;
        m_frozenHead = reservation->head;
        m_frozenCount = count;
        m_preparedOriginalRangeCount = 0;
        out->owner_ = this;
        out->reservation_ = reservation;
        out->origin_ = m_liveFrozenOrigin;
        if (receipt != nullptr) {
            receipt->owner_ = this;
            receipt->reservation_ = reservation;
            receipt->reservationId_ = m_frozenReservationId;
            receipt->head_ = m_frozenHead;
            receipt->origin_ = m_liveFrozenOrigin;
            receipt->count_ = count;
            receipt->state_ = PreparedBatchDispositionReceipt::State::Prepared;
            m_frozenDispositionReceipt = receipt;
        }
        return true;
    }

    bool ReadPreparedAt(const PreparedBatchProof& proof, std::uint32_t offset,
        CommandRecord* out) const noexcept {
        if (out == nullptr || !FrozenProofMatches(proof) ||
            offset >= m_frozenCount) return false;
        *out = m_publicationScratch[offset];
        return true;
    }

    bool PreparedBatchMatches(const PreparedBatchProof& proof,
        const BatchReservation* reservation, std::uint32_t count) const noexcept {
        return FrozenProofMatches(proof) && proof.reservation_ == reservation &&
            count == m_frozenCount;
    }

    bool PreparedBatchContains(const PreparedBatchProof& proof,
        const OriginalBatchRange& range) const noexcept {
        return FrozenProofMatches(proof) && range.owner_ == this &&
            range.origin_ == proof.origin_ && range.State() == OriginalRangeState::Prepared;
    }

    bool IssueOriginalBatchRange(const PreparedBatchProof& proof,
        std::uint32_t offset, std::uint32_t count, const void* client,
        OriginalBatchRange* out) noexcept {
        if (out == nullptr || client == nullptr || out->Owner() != nullptr ||
            out->State() != OriginalRangeState::Empty || !FrozenProofMatches(proof) ||
            count == 0 || offset >= m_frozenCount ||
            count > m_frozenCount - offset ||
            m_preparedOriginalRangeCount >= m_preparedOriginalRanges.size()) return false;
        const auto head = m_frozenHead + offset;
        for (std::uint32_t i = 0; i < count; ++i)
            if (m_originalRanges[(head + i) & m_mask] != nullptr) return false;
        out->owner_ = this;
        out->client_ = client;
        out->origin_ = proof.origin_;
        out->head_ = head;
        out->count_ = count;
        out->state_.store(OriginalRangeState::Prepared, std::memory_order_relaxed);
        for (std::uint32_t i = 0; i < count; ++i)
            m_originalRanges[(head + i) & m_mask] = out;
        m_preparedOriginalRanges[m_preparedOriginalRangeCount++] = out;
        return true;
    }

    // All original ranges and record bytes were frozen before owner commit.
    bool IssueOriginalControlHeaderRange(const PreparedBatchProof& proof,
        std::uint32_t offset, TimelineArmTable::OriginalControlHeaderRowOwner&& owner,
        const void* client, OriginalBatchRange* out,
        OriginalControlOriginPin* originalOrigin = nullptr) noexcept {
        if ((originalOrigin != nullptr && (originalOrigin->Valid() ||
                !HasAuthenticOriginalPreparedControlScope(owner, m_controlReceiver))) || !owner.Valid() || m_controlReceiver == nullptr || owner.Owner() != m_controlReceiver ||
            out == nullptr || out->ControlOwner() != nullptr) return false;
        CommandRecord record{};
        if (!ReadPreparedAt(proof, offset, &record)) return false;
        const auto token = owner.Token();
        const auto handle = (static_cast<std::uint64_t>(token.renderGeneration) << 32) |
            token.renderSlot;
        const auto& play = record.payload.play;
        const auto& header = play.batch;
        if (record.opcode != kOpcodePlay || record.version != kCommandVersion ||
            record.flags != 0 || header.batchVersion != kPlayLedBatchVersion ||
            header.batchKind != PlayLedBatchKind::TimelineControlArm || header.reserved != 0 ||
            header.batchCommandCount != 1 ||
            header.armToken != ((static_cast<std::uint64_t>(token.generation) << 32) | token.slot) ||
            play.handle != handle || play.sourceIdentity != handle || play.sourceStartFrame != 0 ||
            !IssueOriginalBatchRange(proof, offset, 1, client, out)) return false;
        out->kind_ = OriginalRangeKind::ControlHeader;
        out->controlRecord_ = record;
        out->controlOwner_ = std::move(owner);
        if (originalOrigin != nullptr) {
            out->controlOriginPins_.store(1u, std::memory_order_relaxed);
            out->controlOriginWord_.store(ControlWord(ControlOriginMode::Normal, 0, 1),
                std::memory_order_release);
            SetControlOriginPin(*out, *originalOrigin);
        }
        return true;
    }

    PublishedBatchView ConsumePreparedBatch(PreparedBatchProof&& proof) noexcept {
        assert(FrozenProofMatches(proof));
        if (!FrozenProofMatches(proof)) return {};
        PublishedBatchView published;
        published.owner_ = this;
        published.origin_ = proof.origin_;
        published.count_ = m_frozenCount;
        auto* reservation = proof.reservation_;
        const auto head = m_frozenHead;
        const auto count = m_frozenCount;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto opcode = m_publicationScratch[i].opcode;
            if (opcode < 32u) m_opcodeSeen |= 1u << opcode;
        }
        m_sequence += count;
        for (std::uint32_t i = 0; i < m_preparedOriginalRangeCount; ++i) {
            m_preparedOriginalRanges[i]->state_.store(
                OriginalRangeState::Published, std::memory_order_release);
            m_preparedOriginalRanges[i] = nullptr;
        }
        m_preparedOriginalRangeCount = 0;
        m_lastPublishedFrozenOrigin = proof.origin_;
        if (m_frozenDispositionReceipt != nullptr) {
            assert(DispositionReceiptMatches(*m_frozenDispositionReceipt, proof));
            m_frozenDispositionReceipt->state_ = PreparedBatchDispositionReceipt::State::Published;
            m_frozenDispositionReceipt = nullptr;
        }
        m_liveFrozenOrigin = 0;
        m_frozenReservation = nullptr;
        m_frozenReservationId = 0;
        m_frozenCount = 0;
        proof.owner_ = nullptr;
        proof.reservation_ = nullptr;
        proof.origin_ = 0;
        m_head.store(head + count, std::memory_order_release);
        ClearReservation(reservation);
        return published;
    }

    bool ReadPublishedForProducer(const PublishedBatchView& published,
        std::uint32_t offset, CommandRecord* out) const noexcept {
        if (out == nullptr || published.owner_ != this || published.origin_ == 0 ||
            published.origin_ != m_lastPublishedFrozenOrigin || offset >= published.count_)
            return false;
        *out = m_publicationScratch[offset];
        return true;
    }

    // Audio-side lookup comes from the actual ring position, never its header.
    OriginalBatchRange* OriginalRangeAtFront() const noexcept {
        const auto tail = m_tail.load(std::memory_order_relaxed);
        if (tail == m_head.load(std::memory_order_acquire)) return nullptr;
        auto* range = m_originalRanges[tail & m_mask];
        return range != nullptr && range->owner_ == this && range->head_ == tail &&
            range->State() == OriginalRangeState::Published ? range : nullptr;
    }

    bool CompleteOriginalRange(OriginalBatchRange& range) noexcept {
        if (range.owner_ != this || range.State() != OriginalRangeState::Published ||
            (range.kind_ == OriginalRangeKind::ControlHeader &&
                range.controlDisposition_ == OriginalControlDisposition::None) ||
            m_tail.load(std::memory_order_relaxed) - range.head_ < range.count_)
            return false;
        range.state_.store(OriginalRangeState::Completed, std::memory_order_release);
        return true;
    }

    // Completion is the last consumer access; the producer may reuse it now.
    bool CompleteOriginalControlHeaderRange(OriginalBatchRange& range,
        OriginalControlDisposition disposition) noexcept {
        if (range.owner_ != this || range.Kind() != OriginalRangeKind::ControlHeader ||
            range.State() != OriginalRangeState::Published ||
            disposition == OriginalControlDisposition::None ||
            m_tail.load(std::memory_order_relaxed) - range.head_ < range.count_) return false;
        if (range.controlNativeActorHeld_.load(std::memory_order_acquire)) return false;
        range.controlDisposition_ = disposition;
        CloseControlOriginWhenFinished(range);
        range.state_.store(OriginalRangeState::Completed, std::memory_order_release);
        return true;
    }

    // Engine shutdown only, after producers, callbacks and workers have joined.
    void DiscardPublishedAfterQuiescence() noexcept {
        assert(m_liveFrozenOrigin == 0);
        CommandRecord discarded{};
        for (;;) {
            auto* range = OriginalRangeAtFront();
            OriginalControlMutationActor actor;
            if (range != nullptr && range->kind_ == OriginalRangeKind::ControlHeader) {
                const auto acquired = BorrowOriginalFrontControlActor(*range, &actor);
                assert(acquired != NativeControlActorStatus::Refused);
                if (acquired == NativeControlActorStatus::Refused) return;
                if (acquired == NativeControlActorStatus::Issued) {
                    const bool wrote = actor.BeforeFirstWrite();
                    assert(wrote);
                    if (!wrote) return;
                }
            }
            if (!TryPop(discarded)) return;
            if (range != nullptr && range->kind_ == OriginalRangeKind::ControlHeader) {
                actor.Release();
                const bool completed = CompleteOriginalControlHeaderRange(*range,
                    OriginalControlDisposition::QuiescedDiscard);
                assert(completed);
                if (!completed) return;
            }
        }
    }

    // Drops producer admission without changing queue-visible state.
    void CancelReservedBatch(BatchReservation* reservation) noexcept {
        if (m_liveFrozenOrigin != 0) {
            if (reservation == m_frozenReservation) AbortFrozenReservation();
            return;
        }
        if (ReservationMatches(reservation)) {
            ClearReservation(reservation);
        }
    }

    // Returns proof that this exact producer reservation was removed before
    // any of its records became visible to the consumer.
    bool CancelReservedBatchExact(BatchReservation* reservation) noexcept {
        if (m_liveFrozenOrigin != 0) {
            if (reservation != m_frozenReservation || !ReservationMatches(reservation)) return false;
            AbortFrozenReservation();
            return true;
        }
        if (!ReservationMatches(reservation)) return false;
        ClearReservation(reservation);
        return true;
    }

    // Counts a refusal that was decided by a `Full()` pre-check rather than
    // by TryPush, so every VN_BACKPRESSURE surfaced by the API is counted
    // exactly once regardless of which path refused.
    void NoteBackpressure() { ++m_backpressure; }

    // Pushes one record, stamping its sequence number. Returns false (and
    // counts the refusal) when the queue is full.
    bool TryPush(CommandRecord record) {
        if (m_reservationLive) {
            return false;
        }
        // Every producer must speak the frozen vocabulary. In Debug this aborts
        // on an out-of-range opcode (a feature that grew the audio-thread
        // vocabulary without declaring it); it compiles out of Release.
        assert(record.opcode >= kOpcodePlay &&
               record.opcode <= kMaxKnownOpcode &&
               "command opcode outside the frozen audio-thread vocabulary");
        const std::uint64_t head = m_head.load(std::memory_order_relaxed);
        const std::uint64_t tail = m_tail.load(std::memory_order_acquire);
        if (head - tail >= m_slots.size()) {
            ++m_backpressure;
            return false;
        }
        record.version = kCommandVersion;
        record.sequence = m_sequence++;
        // Record the admitted opcode (one bit per opcode) so a composed scene
        // can assert it used no vocabulary beyond the amendment set. Producer
        // (logic) thread only; not read on the audio thread.
        if (record.opcode < 32u) {
            m_opcodeSeen |= (1u << record.opcode);
        }
        // Completeness patch for the vocabulary assertion mechanism, pure
        // observation: the 32-bit mask cannot represent opcodes >= 32, so a
        // beyond-vocabulary opcode that high would be invisible to the mask
        // check alone. This sticky bit observes ANY admitted opcode beyond
        // kMaxKnownOpcode, in Release too — in Debug the vocabulary assert
        // above aborts such a push first, making this the Release-side
        // observer.
        if (record.opcode > kMaxKnownOpcode) {
            m_opcodeOverflowSeen = true;
        }
        m_slots[head & m_mask] = record;
        m_originalRanges[head & m_mask] = nullptr;
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    // Publishes a contiguous producer batch atomically. Every slot is fully
    // initialized before the single release-store advances head, so the audio
    // consumer observes either none of the records or the complete batch.
    bool TryPushBatch(const CommandRecord* records, std::uint32_t count) {
        if (m_reservationLive) {
            return false;
        }
        if (count == 0) {
            return true;
        }
        if (records == nullptr) {
            return false;
        }
        const std::uint64_t head = m_head.load(std::memory_order_relaxed);
        const std::uint64_t tail = m_tail.load(std::memory_order_acquire);
        if (head - tail + count > m_slots.size()) {
            ++m_backpressure;
            return false;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            CommandRecord record = records[i];
            assert(record.opcode >= kOpcodePlay &&
                   record.opcode <= kMaxKnownOpcode &&
                   "command opcode outside the frozen audio-thread vocabulary");
            record.version = kCommandVersion;
            record.sequence = m_sequence++;
            if (record.opcode < 32u) {
                m_opcodeSeen |= (1u << record.opcode);
            }
            if (record.opcode > kMaxKnownOpcode) {
                m_opcodeOverflowSeen = true;
            }
            m_slots[(head + i) & m_mask] = record;
            m_originalRanges[(head + i) & m_mask] = nullptr;
        }
        m_head.store(head + count, std::memory_order_release);
        return true;
    }

    // Bitmask of every opcode this queue has admitted (bit k set = opcode k
    // pushed at least once). The acceptance command-vocabulary check asserts no
    // bit above kMaxKnownOpcode is set. Logic thread only.
    std::uint32_t OpcodeSeenMask() const { return m_opcodeSeen; }

    // True when any admitted opcode exceeded kMaxKnownOpcode — including
    // the >= 32 band the mask cannot represent. Same producer-side channel
    // and reader discipline as OpcodeSeenMask().
    bool OpcodeOverflowSeen() const { return m_opcodeOverflowSeen; }

    // Consumer side (audio thread). Returns false when empty.
    bool TryPop(CommandRecord& out) {
        const std::uint64_t tail = m_tail.load(std::memory_order_relaxed);
        const std::uint64_t head = m_head.load(std::memory_order_acquire);
        if (tail == head) {
            return false;
        }
        out = m_slots[tail & m_mask];
        m_originalRanges[tail & m_mask] = nullptr;
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer side. The producer's published position at the moment of the
    // call, so a drain can fix the work it owes before it starts: without it,
    // records pushed while the drain runs extend the same pass and a producer
    // that keeps writing can hold the audio thread in it indefinitely. Same
    // acquire pairing TryPop already uses on this word, one call earlier.
    std::uint64_t PublishedHead() const {
        return m_head.load(std::memory_order_acquire);
    }

    std::uint64_t SettledFrontier() const {
        return m_settledFrontier.load(std::memory_order_acquire);
    }

    void PublishSettledFrontier() noexcept {
        m_settledFrontier.store(ConsumedCount(), std::memory_order_release);
    }

    // Consumer side. The record at the front without consuming it, so a drain
    // can read a batch header and decide whether the whole batch fits in what
    // it has left. TryPop cannot be undone, and applying part of an atomic
    // batch is worse than deferring all of it. False when empty.
    bool Peek(CommandRecord& out) const {
        return PeekAt(0, out);
    }

    bool PeekAt(std::uint32_t offset, CommandRecord& out) const {
        const std::uint64_t tail = m_tail.load(std::memory_order_relaxed);
        const std::uint64_t head = m_head.load(std::memory_order_acquire);
        if (static_cast<std::uint64_t>(offset) >= head - tail) {
            return false;
        }
        out = m_slots[(tail + offset) & m_mask];
        return true;
    }

    // Consumer side. Records consumed so far. Paired with a PublishedHead taken
    // at drain entry, this gives the work that pass owes without re-reading the
    // producer's position as it advances.
    std::uint64_t ConsumedCount() const {
        return m_tail.load(std::memory_order_relaxed);
    }

    std::uint64_t BackpressureCount() const { return m_backpressure; }

private:
    static bool HasAuthenticOriginalPreparedControlScope(
        const TimelineArmTable::OriginalControlHeaderRowOwner& owner,
        const TimelineArmTable* receiver) noexcept {
        if (!owner.Valid() || receiver == nullptr || owner.Owner() != receiver) return false;
        const auto& scope = owner.OriginalScope();
        const auto token = owner.Token();
        return scope.incarnation != 0 && scope.incarnation == receiver->Incarnation() &&
            scope.sourceStart.Valid() && scope.sourceStart.renderSlot == token.renderSlot &&
            scope.sourceStart.renderGeneration == token.renderGeneration &&
            scope.operation != nullptr && scope.operationIdentity.Valid() &&
            scope.program.program != nullptr && scope.program.programGeneration != 0;
    }
    enum class ControlOriginMode : std::uint8_t {
        Disabled = 0, Normal = 1, ExhaustedNormal = 3, Closed = 4,
        Redirect = 5, ExhaustedRedirect = 7
    };
    static constexpr std::uint64_t kControlAbortBit = 2;
    static constexpr unsigned kControlNativeAttempts = 8;
    static bool IsNormalControlOrigin(ControlOriginMode mode) noexcept {
        return mode == ControlOriginMode::Normal || mode == ControlOriginMode::Redirect;
    }
    static bool IsExhaustedControlOrigin(ControlOriginMode mode) noexcept {
        return mode == ControlOriginMode::ExhaustedNormal || mode == ControlOriginMode::ExhaustedRedirect;
    }
    static bool IsLiveControlOrigin(ControlOriginMode mode) noexcept {
        return IsNormalControlOrigin(mode) || IsExhaustedControlOrigin(mode);
    }
    static constexpr std::uint64_t kControlActivityOne = UINT64_C(1) << 3;
    static constexpr std::uint64_t kControlEpochMax = UINT64_MAX >> 11;
    static ControlOriginMode ControlMode(std::uint64_t word) noexcept {
        return static_cast<ControlOriginMode>(word & 7u);
    }
    static std::uint64_t ControlActivity(std::uint64_t word) noexcept { return (word >> 3) & 255u; }
    static std::uint64_t ControlEpoch(std::uint64_t word) noexcept { return word >> 11; }
    static std::uint64_t ControlWord(ControlOriginMode mode, std::uint64_t count,
        std::uint64_t epoch) noexcept {
        return (epoch << 11) | (count << 3) | static_cast<std::uint64_t>(mode);
    }
    static void ExhaustControlOrigin(OriginalBatchRange& range) noexcept {
        const auto before = range.controlOriginWord_.fetch_or(kControlAbortBit, std::memory_order_acq_rel);
        assert(IsLiveControlOrigin(ControlMode(before)));
        if (ControlMode(before) == ControlOriginMode::Redirect ||
            ControlMode(before) == ControlOriginMode::ExhaustedRedirect) {
            const bool aborted = range.catalogOrigin_.AbortNativeOldCandidate();
            assert(aborted);
            (void)aborted;
        }
    }
    static void CloseControlOriginWhenFinished(OriginalBatchRange& range) noexcept {
        if (range.controlNativeActorHeld_.load(std::memory_order_acquire)) return;
        auto word = range.controlOriginWord_.load(std::memory_order_acquire);
        if (!IsLiveControlOrigin(ControlMode(word)) || ControlActivity(word) != 0) return;
        if (!range.controlOriginWord_.compare_exchange_strong(word,
            ControlWord(ControlOriginMode::Closed, 0, ControlEpoch(word)),
            std::memory_order_acq_rel, std::memory_order_acquire)) return;
        if (ControlMode(word) == ControlOriginMode::Redirect ||
            ControlMode(word) == ControlOriginMode::ExhaustedRedirect) {
            OriginalCatalogStageBaselineLoan old(std::move(range.catalogOrigin_));
            old.Release();
        }
    }
    // No production caller exists here. Only the genuine Engine provisional
    // family issuer may reach this private component port after full scope and
    // target/Source/Queue/C0/Lane/cohort authentication.
    bool TryAttachOriginalControlCatalogOrigin(const OriginalControlOriginPin& original,
        const OriginalCatalogStageBaselineLoan& actualStage,
        std::uint64_t expectedEpoch) noexcept {
        if (!MatchesControlOrigin(original) || !actualStage.Valid() || expectedEpoch == 0 ||
            expectedEpoch >= kControlEpochMax) return false;
        auto& range = *original.range_;
        auto expected = ControlWord(ControlOriginMode::Normal, 0, expectedEpoch);
        if (range.controlOriginWord_.load(std::memory_order_acquire) != expected) return false;
        if (range.catalogOrigin_.Valid() ||
            (range.State() != OriginalRangeState::Prepared &&
             range.State() != OriginalRangeState::Published)) return false;
        OriginalCatalogStageBaselineLoan link;
        if (!actualStage.Borrow(&link)) return false;
        range.catalogOrigin_ = std::move(link);
        if (!range.controlOriginWord_.compare_exchange_strong(expected,
            ControlWord(ControlOriginMode::Redirect, 0, expectedEpoch),
            std::memory_order_acq_rel, std::memory_order_acquire)) {
            OriginalCatalogStageBaselineLoan failed(std::move(range.catalogOrigin_));
            failed.Release();
            return false;
        }
        return true;
    }
    static void ReturnOriginalControlOriginRead(OriginalBatchRange& range) noexcept {
        const auto previous = range.controlOriginWord_.fetch_sub(kControlActivityOne,
            std::memory_order_acq_rel);
        assert(ControlActivity(previous) != 0 &&
            (ControlMode(previous) == ControlOriginMode::Redirect ||
             ControlMode(previous) == ControlOriginMode::ExhaustedRedirect));
        (void)previous;
        // This operation's real OriginalControlOriginPin still retains storage.
        if (range.State() == OriginalRangeState::Completed ||
            range.State() == OriginalRangeState::UnpublishedCancelled)
            CloseControlOriginWhenFinished(range);
    }
    bool TryDetachAbortedOriginalControlCatalogOrigin(const OriginalControlOriginPin& original) noexcept {
        if (!MatchesControlOrigin(original)) return false;
        auto& range = *original.range_;
        auto expected = range.controlOriginWord_.load(std::memory_order_acquire);
        if (ControlMode(expected) != ControlOriginMode::Redirect || ControlActivity(expected) != 0)
            return false;
        const auto borrowed = expected + kControlActivityOne;
        if (!range.controlOriginWord_.compare_exchange_strong(expected, borrowed,
            std::memory_order_acq_rel, std::memory_order_acquire)) return false;
        if (!range.catalogOrigin_.AbortedForNativeDetach()) {
            ReturnOriginalControlOriginRead(range);
            return false;
        }
        expected = borrowed;
        if (!range.controlOriginWord_.compare_exchange_strong(expected,
            ControlWord(ControlOriginMode::Normal, 0, ControlEpoch(borrowed)),
            std::memory_order_acq_rel, std::memory_order_acquire)) {
            ReturnOriginalControlOriginRead(range);
            return false;
        }
        OriginalCatalogStageBaselineLoan old(std::move(range.catalogOrigin_));
        old.Release();
        if (range.State() == OriginalRangeState::Completed ||
            range.State() == OriginalRangeState::UnpublishedCancelled)
            CloseControlOriginWhenFinished(range);
        return true;
    }
    static void RetainRedirectForActualControlActor(OriginalBatchRange& range,
        OriginalControlMutationActor& actor) noexcept {
        const auto mode = ControlMode(range.controlOriginWord_.load(std::memory_order_acquire));
        if (mode != ControlOriginMode::Redirect && mode != ControlOriginMode::ExhaustedRedirect) return;
        if (!range.catalogOrigin_.BorrowNativeOldSibling(&actor.catalogSibling_)) {
            const bool aborted = range.catalogOrigin_.AbortNativeOldCandidate();
            assert(aborted);
            (void)aborted;
            actor.catalogFallback_ = true;
        }
    }
    bool MatchesControlOrigin(const OriginalControlOriginPin& pin) const noexcept {
        if (pin.owner_ != this || pin.range_ == nullptr || pin.origin_ == 0) return false;
        const auto& range = *pin.range_;
        if (range.owner_ != this || range.kind_ != OriginalRangeKind::ControlHeader ||
            range.origin_ != pin.origin_ || range.head_ != pin.head_ || range.count_ != 1 ||
            range.controlOriginPins_.load(std::memory_order_acquire) == 0 ||
            ControlMode(range.controlOriginWord_.load(std::memory_order_acquire)) == ControlOriginMode::Disabled ||
            !HasAuthenticOriginalPreparedControlScope(range.controlOwner_, m_controlReceiver)) return false;
        const auto actual = range.controlOwner_.Token();
        return actual.slot == pin.token_.slot && actual.generation == pin.token_.generation &&
            actual.renderSlot == pin.token_.renderSlot && actual.renderGeneration == pin.token_.renderGeneration &&
            range.controlOwner_.OriginalScope().incarnation == m_controlReceiver->Incarnation();
    }
    bool AddControlOriginPin(OriginalBatchRange& range, bool native) noexcept {
        auto count = range.controlOriginPins_.load(std::memory_order_acquire);
        for (;;) {
            // One count is reserved for the already accepted native consumer.
            const auto limit = native ? UINT32_MAX : UINT32_MAX - 1u;
            if ((!native && count == 0) || count >= limit) {
                if (native) ExhaustControlOrigin(range);
                return false;
            }
            if (range.controlOriginPins_.compare_exchange_weak(count, count + 1u,
                std::memory_order_acq_rel, std::memory_order_acquire)) return true;
        }
    }
    void SetControlOriginPin(OriginalBatchRange& range, OriginalControlOriginPin& out) noexcept {
        out.owner_ = this;
        out.range_ = &range;
        out.origin_ = range.origin_;
        out.head_ = range.head_;
        out.token_ = range.controlOwner_.Token();
    }
    bool BorrowControlOrigin(const OriginalControlOriginPin& pin,
        OriginalControlOriginPin* out) noexcept {
        if (out == nullptr || out->Valid() || !MatchesControlOrigin(pin) ||
            !AddControlOriginPin(*pin.range_, false)) return false;
        SetControlOriginPin(*pin.range_, *out);
        return true;
    }
    void ReturnControlOrigin(OriginalControlOriginPin& pin) noexcept {
        assert(MatchesControlOrigin(pin));
        auto* range = pin.range_;
        if (range->State() == OriginalRangeState::Completed ||
            range->State() == OriginalRangeState::UnpublishedCancelled)
            CloseControlOriginWhenFinished(*range);
        pin.owner_ = nullptr;
        pin.range_ = nullptr;
        pin.origin_ = 0;
        pin.head_ = 0;
        pin.token_ = {};
        // Producer may reset/reissue this actual range immediately after zero.
        // The capability is already clear and no range access follows.
        const auto previous = range->controlOriginPins_.fetch_sub(1u, std::memory_order_acq_rel);
        assert(previous != 0);
        (void)previous;
    }
    bool MatchesControlActor(const OriginalControlMutationActor& actor) const noexcept {
        return MatchesControlOrigin(actor.pin_) &&
            (!actor.native_ || actor.pin_.range_->controlNativeActorHeld_.load(std::memory_order_acquire)) &&
            (!actor.counted_ || ControlActivity(actor.pin_.range_->controlOriginWord_.load(
                std::memory_order_acquire)) != 0);
    }
    bool AddControlActivity(OriginalBatchRange& range, bool native) noexcept {
        auto word = range.controlOriginWord_.load(std::memory_order_acquire);
        for (unsigned attempt = 0; attempt < kControlNativeAttempts; ++attempt) {
            if (!IsLiveControlOrigin(ControlMode(word))) return false;
            if (ControlActivity(word) >= (native ? 255u : 254u)) {
                ExhaustControlOrigin(range);
                return false;
            }
            if (range.controlOriginWord_.compare_exchange_strong(word, word + kControlActivityOne,
                std::memory_order_acq_rel, std::memory_order_acquire)) return true;
        }
        ExhaustControlOrigin(range);
        return false;
    }
    bool BorrowControlActor(const OriginalControlMutationActor& original,
        OriginalControlMutationActor* out) noexcept {
        if (out == nullptr || out->Valid() || !MatchesControlActor(original) ||
            original.pin_.range_->State() != OriginalRangeState::Published) return false;
        OriginalControlOriginPin pin;
        if (!BorrowControlOrigin(original.pin_, &pin)) return false;
        if (!AddControlActivity(*pin.range_, false)) return false;
        out->pin_ = std::move(pin);
        out->counted_ = true;
        out->native_ = false;
        out->wrote_ = false;
        RetainRedirectForActualControlActor(*out->pin_.range_, *out);
        return true;
    }
    bool ControlActorBeforeFirstWrite(OriginalControlMutationActor& actor) noexcept {
        if (!MatchesControlActor(actor) || actor.pin_.range_->State() != OriginalRangeState::Published) return false;
        if (actor.wrote_) return true;
        auto& range = *actor.pin_.range_;
        if (actor.catalogSibling_.Valid()) {
            if (!actor.catalogSibling_.BeforeFirstNativeOldWrite()) return false;
        } else if (actor.catalogFallback_) {
            const bool aborted = range.catalogOrigin_.AbortNativeOldCandidate();
            assert(aborted);
            (void)aborted;
        }
        auto word = range.controlOriginWord_.load(std::memory_order_acquire);
        for (unsigned attempt = 0; attempt < kControlNativeAttempts; ++attempt) {
            const auto mode = ControlMode(word);
            if (IsExhaustedControlOrigin(mode)) { actor.wrote_ = true; return true; }
            if (!IsNormalControlOrigin(mode) || ControlActivity(word) == 0) return false;
            const auto epoch = ControlEpoch(word);
            if (epoch == kControlEpochMax) break;
            if (range.controlOriginWord_.compare_exchange_strong(word,
                ControlWord(mode, ControlActivity(word), epoch + 1u),
                std::memory_order_acq_rel, std::memory_order_acquire)) {
                actor.wrote_ = true;
                return true;
            }
        }
        ExhaustControlOrigin(range);
        actor.wrote_ = true;
        return true;
    }
    void ReturnControlActor(OriginalControlMutationActor& actor) noexcept {
        assert(MatchesControlActor(actor));
        auto pin = std::move(actor.pin_);
        auto& range = *pin.range_;
        OriginalCatalogOldSiblingLoan sibling(std::move(actor.catalogSibling_));
        actor.catalogFallback_ = false;
        sibling.Release();
        if (actor.counted_) {
            const auto old = range.controlOriginWord_.fetch_sub(kControlActivityOne, std::memory_order_acq_rel);
            assert(ControlActivity(old) != 0);
        }
        if (actor.native_) range.controlNativeActorHeld_.store(false, std::memory_order_release);
        actor.counted_ = false;
        actor.native_ = false;
        actor.wrote_ = false;
        if (range.State() == OriginalRangeState::Completed ||
            range.State() == OriginalRangeState::UnpublishedCancelled) CloseControlOriginWhenFinished(range);
        // Local pin protects every range touch until the final zero publication.
        pin.Release();
    }
    enum class NativeControlActorStatus : std::uint8_t { Issued, Unregistered, Refused };
    NativeControlActorStatus BorrowOriginalFrontControlActor(OriginalBatchRange& range,
        OriginalControlMutationActor* out) noexcept {
        if (out == nullptr || out->Valid() || OriginalRangeAtFront() != &range ||
            range.owner_ != this || range.kind_ != OriginalRangeKind::ControlHeader ||
            !range.controlOwner_.Valid() || m_controlReceiver == nullptr ||
            range.controlOwner_.Owner() != m_controlReceiver)
            return NativeControlActorStatus::Refused;
        const auto mode = ControlMode(range.controlOriginWord_.load(std::memory_order_acquire));
        // The default legacy path has no optional registration or scope grant.
        // Preserve its original true typed-owner contract without a new gate.
        if (mode == ControlOriginMode::Disabled) return NativeControlActorStatus::Unregistered;
        if (!IsLiveControlOrigin(mode) ||
            !HasAuthenticOriginalPreparedControlScope(range.controlOwner_, m_controlReceiver))
            return NativeControlActorStatus::Refused;
        bool held = false;
        if (!range.controlNativeActorHeld_.compare_exchange_strong(held, true,
            std::memory_order_acq_rel, std::memory_order_acquire)) return NativeControlActorStatus::Refused;
        if (!AddControlOriginPin(range, true)) {
            range.controlNativeActorHeld_.store(false, std::memory_order_release);
            return NativeControlActorStatus::Refused;
        }
        out->counted_ = AddControlActivity(range, true);
        SetControlOriginPin(range, out->pin_);
        out->native_ = true;
        out->wrote_ = false;
        RetainRedirectForActualControlActor(range, *out);
        return NativeControlActorStatus::Issued;
    }

    bool FrozenProofMatches(const PreparedBatchProof& proof) const noexcept {
        return proof.owner_ == this && proof.origin_ != 0 &&
               proof.origin_ == m_liveFrozenOrigin && m_reservationLive &&
               proof.reservation_ == m_frozenReservation &&
               m_liveReservationId == m_frozenReservationId && m_frozenCount != 0 &&
               m_head.load(std::memory_order_relaxed) == m_frozenHead;
    }

    bool DispositionReceiptMatches(const PreparedBatchDispositionReceipt& receipt,
        const PreparedBatchProof& proof) const noexcept {
        return receipt.owner_ == this && receipt.reservation_ == proof.reservation_ &&
            receipt.reservationId_ == m_frozenReservationId && receipt.head_ == m_frozenHead &&
            receipt.origin_ == proof.origin_ && receipt.count_ == m_frozenCount &&
            receipt.state_ == PreparedBatchDispositionReceipt::State::Prepared;
    }

    bool AbortPreparedDispositionReceipt(PreparedBatchDispositionReceipt& receipt) noexcept {
        if (m_frozenDispositionReceipt != &receipt || receipt.owner_ != this ||
            receipt.state_ != PreparedBatchDispositionReceipt::State::Prepared ||
            receipt.reservation_ != m_frozenReservation ||
            receipt.reservationId_ != m_frozenReservationId || receipt.head_ != m_frozenHead ||
            receipt.origin_ != m_liveFrozenOrigin || receipt.count_ != m_frozenCount ||
            !ReservationMatches(m_frozenReservation)) return false;
        AbortFrozenReservation();
        return receipt.CancelledUnpublishedFrom(*this);
    }

    void AbortFrozenReservation() noexcept {
        PreparedBatchProof proof;
        proof.owner_ = this;
        proof.reservation_ = m_frozenReservation;
        proof.origin_ = m_liveFrozenOrigin;
        proof.Release();
    }

    void AbortPreparedBatch(PreparedBatchProof& proof) noexcept {
        if (!FrozenProofMatches(proof)) return;
        assert(m_frozenDispositionReceipt == nullptr ||
            DispositionReceiptMatches(*m_frozenDispositionReceipt, proof));
        const auto head = m_frozenHead;
        for (std::uint32_t i = 0; i < m_frozenCount; ++i)
            m_originalRanges[(head + i) & m_mask] = nullptr;
        for (std::uint32_t i = 0; i < m_preparedOriginalRangeCount; ++i) {
            CloseControlOriginWhenFinished(*m_preparedOriginalRanges[i]);
            m_preparedOriginalRanges[i]->state_.store(
                OriginalRangeState::UnpublishedCancelled, std::memory_order_release);
            m_preparedOriginalRanges[i] = nullptr;
        }
        m_preparedOriginalRangeCount = 0;
        if (m_frozenDispositionReceipt != nullptr) {
            m_frozenDispositionReceipt->state_ =
                PreparedBatchDispositionReceipt::State::CancelledUnpublished;
            m_frozenDispositionReceipt = nullptr;
        }
        m_liveFrozenOrigin = 0;
        m_frozenReservation = nullptr;
        m_frozenReservationId = 0;
        m_frozenCount = 0;
        ClearReservation(proof.reservation_);
    }

    bool ReservationMatches(const BatchReservation* reservation) const noexcept {
        return reservation != nullptr && m_reservationLive &&
               reservation->owner == this &&
               reservation->id == m_liveReservationId &&
               reservation->head == m_head.load(std::memory_order_relaxed) &&
               reservation->count != 0;
    }

    void ClearReservation(BatchReservation* reservation) noexcept {
        reservation->owner = nullptr;
        reservation->id = 0;
        reservation->head = 0;
        reservation->count = 0;
        m_liveReservationId = 0;
        m_reservationLive = false;
    }

    std::vector<CommandRecord> m_slots;
    const TimelineArmTable* const m_controlReceiver;
    std::vector<CommandRecord> m_publicationScratch;
    std::vector<OriginalBatchRange*> m_originalRanges;
    std::vector<OriginalBatchRange*> m_preparedOriginalRanges;
    std::uint32_t m_preparedOriginalRangeCount = 0;
    std::uint64_t m_nextFrozenOrigin = 0;
    std::uint64_t m_liveFrozenOrigin = 0;
    std::uint64_t m_lastPublishedFrozenOrigin = 0;
    BatchReservation* m_frozenReservation = nullptr;
    PreparedBatchDispositionReceipt* m_frozenDispositionReceipt = nullptr;
    std::uint64_t m_frozenReservationId = 0;
    std::uint64_t m_frozenHead = 0;
    std::uint32_t m_frozenCount = 0;
    std::uint32_t m_mask = 0;
    std::atomic<std::uint64_t> m_head{0}; // next write (producer-owned)
    std::atomic<std::uint64_t> m_tail{0}; // next read (consumer-owned)
    std::atomic<std::uint64_t> m_settledFrontier{0};
    std::uint64_t m_backpressure = 0;     // producer-side refusal counter
    std::uint32_t m_sequence = 0;         // producer-side stamp
    std::uint32_t m_opcodeSeen = 0;       // producer-side admitted-opcode bitmask
    bool m_opcodeOverflowSeen = false;    // sticky: an opcode beyond the
                                          // frozen vocabulary was admitted
    std::uint64_t m_nextReservationId = 0; // producer-side stale-ticket guard
    std::uint64_t m_liveReservationId = 0;
    bool m_reservationLive = false;
};

inline CommandQueue::OriginalControlOriginPin::OriginalControlOriginPin(
    OriginalControlOriginPin&& other) noexcept { *this = std::move(other); }
inline CommandQueue::OriginalControlOriginPin& CommandQueue::OriginalControlOriginPin::operator=(
    OriginalControlOriginPin&& other) noexcept {
    if (this != &other) {
        Release();
        owner_ = std::exchange(other.owner_, nullptr);
        range_ = std::exchange(other.range_, nullptr);
        origin_ = std::exchange(other.origin_, 0);
        head_ = std::exchange(other.head_, 0);
        token_ = std::exchange(other.token_, {});
    }
    return *this;
}
inline bool CommandQueue::OriginalControlOriginPin::Valid() const noexcept {
    return owner_ != nullptr && owner_->MatchesControlOrigin(*this);
}
inline bool CommandQueue::OriginalControlOriginPin::Borrow(OriginalControlOriginPin* out) const noexcept {
    return owner_ != nullptr && owner_->BorrowControlOrigin(*this, out);
}
inline void CommandQueue::OriginalControlOriginPin::Release() noexcept {
    if (owner_ != nullptr) owner_->ReturnControlOrigin(*this);
}
inline CommandQueue::OriginalControlMutationActor::OriginalControlMutationActor(
    OriginalControlMutationActor&& other) noexcept { *this = std::move(other); }
inline CommandQueue::OriginalControlMutationActor& CommandQueue::OriginalControlMutationActor::operator=(
    OriginalControlMutationActor&& other) noexcept {
    if (this != &other) {
        Release();
        pin_ = std::move(other.pin_);
        catalogSibling_ = std::move(other.catalogSibling_);
        catalogFallback_ = std::exchange(other.catalogFallback_, false);
        counted_ = std::exchange(other.counted_, false);
        native_ = std::exchange(other.native_, false);
        wrote_ = std::exchange(other.wrote_, false);
    }
    return *this;
}
inline bool CommandQueue::OriginalControlMutationActor::Valid() const noexcept {
    return pin_.owner_ != nullptr && pin_.owner_->MatchesControlActor(*this);
}
inline bool CommandQueue::OriginalControlMutationActor::Borrow(OriginalControlMutationActor* out) const noexcept {
    return pin_.owner_ != nullptr && pin_.owner_->BorrowControlActor(*this, out);
}
inline bool CommandQueue::OriginalControlMutationActor::BeforeFirstWrite() noexcept {
    return pin_.owner_ != nullptr && pin_.owner_->ControlActorBeforeFirstWrite(*this);
}
inline void CommandQueue::OriginalControlMutationActor::Release() noexcept {
    if (pin_.owner_ != nullptr) pin_.owner_->ReturnControlActor(*this);
}

} // namespace harken

#endif // HARKEN_CORE_COMMAND_QUEUE_H
