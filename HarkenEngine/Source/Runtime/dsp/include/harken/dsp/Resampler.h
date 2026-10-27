// Resample stage: absorbs sample-rate mismatch and pitch ratio between the
// source and the engine rate. The resampler owns a small per-channel input
// FIFO; the source always contributes fixed-size chunks, and output frames
// are emitted only when enough input has accumulated (single forward pass —
// no pull protocol between stages).
//
// LegacyCompatibility retains the original selectable interpolation families
// and high-ratio behavior. Professional44148 uses a bounded five-stage
// half-band path plus a fixed residual FIR for the supported 44.1/48 kHz rate
// pairs. Both coefficient sets are built once off the audio thread and shared
// read-only across voices.
#ifndef HARKEN_DSP_RESAMPLER_H
#define HARKEN_DSP_RESAMPLER_H

#include "harken/core/PcmView.h"
#include "harken/dsp/InterpKernels.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace harken {

enum class ResampleQuality : std::uint8_t {
    Linear = 0,
    Hermite = 1, // cascade default
    Sinc8 = 2,
    Sinc16 = 3
};

enum class ResampleProfile : std::uint8_t {
    LegacyCompatibility = 0,
    Professional44148 = 1
};

// SelectResampleProfile returns the runtime Play default (Linear /
// LegacyCompatibility). Professional44148 remains available to internal
// callers that explicitly select a profile. Pitch changes the effective ratio
// inside the selected profile but never selects a profile.
ResampleProfile SelectResampleProfile(std::uint32_t sourceRate,
                                      std::uint32_t mixRate);

// Shared read-only polyphase coefficient tables. Init() runs on the logic
// thread at engine creation; render only reads.
struct ResampleTables {
    KaiserSincTable<8> sinc8;
    KaiserSincTable<16> sinc16;

    void Init() {
        // Proven parameter pairs: 8 taps beta 5.0 (mid tier), 16 taps
        // beta 9.0 (highest tier, THD below -85 dB).
        sinc8.Init(5.0);
        sinc16.Init(9.0);
    }
};

// M10 professional profile geometry. The implementation remains separately
// testable while Resampler provides the production-facing profile facade.
// The professional M10 envelope is limited to 44.1/48 kHz source and mix
// rates. Five half-band stages plus the bounded residual FIR cover the public
// 1/64..64 pitch-ratio contract, including the maximum 48/44.1 kHz rate
// conversion ratio. The authored [-48, +48] semitone matrix still uses at
// most four stages.
inline constexpr std::uint32_t kMultirateMaxStages = 5;
inline constexpr double kMultirateMaximumEffectiveRatio =
    (48000.0 / 44100.0) * 64.0;
inline constexpr double kMultirateResidualMaximumRatio =
    kMultirateMaximumEffectiveRatio /
    static_cast<double>(std::uint64_t{1} <<
                        kMultirateMaxStages);
inline constexpr double kMultirateResidualTailBlendStart = 1.9;
inline constexpr double kMultirateResidualTailCutoff = 0.125;
inline constexpr std::uint32_t kMultirateHalfbandTaps = 43;
inline constexpr std::uint32_t kMultirateHalfbandSideFrames =
    (kMultirateHalfbandTaps + 1u) / 2u;
inline constexpr std::uint32_t kMultirateHalfbandCenterFrames =
    (kMultirateHalfbandTaps + 1u) / 4u;
inline constexpr std::uint32_t kMultirateHalfbandSideTapCount =
    kMultirateHalfbandTaps / 4u + 1u;
inline constexpr std::uint32_t kMultirateResidualBaseRatioRows = 8;
inline constexpr std::uint32_t kMultirateResidualRatioRows =
    kMultirateResidualBaseRatioRows + 1u;
inline constexpr std::uint32_t kMultirateResidualPhases = 256;
inline constexpr std::uint32_t kMultirateResidualTaps = 16;
inline constexpr std::uint32_t kMultirateFinalRingFrames = 32;
inline constexpr std::uint32_t kMultirateTransitionOverlapFrames = 128;

struct MultirateHalfbandSideTap {
    float coefficient = 0.0f;
    std::uint32_t firstDistance = 0;
    std::uint32_t secondDistance = 0;
};

struct MultirateResampleTables {
    float halfband[kMultirateHalfbandTaps] = {};
    MultirateHalfbandSideTap halfbandSide[
        kMultirateHalfbandSideTapCount] = {};
    float residual[kMultirateResidualRatioRows]
                  [kMultirateResidualPhases]
                  [kMultirateResidualTaps] = {};

    // Builds every coefficient off the audio thread. Render only performs
    // bounded table lookup and interpolation.
    void Init();
};

static_assert(sizeof(MultirateResampleTables) <= 256u * 1024u,
              "professional shared tables exceed the 256 KiB budget");

struct MultirateResampleCounters {
    std::uint64_t rawFramesRead = 0;
    std::uint64_t halfbandOutputFrames = 0;
    std::uint64_t halfbandMultiplyAccumulates = 0;
    std::uint64_t residualOutputFrames = 0;
    std::uint64_t residualMultiplyAccumulates = 0;
    std::uint32_t stageTransitions = 0;
    std::uint32_t discontinuousStageTransitions = 0;
    std::uint32_t maximumActiveStages = 0;
};

struct MultirateHalfbandStageState {
    // A half-band decimator evaluates on odd input phases. Every non-center
    // nonzero coefficient addresses the odd polyphase component, while the
    // center coefficient addresses one delayed even sample. Storing those two
    // components removes all guaranteed-zero history positions.
    float sideHistory[kMaxChannels][kMultirateHalfbandSideFrames] = {};
    float centerHistory[kMaxChannels][kMultirateHalfbandCenterFrames] = {};
    std::uint32_t sideWriteOffset = 0;
    std::uint32_t centerWriteOffset = 0;
    std::uint32_t inputFrames = 0;
};

struct MultirateCandidatePathState {
    MultirateHalfbandStageState stages[kMultirateMaxStages] = {};
    float finalRing[kMaxChannels][kMultirateFinalRingFrames] = {};
    std::uint64_t rawProcessIndex = 0;
    std::uint64_t finalWriteIndex = 0;
    std::int64_t sourceOrigin = 0;
    std::uint64_t validSourceStart = 0;
    std::uint32_t stageCount = 0;
    bool initialized = false;
};

// Ratio-gated professional multirate state. The external raw ring uses the
// same preallocated storage contract as Resampler. Two fixed paths coexist
// only while a new stage topology prewarms or overlaps. Keeping both paths
// resident avoids render-thread allocation and avoids continuously processing
// every half-band level.
struct MultirateResamplerCandidate {
    float* rawRing[kMaxChannels] = {};
    const MultirateResampleTables* tables = nullptr;
    std::uint32_t channelCount = 0;
    double ratio = 1.0;
    double targetRatio = 1.0;
    double rampStep = 0.0;
    std::uint32_t rampRemaining = 0;
    std::uint32_t activeStages = 0;

    std::uint64_t rawWriteIndex = 0;
    std::uint64_t rawEndIndex = 0;
    bool inputEnded = false;

    MultirateCandidatePathState paths[2] = {};
    std::uint64_t sourceFrame = 0;
    double sourceFraction = 0.0;
    std::uint64_t transitionOverlapSource = 0;
    std::uint32_t activePath = 0;
    std::uint32_t transitionPath = 1;
    std::uint32_t transitionTargetStages = 0;
    std::uint32_t overlapFrame = 0;
    bool transitionPreparing = false;
    bool transitionOverlapping = false;
    bool topologyTransitionsEnabled = true;
    MultirateResampleCounters counters = {};

    void Reset(float* const* ringStorage, std::uint32_t channels,
               double inRatio, const MultirateResampleTables* t);
    std::uint32_t FreeFrames() const;
    void PushPCM(const PCMView& pcm, std::uint64_t startFrame,
                 std::uint32_t frames);
    void PushSilence(std::uint32_t frames);
    void MarkInputEnd();
    bool Exhausted() const;
    std::uint32_t Produce(float* const* dst, std::uint32_t dstOffset,
                          std::uint32_t maxFrames);
    std::uint32_t ProduceInstrumentedForTesting(
        float* const* dst, std::uint32_t dstOffset,
        std::uint32_t maxFrames);
    std::uint32_t ProducePerChannelResidualForTesting(
        float* const* dst, std::uint32_t dstOffset,
        std::uint32_t maxFrames);
    void UpdateRatio(double newRatio, std::uint32_t rampFrames);
    void SetTopologyTransitionsEnabledForTesting(bool enabled) {
        topologyTransitionsEnabled = enabled;
    }

    static std::uint32_t ResolveStageCount(double effectiveRatio);

private:
    template <bool Instrumented>
    std::uint32_t ProduceImpl(float* const* dst,
                              std::uint32_t dstOffset,
                              std::uint32_t maxFrames);
};

// A click-free topology transition needs old and new FIR histories to coexist.
// One path is approximately 5 KiB, so the honest bounded implementation cannot
// meet the 8 KiB target without discarding transition state.
static_assert(sizeof(MultirateResamplerCandidate) <= 32u * 1024u,
              "professional multirate state exceeds the 32 KiB ceiling");

// FIFO geometry. The ring must hold one source chunk plus the widest
// kernel's history/look-ahead window; power-of-two capacity keeps ring
// addressing a mask. Absolute indices never wrap in practice (uint64 at
// 48 kHz overflows after ~12 million years).
inline constexpr std::uint32_t kResampleFifoFrames = 512;
inline constexpr std::uint32_t kResampleChunkFrames = 128;
// Zero frames pre-written at reset so the unscaled 16-tap kernel reads seven
// defined history samples at the first output frame. Wider pre-zero history
// requested by a scaled kernel is treated as virtual silence.
inline constexpr std::uint32_t kResampleHistoryPad = 8;

// Ratios above the bounded direct-sinc range use a streaming 16th-order
// low-pass. Eight fixed biquad states per channel summarize arbitrarily long
// input history without widening the FIFO or allocating on the audio thread.
inline constexpr std::uint32_t kResampleHighRatioFilterSections = 8;

struct ResampleFilterSectionState {
    double z1 = 0.0;
    double z2 = 0.0;
};

struct ResamplerParkState {
    static constexpr std::uint32_t kFormatVersion = 5;

    std::uint32_t formatVersion = kFormatVersion;
    std::uint32_t usedBytes = 0;
    ResampleProfile profile = ResampleProfile::LegacyCompatibility;
    ResampleQuality quality = ResampleQuality::Linear;
    std::uint32_t channelCount = 0;
    std::uint32_t activeStages = 0;
    double ratio = 1.0;
    double targetRatio = 1.0;
    double rampStep = 0.0;
    std::uint32_t rampRemaining = 0;
    std::uint64_t rawWriteIndex = 0;
    std::uint64_t rawEndIndex = 0;
    bool inputEnded = false;
    MultirateCandidatePathState paths[2] = {};
    std::uint64_t sourceFrame = 0;
    double sourceFraction = 0.0;
    std::uint64_t transitionOverlapSource = 0;
    std::uint32_t activePath = 0;
    std::uint32_t transitionPath = 1;
    std::uint32_t transitionTargetStages = 0;
    std::uint32_t overlapFrame = 0;
    bool transitionPreparing = false;
    bool transitionOverlapping = false;
    bool topologyTransitionsEnabled = true;
    std::uint64_t legacyReadIndex = 0;
    double legacyReadFraction = 0.0;
    std::uint64_t legacyPhaseHigh = 0;
    std::uint64_t legacyPhaseLow = 0;
    std::uint64_t legacyWriteIndex = 0;
    std::uint64_t legacyInputEndIndex = 0;
    bool legacyInputEnded = false;
    ResampleFilterSectionState legacyHighRatioFilter
        [kMaxChannels][kResampleHighRatioFilterSections] = {};
    double legacyHighRatioPrevious[kMaxChannels] = {};
    double legacyHighRatioCurrent[kMaxChannels] = {};
    std::uint64_t legacyHighRatioFilterIndex = 0;
    bool legacyHighRatioFilterActive = false;
    std::uint64_t rawStartIndex = 0;
    std::uint32_t rawFrameCount = 0;
    float rawFrames[kMaxChannels][kResampleFifoFrames] = {};
};

using ProfessionalResamplerState = ResamplerParkState;

static_assert(std::is_trivially_copyable_v<ResamplerParkState>,
              "resampler park state must be trivially copyable");
static_assert(sizeof(ResamplerParkState) <= 32u * 1024u,
              "resampler park state exceeds the 32 KiB ceiling");

struct ResamplerUnitySpan {
    std::uint32_t ringOffset = 0;
    std::uint32_t firstFrames = 0;
    std::uint32_t frames = 0;
};

struct ResamplerHighRatioChannelState {
    ResampleFilterSectionState filter[kResampleHighRatioFilterSections] = {};
    double previous = 0.0;
    double current = 0.0;
};
static_assert(std::is_trivially_copyable_v<ResamplerHighRatioChannelState>);

struct ResamplerHighRatioColumns {
    ResamplerHighRatioChannelState* channels[kMaxChannels] = {};
};

// Borrows stable, prepared channel rows, ring and optional profile storage.
// The columns table and its addresses must outlive every bound view.
struct ResamplerView {
    float* ring[kMaxChannels] = {};  // kResampleFifoFrames floats each
    const ResampleTables* tables = nullptr;
    std::uint32_t channelCount = 0;
    ResampleQuality quality = ResampleQuality::Hermite;
    double ratio = 1.0;      // input frames consumed per output frame
    double targetRatio = 1.0;
    double rampStep = 0.0;
    std::uint32_t rampRemaining = 0;
    std::uint64_t readIndex = 0; // exact integer interpolation center
    double readFraction = 0.0;   // bounded fractional phase in [0, 1)
    std::uint64_t phaseHigh = 0;
    std::uint64_t phaseLow = 0;
    double readPos = 0.0;        // derived diagnostic mirror
    std::uint64_t writeIndex = 0;  // absolute input frames written
    std::uint64_t inputEndIndex = 0; // absolute end of real input, valid
                                     // only when inputEnded
    bool inputEnded = false;
    std::uint64_t highRatioFilterIndex = 0; // next raw input frame to filter
    bool highRatioFilterActive = false;
    ResampleProfile resampleProfile =
        ResampleProfile::LegacyCompatibility;
    MultirateResamplerCandidate* professional = nullptr;

    // Bind only before a new generation is reset/restored. A later Reset keeps
    // this binding, including historical columns outside the current source.
    void BindHighRatioColumns(const ResamplerHighRatioColumns& columns,
                              std::uint32_t row,
                              std::uint32_t preparedChannels) noexcept {
        assert(preparedChannels <= kMaxChannels);
        highRatioColumns_ = &columns;
        highRatioRow_ = row;
        highRatioPreparedChannels_ = preparedChannels;
    }
    ResamplerHighRatioChannelState& HighRatioChannel(std::uint32_t ch) noexcept {
        assert(ch < highRatioPreparedChannels_ && highRatioColumns_ != nullptr &&
               highRatioColumns_->channels[ch] != nullptr);
        return *std::launder(reinterpret_cast<ResamplerHighRatioChannelState*>(
            reinterpret_cast<std::byte*>(highRatioColumns_->channels[ch]) +
            static_cast<std::size_t>(highRatioRow_) * sizeof(ResamplerHighRatioChannelState)));
    }
    const ResamplerHighRatioChannelState& HighRatioChannel(std::uint32_t ch) const noexcept {
        assert(ch < highRatioPreparedChannels_ && highRatioColumns_ != nullptr &&
               highRatioColumns_->channels[ch] != nullptr);
        return *std::launder(reinterpret_cast<const ResamplerHighRatioChannelState*>(
            reinterpret_cast<const std::byte*>(highRatioColumns_->channels[ch]) +
            static_cast<std::size_t>(highRatioRow_) * sizeof(ResamplerHighRatioChannelState)));
    }
    // The caller supplies a validated complete park state.
    static std::uint32_t RequiredHighRatioChannels(const ResamplerParkState& state) noexcept;

    // Prepares for a new voice: zeroes the ring, writes the history pad,
    // and positions the read head on the first real input frame. Runs on
    // the logic thread at voice start or on audio for a prepared seek.
    void Reset(float* const* ringStorage, std::uint32_t channels,
               ResampleQuality q, double inRatio, const ResampleTables* t);

    // Selects one immutable processing profile for the lifetime of the reset
    // voice. Professional requires caller-owned stable workspace. Both profiles
    // use the caller-owned raw ring; no PCM is duplicated.
    void Reset(float* const* ringStorage, std::uint32_t channels,
               ResampleQuality q, double inRatio, const ResampleTables* t,
               ResampleProfile profile,
               const MultirateResampleTables* multirateTables,
               MultirateResamplerCandidate* workspace);

    // Frames of real input the ring can accept without overwriting the fixed
    // quality-tier history reserve needed by a later ratio change.
    std::uint32_t FreeFrames() const;

    // Source contribution: copies `frames` frames starting at pcm frame
    // `startFrame` into the ring (all channels).
    void PushPCM(const PCMView& pcm, std::uint64_t startFrame,
                 std::uint32_t frames);

    // Look-ahead flush padding once real input has ended.
    void PushSilence(std::uint32_t frames);

    // Declares that no further real input exists beyond what was pushed.
    void MarkInputEnd();

    // True once every real input frame has been consumed (read head passed
    // the declared end).
    bool Exhausted() const;

    // Emits up to maxFrames output frames into planar dst at dstOffset.
    // Returns the count produced; fewer than maxFrames means the ring
    // starved (push more input or silence) or the input is exhausted.
    std::uint32_t Produce(float* const* dst, std::uint32_t dstOffset,
                          std::uint32_t maxFrames);

    // Consumes a complete live unity span without copying. Ring storage stays
    // read-only until the borrower finishes; no refill/reset may intervene.
    bool BorrowLinearUnity(std::uint32_t frames,
                           ResamplerUnitySpan& out) noexcept;

    // Runtime rate change at a block boundary. The read position, ring
    // contents, and accumulated kernel history are preserved while the
    // per-output-frame advance ramps to the new target.
    void UpdateRatio(double newRatio, std::uint32_t rampFrames);

    ResampleProfile Profile() const {
        return resampleProfile;
    }
    std::uint64_t SourceFrame() const {
        return resampleProfile == ResampleProfile::Professional44148
                   ? professional->sourceFrame
                   : readIndex;
    }
    double SourceFraction() const {
        return resampleProfile == ResampleProfile::Professional44148
                   ? professional->sourceFraction
                   : readFraction;
    }
    bool InputEnded() const {
        return resampleProfile == ResampleProfile::Professional44148
                   ? professional->inputEnded
                   : inputEnded;
    }
    double CurrentRatio() const {
        return resampleProfile == ResampleProfile::Professional44148
                   ? professional->ratio
                   : ratio;
    }
    double TargetRatio() const {
        return resampleProfile == ResampleProfile::Professional44148
                   ? professional->targetRatio
                   : targetRatio;
    }
    std::uint32_t RampFramesRemaining() const {
        return resampleProfile == ResampleProfile::Professional44148
                   ? professional->rampRemaining
                   : rampRemaining;
    }
    double RampStep() const {
        return resampleProfile == ResampleProfile::Professional44148
                   ? professional->rampStep
                   : rampStep;
    }
    bool CaptureProfessional(ProfessionalResamplerState* out) const;
    static bool ValidateProfessionalState(
        const ProfessionalResamplerState& state);
    bool RestoreProfessional(
        const ProfessionalResamplerState& state,
        float* const* ringStorage, std::uint32_t channels,
        const ResampleTables* legacyTables,
        const MultirateResampleTables* multirateTables,
        MultirateResamplerCandidate* workspace);
    bool CaptureParkState(ResamplerParkState* out) const;
    static bool ValidateParkState(const ResamplerParkState& state);
    bool RestoreParkState(
        const ResamplerParkState& state,
        float* const* ringStorage, std::uint32_t channels,
        const ResampleTables* legacyTables,
        const MultirateResampleTables* multirateTables,
        MultirateResamplerCandidate* workspace = nullptr);

private:
    bool HasHighRatioBacking(std::uint32_t channels) const noexcept;
    const ResamplerHighRatioColumns* highRatioColumns_ = nullptr;
    std::uint32_t highRatioRow_ = 0;
    std::uint32_t highRatioPreparedChannels_ = 0;
};

// Standalone callers retain complete value-owned filter history. Copies keep
// the existing borrowed ring/profile pointers but rebind their own history.
struct Resampler : ResamplerView {
    Resampler() noexcept;
    Resampler(const Resampler& other) noexcept;
    Resampler& operator=(const Resampler& other) noexcept;
    Resampler(Resampler&& other) noexcept;
    Resampler& operator=(Resampler&& other) noexcept;

private:
    using ResamplerView::BindHighRatioColumns;
    void BindOwnedHighRatioColumns() noexcept;
    ResamplerHighRatioChannelState highRatioOwned_[kMaxChannels] = {};
    ResamplerHighRatioColumns highRatioOwnedColumns_{};
};

static_assert(sizeof(Resampler) <= 32u * 1024u,
              "resampler facade exceeds the 32 KiB state ceiling");

} // namespace harken

#endif // HARKEN_DSP_RESAMPLER_H
