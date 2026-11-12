#ifndef HARKEN_RENDER_DUCKING_RECEIVER_H
#define HARKEN_RENDER_DUCKING_RECEIVER_H

#include "harken/core/EngineConfig.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#endif

namespace harken {

// Exact zero detection: retain subnormal samples and ignore only the sign of zero.
inline bool SignalPlaneIsSilent(const float* samples, std::uint32_t frames) noexcept {
    if (frames == 0) return true;
    if ((std::bit_cast<std::uint32_t>(samples[0]) & 0x7fffffffu) != 0) return false;
    std::uint32_t bits = 0;
    for (std::uint32_t n = 0; n < frames; ++n) bits |= std::bit_cast<std::uint32_t>(samples[n]);
    return (bits & 0x7fffffffu) == 0;
}


inline constexpr double kDuckingBaselineSlope = 5.6568542494923801952;
inline constexpr float kDuckingDefaultAttackMs = 4.156241f;
inline constexpr float kDuckingDefaultReleaseMs = 1.031215f;

struct DuckingPreparedParameters {
    std::uint32_t sampleRate = 0;
    double energyRise = 0;
    double energyFall = 0;
    double mainFall = 0;
    double attack = 0;
    double release = 0;
};

// Parameter-domain transcendental work belongs to unpublished preparation.
inline bool PrepareDuckingParameters(std::uint32_t sampleRate, float attackMs,
                                    float releaseMs,
                                    DuckingPreparedParameters& out) noexcept {
    if (sampleRate == 0 || !std::isfinite(attackMs) ||
        !std::isfinite(releaseMs) || attackMs < .1f || attackMs > 500.0f ||
        releaseMs < .1f || releaseMs > 5000.0f) return false;
    DuckingPreparedParameters next;
    next.sampleRate = sampleRate;
    next.energyRise = std::exp(-1000.0 / (sampleRate * 2.0));
    next.energyFall = std::exp(-1000.0 / (sampleRate * .5));
    next.mainFall = std::pow(.990, 48000.0 / sampleRate);
    next.attack = std::exp(-1000.0 / (sampleRate * static_cast<double>(attackMs)));
    next.release = std::exp(-1000.0 / (sampleRate * static_cast<double>(releaseMs)));
    out = next;
    return true;
}

// One instance belongs to a target definition, not to an input connection or
// a voice. It produces one trajectory shared by all target members/channels.
class DuckingReceiver {
public:
    void SetParameters(const DuckingPreparedParameters& parameters) noexcept {
        if (parameters.sampleRate != m_parameters.sampleRate) Reset();
        m_parameters = parameters;
    }

    void Reset() noexcept {
        m_energy.fill(0);
        m_mainEnvelope = m_preReference = m_postReference = 0;
        m_gain = 1;
        m_channels = 0;
    }

    // Audio-boundary adoption keeps the newly prepared parameter values.
    void AdoptStateFrom(const DuckingReceiver& previous) noexcept {
        assert(m_parameters.sampleRate == previous.m_parameters.sampleRate);
        m_energy = previous.m_energy;
        m_mainEnvelope = previous.m_mainEnvelope;
        m_preReference = previous.m_preReference;
        m_postReference = previous.m_postReference;
        m_gain = previous.m_gain;
        m_channels = previous.m_channels;
    }

    // References are admitted members' absolute sums at pre/post Out Vol,
    // before output filters. They affect only cyclic receivers. No signed
    // sidechain sample is replaced by an absolute per-source detector.
    // The owner derives slope from the complete prepared feedback component.
    bool Process(const float* const* main, const float* const* sidechain,
                 std::uint32_t channels, std::uint32_t frames,
                 const double* preReference, const double* postReference,
                 double slope, double* gains, bool boundMain = false) noexcept {
        const bool feedback = preReference != nullptr && postReference != nullptr;
        if (m_parameters.sampleRate == 0 || main == nullptr || gains == nullptr ||
            channels == 0 || channels > kMaxChannels ||
            ((preReference == nullptr) != (postReference == nullptr)) ||
            !std::isfinite(slope) || slope < 0 || slope > kDuckingBaselineSlope)
            return false;
        for (std::uint32_t ch = 0; ch < channels; ++ch)
            if (main[ch] == nullptr || (sidechain != nullptr && sidechain[ch] == nullptr))
                return false;
        if (channels != m_channels) {
            Reset();
            m_channels = channels;
        }
        std::uint32_t active[kMaxChannels]{};
        std::uint32_t activeCount = 0;
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            if (channels > 2 && m_energy[ch] == 0 && SignalPlaneIsSilent(main[ch], frames) &&
                (sidechain == nullptr || SignalPlaneIsSilent(sidechain[ch], frames))) continue;
            active[activeCount++] = ch;
        }
        const auto process = [&]<bool Two, bool BoundMain>() {
#if defined(_M_X64) || defined(__x86_64__)
            const auto ch0 = active[0], ch1 = active[1];
            __m128d energyState = _mm_set_pd(m_energy[ch1], m_energy[ch0]);
            const auto rise = _mm_set1_pd(m_parameters.energyRise), fall = _mm_set1_pd(m_parameters.energyFall);
            const auto magnitudeMask = _mm_castsi128_pd(_mm_set1_epi64x(0x7fffffffffffffffLL));
            const auto limit = _mm_set1_pd(1.7976931348623157e308);
            __m128d pairedEnergy[kMaxChannels / 2];
            if constexpr (!Two) {
                for (std::uint32_t i = 0; i + 1 < activeCount; i += 2)
                    pairedEnergy[i / 2] = _mm_set_pd(m_energy[active[i + 1]], m_energy[active[i]]);
            }
#endif
            double mainEnvelope = m_mainEnvelope, preEnvelope = m_preReference, postEnvelope = m_postReference, gain = m_gain;
            for (std::uint32_t n = 0; n < frames; ++n) {
                double mainPeak = 0, sideEnergy = 0;
#if defined(_M_X64) || defined(__x86_64__)
                if constexpr (Two) {
                    auto mains = _mm_set_pd(main[ch1][n], main[ch0][n]);
                    mains = _mm_and_pd(mains, magnitudeMask);
                    if constexpr (BoundMain) {
                        const auto ordered = _mm_cmpord_pd(mains, mains);
                        mains = _mm_and_pd(_mm_min_pd(mains,
                            _mm_set1_pd(std::numeric_limits<float>::max())), ordered);
                    } else {
                        mains = _mm_and_pd(mains, _mm_cmple_pd(mains, limit));
                    }
                    mainPeak = _mm_cvtsd_f64(_mm_max_sd(mains, _mm_unpackhi_pd(mains, mains)));
                    auto sides = sidechain != nullptr ? _mm_set_pd(sidechain[ch1][n], sidechain[ch0][n]) : _mm_setzero_pd();
                    sides = _mm_and_pd(sides, _mm_cmple_pd(_mm_and_pd(sides, magnitudeMask), limit));
                    const auto energies = _mm_mul_pd(sides, sides);
                    const auto rising = _mm_cmpgt_pd(energies, energyState);
                    const auto coefficient = _mm_or_pd(_mm_and_pd(rising, rise), _mm_andnot_pd(rising, fall));
                    energyState = _mm_add_pd(energies, _mm_mul_pd(coefficient, _mm_sub_pd(energyState, energies)));
                    sideEnergy = _mm_cvtsd_f64(_mm_max_sd(energyState, _mm_unpackhi_pd(energyState, energyState)));
                } else
#endif
                {
                std::uint32_t index = 0;
#if defined(_M_X64) || defined(__x86_64__)
                for (; index + 1 < activeCount; index += 2) {
                    const auto a = active[index], b = active[index + 1];
                    auto mains = _mm_and_pd(_mm_set_pd(main[b][n], main[a][n]), magnitudeMask);
                    if constexpr (BoundMain) {
                        const auto ordered = _mm_cmpord_pd(mains, mains);
                        mains = _mm_and_pd(_mm_min_pd(mains,
                            _mm_set1_pd(std::numeric_limits<float>::max())), ordered);
                    } else {
                        mains = _mm_and_pd(mains, _mm_cmple_pd(mains, limit));
                    }
                    mainPeak = std::max(mainPeak,
                        _mm_cvtsd_f64(_mm_max_sd(mains, _mm_unpackhi_pd(mains, mains))));
                    auto sides = sidechain != nullptr ? _mm_set_pd(sidechain[b][n], sidechain[a][n]) : _mm_setzero_pd();
                    sides = _mm_and_pd(sides, _mm_cmple_pd(_mm_and_pd(sides, magnitudeMask), limit));
                    const auto energies = _mm_mul_pd(sides, sides);
                    auto& state = pairedEnergy[index / 2];
                    const auto rising = _mm_cmpgt_pd(energies, state);
                    const auto coefficient = _mm_or_pd(_mm_and_pd(rising, rise), _mm_andnot_pd(rising, fall));
                    state = _mm_add_pd(energies, _mm_mul_pd(coefficient, _mm_sub_pd(state, energies)));
                    sideEnergy = std::max(sideEnergy,
                        _mm_cvtsd_f64(_mm_max_sd(state, _mm_unpackhi_pd(state, state))));
                }
#endif
                for (; index < activeCount; ++index) {
                    const auto ch = active[index];
                    double mainSample = main[ch][n];
                    if constexpr (BoundMain) {
                        constexpr double maxSample = std::numeric_limits<float>::max();
                        mainSample = std::clamp(mainSample, -maxSample, maxSample);
                    }
                    if (std::isfinite(mainSample)) mainPeak = std::max(mainPeak, std::abs(mainSample));
                    double side = sidechain != nullptr ? sidechain[ch][n] : 0;
                    if (!std::isfinite(side)) side = 0;
                    const double energy = side * side;
                    const double coefficient = energy > m_energy[ch]
                        ? m_parameters.energyRise : m_parameters.energyFall;
                    m_energy[ch] = energy + coefficient * (m_energy[ch] - energy);
                    sideEnergy = std::max(sideEnergy, m_energy[ch]);
                }
                }
                mainEnvelope = std::max(mainPeak, mainEnvelope * m_parameters.mainFall);
                double denominator = std::max(mainEnvelope, 1e-9);
                double effectiveSlope = kDuckingBaselineSlope;
                // A temporary feedforward interval still consumes audio time.
                // Do not resurrect a frozen reference when a return path reopens.
                preEnvelope *= m_parameters.mainFall;
                postEnvelope *= m_parameters.mainFall;
                if (feedback) {
                    const double pre = std::isfinite(preReference[n])
                        ? std::max(0.0, preReference[n]) : 0;
                    const double post = std::isfinite(postReference[n])
                        ? std::max(0.0, postReference[n]) : 0;
                    preEnvelope = std::max(pre, preEnvelope);
                    postEnvelope = std::max(post, postEnvelope);
                    denominator = std::max(std::max(denominator, preEnvelope), postEnvelope);
                    effectiveSlope = slope;
                }
                // A removed input is distinct from a still-connected zero signal.
                // Consume detector time above, but do not re-attack stale energy
                // when leaving a feedback component after its last input is gone.
                const double desired = sidechain == nullptr ? 1.0 : std::max(0.0,
                    1.0 - effectiveSlope * std::sqrt(sideEnergy) / denominator);
                const double coefficient = desired < gain
                    ? m_parameters.attack : m_parameters.release;
                gain = desired + coefficient * (gain - desired);
                gains[n] = gain;
            }
#if defined(_M_X64) || defined(__x86_64__)
            if constexpr (Two) {
                m_energy[ch0] = _mm_cvtsd_f64(energyState);
                m_energy[ch1] = _mm_cvtsd_f64(_mm_unpackhi_pd(energyState, energyState));
            } else {
                for (std::uint32_t i = 0; i + 1 < activeCount; i += 2) {
                    m_energy[active[i]] = _mm_cvtsd_f64(pairedEnergy[i / 2]);
                    m_energy[active[i + 1]] = _mm_cvtsd_f64(_mm_unpackhi_pd(pairedEnergy[i / 2], pairedEnergy[i / 2]));
                }
            }
#endif
            m_mainEnvelope = mainEnvelope;
            m_preReference = preEnvelope;
            m_postReference = postEnvelope;
            m_gain = gain;
        };
#if defined(_M_X64) || defined(__x86_64__)
        if (activeCount == 2) {
            if (boundMain) process.template operator()<true, true>();
            else process.template operator()<true, false>();
        } else
#endif
        {
            if (boundMain) process.template operator()<false, true>();
            else process.template operator()<false, false>();
        }
        return true;
    }

    // Do not narrow the gain before multiplication: a deep gain can still
    // produce representable PCM when multiplied by a large finite sample.
    static void Apply(float* samples, const double* gains, std::uint32_t frames) noexcept {
        for (std::uint32_t n = 0; n < frames; ++n)
            samples[n] = static_cast<float>(static_cast<double>(samples[n]) * gains[n]);
    }
    static void Apply(float* const* planes, std::uint32_t channels,
                      std::uint32_t frames, const double* gains) noexcept {
        for (std::uint32_t ch = 0; ch < channels; ++ch)
            Apply(planes[ch], gains, frames);
    }

private:
    DuckingPreparedParameters m_parameters{};
    std::array<double, kMaxChannels> m_energy{};
    double m_mainEnvelope = 0, m_preReference = 0, m_postReference = 0, m_gain = 1;
    std::uint32_t m_channels = 0;
};

} // namespace harken

#endif // HARKEN_RENDER_DUCKING_RECEIVER_H
