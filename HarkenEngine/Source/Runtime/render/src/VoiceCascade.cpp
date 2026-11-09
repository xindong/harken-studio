#include "harken/render/Voice.h"
#include "harken/render/SignalRenderPlan.h"
#include "harken/render/EffectChain.h"
#include "harken/render/VoiceTimelineEligibility.h"

#include "MonitorCaptureHub.h"

#include "harken/abi/TimelineStopContract.h"
#include "harken/core/ChannelLayout.h"
#include "harken/core/SnapshotBuffer.h"
#include "harken/dsp/InterpKernels.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

namespace harken {

namespace {

// Match the ordinary native-channel fold, including its last-source-wins order.
void MapNativeImageChannels(std::uint32_t sourceChannels, std::uint32_t mixChannels,
                            std::uint32_t mixLayout, int* sources) noexcept {
    std::fill_n(sources, mixChannels, -1);
    ChannelRole roles[kMaxChannels];
    const auto sourceLayout = LayoutFromChannelCount(sourceChannels);
    if (sourceLayout != VN_CHANNEL_LAYOUT_DEFAULT) LayoutRoles(sourceLayout, roles);
    for (std::uint32_t source = 0; source < sourceChannels; ++source) {
        const int destination = sourceLayout == VN_CHANNEL_LAYOUT_DEFAULT
            ? (source < mixChannels ? static_cast<int>(source) : -1)
            : ChannelOfRole(mixLayout, roles[source]);
        if (destination >= 0) sources[destination] = static_cast<int>(source);
    }
}

// Materialize one stable matrix row, retaining the VoiceGain float boundary.
void ProjectSignalImage(float* image, const float* left, const float* right,
                        std::uint32_t frames, float a, float b, float voiceGain = 1.0f) noexcept {
    const auto project = [&]<bool Stereo, bool Gain>() {
        std::uint32_t n = 0;
#if defined(HARKEN_OUTPUT_GAIN_SSE2)
        const auto av = _mm_set1_ps(a), bv = _mm_set1_ps(b), gv = _mm_set1_ps(voiceGain);
        for (; n + 4 <= frames; n += 4) {
            auto l = _mm_loadu_ps(left + n);
            if constexpr (Gain) l = _mm_mul_ps(l, gv);
            auto r = _mm_setzero_ps();
            if constexpr (Stereo) {
                r = _mm_loadu_ps(right + n);
                if constexpr (Gain) r = _mm_mul_ps(r, gv);
                r = _mm_mul_ps(r, bv);
            }
            _mm_storeu_ps(image + n, _mm_add_ps(_mm_mul_ps(l, av), r));
        }
#endif
        for (; n < frames; ++n) {
            const float l = Gain ? left[n] * voiceGain : left[n];
            const float r = Stereo ? (Gain ? right[n] * voiceGain : right[n]) : 0.0f;
            image[n] = l * a + (Stereo ? r * b : 0.0f);
        }
    };
    if (right != nullptr) {
        if (voiceGain == 1.0f) project.template operator()<true, false>();
        else project.template operator()<true, true>();
    } else {
        if (voiceGain == 1.0f) project.template operator()<false, false>();
        else project.template operator()<false, true>();
    }
}

struct PreparedInput {
    const float* source = nullptr;
    const double* gains = nullptr;
    const float* levels = nullptr;
    unsigned levelBegin = 0;
#if defined(HARKEN_OUTPUT_GAIN_SSE2)
    __m128 level;
#endif
    float scalarLevel = 1.0f;
    bool active = false;
};

// Template recursion fixes the voice count and addition order at compile time.
// Input pointers and the output multiplier are loaded before the frame loop.
#if defined(HARKEN_OUTPUT_GAIN_SSE2)
template<unsigned Voice, unsigned Count, unsigned Outputs, bool Ramping>
inline void AddFixed4(const PreparedInput* inputs, __m128* sums,
                           unsigned outputCount, unsigned n) {
    const auto& input = inputs[Voice];
    if (input.active) {
        auto received = _mm_loadu_ps(input.source + n);
        if (input.gains != nullptr) {
            const auto low = _mm_mul_pd(_mm_cvtps_pd(received), _mm_loadu_pd(input.gains + n));
            const auto high = _mm_mul_pd(_mm_cvtps_pd(_mm_movehl_ps(received, received)),
                                         _mm_loadu_pd(input.gains + n + 2));
            received = _mm_movelh_ps(_mm_cvtpd_ps(low), _mm_cvtpd_ps(high));
        }
        const auto level = Ramping ? _mm_loadu_ps(input.levels + n - input.levelBegin) : input.level;
        const auto contribution = _mm_mul_ps(received, level);
        const unsigned lanes = Outputs == 0 ? outputCount : Outputs;
        for (unsigned lane = 0; lane < lanes; ++lane)
            sums[lane] = _mm_add_ps(sums[lane], contribution);
    }
    if constexpr (Voice + 1 < Count) AddFixed4<Voice + 1, Count, Outputs, Ramping>(inputs, sums, outputCount, n);
}

#endif
template<unsigned Voice, unsigned Count, unsigned Outputs, bool Ramping>
inline void AddFixed1(const PreparedInput* inputs, float* sums,
                           unsigned outputCount, unsigned n) {
    const auto& input = inputs[Voice];
    if (input.active) {
        const float received = input.gains == nullptr ? input.source[n]
            : static_cast<float>(static_cast<double>(input.source[n]) * input.gains[n]);
        const float level = Ramping ? input.levels[n - input.levelBegin] : input.scalarLevel;
        const float contribution = received * level;
        const unsigned lanes = Outputs == 0 ? outputCount : Outputs;
        for (unsigned lane = 0; lane < lanes; ++lane) sums[lane] += contribution;
    }
    if constexpr (Voice + 1 < Count) AddFixed1<Voice + 1, Count, Outputs, Ramping>(inputs, sums, outputCount, n);
}

template<unsigned Count, unsigned Outputs, bool Ramping>
inline void FoldFixedLanes(const PreparedInput* inputs, float* const* destinations,
                                unsigned outputCount, unsigned begin, unsigned frames) {
    const unsigned lanes = Outputs == 0 ? outputCount : Outputs;
    unsigned n = begin;
#if defined(HARKEN_OUTPUT_GAIN_SSE2)
    for (; n + 4 <= begin + frames; n += 4) {
        __m128 sums[kMaxChannels];
        for (unsigned lane = 0; lane < lanes; ++lane) sums[lane] = _mm_loadu_ps(destinations[lane] + n);
        AddFixed4<0, Count, Outputs, Ramping>(inputs, sums, outputCount, n);
        for (unsigned lane = 0; lane < lanes; ++lane) _mm_storeu_ps(destinations[lane] + n, sums[lane]);
    }
#endif
    for (; n < begin + frames; ++n) {
        float sums[kMaxChannels];
        for (unsigned lane = 0; lane < lanes; ++lane) sums[lane] = destinations[lane][n];
        AddFixed1<0, Count, Outputs, Ramping>(inputs, sums, outputCount, n);
        for (unsigned lane = 0; lane < lanes; ++lane) destinations[lane][n] = sums[lane];
    }
}


void MixReceivedOutput(const float* source, std::span<float* const> destinations, const double* gains,
                       std::uint32_t frames, const OutputGain& output) noexcept {
    if (output.remainingFrames == 0) {
        const auto mix = [&]<bool Receiving, unsigned Outputs>() {
        const auto outputCount = Outputs == 0 ? destinations.size() : Outputs;
        std::uint32_t n = 0;
#if defined(HARKEN_OUTPUT_GAIN_SSE2)
        const auto level = _mm_set1_ps(output.target);
        for (; n + 4 <= frames; n += 4) {
            const auto input = _mm_loadu_ps(source + n);
            auto received = input;
            if constexpr (Receiving) {
                const auto low = _mm_mul_pd(_mm_cvtps_pd(input), _mm_loadu_pd(gains + n));
                const auto high = _mm_mul_pd(_mm_cvtps_pd(_mm_movehl_ps(input, input)), _mm_loadu_pd(gains + n + 2));
                received = _mm_movelh_ps(_mm_cvtpd_ps(low), _mm_cvtpd_ps(high));
            }
            const auto contribution = _mm_mul_ps(received, level);
            for (std::size_t lane = 0; lane < outputCount; ++lane)
                _mm_storeu_ps(destinations[lane] + n, _mm_add_ps(_mm_loadu_ps(destinations[lane] + n), contribution));
        }
#endif
        for (; n < frames; ++n) {
            const float received = !Receiving ? source[n] : static_cast<float>(static_cast<double>(source[n]) * gains[n]);
            const float contribution = received * output.target;
            for (std::size_t lane = 0; lane < outputCount; ++lane) destinations[lane][n] += contribution;
        }
        };
        const auto dispatch = [&]<bool Receiving>() {
            if (destinations.size() == 1) mix.template operator()<Receiving, 1>();
            else if (destinations.size() == 2) mix.template operator()<Receiving, 2>();
            else mix.template operator()<Receiving, 0>();
        };
        if (gains == nullptr) dispatch.template operator()<false>();
        else dispatch.template operator()<true>();
        return;
    }
    // Retain the intermediate float boundary and the existing ramp kernel.
    float tile[128];
    for (std::uint32_t begin = 0; begin < frames; begin += 128) {
        const auto count = std::min(128u, frames - begin);
        for (std::uint32_t n = 0; n < count; ++n)
            tile[n] = gains == nullptr ? source[begin + n] : static_cast<float>(static_cast<double>(source[begin + n]) * gains[begin + n]);
        for (auto* destination : destinations) output.MixAdd(tile, destination + begin, count, begin);
    }
}

// Scatters a voice's post-pan/post-width tap into each active send target,
// scaled by the send's ramped level and added to the target bus's input. The
// level interpolates per sample exactly like a bus gain (block-boundary linear),
// and its multi-block ramp plan advances one block here — once per rendered
// block, since RenderVoiceBlock runs once per voice per block. tap holds the
// stereo block for [offset, offset+produced); each target gets a scaled copy of
// that same region so the spatial image travels with the send.
void ScatterVoiceSends(VoiceRenderContext& ctx, float* const* busPlanes,
                       float* const* tap, std::uint32_t offset,
                       std::uint32_t produced, std::uint32_t mixChannels) {
    for (std::uint32_t s = 0; s < kMaxVoiceSendSlots; ++s) {
        VoiceSendSlot& send = ctx.voiceSends[s];
        if (!send.active) {
            continue;
        }
        if (send.ramping) {
            send.gain.SetBlockTarget(send.rampStartGain, send.rampEndGain,
                                     send.rampBlocksDone, send.rampBlocksTotal);
        }
        float* const* dst = busPlanes + send.targetBusIndex * mixChannels;
        send.gain.MixAddBlock(tap, dst, mixChannels, offset, produced);
        if (send.ramping) {
            ++send.rampBlocksDone;
            if (send.rampBlocksDone >= send.rampBlocksTotal) {
                send.ramping = false;
            }
        }
    }
}

// Source decode: contributes fixed-size chunks of registered PCM into the
// resampler's FIFO. Returns false when nothing further can be fed (input
// fully consumed and flushed).
//
// Looping is a read-position wrap inside this stage: while passes remain (or
// forever, when infinite) the forward read stops at loopEndFrame and jumps
// back to loopStartFrame WITHOUT declaring the input ended, so the ring holds
// the pre-seam frames next to the post-seam frames and the interpolation
// kernel reads real neighbours across the join — the seam is seamless because
// it is a feed property, not a special case. MarkInputEnd fires only when the
// final traversal reaches the true file end.
bool FeedResampler(VoiceRenderContext& ctx) {
    ResamplerView& rs = ctx.resampler;
    if (ctx.srcCursor < ctx.pcm.frames) {
        // A wrap is due at the region end while more traversals remain; the
        // final traversal instead reads on through the outro to the file
        // end. loopEndFrame is clamped to pcm.frames at play time, so the
        // limit is always a valid forward target above srcCursor.
        const bool wrapAtLimit =
            ctx.loopInfinite || ctx.loopPassesRemaining > 1;
        const std::uint64_t limit =
            wrapAtLimit ? ctx.loopEndFrame : ctx.pcm.frames;
        const std::uint64_t remaining = limit - ctx.srcCursor;
        std::uint32_t chunk = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(kResampleChunkFrames,
                std::min<std::uint64_t>(remaining, rs.FreeFrames())));
        if (chunk == 0) {
            return false;
        }
        if (ctx.mediaDecoder) {
            PCMView decoded;
            if (!ctx.mediaDecoder->Read(ctx.srcCursor, chunk, ctx.mediaRenderThread, decoded) ||
                decoded.frames == 0 || decoded.frames > chunk) {
                ctx.srcCursor = ctx.pcm.frames;
                rs.MarkInputEnd();
                return true;
            }
            chunk = static_cast<std::uint32_t>(decoded.frames);
            rs.PushPCM(decoded, 0, chunk);
        } else {
            rs.PushPCM(ctx.pcm, ctx.srcCursor, chunk);
        }
        ctx.srcCursor += chunk;
        if (ctx.srcCursor == limit) {
            if (wrapAtLimit) {
                // Seamless wrap: keep feeding from the loop start, no
                // MarkInputEnd. Each completed traversal consumes one pass.
                ctx.srcCursor = ctx.loopStartFrame;
                if (!ctx.loopInfinite) {
                    --ctx.loopPassesRemaining;
                    ++ctx.completedLoopPasses;
                }
            } else {
                rs.MarkInputEnd();
            }
        }
        return true;
    }
    if (!rs.InputEnded()) {
        rs.MarkInputEnd();
        return true;
    }
    if (!rs.Exhausted()) {
        // Look-ahead flush: zero padding lets the kernel consume the real
        // tail samples it still needs neighbors for.
        const std::uint32_t pad =
            std::min(kResampleChunkFrames, rs.FreeFrames());
        if (pad == 0) {
            return false;
        }
        rs.PushSilence(pad);
        return true;
    }
    return false;
}

// Source decode for a streamed voice: contributes frames from the stream
// ring instead of registered PCM. Returns false when nothing further can be
// fed this block — either the resampler FIFO is full, the flushed input is
// fully consumed, or the ring is dry.
//
// A dry ring while the source is still live is NOT an end condition: the
// stage simply feeds nothing, the block reports fewer frames than the
// target, and the voice stays Active — that is the starvation gap, and the
// next block retries against whatever the streaming thread delivered in
// the meantime. Only the producer's generation-bound terminal winner
// (natural end of file and mid-stream integrity abort look identical)
// converts a drained ring into MarkInputEnd and the normal look-ahead flush.
bool FeedResamplerFromStream(VoiceRenderContext& ctx) {
    if (ctx.mediaDecodeFailed) return false;
    ResamplerView& rs = ctx.resampler;
    StreamRing& ring = *ctx.streamRing;
    // Sample end-of-data BEFORE reading the write index. The producer
    // release-publishes the terminal winner after the final byte/frame publication, so
    // an acquire load that observes it happens-after that publish and the
    // span read below must then see the final frames — a dry ring observed
    // after ended==true is genuinely drained. Reading in the opposite
    // order could pair a stale (empty) span with a fresh ended flag and
    // truncate the tail.
    const bool ended = ring.SourceEnded();
    const std::uint32_t span = ring.BufferedFrames();
    if (span > 0) {
        const std::uint32_t chunk = std::min(kResampleChunkFrames,
            std::min(span, rs.FreeFrames()));
        if (chunk == 0) return false;
        if (ctx.mediaDecoder != nullptr) {
            PCMView decoded;
            const auto result = ctx.mediaDecoder->ReadStream(ring, chunk, ctx.mediaRenderThread, decoded);
            if (result == StreamDecodeResult::Dry) return false;
            if (result != StreamDecodeResult::Ready || !decoded.frames || decoded.frames > chunk) {
                ctx.mediaDecodeFailed = true;
                return false;
            }
            rs.PushPCM(decoded, 0, static_cast<std::uint32_t>(decoded.frames));
            ring.DeliverFrames(static_cast<std::uint32_t>(decoded.frames));
            return true;
        }
        // Copy complete interleaved frames, including a physical byte wrap.
        // The FIFO owns the samples before their source bytes are released.
        float samples[kResampleChunkFrames * kMaxChannels];
        const auto count = chunk * ring.bytesPerFrame;
        ring.CopyOut(ring.readIndex.load(std::memory_order_relaxed), samples, count);
        PCMView view;
        for (std::uint32_t ch = 0; ch < ring.channelCount; ++ch)
            view.channels[ch] = samples + ch;
        view.channelCount = ring.channelCount;
        view.stride = ring.channelCount;
        view.sampleRate = ctx.pcm.sampleRate;
        view.frames = chunk;
        rs.PushPCM(view, 0, chunk);
        ring.DeliverFrames(chunk);
        ring.ConsumeBytes(count);
        return true;
    }
    if (!ended) {
        return false; // dry but live (as of the pre-span sample): the
                      // starvation gap; the next block retries
    }
    if (!rs.InputEnded()) {
        rs.MarkInputEnd();
        return true;
    }
    if (!rs.Exhausted()) {
        // Look-ahead flush, exactly as for registered PCM.
        const std::uint32_t pad =
            std::min(kResampleChunkFrames, rs.FreeFrames());
        if (pad == 0) {
            return false;
        }
        rs.PushSilence(pad);
        return true;
    }
    return false;
}

struct TailFrameAggregate {
    std::uint32_t total = 0;
    std::uint32_t suffix = 0;
};

enum class FixedToneSection : std::uint8_t {
    Lowpass,
    Highpass,
    LowShelf,
    HighShelf
};

void ProcessFixedToneSection(
    VoiceRenderContext& ctx, FixedToneSection section,
    BiquadTransitionState<kMaxChannels>& transition, bool& enabled,
    BiquadCoeffs& coeffs, BiquadState (&state)[kMaxChannels],
    std::uint32_t& tailFrames, BiquadParameterGlide& glide,
    float* const* channels, std::uint32_t channelCount, std::uint32_t offset,
    std::uint32_t frames) {
    if (!enabled && !transition.transitioning && !glide.active) {
        if (frames != 0) {
            const auto count = std::min(channelCount, kMaxChannels);
            for (std::uint32_t ch = 0; ch < count; ++ch) {
                transition.lastInput[ch] = channels[ch][offset + frames - 1u];
            }
        }
        return;
    }
    if (transition.transitioning || !glide.active ||
        ctx.biquadTrig == nullptr) {
        BiquadProcessSmoothed(transition, enabled, coeffs, state, tailFrames,
                              channels, channelCount, offset, frames);
        return;
    }
    constexpr std::uint32_t kControlFrames = 16;
    std::uint32_t consumed = 0;
    while (consumed < frames) {
        const std::uint32_t controlOffset =
            glide.progress % kControlFrames;
        const std::uint32_t count =
            std::min(kControlFrames - controlOffset, frames - consumed);
        BiquadProcessSmoothed(transition, enabled, coeffs, state, tailFrames,
                              channels, channelCount, offset + consumed,
                              count);
        consumed += count;
        glide.Advance(count);
        const float frequencyHz = glide.FrequencyHz();
        const float q = glide.Q();
        const BiquadTrigPoint trig =
            ctx.biquadTrig->Lookup(static_cast<float>(ctx.engineSampleRate),
                                   frequencyHz);
        BiquadCoeffs candidate;
        switch (section) {
        case FixedToneSection::Lowpass:
            candidate = MakeLowpassFromTrig(trig, q);
            break;
        case FixedToneSection::Highpass:
            candidate = MakeHighpassFromTrig(trig, q);
            break;
        case FixedToneSection::LowShelf:
            candidate =
                MakeLowShelfFromTrig(trig, q, glide.currentGainDb);
            break;
        case FixedToneSection::HighShelf:
            candidate =
                MakeHighShelfFromTrig(trig, q, glide.currentGainDb);
            break;
        }
        if (IsBiquadStable(candidate)) {
            coeffs = candidate;
            tailFrames = EstimateBiquadTailFrames(candidate);
        }
        if (!glide.active &&
            IsBiquadStable(transition.targetCoeffs)) {
            coeffs = transition.targetCoeffs;
            tailFrames = transition.targetTailFrames;
        }
    }
}

void AddTailContribution(TailFrameAggregate& aggregate,
                         std::uint32_t position,
                         std::uint32_t rearmPosition,
                         std::uint32_t frames) {
    aggregate.total = SaturatingTailAdd(aggregate.total, frames);
    if (position >= rearmPosition) {
        aggregate.suffix = SaturatingTailAdd(aggregate.suffix, frames);
    }
}

TailFrameAggregate AggregateTailFrames(const VoiceRenderContext& ctx) {
    TailFrameAggregate aggregate;
    for (std::uint32_t s = 0; s < ctx.fxSlotCount; ++s) {
        const InsertEffect& fx = ctx.fxSlots[s];
        if (fx.process != nullptr && fx.tailFrames != nullptr) {
            AddTailContribution(aggregate, s, ctx.tailRearmPosition,
                                fx.tailFrames(fx.state));
        }
    }
    const std::uint32_t lpfTail = BiquadSectionTailFrames(
        ctx.lpfTransition, ctx.lpfGlide, ctx.lpfEnabled,
        ctx.lpfTailFrames);
    if (lpfTail != 0) {
        AddTailContribution(
            aggregate,
            TailContributionPositionAfterEffects(
                ctx.fxSlotCount, PostEffectTailContribution::Lowpass),
            ctx.tailRearmPosition, lpfTail);
    }
    const std::uint32_t hpfTail = BiquadSectionTailFrames(
        ctx.hpfTransition, ctx.hpfGlide, ctx.hpfEnabled,
        ctx.hpfTailFrames);
    if (hpfTail != 0) {
        AddTailContribution(
            aggregate,
            TailContributionPositionAfterEffects(
                ctx.fxSlotCount, PostEffectTailContribution::Highpass),
            ctx.tailRearmPosition, hpfTail);
    }
    const std::uint32_t shelfTail = BiquadSectionTailFrames(
        ctx.shelfTransition, ctx.shelfGlide, ctx.shelfEnabled,
        ctx.shelfTailFrames);
    if (shelfTail != 0) {
        AddTailContribution(
            aggregate,
            TailContributionPositionAfterEffects(
                ctx.fxSlotCount, PostEffectTailContribution::Shelf),
            ctx.tailRearmPosition, shelfTail);
    }
    const std::uint32_t highShelfTail = BiquadSectionTailFrames(
        ctx.highShelfTransition, ctx.highShelfGlide,
        ctx.highShelfEnabled,
        ctx.highShelfTailFrames);
    if (highShelfTail != 0) {
        AddTailContribution(
            aggregate,
            TailContributionPositionAfterEffects(
                ctx.fxSlotCount, PostEffectTailContribution::HighShelf),
            ctx.tailRearmPosition, highShelfTail);
    }
    return aggregate;
}

void ApplyTimelineControlEvent(VoiceRenderContext& ctx,
                               const VoiceTimelineEvent& event,
                               std::uint32_t blockFrames) noexcept {
    if (event.Kind() == TimelineEventKind::VariableWeight) {
        if (event.Weight().variableWeightOwner.Valid()) {
            (void)ctx.ApplyVariableWeightControl(event.Weight().variableWeightOwner, event.Weight().variableWeight);
            return;
        }
        if (ctx.variableWeightControlCount != 0) return;
        float validated = 0.0f;
        if ((!ctx.variableWeightActive ||
             event.Weight().variableWeight.segmentGeneration >
                 ctx.variableWeightSegment.segmentGeneration) &&
            ResolveWeightSegment(event.Weight().variableWeight,
                                 event.Weight().variableWeight.startFrame,
                                 &validated)) {
            ctx.variableWeightSegment = event.Weight().variableWeight;
            ctx.variableWeightActive = true;
        }
    } else if (event.Kind() == TimelineEventKind::Envelope) {
        const std::uint32_t envelopeIndex =
            event.envelopeDirection == TimelineEnvelopeDirection::FadeOut ? 1u
                                                                          : 0u;
        VoiceRenderContext::TimelineEnvelopeState& envelope =
            ctx.timelineEnvelopes[envelopeIndex];
        envelope.active = event.endFrame > event.frame &&
            event.envelopeElapsedFrames <= UINT64_MAX - (event.endFrame - event.frame);
        envelope.fadeOut = envelopeIndex != 0;
        envelope.useCurve = event.envelopeCurve != nullptr;
        envelope.startFrame = event.frame;
        envelope.endFrame = event.endFrame;
        envelope.elapsedFrames = event.envelopeElapsedFrames;
        if (envelope.useCurve) {
            assert(envelope.curve != nullptr);
            *envelope.curve = *event.envelopeCurve;
        }
        if (envelope.fadeOut) {
            ctx.timelineFadeOutCoveredSample =
                event.endFrame == event.frame;
        }
    } else if (event.Kind() == TimelineEventKind::Stop) {
        if (event.authoredRootStop) {
            ctx.authoredRootStopApplied = true;
            // The root deadline cannot restart an earlier timeline declick.
            if (ctx.timelineStopPending ||
                (ctx.timelineFallbackStopActive && ctx.fading)) return;
        }
        if (!ctx.monitorControlAdopted) {
            ctx.monitorControlAdopted = true;
            ctx.monitorTimelineControlAdopted = true;
            ctx.monitorAdoptedControlCause = event.Stop().controlCause;
        }
        ctx.timelineStopPending = true;
        ctx.timelineStopOffset = event.offset;
        ctx.timelineStopFrame = event.authoredRootStop ? ctx.authoredRootStopFrame :
            ctx.timelineEvents.quantumIndex * blockFrames + event.offset;
        ctx.timelineStopAuthored = event.authoredFadeOutCandidate;
        ctx.timelineStopDeclickFrames = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(kTimelineStopDeclickMs) *
                 ctx.engineSampleRate +
             999u) /
            1000u);
    }
}

bool NeedsBlendScratch(const VoiceRenderContext& ctx) noexcept {
    for (std::uint32_t slot = 0; slot < ctx.fxSlotCount; ++slot) {
        if (ctx.fxSlots[slot].process != nullptr &&
            (ctx.fxBypassRamping[slot] || ctx.fxBypassMix[slot] != 0.0f)) {
            return true;
        }
    }
    return false;
}

bool PreparePlainPcm(const VoiceRenderContext& ctx, std::uint32_t blockFrames,
                     std::uint32_t offset, std::uint32_t& feedFrames) noexcept {
#if HARKEN_STAGE_TAPS_ACTIVE
    return false;
#else
    const auto& rs = ctx.resampler;
    if (offset >= blockFrames || ctx.pcm.channelCount == 0 ||
        ctx.pcm.channelCount > kMaxChannels || ctx.phase != SourcePhase::Active ||
        ctx.streamParkActive || ctx.mediaDecoder != nullptr ||
        ctx.voiceSendActiveCount != 0 || ctx.dynamicVoiceTapActive ||
        ctx.tailStarted || ctx.tailRemaining != 0 ||
        ctx.tailRearmPosition != kNoTailRearmPosition ||
        ctx.lpfEnabled || ctx.lpfTransition.transitioning || ctx.lpfGlide.active ||
        ctx.hpfEnabled || ctx.hpfTransition.transitioning || ctx.hpfGlide.active ||
        ctx.shelfEnabled || ctx.shelfTransition.transitioning || ctx.shelfGlide.active ||
        ctx.highShelfEnabled || ctx.highShelfTransition.transitioning || ctx.highShelfGlide.active ||
        ctx.fading || ctx.timelineStopPending || ctx.timelineFallbackStopActive ||
        ctx.resyncLatched || ctx.resyncFadeInDone < ctx.resyncFadeFrames ||
        ctx.resyncFadeOutTotal != 0 ||
        ctx.authoredRootStopApplied || ctx.timelineEvents.eventCount != 0 ||
        ctx.timelineEnvelopes[0].active || ctx.timelineEnvelopes[1].active ||
        ctx.sourceExits != nullptr ||
        ctx.variableWeightActive || ctx.variableWeightControlCount != 0 ||
        ctx.gain.remainingFrames != 0 || ctx.outputGain.remainingFrames != 0 ||
        (ctx.pcm.channelCount <= 2 && (ctx.spatialMatrixDirty || !ctx.spatialMatrix.Settled())) ||
        rs.resampleProfile != ResampleProfile::LegacyCompatibility ||
        rs.quality != ResampleQuality::Linear || rs.ratio != 1.0 ||
        rs.targetRatio != 1.0 || rs.rampRemaining != 0 || rs.readFraction != 0.0 ||
        rs.inputEnded || rs.highRatioFilterActive || rs.readIndex > rs.writeIndex ||
        rs.writeIndex - rs.readIndex > kResampleFifoFrames) {
        return false;
    }
    for (std::uint32_t slot = 0; slot < ctx.fxSlotCount; ++slot) {
        if (ctx.fxSlots[slot].process != nullptr) return false;
    }
    const std::uint64_t blockStart = ctx.timelineEvents.quantumIndex * blockFrames;
    if (blockFrames > UINT64_MAX - blockStart ||
        ctx.authoredSourceDeadlineFrame <= blockStart + blockFrames ||
        ctx.authoredRootStopFrame < blockStart + blockFrames) return false;

    const std::uint64_t frames = blockFrames - offset;
    const std::uint64_t available = rs.writeIndex - rs.readIndex;
    const std::uint64_t needed = frames + 1 > available ? frames + 1 - available : 0;
    const std::uint64_t extra =
        ((needed + kResampleChunkFrames - 1) / kResampleChunkFrames) * kResampleChunkFrames;
    if (ctx.streamRing != nullptr) {
        // Keep the producer's unread prefix intact until the existing feed
        // helper copies it into the voice-owned FIFO, including byte wraps.
        if (rs.phaseHigh != 0 || rs.phaseLow != 0 ||
            frames > UINT64_MAX - rs.readIndex || extra > rs.FreeFrames() ||
            extra > UINT64_MAX - rs.writeIndex || ctx.streamRing->SourceEnded())
            return false;
        if (ctx.streamRing->BufferedFrames() < extra) return false;
        feedFrames = static_cast<std::uint32_t>(extra);
        return true;
    }
    const std::uint64_t limit = ctx.loopInfinite || ctx.loopPassesRemaining > 1
        ? ctx.loopEndFrame : ctx.pcm.frames;
    // Every feed stays a complete chunk, matching the interleaved source pass.
    if (frames > UINT64_MAX - rs.readIndex || extra > rs.FreeFrames() ||
        extra > UINT64_MAX - rs.writeIndex || ctx.srcCursor >= limit ||
        extra >= limit - ctx.srcCursor) return false;
    feedFrames = static_cast<std::uint32_t>(extra);
    return true;
#endif
}

void CopyPlainPcm(const VoiceRenderContext& ctx, float* const* planes,
                  std::uint32_t offset) {
    const auto span = ctx.cascade.borrowed;
    for (std::uint32_t ch = 0; ch < ctx.pcm.channelCount; ++ch) {
        if (planes[ch] == nullptr) break;
        std::memcpy(planes[ch] + offset, ctx.resampler.ring[ch] + span.ringOffset,
                    span.firstFrames * sizeof(float));
        std::memcpy(planes[ch] + offset + span.firstFrames, ctx.resampler.ring[ch],
                    (span.frames - span.firstFrames) * sizeof(float));
    }
}

bool RunPlainPcm(VoiceRenderContext& ctx, std::uint32_t frames) {
    const auto feedFrames = ctx.cascade.plainPcmFeedFrames;
    if (feedFrames != 0) {
        if (ctx.streamRing != nullptr) {
            // Preparation proved that each complete feed fits both FIFOs.
            // Only the voice-owned FIFO is borrowed; the stream read lease
            // ends inside FeedResamplerFromStream, after its PCM copy.
            for (std::uint32_t fed = 0; fed < feedFrames; fed += kResampleChunkFrames) {
                const bool supplied = FeedResamplerFromStream(ctx);
                assert(supplied);
                (void)supplied;
            }
        } else {
            // Preparation proved that every complete chunk fits before the next
            // source boundary. One push preserves the same ring contents and cursors.
            ctx.resampler.PushPCM(ctx.pcm, ctx.srcCursor, feedFrames);
            ctx.srcCursor += feedFrames;
        }
    }
    if (!ctx.resampler.BorrowLinearUnity(frames, ctx.cascade.borrowed)) return false;
    ctx.monitorPreChainFrames = 0;
    if (ctx.monitorPreChain[0] != nullptr && harken::monitor::AudioLiveArmed()) {
        CopyPlainPcm(ctx, ctx.monitorPreChain, 0);
        ctx.monitorPreChainFrames = frames;
    }
    const auto last = (ctx.cascade.borrowed.ringOffset + frames - 1) &
                      (kResampleFifoFrames - 1);
    for (std::uint32_t ch = 0; ch < ctx.pcm.channelCount; ++ch) {
        const float input = ctx.resampler.ring[ch][last];
        ctx.lpfTransition.lastInput[ch] = input;
        ctx.hpfTransition.lastInput[ch] = input;
        ctx.shelfTransition.lastInput[ch] = input;
        ctx.highShelfTransition.lastInput[ch] = input;
    }
    ctx.logicalSourceOutputFramesThisBlock = frames;
    if (ctx.streamRing != nullptr) ctx.starvedThisBlock = false;
    ctx.cascade.produced = frames;
    ctx.phase = SourcePhase::Active;
    ctx.lastResult = {frames, SourcePhase::Active};
    return true;
}

void MaterializePlainPcm(VoiceRenderContext& ctx) {
    const auto span = ctx.cascade.borrowed;
    CopyPlainPcm(ctx, ctx.cascade.scratch, ctx.cascade.offset);
    output_gain_detail::MulConstant(ctx.cascade.scratch, ctx.pcm.channelCount,
                                    ctx.cascade.offset, span.frames, ctx.gain.target);
    ctx.cascade.borrowed = {};
}

void MixPlainPcm(const float* src, float* dst, std::uint32_t frames,
                 float voiceGain, float scale) noexcept {
    if (voiceGain == 1.0f) {
        output_gain_detail::MixScaled(src, dst, frames, scale);
        return;
    }
    std::uint32_t i = 0;
#if defined(HARKEN_OUTPUT_GAIN_SSE2)
    const __m128 gain = _mm_set1_ps(voiceGain);
    const __m128 output = _mm_set1_ps(scale);
    for (; i + 4 <= frames; i += 4) {
        const __m128 value = _mm_mul_ps(_mm_loadu_ps(src + i), gain);
        _mm_storeu_ps(dst + i, _mm_add_ps(_mm_loadu_ps(dst + i),
                                       _mm_mul_ps(value, output)));
    }
#endif
    for (; i < frames; ++i) {
        const float value = src[i] * voiceGain;
        dst[i] += value * scale;
    }
}

bool CanCombineSpatialOutput(const VoiceRenderContext& ctx,
                             std::uint32_t mixChannels) noexcept {
    for (std::uint32_t source = 0; source < ctx.pcm.channelCount; ++source)
        for (std::uint32_t ch = 0; ch < mixChannels; ++ch)
            if (!output_gain_detail::CanCombineMixGains(
                    ctx.spatialMatrix.target.gain[source][ch], ctx.outputGain.target))
                return false;
    return true;
}

void FoldPlainPcm(VoiceRenderContext& ctx, float* const* busPlanes,
                  std::uint32_t mixChannels,
                  output_gain_detail::MonoStereoMixKernel monoStereoMix) {
    const auto span = ctx.cascade.borrowed;
    float* const* bus = busPlanes + ctx.busIndex * mixChannels;
    if (ctx.pcm.channelCount == 1 && mixChannels == 2 &&
        ctx.spatialMatrix.target.gain[0][0] != 0.0f &&
        ctx.spatialMatrix.target.gain[0][1] != 0.0f) {
        const float leftScale = ctx.spatialMatrix.target.gain[0][0] * ctx.outputGain.target;
        const float rightScale = ctx.spatialMatrix.target.gain[0][1] * ctx.outputGain.target;
        float* left = bus[0] + ctx.cascade.offset;
        float* right = bus[1] + ctx.cascade.offset;
        monoStereoMix(ctx.resampler.ring[0] + span.ringOffset,
            left, right, span.firstFrames, ctx.gain.target, leftScale, rightScale);
        if (span.frames > span.firstFrames) {
            monoStereoMix(ctx.resampler.ring[0],
                left + span.firstFrames, right + span.firstFrames, span.frames - span.firstFrames,
                ctx.gain.target, leftScale, rightScale);
        }
    } else {
        for (std::uint32_t ch = 0; ch < mixChannels; ++ch) {
            const auto mix = [&](std::uint32_t source, float matrixGain) {
                if (source >= ctx.pcm.channelCount) return;
                const float scale = matrixGain * ctx.outputGain.target;
                float* dst = bus[ch] + ctx.cascade.offset;
                MixPlainPcm(ctx.resampler.ring[source] + span.ringOffset, dst,
                             span.firstFrames, ctx.gain.target, scale);
                MixPlainPcm(ctx.resampler.ring[source], dst + span.firstFrames,
                             span.frames - span.firstFrames, ctx.gain.target, scale);
            };
            const float a = ctx.spatialMatrix.target.gain[0][ch];
            const float b = ctx.spatialMatrix.target.gain[1][ch];
            if (b == 0.0f) {
                if (a != 0.0f) mix(0, a);
            } else if (a == 0.0f) {
                mix(1, b);
            } else {
                mix(0, a);
                mix(1, b);
            }
        }
    }
    ctx.spatialMatrixHasRendered = true;
    ctx.outputGain.Advance(span.frames);
}

void ApplyPendingTailRearm(VoiceRenderContext& ctx,
                           const TailFrameAggregate& aggregate) {
    if (ctx.tailRearmPosition == kNoTailRearmPosition) {
        return;
    }
    if (aggregate.suffix == kInfiniteTail) {
        // Restart only when this batch's updates newly made the suffix
        // unbounded. A suffix that was already infinite before the update
        // (e.g. an unrelated occupied slot whose tail never stops) is not a
        // fresh event: elapsed keeps accumulating through the ordinary
        // infinite-tail path below instead of restarting from zero.
        if (!ctx.tailRearmSuffixWasInfinite) {
            ctx.tailElapsed = 0;
        }
    } else if (aggregate.total != kInfiniteTail) {
        const std::uint32_t prefix =
            (aggregate.total > aggregate.suffix)
                ? (aggregate.total - aggregate.suffix)
                : 0u;
        ctx.tailElapsed = std::min(ctx.tailElapsed, prefix);
    }
    ctx.tailRearmPosition = kNoTailRearmPosition;
    ctx.tailRearmSuffixWasInfinite = false;
}

} // namespace

std::uint64_t VoiceCascadeScratchBytes(
    const VoiceRenderContext& ctx, std::uint32_t blockFrames,
    std::uint32_t mixChannels) noexcept {
    const bool needsBlendScratch = NeedsBlendScratch(ctx);
    const std::uint32_t planes = ctx.pcm.channelCount + std::max(
        needsBlendScratch ? ctx.pcm.channelCount : 0u,
        (ctx.voiceSendActiveCount > 0 || ctx.signalMemberActive) ? mixChannels : 0u);
    return static_cast<std::uint64_t>(planes) * blockFrames * sizeof(float);
}

bool PrepareVoiceCascade(VoiceRenderContext& ctx, BlockArena& arena,
                         std::uint32_t blockFrames, std::uint32_t mixChannels,
                         bool readOnlySignalImage, std::uint32_t mixLayout) {
    const std::uint32_t channels = ctx.pcm.channelCount;
    ctx.cascade.runnable = false;
    ctx.cascade.signalImageReady = false;
    ctx.cascade.signalImageReadOnly = readOnlySignalImage;
    ctx.cascade.borrowed = {};
    ctx.cascade.plainPcmCandidate = false;
    ctx.logicalSourceOutputFramesThisBlock = 0;
    ctx.stagedSourceTerminal = SourceTerminalKind::None;
    ctx.stagedSourceTerminalOffset = 0;
    ctx.stagedPlaybackEnd = SourceTerminalKind::None;
    ctx.stagedPlaybackEndOffset = 0;
    ctx.monitorStoppedBeforeCascade = false;

    const VoiceEventSpan& timelineSpan = ctx.timelineEvents;

    const auto rootCutoff = SelectAuthoredRootStop(
        timelineSpan, blockFrames, ctx.authoredRootStopFrame,
        ctx.authoredRootStopControl.Stop().authoredStopOperation);
    ctx.authoredRootStopFrame = rootCutoff.frame;
    ctx.authoredRootStopEvent = rootCutoff.eventIndex;
    if (rootCutoff.eventIndex != UINT32_MAX) {
        ctx.authoredRootStopControl = timelineSpan.events[rootCutoff.eventIndex];
    }
    std::uint32_t earliestStart = blockFrames;
    for (std::uint32_t i = 0; i < timelineSpan.eventCount; ++i) {
        if (!TimelineEventGuardEligible(timelineSpan, i)) continue;
        const auto& event = timelineSpan.events[i];
        if (event.offset >= blockFrames) continue;
        if (event.Kind() == TimelineEventKind::Start) {
            earliestStart = std::min<std::uint32_t>(earliestStart, event.offset);
        }
    }
    const auto quantumStart = timelineSpan.quantumIndex * blockFrames;
    const bool authoredDeadlineArrives =
        ctx.authoredSourceDeadlineFrame != UINT64_MAX &&
        ctx.authoredSourceDeadlineFrame < quantumStart + blockFrames;
    const bool rootStopArrived = ctx.authoredRootStopFrame < quantumStart + blockFrames;
    const bool prebufferWaiting = ctx.streamRing != nullptr && !ctx.streamGateOpen &&
        ctx.streamRing->BufferedFrames() < ctx.streamRing->PrebufferTarget(ctx.prebufferTargetFrames) &&
        !ctx.streamRing->SourceEnded();
    if (rootStopArrived && (prebufferWaiting ||
        (ctx.timelineGated && ctx.authoredRootStopFrame <= quantumStart + earliestStart))) {
        ctx.authoredRootStopApplied = true;
        if (!ctx.monitorControlAdopted) {
            ctx.monitorControlAdopted = true;
            ctx.monitorTimelineControlAdopted = true;
            ctx.monitorAdoptedControlCause = ctx.authoredRootStopControl.Stop().controlCause;
        }
        ctx.stagedSourceTerminal = SourceTerminalKind::Stop;
        ctx.stagedSourceTerminalOffset = static_cast<std::uint32_t>(
            ctx.authoredRootStopFrame > quantumStart ? ctx.authoredRootStopFrame - quantumStart : 0);
        ctx.stagedPlaybackEnd = SourceTerminalKind::Stop;
        ctx.stagedPlaybackEndOffset = ctx.stagedSourceTerminalOffset;
        ctx.phase = SourcePhase::Done;
        ctx.starvedThisBlock = false;
        ctx.lastResult = StageResult{0, SourcePhase::Done};
        return false;
    }

    // Prebuffer gate for streamed voices: until the ring holds the target
    // (or the source already ended — a short file can never reach it), the
    // voice contributes nothing and no state advances, so the gated span
    // behaves exactly like blocks before the play command applied. The
    // sample-accurate start offset is therefore still pending when the
    // gate opens. Underruns after the gate opens are starvation, never a
    // return to Preparing.
    if (ctx.streamRing != nullptr && !ctx.streamGateOpen) {
        if (ctx.streamRing->BufferedFrames() >= ctx.streamRing->PrebufferTarget(ctx.prebufferTargetFrames) ||
            ctx.streamRing->SourceEnded() || authoredDeadlineArrives) {
            ctx.streamGateOpen = true;
        } else {
            ctx.starvedThisBlock = false;
#if HARKEN_STAGE_TAPS_ACTIVE
            ctx.tapPostResample = 0.0f;
            ctx.tapPostFx = 0.0f;
            ctx.tapPostFilter = 0.0f;
            ctx.tapPostGain = 0.0f;
#endif
            ctx.lastResult = StageResult{0, ctx.phase};
            return false;
        }
    }

    std::uint32_t offset =
        ctx.firstBlock ? std::min(ctx.startOffsetFrames, blockFrames) : 0;
    if (!ctx.authoredRootStopApplied && ctx.authoredRootStopFrame < quantumStart) {
        auto stop = ctx.authoredRootStopControl;
        stop.offset = static_cast<std::uint16_t>(offset);
        ApplyTimelineControlEvent(ctx, stop, blockFrames);
    }
    if (ctx.authoredRootStopApplied && ctx.timelineStopPending) {
        ctx.timelineStopOffset = ctx.timelineStopFrame > quantumStart
            ? static_cast<std::uint32_t>(ctx.timelineStopFrame - quantumStart) : 0;
    }
    const auto pendingStopFrame = ctx.timelineFallbackStopActive && ctx.fading
        ? ctx.timelineFallbackStopFrame : ctx.timelineStopFrame;
    if (ctx.authoredRootStopApplied && pendingStopFrame < quantumStart &&
        quantumStart - pendingStopFrame >= ctx.timelineStopDeclickFrames) {
        ctx.monitorStoppedBeforeCascade = true;
        ctx.stagedPlaybackEnd = SourceTerminalKind::Stop;
        ctx.phase = SourcePhase::Done;
        ctx.starvedThisBlock = false;
        ctx.lastResult = StageResult{0, SourcePhase::Done};
        return false;
    }
    bool started = !ctx.timelineGated;
    for (std::uint32_t i = 0; i < timelineSpan.eventCount; ++i) {
        const VoiceTimelineEvent& event = timelineSpan.events[i];
        if (!TimelineEventPrecedesRootStop(ctx, i, blockFrames)) continue;
        if (event.Kind() == TimelineEventKind::SetStartCursor) {
            if (ctx.timelineGated) {
                ctx.srcCursor = event.value;
            }
            continue;
        }
        if (event.Kind() == TimelineEventKind::Start) {
            if (ctx.timelineGated) {
                offset = std::min<std::uint32_t>(event.offset, blockFrames);
                if (ctx.pendingImmediateWeightRebase) {
                    const std::uint64_t actualStart = quantumStart + offset;
                    const std::uint64_t originalStart =
                        ctx.pendingImmediateWeightBaseFrame;
                    const bool moveForward = actualStart >= originalStart;
                    const std::uint64_t shift = moveForward
                        ? actualStart - originalStart
                        : originalStart - actualStart;
                    for (std::uint32_t weight = 0;
                         weight < ctx.pendingImmediateWeightCount; ++weight) {
                        if (weight >= ctx.variableWeightControlCount) break;
                        auto& binding = ctx.variableWeightControls[weight];
                        if (binding.segment.segmentGeneration != binding.initialRebaseGeneration) continue;
                        auto& segment = binding.segment;
                        const bool safe = moveForward
                            ? segment.startFrame <= UINT64_MAX - shift &&
                                segment.endFrame <= UINT64_MAX - shift
                            : segment.startFrame >= shift &&
                                segment.endFrame >= shift;
                        assert(safe);
                        if (safe) {
                            segment.startFrame = moveForward
                                ? segment.startFrame + shift : segment.startFrame - shift;
                            segment.endFrame = moveForward
                                ? segment.endFrame + shift : segment.endFrame - shift;
                        }
                    }
                    ctx.pendingImmediateWeightCount = 0;
                    ctx.pendingImmediateWeightRebase = false;
                }
                if (ctx.pendingAuthoredDurationFrames != UINT64_MAX) {
                    const std::uint64_t actualStart = quantumStart + offset;
                    // The logic-side reservation checked this addition before
                    // publishing the Start event.
                    if (ctx.pendingAuthoredDurationFrames <=
                        UINT64_MAX - actualStart) {
                        ctx.authoredSourceDeadlineFrame =
                            actualStart + ctx.pendingAuthoredDurationFrames;
                    }
                    ctx.pendingAuthoredDurationFrames = UINT64_MAX;
                }
                ctx.timelineGated = false;
                started = true;
            }
            continue;
        }
        if ((event.Kind() == TimelineEventKind::Envelope ||
             event.Kind() == TimelineEventKind::VariableWeight ||
             event.Kind() == TimelineEventKind::Stop) &&
            event.offset == offset) {
            ApplyTimelineControlEvent(ctx, event, blockFrames);
        }
    }
    if (!started) {
        ctx.lastResult = StageResult{0, ctx.phase};
        return false;
    }
    // Scratch for the whole cascade comes from the per-block arena; stages
    // own no buffers. Exhaustion degrades to "this voice is silent this
    // block" with zero state advance: no render-state field is touched, so
    // the next block retries identically (sample-accurate start included);
    // only the observational taps update, to report the silence. The
    // arena's overflow counter records the event.
    if (ctx.resyncOnStarve && !ctx.resyncLatched && ctx.resyncFadeOutTotal == 0 &&
        ctx.streamRing != nullptr && ctx.streamGateOpen && !ctx.streamRing->SourceEnded()) {
        const double ratio = ctx.resampler.CurrentRatio();
        const double buffered = static_cast<double>(ctx.streamRing->BufferedFrames()) +
            static_cast<double>(ctx.resampler.writeIndex - ctx.resampler.readIndex);
        const double available = ratio > 0.0 ? buffered / ratio : 0.0;
        // A just-opened prebuffer gate must not read as an imminent underrun.
        const std::uint32_t fade = std::max(1u, std::min(ctx.resyncFadeFrames, ctx.prebufferTargetFrames / 2u));
        // Blocks drain whole, so the decision is taken one block early.
        if (available < static_cast<double>(2u * blockFrames + fade)) {
            ctx.resyncFadeOutTotal = static_cast<std::uint32_t>(std::clamp(
                available - static_cast<double>(kResampleHistoryPad), 1.0,
                static_cast<double>(fade)));
            ctx.resyncFadeOutDone = 0;
        }
    }
    ctx.cascade.plainPcmCandidate =
        PreparePlainPcm(ctx, blockFrames, offset, ctx.cascade.plainPcmFeedFrames);
    float* scratch[kMaxChannels] = {};
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        scratch[ch] = ctx.cascade.plainPcmCandidate
            ? arena.ReserveFloatsHigh(blockFrames) : arena.AllocFloatsHigh(blockFrames);
        if (scratch[ch] == nullptr) {
            // Like the taps, the starvation flag is a per-block
            // observation, not render state: a degrade-silence block saw
            // no ring shortfall, so it must not re-report the previous
            // block's fact.
            ctx.starvedThisBlock = false;
#if HARKEN_STAGE_TAPS_ACTIVE
            // The voice contributes nothing this block; taps report that
            // silence honestly instead of holding the previous block's
            // values. (Render state stays untouched so the next block
            // retries identically — taps are observation, not state.)
            ctx.tapPostResample = 0.0f;
            ctx.tapPostFx = 0.0f;
            ctx.tapPostFilter = 0.0f;
            ctx.tapPostGain = 0.0f;
#endif
            ctx.lastResult = StageResult{0, ctx.phase};
            return false;
        }
    }

    // A blending slot (bypassed, or crossfading this block) needs the slot
    // input preserved as the dry signal while process overwrites the scratch
    // with the wet result. One reusable dry plane per channel serves every
    // such slot in turn. Allocated here, before any state advances, so an
    // arena shortfall degrades exactly like the scratch shortfall above:
    // silent block, no state touched, identical retry next block.
    const bool needsBlendScratch = NeedsBlendScratch(ctx);
    float* dry[kMaxChannels] = {};
    if (needsBlendScratch) {
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            dry[ch] = arena.AllocFloatsHigh(blockFrames);
            if (dry[ch] == nullptr) {
                ctx.starvedThisBlock = false;
#if HARKEN_STAGE_TAPS_ACTIVE
                ctx.tapPostResample = 0.0f;
                ctx.tapPostFx = 0.0f;
                ctx.tapPostFilter = 0.0f;
                ctx.tapPostGain = 0.0f;
#endif
                ctx.lastResult = StageResult{0, ctx.phase};
                return false;
            }
        }
    }
    // This voice's blend planes are dead when Fold starts, after Run completes.
    // Fold fills every produced tap sample before scattering, so these planes
    // can also hold its send image. Only extra mix channels need new planes.
    // Reserve in fixed slot/channel order before any worker runs, including
    // active sends that will produce zero frames. Reservation and tight-arena
    // degradation therefore remain independent of worker completion order.
    //
    // A tap shortfall does NOT decline the voice: it only drops this block's
    // sends (the pre-split fold's "partial reserve consumed, sends dropped"
    // semantics), so tapReady gates the scatter, never the cascade.
    float* tap[kMaxChannels] = {};
    bool tapReady = false;
    if (ctx.voiceSendActiveCount > 0 || ctx.signalMemberActive) {
        tapReady = true;
        const bool nativeImage = readOnlySignalImage && channels > 2 && mixChannels > 2 &&
            LayoutChannelCount(mixLayout) == mixChannels;
        int imageSources[kMaxChannels];
        if (nativeImage) MapNativeImageChannels(channels, mixChannels, mixLayout, imageSources);
        for (std::uint32_t ch = 0; ch < mixChannels; ++ch) {
            if (nativeImage && imageSources[ch] >= 0) {
                tap[ch] = scratch[imageSources[ch]];
                continue;
            }
            if (readOnlySignalImage && channels <= 2 && !ctx.spatialMatrixDirty && ctx.spatialMatrix.Settled()) {
                for (std::uint32_t prior = 0; prior < ch; ++prior)
                    if (std::bit_cast<std::uint32_t>(ctx.spatialMatrix.target.gain[0][prior]) ==
                            std::bit_cast<std::uint32_t>(ctx.spatialMatrix.target.gain[0][ch]) &&
                        (channels == 1 || std::bit_cast<std::uint32_t>(ctx.spatialMatrix.target.gain[1][prior]) ==
                            std::bit_cast<std::uint32_t>(ctx.spatialMatrix.target.gain[1][ch]))) {
                        tap[ch] = tap[prior];
                        break;
                    }
                if (tap[ch] != nullptr) continue;
            }
            tap[ch] = needsBlendScratch && ch < channels
                ? dry[ch] : arena.ReserveFloatsHigh(blockFrames);
            if (tap[ch] == nullptr) {
                tapReady = false;
                break;
            }
        }
    }
    if (ctx.signalMemberActive && !tapReady) {
        // Collective receiving cannot silently bypass a member whose image
        // could not be reserved. Keep this block silent and retry next time.
        ctx.starvedThisBlock = false;
#if HARKEN_STAGE_TAPS_ACTIVE
        ctx.tapPostResample = 0.0f;
        ctx.tapPostFx = 0.0f;
        ctx.tapPostFilter = 0.0f;
        ctx.tapPostGain = 0.0f;
#endif
        ctx.lastResult = StageResult{0, ctx.phase};
        return false;
    }
    ctx.firstBlock = false;

    // Scratch always includes nulls beyond the source channels. Optional planes
    // are read only when this block needs blending or has a complete send tap;
    // otherwise their old non-owning pointers remain unread. Re-enabling either
    // path republishes its full array before any worker or fold can consume it.
    std::copy_n(scratch, kMaxChannels, ctx.cascade.scratch);
    if (needsBlendScratch) std::copy_n(dry, kMaxChannels, ctx.cascade.dry);
    if (tapReady) std::copy_n(tap, kMaxChannels, ctx.cascade.tap);
    ctx.cascade.needBlend = needsBlendScratch;
    ctx.cascade.tapReady = tapReady;
    ctx.cascade.offset = offset;
    ctx.cascade.produced = 0;
    ctx.cascade.runnable = true;
    return true;
}

void RunVoiceCascade(VoiceRenderContext& ctx, std::uint32_t blockFrames) {
    const std::uint32_t channels = ctx.pcm.channelCount;
    const VoiceEventSpan& timelineSpan = ctx.timelineEvents;
    const std::uint32_t offset = ctx.cascade.offset;
    std::uint32_t target = blockFrames - offset;
    if (ctx.streamParkActive && ctx.fading) {
        target = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            target, ctx.fadeTotalFrames - ctx.fadeDoneFrames));
    }
    std::uint32_t sourceTarget = target;
    const std::uint64_t sourceBlockStart =
        timelineSpan.quantumIndex * blockFrames + offset;
    if (ctx.authoredSourceDeadlineFrame != UINT64_MAX) {
        if (ctx.authoredSourceDeadlineFrame <= sourceBlockStart) {
            sourceTarget = 0;
        } else {
            sourceTarget = std::min<std::uint32_t>(
                sourceTarget,
                static_cast<std::uint32_t>(std::min<std::uint64_t>(
                    ctx.authoredSourceDeadlineFrame - sourceBlockStart,
                    UINT32_MAX)));
        }
    }
    float** scratch = ctx.cascade.scratch;
    float** dry = ctx.cascade.dry;

    if (ctx.cascade.plainPcmCandidate) {
        if (RunPlainPcm(ctx, sourceTarget)) return;
        for (std::uint32_t ch = 0; ch < channels; ++ch)
            std::memset(scratch[ch], 0, blockFrames * sizeof(float));
    }

    // --- source decode + resample (single forward pass) ---
    std::uint32_t produced = 0;
    // A latched resync voice renders silence for its moving timeline until a
    // prepared ring switches in, or until its clip would have ended.
    std::uint32_t resyncSwitchAt = UINT32_MAX;
    bool resyncEnded = false;
    const auto resyncTerminal = ctx.resyncLatched && ctx.streamRing != nullptr
        ? ctx.streamRing->TerminalReason() : StreamTerminalReason::None;
    const bool resyncFailed = resyncTerminal == StreamTerminalReason::ProducerFailure ||
                              resyncTerminal == StreamTerminalReason::IntegrityAbort;
    if (ctx.resyncLatched) {
        std::uint32_t silent = sourceTarget;
        if (resyncFailed) {
            silent = 0;
        } else if (ctx.resyncSwitchFrame != UINT64_MAX && ctx.resyncSwitchFrame < sourceBlockStart) {
            ctx.resyncSwitchFrame = UINT64_MAX;
            ctx.resyncLatchedThisBlock = true;
        } else if (ctx.resyncSwitchFrame != UINT64_MAX &&
                   ctx.resyncSwitchFrame - sourceBlockStart < sourceTarget) {
            silent = static_cast<std::uint32_t>(ctx.resyncSwitchFrame - sourceBlockStart);
            resyncSwitchAt = silent;
            ctx.resyncSwitchFrame = UINT64_MAX;
            ctx.resyncLatched = false;
            ctx.resyncFadeInDone = 0;
        } else if (ctx.resyncEndFrame < sourceBlockStart + sourceTarget) {
            silent = ctx.resyncEndFrame > sourceBlockStart
                ? static_cast<std::uint32_t>(ctx.resyncEndFrame - sourceBlockStart) : 0u;
            resyncEnded = true;
        }
        for (std::uint32_t ch = 0; ch < channels; ++ch)
            std::fill_n(scratch[ch] + offset, silent, 0.0f);
        produced = silent;
    }
    while (!ctx.resyncLatched && produced < sourceTarget) {
        produced += ctx.resampler.Produce(scratch, offset + produced,
                                          sourceTarget - produced);
        if (produced >= sourceTarget) {
            break;
        }
        const bool fed = (ctx.streamRing != nullptr)
                             ? FeedResamplerFromStream(ctx)
                             : FeedResampler(ctx);
        if (!fed) {
            break;
        }
    }
    if (ctx.mediaDecodeFailed) {
        for (std::uint32_t channel = 0; channel < channels; ++channel)
            std::fill_n(scratch[channel] + offset, sourceTarget, 0.0f);
        produced = 0;
    }
    ctx.logicalSourceOutputFramesThisBlock = produced;
    if (ctx.resyncFadeOutTotal != 0 && !ctx.resyncLatched &&
        ctx.streamRing != nullptr &&
        ctx.streamRing->TerminalReason() == StreamTerminalReason::NaturalEnd) {
        // The producer finished filling the ring, not playing its buffered tail.
        // Reverse a preventive fade from its last gain using the existing ramp.
        if (ctx.resyncFadeOutDone != 0) {
            const auto numerator = ctx.resyncFadeOutTotal -
                std::min(ctx.resyncFadeOutDone - 1u, ctx.resyncFadeOutTotal);
            ctx.resyncFadeInDone = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(numerator) * ctx.resyncFadeInDone /
                (static_cast<std::uint64_t>(ctx.resyncFadeOutTotal) + 1u));
        }
        ctx.resyncFadeOutTotal = 0;
    }
    if (ctx.resyncFadeInDone < ctx.resyncFadeFrames && !ctx.resyncLatched) {
        const std::uint32_t begin = resyncSwitchAt == UINT32_MAX ? 0u : resyncSwitchAt;
        const std::uint32_t count = std::min(produced - std::min(produced, begin),
            ctx.resyncFadeFrames - ctx.resyncFadeInDone);
        const float step = 1.0f / static_cast<float>(ctx.resyncFadeFrames);
        for (std::uint32_t ch = 0; ch < channels; ++ch)
            for (std::uint32_t i = 0; i < count; ++i)
                scratch[ch][offset + begin + i] *= static_cast<float>(ctx.resyncFadeInDone + i + 1u) * step;
        ctx.resyncFadeInDone += count;
    }
    const std::uint32_t dataBegin = resyncSwitchAt == UINT32_MAX ? 0u : resyncSwitchAt;
    if (ctx.resyncFadeOutTotal != 0 && !ctx.resyncLatched) {
        const float step = 1.0f / static_cast<float>(ctx.resyncFadeOutTotal + 1u);
        for (std::uint32_t i = dataBegin; i < produced; ++i) {
            const std::uint32_t k = ctx.resyncFadeOutDone + (i - dataBegin);
            const float gain = k >= ctx.resyncFadeOutTotal ? 0.0f
                : static_cast<float>(ctx.resyncFadeOutTotal - k) * step;
            for (std::uint32_t ch = 0; ch < channels; ++ch) scratch[ch][offset + i] *= gain;
        }
        ctx.resyncFadeOutDone += produced - std::min(produced, dataBegin);
    }
    const bool resyncFadedOut = ctx.resyncFadeOutTotal != 0 &&
        ctx.resyncFadeOutDone >= ctx.resyncFadeOutTotal;
    if (ctx.resyncOnStarve && !ctx.resyncLatched && ctx.streamRing != nullptr &&
        ctx.streamGateOpen && (produced < sourceTarget || resyncFadedOut) &&
        !ctx.streamRing->SourceEnded() && !ctx.resampler.Exhausted()) {
        // The source is still live but the ring cannot keep up: fade out,
        // latch, and pin where the timeline would find the source.
        if (ctx.resyncFadeOutTotal == 0) {
            const std::uint32_t fade = std::min(produced - std::min(produced, dataBegin),
                ctx.resyncFadeFrames);
            const float step = 1.0f / static_cast<float>(fade + 1u);
            for (std::uint32_t ch = 0; ch < channels; ++ch)
                for (std::uint32_t i = 0; i < fade; ++i)
                    scratch[ch][offset + produced - fade + i] *= static_cast<float>(fade - i) * step;
        }
        ctx.resyncFadeOutTotal = 0;
        const std::uint64_t local = ctx.resampler.SourceFrame();
        const std::uint64_t advance = local > kResampleHistoryPad ? local - kResampleHistoryPad : 0u;
        const std::uint64_t source = std::min(ctx.streamFrameCount,
            ctx.streamActivationStartFrame + std::min(advance, ctx.streamFrameCount));
        const double ratio = ctx.resampler.CurrentRatio();
        ctx.resyncRatio = ratio;
        ctx.resyncMappable = ratio > 0.0 && ratio == ctx.resampler.TargetRatio() &&
            ctx.resampler.RampFramesRemaining() == 0;
        ctx.resyncTimelineAnchor = sourceBlockStart + produced;
        ctx.resyncSourceAnchor = source;
        const double remaining = ratio > 0.0
            ? std::ceil(static_cast<double>(ctx.streamFrameCount - source) / ratio) : 0.0;
        ctx.resyncEndFrame = ctx.resyncTimelineAnchor + static_cast<std::uint64_t>(remaining);
        for (std::uint32_t ch = 0; ch < channels; ++ch)
            std::fill_n(scratch[ch] + offset + produced, sourceTarget - produced, 0.0f);
        produced = sourceTarget;
        // The silence stands in for source time, so progress follows the timeline.
        ctx.logicalSourceOutputFramesThisBlock = produced;
        ctx.resyncLatched = true;
        ctx.resyncLatchedThisBlock = true;
    }
    const bool sourceExhausted = ctx.resampler.Exhausted();
    const bool deadlineFallsInRenderedSpan =
        ctx.authoredSourceDeadlineFrame != UINT64_MAX &&
        ctx.authoredSourceDeadlineFrame <= sourceBlockStart + sourceTarget;
    const bool authoredDeadlineReached =
        deadlineFallsInRenderedSpan &&
        (!sourceExhausted || produced == sourceTarget);
    if (authoredDeadlineReached && produced < sourceTarget) {
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            std::fill_n(scratch[channel] + offset + produced,
                        sourceTarget - produced, 0.0f);
        }
        produced = sourceTarget;
    }
    const auto sourceBoundary = offset + produced;
    const bool sourceEnded = sourceExhausted || authoredDeadlineReached || resyncEnded || resyncFailed || ctx.mediaDecodeFailed;
    auto sourceResult = SourceTerminalKind::None;
    if (sourceEnded) {
        sourceResult = SourceTerminalKind::Natural;
        if ((!authoredDeadlineReached || resyncFailed) && !resyncEnded && ctx.streamRing != nullptr) {
            switch (ctx.streamRing->TerminalReason()) {
            case StreamTerminalReason::NaturalEnd:
                if (ctx.sourceTerminal != nullptr && ctx.sourceTerminalStreamGeneration !=
                        ctx.streamRing->generation.load(std::memory_order_acquire))
                    sourceResult = SourceTerminalKind::IntegrityAbort;
                break;
            case StreamTerminalReason::ProducerFailure:
                sourceResult = SourceTerminalKind::ProducerFailure; break;
            case StreamTerminalReason::IntegrityAbort:
                sourceResult = SourceTerminalKind::IntegrityAbort; break;
            default: sourceResult = SourceTerminalKind::None; break;
            }
        }
    }
    if (ctx.mediaDecodeFailed) sourceResult = SourceTerminalKind::IntegrityAbort;
    std::uint32_t stopCutoff = UINT32_MAX;

#if HARKEN_STAGE_TAPS_ACTIVE
    // Source decode and resample run as one fused forward pass, so this
    // single boundary observes both.
    ctx.tapPostResample = StageTapPeak(scratch, channels, offset, produced);
#endif

    ctx.monitorPreChainFrames = 0;
    if (produced > 0 && ctx.monitorPreChain[0] != nullptr &&
        harken::monitor::AudioLiveArmed()) {
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            if (ctx.monitorPreChain[ch] == nullptr) {
                break;
            }
            std::memcpy(ctx.monitorPreChain[ch], scratch[ch] + offset,
                        static_cast<std::size_t>(produced) * sizeof(float));
        }
        ctx.monitorPreChainFrames = produced;
    }

    // --- lifecycle + tail drain (data, not control flow) ---
    SourcePhase phase = SourcePhase::Active;
    if (sourceEnded) {
        // Aggregate tail is the serial sum of every occupied slot's report,
        // regardless of bypass state (a bypassed effect can still ring). It is
        // recomputed every drain block from the slots still occupied, so
        // clearing or replacing an effect changes the remaining life instead
        // of freezing the value captured when drain began. Empty slots report
        // nothing; an infinite contribution dominates the finite sum.
        const TailFrameAggregate aggregate = AggregateTailFrames(ctx);
        ApplyPendingTailRearm(ctx, aggregate);
        const std::uint32_t aggregateTail = aggregate.total;
        ctx.tailStarted = true;
        if (aggregateTail == kInfiniteTail) {
            // Infinite aggregate tail: fill the rest of the block with
            // silence-driven processing (the chain keeps ringing) and never
            // auto-complete. The drained frames are still counted (saturating,
            // so a very long drain cannot wrap) so that if the infinite slot is
            // later cleared, the surviving finite tail resumes from the true
            // elapsed position rather than restarting from zero. The voice ends
            // only on a Stop (an applied cut, or a fade ramp reaching zero
            // below), or when the infinite slot goes away and the remaining
            // finite tail drains.
            if (produced < target) {
                const std::uint64_t elapsed =
                    static_cast<std::uint64_t>(ctx.tailElapsed) +
                    (target - produced);
                ctx.tailElapsed =
                    (elapsed >= static_cast<std::uint64_t>(kInfiniteTail))
                        ? (kInfiniteTail - 1u)
                        : static_cast<std::uint32_t>(elapsed);
                produced = target;
            }
            ctx.tailRemaining = kInfiniteTail;
            phase = SourcePhase::Draining;
        } else {
            // Frames of aggregate tail still unheard = the serial aggregate
            // minus what has already drained since the source ended.
            std::uint32_t remaining =
                (aggregateTail > ctx.tailElapsed)
                    ? (aggregateTail - ctx.tailElapsed)
                    : 0u;
            if (produced < target && remaining > 0) {
                // The source contributes silence (scratch is already zeroed)
                // and the cascade below runs unchanged over these frames.
                const std::uint32_t drain =
                    std::min(target - produced, remaining);
                ctx.tailElapsed += drain;
                produced += drain;
                remaining -= drain;
            }
            ctx.tailRemaining = remaining;
            phase = (remaining > 0) ? SourcePhase::Draining
                                    : SourcePhase::Done;
        }
    }

    // Starvation is a data fact of this block: the gate is open (the gated
    // path returned above), the source has not ended (phase stayed Active —
    // an ended source enters the drain block instead), and the ring could
    // not fill the block. The pool turns this flag into cooldown-spaced
    // STARVED notifications after the block renders.
    if (ctx.streamRing != nullptr) {
        ctx.starvedThisBlock =
            (phase == SourcePhase::Active) &&
            (ctx.logicalSourceOutputFramesThisBlock < sourceTarget || ctx.resyncLatchedThisBlock);
    }

    // --- insert-effect chain ---
    // Ascending slots share the bus chain's one-block bypass crossfade.
    if (produced > 0) {
        for (std::uint32_t s = 0; s < ctx.fxSlotCount; ++s) {
            ProcessChainSlot(ctx.fxSlots[s], ctx.fxBypassRamping[s],
                             ctx.fxBypassMix[s], ctx.fxBypassTarget[s],
                             scratch, dry, channels, offset, produced, nullptr,
                             ctx.fxSlotEffectRef[s].type == VN_EFFECT_LIMITER
                                 ? static_cast<LimiterState*>(ctx.fxSlots[s].state)
                                 : nullptr);
        }
    }

#if HARKEN_STAGE_TAPS_ACTIVE
    ctx.tapPostFx = StageTapPeak(scratch, channels, offset, produced);
#endif

    // --- fixed tone strip: LPF, HPF, low shelf, high shelf ---
    if (produced > 0) {
        ProcessFixedToneSection(
            ctx, FixedToneSection::Lowpass, ctx.lpfTransition,
            ctx.lpfEnabled, ctx.lpfCoeffs, ctx.lpfState, ctx.lpfTailFrames,
            ctx.lpfGlide, scratch, channels, offset, produced);
        ProcessFixedToneSection(
            ctx, FixedToneSection::Highpass, ctx.hpfTransition,
            ctx.hpfEnabled, ctx.hpfCoeffs, ctx.hpfState, ctx.hpfTailFrames,
            ctx.hpfGlide, scratch, channels, offset, produced);
        ProcessFixedToneSection(
            ctx, FixedToneSection::LowShelf, ctx.shelfTransition,
            ctx.shelfEnabled, ctx.shelfCoeffs, ctx.shelfState,
            ctx.shelfTailFrames, ctx.shelfGlide, scratch, channels, offset,
            produced);
        ProcessFixedToneSection(
            ctx, FixedToneSection::HighShelf, ctx.highShelfTransition,
            ctx.highShelfEnabled, ctx.highShelfCoeffs, ctx.highShelfState,
            ctx.highShelfTailFrames, ctx.highShelfGlide, scratch, channels,
            offset, produced);
    }

#if HARKEN_STAGE_TAPS_ACTIVE
    ctx.tapPostFilter = StageTapPeak(scratch, channels, offset, produced);
#endif

    // --- gain (frame-accurate OutputGain), then fade-stop on top ---
    // Consume only produced samples: zero produced is a no-op, and the
    // offset prefix before the voice starts is not part of this clock.
    ctx.gain.Apply(scratch, channels, offset, produced);
    WeightSegmentV1 priorWeight{};
    bool priorWeightActive = false;
    std::uint32_t weightChangeOffset = UINT32_MAX;
    for (std::uint32_t eventIndex = 0;
         eventIndex < timelineSpan.eventCount; ++eventIndex) {
        const VoiceTimelineEvent& event = timelineSpan.events[eventIndex];
        if (!TimelineEventPrecedesRootStop(ctx, eventIndex, blockFrames)) continue;
        if (event.Kind() == TimelineEventKind::VariableWeight &&
            !event.Weight().variableWeightOwner.Valid() &&
            event.offset > offset && event.offset < offset + produced) {
            priorWeight = ctx.variableWeightSegment;
            priorWeightActive = ctx.variableWeightActive;
            ApplyTimelineControlEvent(ctx, event, blockFrames);
            weightChangeOffset = event.offset - offset;
            break;
        }
    }
    if ((ctx.variableWeightActive || priorWeightActive) && produced != 0) {
        const double currentInverseSpan = ctx.variableWeightActive
            ? 1.0 / static_cast<double>(
                  ctx.variableWeightSegment.endFrame -
                  ctx.variableWeightSegment.startFrame - 1u)
            : 0.0;
        const double priorInverseSpan = priorWeightActive
            ? 1.0 / static_cast<double>(
                  priorWeight.endFrame - priorWeight.startFrame - 1u)
            : 0.0;
        const auto weightAt = [](const WeightSegmentV1& segment,
                                 double inverseSpan,
                                 std::uint64_t frame) noexcept {
            if (frame >= segment.endFrame - 1u) {
                return segment.endWeight;
            }
            if (frame <= segment.startFrame) {
                return segment.startWeight;
            }
            return static_cast<float>(
                static_cast<double>(segment.startWeight) +
                (static_cast<double>(segment.endWeight) -
                 static_cast<double>(segment.startWeight)) *
                    static_cast<double>(frame - segment.startFrame) *
                    inverseSpan);
        };
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            float* samples = scratch[channel] + offset;
            for (std::uint32_t index = 0; index < produced; ++index) {
                const std::uint64_t frame =
                    ctx.timelineEvents.quantumIndex * blockFrames + offset +
                    index;
                const bool beforeChange =
                    weightChangeOffset != UINT32_MAX &&
                    index < weightChangeOffset;
                if (beforeChange && !priorWeightActive) continue;
                const WeightSegmentV1& segment = beforeChange
                    ? priorWeight : ctx.variableWeightSegment;
                samples[index] *= weightAt(
                    segment,
                    beforeChange ? priorInverseSpan : currentInverseSpan,
                    frame);
            }
        }
    }
    if (ctx.variableWeightControlCount != 0 && produced != 0) {
        for (std::uint32_t index = 0; index < produced; ++index) {
            for (std::uint32_t eventIndex = 0; eventIndex < timelineSpan.eventCount; ++eventIndex) {
                const auto& event = timelineSpan.events[eventIndex];
                if (event.Kind() == TimelineEventKind::VariableWeight && event.Weight().variableWeightOwner.Valid() &&
                    event.offset == offset + index && event.offset > offset &&
                    TimelineEventPrecedesRootStop(ctx, eventIndex, blockFrames))
                    ApplyTimelineControlEvent(ctx, event, blockFrames);
            }
            const auto frame = timelineSpan.quantumIndex * blockFrames + offset + index;
            float combined = 1.0f;
            for (std::uint32_t control = 0; control < ctx.variableWeightControlCount; ++control) {
                const auto& binding = ctx.variableWeightControls[control];
                if (!binding.active) continue;
                const auto& segment = binding.segment;
                const float value = frame <= segment.startFrame ? segment.startWeight
                    : frame >= segment.endFrame - 1u ? segment.endWeight
                    : static_cast<float>(static_cast<double>(segment.startWeight) +
                        (static_cast<double>(segment.endWeight) - segment.startWeight) *
                        static_cast<double>(frame - segment.startFrame) * binding.inverseSpan);
                combined *= value;
            }
            for (std::uint32_t channel = 0; channel < channels; ++channel)
                scratch[channel][offset + index] *= combined;
        }
    }
    for (std::uint32_t eventIndex = 0;
         eventIndex < timelineSpan.eventCount; ++eventIndex) {
        const VoiceTimelineEvent& event = timelineSpan.events[eventIndex];
        if (!TimelineEventPrecedesRootStop(ctx, eventIndex, blockFrames)) continue;
        if (event.offset > offset && event.offset < offset + produced &&
            (event.Kind() == TimelineEventKind::Envelope ||
             event.Kind() == TimelineEventKind::Stop)) {
            ApplyTimelineControlEvent(ctx, event, blockFrames);
        }
    }
    for (VoiceRenderContext::TimelineEnvelopeState& envelope :
         ctx.timelineEnvelopes) {
        if (!envelope.active || produced == 0) {
            continue;
        }
        const std::uint64_t start = envelope.startFrame;
        const std::uint64_t end = envelope.endFrame;
        const std::uint64_t length = end > start ? envelope.elapsedFrames + (end - start) : 0;
        const float reciprocal = length > 1
            ? 1.0f / static_cast<float>(length - 1)
            : 0.0f;
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            float* samples = scratch[ch] + offset;
            for (std::uint32_t i = 0; i < produced; ++i) {
                const std::uint64_t absoluteFrame =
                    ctx.timelineEvents.quantumIndex * blockFrames + offset + i;
                if (absoluteFrame < start || absoluteFrame >= end) {
                    continue;
                }
                const std::uint64_t done = envelope.elapsedFrames + (absoluteFrame - start);
                const float progress = length == 1
                    ? 1.0f
                    : done == 0
                        ? 0.0f
                        : done >= length - 1
                              ? 1.0f
                              : static_cast<float>(done) * reciprocal;
                float envelopeGain = progress;
                if (envelope.useCurve) {
                    const float curveX = InterpLinear(
                        envelope.curve->xMin, envelope.curve->xMax, progress);
                    envelopeGain = envelope.curve->Eval(curveX);
                }
                if (envelope.fadeOut) {
                    envelopeGain = 1.0f - envelopeGain;
                    ctx.timelineFadeOutCoveredSample = true;
                }
                samples[i] *= envelopeGain;
            }
        }
        const std::uint64_t renderedEnd =
            ctx.timelineEvents.quantumIndex * blockFrames + offset + produced;
        if (renderedEnd >= end) {
            envelope.active = false;
        }
    }
    // Each source transition contributes independently, including an exit
    // inherited by a later voice partway through its absolute fade interval.
    for (auto* binding = ctx.sourceExits; binding != nullptr; binding = binding->next) {
        if (!binding->active || !binding->source->fade) continue;
        const auto& exit = *binding->source;
        const auto length = exit.stopFrame - exit.fadeStartFrame + exit.fadeElapsedFrames;
        const auto inverse = length > 1 ? 1.0f / static_cast<float>(length - 1u) : 0.0f;
        for (std::uint32_t index = 0; index < produced; ++index) {
            const auto frame = sourceBlockStart + index;
            if (frame < exit.fadeStartFrame) continue;
            const float progress = length <= 1 || frame >= exit.stopFrame - 1u ? 1.0f :
                static_cast<float>(frame - exit.fadeStartFrame + exit.fadeElapsedFrames) * inverse;
            const auto gain = 1.0f - exit.curve.Eval(InterpLinear(exit.curve.xMin, exit.curve.xMax, progress));
            for (std::uint32_t channel = 0; channel < channels; ++channel)
                scratch[channel][offset + index] *= gain;
        }
    }
    // Timeline controls follow the output clock even while a live stream is
    // dry. Adopt the silent suffix only after scaling the produced prefix so a
    // later envelope cannot replace the envelope used by earlier samples.
    if (phase == SourcePhase::Active && produced < target) {
        for (std::uint32_t eventIndex = 0;
             eventIndex < timelineSpan.eventCount; ++eventIndex) {
            const auto& event = timelineSpan.events[eventIndex];
            if (event.offset > offset && event.offset >= offset + produced &&
                event.offset < offset + target &&
                TimelineEventPrecedesRootStop(ctx, eventIndex, blockFrames) &&
                (event.Kind() == TimelineEventKind::Envelope ||
                 event.Kind() == TimelineEventKind::VariableWeight ||
                 event.Kind() == TimelineEventKind::Stop)) {
                ApplyTimelineControlEvent(ctx, event, blockFrames);
            }
        }
    }
    // A source-exit cutoff is frozen after all Page/lane claims and has no
    // span event. Adopt an in-block persistent Stop after earlier controls.
    const auto blockStart = ctx.timelineEvents.quantumIndex * blockFrames;
    if (!ctx.authoredRootStopApplied && ctx.authoredRootStopEvent == UINT32_MAX &&
        ctx.authoredRootStopFrame < blockStart + blockFrames) {
        auto stop = ctx.authoredRootStopControl;
        stop.offset = static_cast<std::uint16_t>(ctx.authoredRootStopFrame > blockStart
            ? ctx.authoredRootStopFrame - blockStart : 0);
        ApplyTimelineControlEvent(ctx, stop, blockFrames);
    }
    const VoiceRenderContext::TimelineEnvelopeState& stopFadeOut =
        ctx.timelineEnvelopes[1];
    const std::uint64_t timelineStopFrame =
        ctx.authoredRootStopApplied ? ctx.timelineStopFrame :
            ctx.timelineEvents.quantumIndex * blockFrames + ctx.timelineStopOffset;
    bool sourceExitCovered = false;
    for (auto* binding = ctx.sourceExits; binding != nullptr; binding = binding->next)
        sourceExitCovered = sourceExitCovered || (binding->active && binding->source->stop &&
            binding->source->stopFrame == timelineStopFrame);
    // An applied music exit is an exact cutoff, including an explicitly absent fade.
    // Ordinary Stop controls still use their fallback declick when uncovered.
    const bool authoredStopCovered =
        ctx.timelineStopPending && (sourceExitCovered ||
        (ctx.timelineStopAuthored && ctx.timelineFadeOutCoveredSample && stopFadeOut.fadeOut &&
        stopFadeOut.endFrame == timelineStopFrame));
    const bool startFallbackStop =
        ctx.timelineStopPending && !authoredStopCovered && !ctx.fading;
    if (startFallbackStop) {
        ctx.timelineFallbackStopActive = true;
        ctx.timelineFallbackStopFrame = timelineStopFrame;
        ctx.fading = true;
        ctx.fadeTotalFrames =
            std::max<std::uint32_t>(1, ctx.timelineStopDeclickFrames);
        ctx.fadeDoneFrames = 0;
        ctx.fadeStartOffsetInBlock =
            ctx.timelineStopOffset > offset
                ? ctx.timelineStopOffset - offset
                : 0;
        ctx.timelineStopPending = false;
        ctx.timelineEnvelopes[0].active = false;
        ctx.timelineEnvelopes[1].active = false;
    }
    if (ctx.authoredRootStopApplied && ctx.timelineFallbackStopActive && ctx.fading) {
        const auto renderFrame = ctx.timelineEvents.quantumIndex * blockFrames + offset;
        const auto elapsed = renderFrame > ctx.timelineFallbackStopFrame
            ? renderFrame - ctx.timelineFallbackStopFrame : 0;
        ctx.fadeDoneFrames = std::max(ctx.fadeDoneFrames,
            std::min(elapsed, ctx.fadeTotalFrames));
    }
    const std::uint32_t fadeStartOffset = ctx.fadeStartOffsetInBlock;
    if (ctx.fading && produced > 0) {
        const std::uint32_t fadeBegin =
            std::min(ctx.fadeStartOffsetInBlock, produced);
        const std::uint32_t fadeSamples = produced - fadeBegin;
        const std::uint64_t left = ctx.fadeTotalFrames - ctx.fadeDoneFrames;
        const std::uint32_t n = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(fadeSamples, left));
        const float inv = 1.0f / static_cast<float>(ctx.fadeTotalFrames);
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            float* p = scratch[ch] + offset + fadeBegin;
            for (std::uint32_t i = 0; i < n; ++i) {
                const float g =
                    static_cast<float>(ctx.fadeTotalFrames -
                                       (ctx.fadeDoneFrames + i + 1)) *
                    inv;
                p[i] *= g;
            }
        }
        ctx.fadeDoneFrames += n;
        if (ctx.fadeDoneFrames >= ctx.fadeTotalFrames) {
            produced = fadeBegin + n;
            phase = SourcePhase::Done;
            stopCutoff = offset + produced;
        }
    }

    // Fading out silence still advances the fade clock. When the source starves
    // (produced < target with the phase still Active — an ended source would
    // have entered the drain block above, and a fade that completed over the
    // rendered frames left the phase Done above), the gap frames beyond
    // `produced` are silence the block never emits, so they take no gain
    // scaling; counting them keeps the fade clock advancing by a full block of
    // wall time. Without this, a Vn_Stop(fadeMs>0) issued while the source is
    // starved would never reach fadeTotalFrames and the voice would hang
    // forever. `produced` keeps its real value — no silence is folded past what
    // actually rendered — and the fadedOut criterion in VoicePool observes the
    // completed clock unchanged.
    if (ctx.fading && produced < target && phase == SourcePhase::Active) {
        const std::uint32_t silentFadeBegin =
            std::max(produced, std::min(fadeStartOffset, target));
        const std::uint64_t left = ctx.fadeTotalFrames - ctx.fadeDoneFrames;
        const std::uint32_t advanced = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(target - silentFadeBegin, left));
        ctx.fadeDoneFrames += advanced;
        if (ctx.fadeDoneFrames >= ctx.fadeTotalFrames) {
            phase = SourcePhase::Done;
            stopCutoff = offset + silentFadeBegin + advanced;
        }
    }
    ctx.fadeStartOffsetInBlock = 0;

    if (ctx.timelineStopPending && authoredStopCovered) {
        stopCutoff = ctx.timelineStopOffset;
        produced = std::min<std::uint32_t>(
            produced,
            ctx.timelineStopOffset > offset
                ? ctx.timelineStopOffset - offset
                : 0);
        phase = SourcePhase::Done;
    }
    if (ctx.timelineStopPending) {
        ctx.timelineStopPending = false;
        ctx.timelineStopAuthored = false;
        ctx.timelineEnvelopes[0].active = false;
        ctx.timelineEnvelopes[1].active = false;
    }

    // Select after all same-block clipping. At an equal boundary Stop wins;
    // a previously published source end remains immutable through tail Stop.
    if (stopCutoff != UINT32_MAX &&
        (sourceResult == SourceTerminalKind::None || stopCutoff <= sourceBoundary)) {
        ctx.stagedSourceTerminal = ctx.streamParkActive
            ? SourceTerminalKind::Parked : SourceTerminalKind::Stop;
        ctx.stagedSourceTerminalOffset = stopCutoff;
    } else if (sourceResult != SourceTerminalKind::None) {
        ctx.stagedSourceTerminal = sourceResult;
        ctx.stagedSourceTerminalOffset = sourceBoundary;
    }
    if (phase == SourcePhase::Done) {
        ctx.stagedPlaybackEnd = stopCutoff != UINT32_MAX
            ? ctx.streamParkActive ? SourceTerminalKind::Parked : SourceTerminalKind::Stop
            : sourceResult;
        ctx.stagedPlaybackEndOffset = stopCutoff != UINT32_MAX ? stopCutoff : offset + produced;
    }

#if HARKEN_STAGE_TAPS_ACTIVE
    // Measured after both the gain stage and the fade multiply, in the
    // voice's source channels — before the bus fold-in applies any
    // spatial pan/width gains. Pan correctness is proven by output-buffer
    // golden cases, not by this tap.
    ctx.tapPostGain = StageTapPeak(scratch, channels, offset, produced);
#endif

    ctx.cascade.produced = produced;
    ctx.phase = phase;
    ctx.lastResult = StageResult{produced, phase};
}

template<bool imageOnly>
static void FoldVoiceIntoBusesImpl(VoiceRenderContext& ctx, float* const* busPlanes,
                        std::uint32_t blockFrames, std::uint32_t mixChannels,
                        std::uint32_t mixLayout, std::uint64_t sourceFrameBegin,
                        output_gain_detail::MonoStereoMixKernel monoStereoMix) {
    const std::uint32_t channels = ctx.pcm.channelCount;
    const std::uint32_t offset = ctx.cascade.offset;
    const std::uint32_t produced = ctx.cascade.produced;
    const bool monitorArmed = produced > 0 && harken::monitor::AudioLiveArmed();
    const bool directImage = imageOnly && ctx.cascade.signalImageReadOnly && ctx.cascade.borrowed.frames != 0 &&
        !monitorArmed && !ctx.dynamicVoiceTapActive && channels <= 2 &&
        !ctx.spatialMatrixDirty && ctx.spatialMatrix.Settled();
    if (ctx.cascade.borrowed.frames != 0 &&
        !directImage && (channels > 2 || imageOnly || monitorArmed || !CanCombineSpatialOutput(ctx, mixChannels))) {
        MaterializePlainPcm(ctx);
    }
    if (monitorArmed) {
        const std::uint64_t voiceHandle = ctx.dynamicSourceIdentity;
        if (voiceHandle != 0) {
            const std::uint64_t voiceGeneration = static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(voiceHandle >> 32));
            const std::uint64_t frameBegin = sourceFrameBegin + offset;
            if (ctx.monitorPreChainFrames == produced &&
                ctx.monitorPreChain[0] != nullptr) {
                harken::monitor::PublishPlanarAudioChunk(
                    VN_MONITOR_TAP_VOICE_PRE_CHAIN, voiceHandle, voiceGeneration,
                    0, ctx.monitorPreChain, produced, channels, 0, 0,
                    ctx.engineSampleRate, frameBegin);
            }
            harken::monitor::PublishPlanarAudioChunk(
                VN_MONITOR_TAP_VOICE_POST_CHAIN, voiceHandle, voiceGeneration, 0,
                ctx.cascade.scratch, produced, channels, offset, 0,
                ctx.engineSampleRate, frameBegin);
        }
    }
    if (ctx.cascade.borrowed.frames != 0 && !directImage) {
        FoldPlainPcm(ctx, busPlanes, mixChannels, monoStereoMix);
        return;
    }
    float** scratch = ctx.cascade.scratch;
    (void)blockFrames; // scratch was sized in the pre-pass; nothing to reserve

    // One post-matrix image feeds both the dry bus and every send. Matrix
    // state advances only for actual output frames, including worker folds.
    if (produced > 0) {
        float* const* dry = imageOnly ? nullptr : busPlanes + ctx.busIndex * mixChannels;
        float** tap = ctx.cascade.tap;
        const bool haveTap = ctx.cascade.tapReady;
        if (channels <= 2) {
            if (ctx.spatialMatrixDirty) {
                const auto matrix = MakeSpatialMatrix(
                    channels, mixLayout, ctx.pan2d, ctx.pan, ctx.width,
                    ctx.spatialized ? ctx.spatializationMix : 0.0f,
                    *ctx.biquadTrig);
                ctx.spatialMatrix.Set(matrix, ctx.spatialMatrixHasRendered
                    ? BiquadSmoothingFrames(ctx.engineSampleRate) : 1u);
                ctx.spatialMatrixDirty = false;
            }
            const float* src0 = scratch[0] + offset;
            const float* src1 = channels == 2 ? scratch[1] + offset : nullptr;
            if (ctx.spatialMatrix.Settled()) {
                if (channels == 1 && mixChannels == 2 && !haveTap &&
                    ctx.outputGain.remainingFrames == 0 &&
                    ctx.spatialMatrix.target.gain[0][0] != 0.0f &&
                    ctx.spatialMatrix.target.gain[0][1] != 0.0f &&
                    CanCombineSpatialOutput(ctx, mixChannels)) {
                    monoStereoMix(src0, dry[0] + offset, dry[1] + offset,
                        produced, 1.0f,
                        ctx.spatialMatrix.target.gain[0][0] * ctx.outputGain.target,
                        ctx.spatialMatrix.target.gain[0][1] * ctx.outputGain.target);
                } else {
                    for (std::uint32_t ch = 0; ch < mixChannels; ++ch) {
                        const float a = ctx.spatialMatrix.target.gain[0][ch];
                        const float b = ctx.spatialMatrix.target.gain[1][ch];
                        float* dst = imageOnly ? nullptr : dry[ch] + offset;
                        float* image = haveTap ? tap[ch] + offset : nullptr;
                        if constexpr (imageOnly) {
                            if (ctx.cascade.signalImageReadOnly) {
                                bool shared = false;
                                for (std::uint32_t prior = 0; prior < ch; ++prior) {
                                    if (std::bit_cast<std::uint32_t>(ctx.spatialMatrix.target.gain[0][prior]) == std::bit_cast<std::uint32_t>(a) &&
                                        (src1 == nullptr || std::bit_cast<std::uint32_t>(ctx.spatialMatrix.target.gain[1][prior]) == std::bit_cast<std::uint32_t>(b))) {
                                        tap[ch] = tap[prior];
                                        shared = true;
                                        break;
                                    }
                                }
                                if (shared) continue;
                            }
                        }
                        if (image != nullptr) {
                            if (directImage) {
                                const auto span = ctx.cascade.borrowed;
                                const auto project = [&](std::uint32_t first, std::uint32_t count, std::uint32_t ringOffset) {
                                    const auto* left = ctx.resampler.ring[0] + ringOffset;
                                    const auto* right = channels == 2 ? ctx.resampler.ring[1] + ringOffset : nullptr;
                                    ProjectSignalImage(image + first, left, right, count, a, b, ctx.gain.target);
                                };
                                project(0, span.firstFrames, span.ringOffset);
                                project(span.firstFrames, span.frames - span.firstFrames, 0);
                            } else {
                                ProjectSignalImage(image, src0, src1, produced, a, b);
                            }
                            if (!imageOnly) ctx.outputGain.MixAdd(image, dst, produced, 0);
                        } else if (b == 0.0f) {
                            if (a != 0.0f) ctx.outputGain.MixAdd(src0, dst, produced, 0, a);
                        } else if (a == 0.0f) {
                            ctx.outputGain.MixAdd(src1, dst, produced, 0, b);
                        } else {
                            ctx.outputGain.MixAdd(src0, dst, produced, 0, a);
                            ctx.outputGain.MixAdd(src1, dst, produced, 0, b);
                        }
                    }
                }
            } else {
                // Share the frame clock across planar lanes, then vectorize
                // along samples. Bounded stack tiles also reuse OutputGain's
                // existing ramp kernel without evaluating it per matrix cell.
                float alpha[128];
                float imageTile[128];
                auto& transition = ctx.spatialMatrix;
                for (std::uint32_t begin = 0; begin < produced; begin += 128) {
                    const auto count = std::min(128u, produced - begin);
                    const auto rampCount = transition.progress < transition.frames - 1
                        ? std::min(count, transition.frames - 1 - transition.progress) : 0u;
                    for (std::uint32_t i = 0; i < rampCount; ++i)
                        alpha[i] = static_cast<float>(transition.progress + i) *
                                   transition.inverseSpan;
                    std::fill_n(alpha + rampCount, count - rampCount, 1.0f);
                    transition.progress += std::min(count, transition.frames - transition.progress);
                    for (std::uint32_t ch = 0; ch < mixChannels; ++ch) {
                        const float a = transition.start.gain[0][ch];
                        const float b = transition.start.gain[1][ch];
                        const float ta = transition.target.gain[0][ch];
                        const float tb = transition.target.gain[1][ch];
                        float* image = haveTap ? tap[ch] + offset + begin : imageTile;
                        if (src1 != nullptr) {
                            for (std::uint32_t i = 0; i < count; ++i) {
                                const float ga = alpha[i] >= 1.0f ? ta : a + (ta - a) * alpha[i];
                                const float gb = alpha[i] >= 1.0f ? tb : b + (tb - b) * alpha[i];
                                image[i] = src0[begin + i] * ga + src1[begin + i] * gb;
                            }
                        } else {
                            for (std::uint32_t i = 0; i < count; ++i) {
                                const float ga = alpha[i] >= 1.0f ? ta : a + (ta - a) * alpha[i];
                                image[i] = src0[begin + i] * ga;
                            }
                        }
                        if (!imageOnly) ctx.outputGain.MixAdd(image, dry[ch] + offset + begin,
                                              count, begin);
                    }
                }
            }
            ctx.spatialMatrixHasRendered = true;
        } else {
            if constexpr (imageOnly) {
                if (ctx.cascade.signalImageReadOnly && mixChannels > 2) {
                    int imageSources[kMaxChannels];
                    MapNativeImageChannels(channels, mixChannels, mixLayout, imageSources);
                    for (std::uint32_t ch = 0; ch < mixChannels; ++ch) {
                        if (imageSources[ch] >= 0) tap[ch] = scratch[imageSources[ch]];
                        else std::memset(tap[ch] + offset, 0, produced * sizeof(float));
                    }
                    return;
                }
            }
            if (haveTap) {
                for (std::uint32_t ch = 0; ch < mixChannels; ++ch)
                    std::memset(tap[ch] + offset, 0, produced * sizeof(float));
            }
            ChannelRole roles[kMaxChannels];
            const auto sourceLayout = LayoutFromChannelCount(channels);
            if (sourceLayout != VN_CHANNEL_LAYOUT_DEFAULT) LayoutRoles(sourceLayout, roles);
            for (std::uint32_t sc = 0; sc < channels; ++sc) {
                const int destination = sourceLayout == VN_CHANNEL_LAYOUT_DEFAULT
                    ? (sc < mixChannels ? static_cast<int>(sc) : -1)
                    : ChannelOfRole(mixLayout, roles[sc]);
                if (destination < 0) continue;
                const auto ch = static_cast<std::uint32_t>(destination);
                const float* src = scratch[sc] + offset;
                if (mixChannels == 2) {
                    auto& balance = ch == 0 ? ctx.balanceLeftTransition : ctx.balanceRightTransition;
                    for (std::uint32_t i = 0; i < produced; ++i) {
                        const float value = src[i] * balance.Value();
                        if (!imageOnly) dry[ch][offset + i] += value * ctx.outputGain.At(i);
                        if (haveTap) tap[ch][offset + i] = value;
                        balance.AdvanceFrame();
                    }
                } else {
                    if (!imageOnly) ctx.outputGain.MixAdd(src, dry[ch] + offset, produced, 0);
                    if (haveTap) std::memcpy(tap[ch] + offset, src, produced * sizeof(float));
                }
            }
        }
        if (imageOnly) return;
        ctx.outputGain.Advance(produced);
        if (haveTap) {
            ScatterVoiceSends(ctx, busPlanes, tap, offset, produced,
                              mixChannels);
        }
    }

}

bool PrepareVoiceSignalImage(VoiceRenderContext& ctx, std::uint32_t blockFrames,
                             std::uint32_t mixChannels, std::uint32_t mixLayout,
                             std::uint64_t sourceFrameBegin) {
    if (!ctx.cascade.runnable || !ctx.cascade.tapReady) return false;
    if (!ctx.cascade.signalImageReady) {
        FoldVoiceIntoBusesImpl<true>(ctx, nullptr, blockFrames, mixChannels, mixLayout,
                              sourceFrameBegin, &output_gain_detail::MixMonoToStereoScaled);
        ctx.cascade.signalImageReady = true;
    }
    return true;
}

template<bool Ramping>
static void FoldFourSignalImageRange(std::span<const SignalMemberImage, 4> images,
                          const double* const* gains, float* const* busPlanes,
                          std::uint32_t channels, unsigned begin, unsigned frames,
                          const float* const* levels = nullptr) {
    constexpr unsigned Count = 4;
    float* const* buses = busPlanes + images[0].voice->busIndex * channels;
    unsigned done = 0;
    for (unsigned ch = 0; ch < channels; ++ch) {
        if ((done & (1u << ch)) != 0) continue;
        PreparedInput inputs[Count];
        bool anyActive = false;
        for (unsigned v = 0; v < Count; ++v) {
            inputs[v].source = images[v].planes[ch];
            inputs[v].gains = gains[v];
            if constexpr (Ramping) {
                inputs[v].levels = levels[v];
                inputs[v].levelBegin = begin;
            }
            inputs[v].scalarLevel = images[v].outputGain->target;
#if defined(HARKEN_OUTPUT_GAIN_SSE2)
            inputs[v].level = _mm_set1_ps(images[v].outputGain->target);
#endif
            inputs[v].active = (images[v].silentChannels & (1u << ch)) == 0;
            anyActive |= inputs[v].active;
        }
        done |= 1u << ch;
        if (!anyActive) continue;
        float* destinations[kMaxChannels]{};
        unsigned outputCount = 0;
        destinations[outputCount++] = buses[ch];
        // Sharing is valid only when every voice agrees for this lane pair.
        // A mismatch splits lanes, never changes or combines contributions.
        for (unsigned lane = ch + 1; lane < channels; ++lane) {
            if ((done & (1u << lane)) != 0) continue;
            bool shared = true;
            for (unsigned v = 0; v < Count; ++v) {
                const bool active = (images[v].silentChannels & (1u << lane)) == 0;
                shared &= active == inputs[v].active &&
                    (!active || images[v].planes[lane] == inputs[v].source);
            }
            if (shared) {
                destinations[outputCount++] = buses[lane];
                done |= 1u << lane;
            }
        }
        if (outputCount == 1) FoldFixedLanes<Count, 1, Ramping>(inputs, destinations, outputCount, begin, frames);
        else if (outputCount == 2) FoldFixedLanes<Count, 2, Ramping>(inputs, destinations, outputCount, begin, frames);
        else FoldFixedLanes<Count, 0, Ramping>(inputs, destinations, outputCount, begin, frames);
    }
}

void FoldFourSignalImages(std::span<const SignalMemberImage, 4> images,
                          const double* const* gains, float* const* busPlanes,
                          std::uint32_t channels) {
    bool ramping = false;
    for (const auto& image : images) ramping |= image.outputGain->remainingFrames != 0;
    if (!ramping) {
        FoldFourSignalImageRange<false>(images, gains, busPlanes, channels, images[0].offset, images[0].frames);
        return;
    }
#if defined(HARKEN_OUTPUT_GAIN_SSE2)
    // FTZ without DAZ makes the original ramp kernel's intermediate multiply
    // observable. Keep that kernel for this uncommon floating-point mode.
    if ((_mm_getcsr() & 0x8040u) == 0x8000u) {
        for (unsigned v = 0; v < 4; ++v) {
            const auto& image = images[v];
            for (unsigned ch = 0; ch < channels; ++ch) {
                if ((image.silentChannels & (1u << ch)) != 0) continue;
                auto* destination = busPlanes[image.voice->busIndex * channels + ch] + image.offset;
                const auto* source = image.planes[ch] + image.offset;
                if (gains[v] != nullptr || image.voice->cascade.signalImageReadOnly)
                    MixReceivedOutput(source, {&destination, 1},
                        gains[v] == nullptr ? nullptr : gains[v] + image.offset,
                        image.frames, *image.outputGain);
                else image.outputGain->MixAdd(source, destination, image.frames, 0);
            }
        }
        return;
    }
#endif
    float storage[4][128];
    const float* levels[]{storage[0], storage[1], storage[2], storage[3]};
    for (unsigned begin = 0; begin < images[0].frames; begin += 128) {
        const auto count = std::min(128u, images[0].frames - begin);
        for (unsigned v = 0; v < 4; ++v) images[v].outputGain->Fill(storage[v], count, begin);
        FoldFourSignalImageRange<true>(images, gains, busPlanes, channels,
            images[0].offset + begin, count, levels);
    }
}


void FoldVoiceIntoBuses(VoiceRenderContext& ctx, float* const* busPlanes,
                        std::uint32_t blockFrames, std::uint32_t mixChannels,
                        std::uint32_t mixLayout, std::uint64_t sourceFrameBegin,
                        output_gain_detail::MonoStereoMixKernel monoStereoMix,
                        std::uint32_t silentChannels, const double* signalGains) {
    if (!ctx.cascade.signalImageReady) {
        FoldVoiceIntoBusesImpl<false>(ctx, busPlanes, blockFrames, mixChannels, mixLayout,
                              sourceFrameBegin, monoStereoMix);
        return;
    }
    const auto offset = ctx.cascade.offset;
    const auto produced = ctx.cascade.produced;
    if (produced == 0) return;
    float* const* dry = busPlanes + ctx.busIndex * mixChannels;
    std::uint32_t mixedChannels = silentChannels;
    for (std::uint32_t ch = 0; ch < mixChannels; ++ch) {
        if ((mixedChannels & (1u << ch)) != 0) continue;
        if (signalGains != nullptr || ctx.cascade.signalImageReadOnly) {
            float* destinations[kMaxChannels]{};
            std::uint32_t count = 0;
            for (std::uint32_t lane = ch; lane < mixChannels; ++lane) {
                if ((mixedChannels & (1u << lane)) == 0 && ctx.cascade.tap[ch] == ctx.cascade.tap[lane]) {
                    destinations[count++] = dry[lane] + offset;
                    mixedChannels |= 1u << lane;
                }
            }
            MixReceivedOutput(ctx.cascade.tap[ch] + offset, {destinations, count},
                              signalGains == nullptr ? nullptr : signalGains + offset, produced, ctx.outputGain);
        } else
            ctx.outputGain.MixAdd(ctx.cascade.tap[ch] + offset, dry[ch] + offset, produced, 0);
    }
    ctx.outputGain.Advance(produced);
    ScatterVoiceSends(ctx, busPlanes, ctx.cascade.tap, offset, produced, mixChannels);
}

#if (defined(_M_X64) || defined(__x86_64__)) && (defined(__clang__) || defined(__GNUC__))
__attribute__((aligned(64)))
#endif
StageResult RenderVoiceBlock(VoiceRenderContext& ctx, BlockArena& arena,
                             float* const* busPlanes, std::uint32_t blockFrames,
                             std::uint32_t mixChannels,
                             std::uint32_t mixLayout,
                             std::uint64_t sourceFrameBegin,
                             output_gain_detail::MonoStereoMixKernel monoStereoMix) {
    // Direct and serial callers use the same phases as the worker path.
    // A declined prepare (closed gate or arena shortfall) leaves lastResult
    // carrying {0, phase}, without advancing the spatial fold.
    if (!PrepareVoiceCascade(ctx, arena, blockFrames, mixChannels)) {
        return ctx.lastResult;
    }
    RunVoiceCascade(ctx, blockFrames);
    FoldVoiceIntoBuses(ctx, busPlanes, blockFrames, mixChannels, mixLayout,
                       sourceFrameBegin, monoStereoMix);
    return ctx.lastResult;
}

} // namespace harken
