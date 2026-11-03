#ifndef HARKEN_RENDER_DUCKING_RECEIVER_H
#define HARKEN_RENDER_DUCKING_RECEIVER_H

#include "harken/core/EngineConfig.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace harken {

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

    // References are admitted members' absolute sums at pre/post Out Vol,
    // before output filters. They affect only cyclic receivers. No signed
    // sidechain sample is replaced by an absolute per-source detector.
    // The owner derives slope from the complete prepared feedback component.
    bool Process(const float* const* main, const float* const* sidechain,
                 std::uint32_t channels, std::uint32_t frames,
                 const double* preReference, const double* postReference,
                 double slope, double* gains) noexcept {
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
        for (std::uint32_t n = 0; n < frames; ++n) {
            double mainPeak = 0, sideEnergy = 0;
            for (std::uint32_t ch = 0; ch < channels; ++ch) {
                const double mainSample = main[ch][n];
                if (std::isfinite(mainSample)) mainPeak = std::max(mainPeak, std::abs(mainSample));
                double side = sidechain != nullptr ? sidechain[ch][n] : 0;
                if (!std::isfinite(side)) side = 0;
                const double energy = side * side;
                const double coefficient = energy > m_energy[ch]
                    ? m_parameters.energyRise : m_parameters.energyFall;
                m_energy[ch] = energy + coefficient * (m_energy[ch] - energy);
                sideEnergy = std::max(sideEnergy, m_energy[ch]);
            }
            m_mainEnvelope = std::max(mainPeak, m_mainEnvelope * m_parameters.mainFall);
            double denominator = std::max(m_mainEnvelope, 1e-9);
            double effectiveSlope = kDuckingBaselineSlope;
            // A temporary feedforward interval still consumes audio time.
            // Do not resurrect a frozen reference when a return path reopens.
            m_preReference *= m_parameters.mainFall;
            m_postReference *= m_parameters.mainFall;
            if (feedback) {
                const double pre = std::isfinite(preReference[n])
                    ? std::max(0.0, preReference[n]) : 0;
                const double post = std::isfinite(postReference[n])
                    ? std::max(0.0, postReference[n]) : 0;
                m_preReference = std::max(pre, m_preReference);
                m_postReference = std::max(post, m_postReference);
                denominator = std::max({denominator, m_preReference, m_postReference});
                effectiveSlope = slope;
            }
            const double desired = std::max(0.0,
                1.0 - effectiveSlope * std::sqrt(sideEnergy) / denominator);
            const double coefficient = desired < m_gain
                ? m_parameters.attack : m_parameters.release;
            m_gain = desired + coefficient * (m_gain - desired);
            gains[n] = m_gain;
        }
        return true;
    }

    // Do not narrow the gain before multiplication: a deep gain can still
    // produce representable PCM when multiplied by a large finite sample.
    static void Apply(float* const* planes, std::uint32_t channels,
                      std::uint32_t frames, const double* gains) noexcept {
        for (std::uint32_t ch = 0; ch < channels; ++ch)
            for (std::uint32_t n = 0; n < frames; ++n)
                planes[ch][n] = static_cast<float>(static_cast<double>(planes[ch][n]) * gains[n]);
    }

private:
    DuckingPreparedParameters m_parameters{};
    std::array<double, kMaxChannels> m_energy{};
    double m_mainEnvelope = 0, m_preReference = 0, m_postReference = 0, m_gain = 1;
    std::uint32_t m_channels = 0;
};

} // namespace harken

#endif // HARKEN_RENDER_DUCKING_RECEIVER_H
