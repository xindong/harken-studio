// Gain stage with block-boundary ramping: a gain change takes effect as a
// linear interpolation from the previous block's value to the new value
// across one block, which removes zipper noise. A voice's first block snaps
// to its initial gain — there is no fictitious prior value to ramp from.
//
// OutputGain is the frame-accurate path: N = ceil(ms * rate / 1000) actual
// samples, evaluated from a fixed start/target/total with the §3 low+step
// *distance formula. GainRamp keeps its one-block / Send duration.
//
// Pure math: no allocation, audio-thread safe.
#ifndef HARKEN_DSP_GAIN_RAMP_H
#define HARKEN_DSP_GAIN_RAMP_H

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

#if defined(_M_X64) || defined(__x86_64__)
#define HARKEN_OUTPUT_GAIN_SSE2 1
#include <emmintrin.h>
#endif

namespace harken {

// Converts a volume in dB with the same float pow the audio-thread gain
// apply path uses. Succeeds only when both the dB value and the converted
// gain are finite and the gain is non-negative. Negative underflow to 0 is
// legal; positive overflow is not truncated or replaced with a maximum.
inline bool TryVolumeGainFromDb(float volumeDb,
                                float* outLinearGain = nullptr) noexcept {
    if (!std::isfinite(volumeDb)) {
        return false;
    }
    const float linear = std::pow(10.0f, volumeDb / 20.0f);
    if (!std::isfinite(linear) || linear < 0.0f) {
        return false;
    }
    if (outLinearGain != nullptr) {
        *outLinearGain = linear;
    }
    return true;
}

namespace output_gain_detail {

inline float MinGain(float a, float b) noexcept {
    return a < b ? a : b;
}

inline float MaxGain(float a, float b) noexcept {
    return a > b ? a : b;
}

inline void MulConstant(float* const* channels, std::uint32_t channelCount,
                        std::uint32_t offset, std::uint32_t frames,
                        float gain) noexcept {
    if (frames == 0 || channelCount == 0 || gain == 1.0f) {
        return;
    }
    for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
        float* p = channels[ch] + offset;
        std::uint32_t i = 0;
#if defined(HARKEN_OUTPUT_GAIN_SSE2)
        const __m128 g = _mm_set1_ps(gain);
        for (; i + 4 <= frames; i += 4) {
            _mm_storeu_ps(p + i, _mm_mul_ps(_mm_loadu_ps(p + i), g));
        }
#endif
        for (; i < frames; ++i) {
            p[i] *= gain;
        }
    }
}

inline void MixScaled(const float* src, float* dst, std::uint32_t frames,
                      float gain) noexcept {
    if (frames == 0) {
        return;
    }
    std::uint32_t i = 0;
#if defined(HARKEN_OUTPUT_GAIN_SSE2)
    const __m128 g = _mm_set1_ps(gain);
    for (; i + 4 <= frames; i += 4) {
        const __m128 s = _mm_loadu_ps(src + i);
        const __m128 d = _mm_loadu_ps(dst + i);
        _mm_storeu_ps(dst + i, _mm_add_ps(d, _mm_mul_ps(s, g)));
    }
#endif
    for (; i < frames; ++i) {
        dst[i] += src[i] * gain;
    }
}

// Reuse the rounded mono voice sample without combining either output scale.
template<bool ApplyVoiceGain, bool EqualOutputScale>
inline void MixMonoToStereoScaledImpl(const float* src, float* left, float* right,
    std::uint32_t frames, float voiceGain, float leftScale, float rightScale) noexcept {
    std::uint32_t i = 0;
#if defined(HARKEN_OUTPUT_GAIN_SSE2)
    const __m128 voice = _mm_set1_ps(voiceGain);
    const __m128 leftGain = _mm_set1_ps(leftScale);
    const __m128 rightGain = _mm_set1_ps(rightScale);
    const auto vectorFrames = frames & ~std::uint32_t{3};
    for (; i < vectorFrames; i += 4) {
        __m128 value = _mm_loadu_ps(src + i);
        if constexpr (ApplyVoiceGain) value = _mm_mul_ps(value, voice);
        const __m128 leftContribution = _mm_mul_ps(value, leftGain);
        _mm_storeu_ps(left + i, _mm_add_ps(_mm_loadu_ps(left + i),
            leftContribution));
        const __m128 rightContribution = EqualOutputScale
            ? leftContribution : _mm_mul_ps(value, rightGain);
        _mm_storeu_ps(right + i, _mm_add_ps(_mm_loadu_ps(right + i),
            rightContribution));
    }
#endif
#if defined(HARKEN_OUTPUT_GAIN_SSE2)
    // Complete vectors leave at most three ordered scalar contributions.
    for (std::uint32_t tail = 0; tail < 3 && i < frames; ++tail, ++i) {
#else
    for (; i < frames; ++i) {
#endif
        float value = src[i];
        if constexpr (ApplyVoiceGain) value *= voiceGain;
        const float leftContribution = value * leftScale;
        left[i] += leftContribution;
        const float rightContribution = EqualOutputScale ? leftContribution : value * rightScale;
        right[i] += rightContribution;
    }
}

inline void MixMonoToStereoScaled(const float* src, float* left, float* right,
    std::uint32_t frames, float voiceGain, float leftScale, float rightScale) noexcept {
    if (std::bit_cast<std::uint32_t>(leftScale) == std::bit_cast<std::uint32_t>(rightScale)) {
        if (voiceGain == 1.0f)
            MixMonoToStereoScaledImpl<false, true>(src, left, right, frames, voiceGain, leftScale, rightScale);
        else
            MixMonoToStereoScaledImpl<true, true>(src, left, right, frames, voiceGain, leftScale, rightScale);
    } else {
        if (voiceGain == 1.0f)
            MixMonoToStereoScaledImpl<false, false>(src, left, right, frames, voiceGain, leftScale, rightScale);
        else
            MixMonoToStereoScaledImpl<true, false>(src, left, right, frames, voiceGain, leftScale, rightScale);
    }
}

using MonoStereoMixKernel = void (*)(const float*, float*, float*, std::uint32_t,
                                    float, float, float) noexcept;

// Select on the control thread before publishing a VoicePool to render readers.
MonoStereoMixKernel SelectMonoStereoMixKernel() noexcept;

inline std::uint32_t CappedInterp(std::uint64_t remainingFrames,
                                 std::uint32_t startFrame,
                                 std::uint32_t limit) noexcept {
    if (remainingFrames <= static_cast<std::uint64_t>(startFrame) + 1u) {
        return 0;
    }
    const std::uint64_t available =
        remainingFrames - 1u - static_cast<std::uint64_t>(startFrame);
    return available > limit ? limit : static_cast<std::uint32_t>(available);
}

inline void FillConstant(float* dest, std::uint32_t count, float gain) noexcept {
    for (std::uint32_t i = 0; i < count; ++i) {
        dest[i] = gain;
    }
}

inline void MulGain(float* const* channels, std::uint32_t channelCount,
                    std::uint32_t index, float gain) noexcept {
    for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
        channels[ch][index] *= gain;
    }
}

#if defined(HARKEN_OUTPUT_GAIN_SSE2)
inline void MulGain4(float* const* channels, std::uint32_t channelCount,
                     std::uint32_t index, __m128 gain) noexcept {
    for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
        float* p = channels[ch] + index;
        _mm_storeu_ps(p, _mm_mul_ps(_mm_loadu_ps(p), gain));
    }
}

// One 4-wide from a double absolute-position anchor. Lanes are only the
// small integers 0..3 (or 0..-3 when falling). Do not accumulate a float
// offset across the 128-frame tile.
inline __m128 FloatTileGain4(double low, double slope, std::uint64_t distance,
                             bool up) noexcept {
    const float af =
        static_cast<float>(low + slope * static_cast<double>(distance));
    const float sf = static_cast<float>(slope);
    const float dir = up ? 1.0f : -1.0f;
    return _mm_add_ps(
        _mm_set1_ps(af),
        _mm_mul_ps(_mm_set1_ps(sf),
                   _mm_setr_ps(0.0f, dir, 2.0f * dir, 3.0f * dir)));
}
#endif

} // namespace output_gain_detail

struct OutputGain {
    float current = 1.0f;
    float start = 1.0f;
    float target = 1.0f;
    double step = 0.0;
    std::uint64_t totalFrames = 0;
    std::uint64_t remainingFrames = 0;

    void Set(float gain, std::uint64_t frames = 0) {
        start = current;
        target = gain;
        totalFrames = frames;
        remainingFrames = frames;
        step = frames == 0
                   ? 0.0
                   : std::abs(static_cast<double>(gain) -
                              static_cast<double>(current)) /
                         static_cast<double>(frames);
        if (frames == 0 || gain == current) {
            current = gain;
            remainingFrames = 0;
        }
    }

    float At(std::uint32_t frame) const {
        return EvaluateK(static_cast<std::uint64_t>(frame) + 1);
    }

    void Advance(std::uint32_t frames) {
        if (frames == 0) {
            return;
        }
        current = At(frames - 1);
        remainingFrames =
            frames >= remainingFrames ? 0 : remainingFrames - frames;
    }

    // Planar in-place apply. Time advances once; the last produced frame and
    // current share one evaluation. Planes must not overlap.
    void Apply(float* const* channels, std::uint32_t channelCount,
               std::uint32_t offset, std::uint32_t frames) {
        switch (channelCount) {
        case 1:
            ApplyImpl<1>(channels, channelCount, offset, frames);
            break;
        case 2:
            ApplyImpl<2>(channels, channelCount, offset, frames);
            break;
        case 4:
            ApplyImpl<4>(channels, channelCount, offset, frames);
            break;
        case 6:
            ApplyImpl<6>(channels, channelCount, offset, frames);
            break;
        case 8:
            ApplyImpl<8>(channels, channelCount, offset, frames);
            break;
        default:
            ApplyImpl<0>(channels, channelCount, offset, frames);
            break;
        }
    }

    // dest[i] == At(startFrame + i). Does not change current or remaining.
    void Fill(float* dest, std::uint32_t count,
              std::uint32_t startFrame) const {
        if (dest == nullptr || count == 0) {
            return;
        }
        FillGains(dest, count, startFrame);
    }

    // dst[i] += src[i] * scale * At(startFrame + i). Does not Advance.
    void MixAdd(const float* src, float* dst, std::uint32_t frames,
                std::uint32_t startFrame, float scale = 1.0f) const {
        if (src == nullptr || dst == nullptr || frames == 0) {
            return;
        }
        if (remainingFrames == 0 || startFrame >= remainingFrames) {
            output_gain_detail::MixScaled(src, dst, frames, scale * target);
            return;
        }
        std::uint32_t i = 0;
        const std::uint32_t interp = output_gain_detail::CappedInterp(
            remainingFrames, startFrame, frames);
        if (interp != 0) {
            float tile[128];
            for (std::uint32_t b = 0; b < interp; b += 128) {
                const std::uint32_t n = interp - b < 128u ? interp - b : 128u;
                StoreRampTile(tile, n, startFrame + b);
                for (std::uint32_t k = 0; k < n; ++k) {
                    dst[b + k] += src[b + k] * scale * tile[k];
                }
            }
            i = interp;
        }
        if (i < frames) {
            output_gain_detail::MixScaled(src + i, dst + i, frames - i,
                                          scale * target);
        }
    }

private:
    float EvaluateK(std::uint64_t k) const {
        if (remainingFrames == 0 || k >= remainingFrames) {
            return target;
        }
        const std::uint64_t rest = remainingFrames - k;
        const double low =
            static_cast<double>(output_gain_detail::MinGain(start, target));
        const std::uint64_t distance =
            target >= start ? totalFrames - rest : rest;
        return static_cast<float>(low + step * static_cast<double>(distance));
    }

    template <unsigned kFixedChannels>
    void ApplyImpl(float* const* channels, std::uint32_t channelCount,
                   std::uint32_t offset, std::uint32_t frames) {
        const std::uint32_t ch =
            kFixedChannels != 0 ? kFixedChannels : channelCount;
        if (frames == 0) {
            return;
        }
        if (ch == 0 || channels == nullptr) {
            Advance(frames);
            return;
        }
        if (remainingFrames == 0) {
            output_gain_detail::MulConstant(channels, ch, offset, frames,
                                            target);
            return;
        }

        const std::uint32_t body = frames - 1;
        const std::uint64_t active64 =
            static_cast<std::uint64_t>(body) < remainingFrames - 1
                ? static_cast<std::uint64_t>(body)
                : remainingFrames - 1;
        const std::uint32_t active = static_cast<std::uint32_t>(active64);
        const bool up = target >= start;
        const double low =
            static_cast<double>(output_gain_detail::MinGain(start, target));
        const double slope = step;
        const double maximum =
            static_cast<double>(output_gain_detail::MaxGain(start, target));
        const bool useFloatTile =
            maximum <= 1.0e30 &&
            slope >= static_cast<double>(std::numeric_limits<float>::min()) &&
            slope >= maximum * (4.0 * static_cast<double>(
                                          std::numeric_limits<float>::epsilon()));

        for (std::uint32_t b = 0; b < active; b += 128) {
            const std::uint32_t count =
                active - b < 128u ? active - b : 128u;
            ApplyTile(channels, ch, offset + b, count, b, up, low, slope,
                      useFloatTile);
        }
        output_gain_detail::MulConstant(channels, ch, offset + active,
                                        body - active, target);
        const float last = At(frames - 1);
        output_gain_detail::MulGain(channels, ch, offset + frames - 1, last);
        current = last;
        remainingFrames =
            frames >= remainingFrames ? 0 : remainingFrames - frames;
    }

    void FillGains(float* dest, std::uint32_t count,
                   std::uint32_t startFrame) const {
        std::uint32_t i = 0;
        const std::uint32_t interp = output_gain_detail::CappedInterp(
            remainingFrames, startFrame, count);
        if (interp != 0) {
            for (std::uint32_t b = 0; b < interp; b += 128) {
                const std::uint32_t n = interp - b < 128u ? interp - b : 128u;
                StoreRampTile(dest + b, n, startFrame + b);
            }
            i = interp;
        }
        output_gain_detail::FillConstant(dest + i, count - i, target);
    }

    void StoreRampTile(float* dest, std::uint32_t count,
                       std::uint32_t tileBegin) const {
        const bool up = target >= start;
        const double low =
            static_cast<double>(output_gain_detail::MinGain(start, target));
        const double slope = step;
        const double maximum =
            static_cast<double>(output_gain_detail::MaxGain(start, target));
        const bool useFloatTile =
            maximum <= 1.0e30 &&
            slope >= static_cast<double>(std::numeric_limits<float>::min()) &&
            slope >= maximum * (4.0 * static_cast<double>(
                                          std::numeric_limits<float>::epsilon()));
        const std::uint64_t lowDistance =
            up ? totalFrames - remainingFrames + tileBegin + 1
               : remainingFrames - tileBegin - count;
        std::uint32_t i = 0;
#if defined(HARKEN_OUTPUT_GAIN_SSE2)
        for (; i + 4 <= count; i += 4) {
            __m128 gain;
            if (useFloatTile) {
                const std::uint64_t distance =
                    lowDistance + (up ? i : count - 1 - i);
                gain = output_gain_detail::FloatTileGain4(low, slope, distance,
                                                          up);
            } else {
                const double d = static_cast<double>(lowDistance) +
                                 static_cast<double>(up ? i : count - 1 - i);
                const double dir = up ? 1.0 : -1.0;
                const __m128 lo = _mm_cvtpd_ps(_mm_add_pd(
                    _mm_set1_pd(low),
                    _mm_mul_pd(_mm_set1_pd(slope), _mm_setr_pd(d, d + dir))));
                const __m128 hi = _mm_cvtpd_ps(_mm_add_pd(
                    _mm_set1_pd(low),
                    _mm_mul_pd(_mm_set1_pd(slope),
                               _mm_setr_pd(d + 2.0 * dir, d + 3.0 * dir))));
                gain = _mm_movelh_ps(lo, hi);
            }
            _mm_storeu_ps(dest + i, gain);
        }
#endif
        for (; i < count; ++i) {
            const std::uint64_t distance =
                lowDistance + (up ? i : count - 1 - i);
            dest[i] = static_cast<float>(low +
                                         slope * static_cast<double>(distance));
        }
    }

    void ApplyTile(float* const* channels, std::uint32_t channelCount,
                   std::uint32_t offset, std::uint32_t count,
                   std::uint32_t tileBegin, bool up, double low, double slope,
                   bool useFloatTile) const {
        const std::uint64_t lowDistance =
            up ? totalFrames - remainingFrames + tileBegin + 1
               : remainingFrames - tileBegin - count;
        std::uint32_t i = 0;
#if defined(HARKEN_OUTPUT_GAIN_SSE2)
        for (; i + 4 <= count; i += 4) {
            __m128 gain;
            if (useFloatTile) {
                const std::uint64_t distance =
                    lowDistance + (up ? i : count - 1 - i);
                gain = output_gain_detail::FloatTileGain4(low, slope, distance,
                                                          up);
            } else {
                const double d = static_cast<double>(lowDistance) +
                                 static_cast<double>(up ? i : count - 1 - i);
                const double dir = up ? 1.0 : -1.0;
                const __m128 lo = _mm_cvtpd_ps(_mm_add_pd(
                    _mm_set1_pd(low),
                    _mm_mul_pd(_mm_set1_pd(slope), _mm_setr_pd(d, d + dir))));
                const __m128 hi = _mm_cvtpd_ps(_mm_add_pd(
                    _mm_set1_pd(low),
                    _mm_mul_pd(_mm_set1_pd(slope),
                               _mm_setr_pd(d + 2.0 * dir, d + 3.0 * dir))));
                gain = _mm_movelh_ps(lo, hi);
            }
            output_gain_detail::MulGain4(channels, channelCount, offset + i,
                                         gain);
        }
#endif
        for (; i < count; ++i) {
            const std::uint64_t distance =
                lowDistance + (up ? i : count - 1 - i);
            const float gain =
                static_cast<float>(low + slope * static_cast<double>(distance));
            output_gain_detail::MulGain(channels, channelCount, offset + i,
                                        gain);
        }
    }
};

struct GainRamp {
    float current = 1.0f; // gain in effect at the start of the next block
    float target = 1.0f;  // gain requested for the end of the next block
    bool started = false; // false until the first block snaps

    // Initial voice gain: both ends snap, no ramp.
    void SnapTo(float gain) {
        current = gain;
        target = gain;
        started = true;
    }

    // Requests a new gain; the next block ramps current -> target.
    void SetTarget(float gain) {
        if (!started) {
            SnapTo(gain);
            return;
        }
        target = gain;
    }

    // Multiplies the ramp into planar channels [offset, offset+frames).
    // Sample n scales by current + (target-current)*(n+1)/frames, so the
    // block's last sample lands exactly on target; current becomes target
    // for the following block.
    void ApplyBlock(float* const* channels, std::uint32_t channelCount,
                    std::uint32_t offset, std::uint32_t frames) {
        if (frames == 0) {
            return;
        }
        if (current == target) {
            for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
                float* p = channels[ch] + offset;
                for (std::uint32_t i = 0; i < frames; ++i) {
                    p[i] *= current;
                }
            }
            return;
        }
        const float step = (target - current) / static_cast<float>(frames);
        for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
            float* p = channels[ch] + offset;
            float g = current;
            for (std::uint32_t i = 0; i < frames; ++i) {
                g += step;
                p[i] *= g;
            }
        }
        current = target;
    }
};

} // namespace harken

#endif // HARKEN_DSP_GAIN_RAMP_H
