#ifndef HARKEN_RENDER_SIGNAL_BLOCK_EXCHANGE_H
#define HARKEN_RENDER_SIGNAL_BLOCK_EXCHANGE_H

#include "harken/core/ChannelLayout.h"
#include "harken/dsp/GainRamp.h"
#include <harken/ObjectId.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cassert>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace harken {

struct SignalBlockFormat {
    std::uint32_t channels = 0;
    std::uint32_t capacityFrames = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t layout = 0;
};

struct SignalBlockIdentity {
    std::uint64_t world = 0;
    std::uint64_t transport = 0;
    std::uint64_t deviceFormat = 0;
    std::uint64_t epoch = 0;
    std::uint64_t frameBegin = 0;
    std::uint32_t frames = 0;
};

struct SignalConnectionInput {
    std::uint32_t source = 0;
    std::uint32_t target = 0;
    float level = 1.0f;
    VnObjectId stableId{};
};

// Owned by a prepared relation plan. Logic prepares unpublished storage; the
// audio coordinator exclusively executes it. Its owner provides publication
// and retirement fences. Worker jobs never access these mutable banks.
class SignalBlockExchange {
public:
    SignalBlockExchange() = default;
    SignalBlockExchange(const SignalBlockExchange&) = delete;
    SignalBlockExchange& operator=(const SignalBlockExchange&) = delete;

    bool Prepare(const SignalBlockFormat& format, std::uint32_t sources,
                 std::uint32_t targets,
                 std::span<const SignalConnectionInput> connections) noexcept {
        if (m_open || format.channels == 0 || format.capacityFrames == 0 ||
            format.sampleRate == 0 ||
            LayoutChannelCount(format.layout) != format.channels ||
            connections.size() > UINT32_MAX) return false;
        std::size_t planeSamples = 0, sourceSamples = 0, targetSamples = 0;
        if (!Multiply(format.channels, format.capacityFrames, planeSamples) ||
            !Multiply(planeSamples, sources, sourceSamples) ||
            !Multiply(sourceSamples, 2, sourceSamples) ||
            !Multiply(planeSamples, targets, targetSamples) ||
            sourceSamples > std::vector<float>().max_size() ||
            targetSamples > std::vector<float>().max_size() ||
            planeSamples > std::vector<double>().max_size()) return false;
        for (const auto& row : connections) {
            if (row.source >= sources || row.target >= targets ||
                !ValidLevel(row.level)) return false;
        }
        try {
            SignalBlockExchange next;
            next.m_format = format;
            next.m_sources = sources;
            next.m_targets = targets;
            next.m_planeSamples = planeSamples;
            next.m_sourceBank.resize(sourceSamples, 0.0f);
            next.m_targetPcm.resize(targetSamples, 0.0f);
            next.m_accumulator.resize(planeSamples, 0.0);
            next.m_gainScratch.resize(format.capacityFrames, 0.0f);
            next.m_captured.resize(sources, 0);
            next.m_connections.resize(connections.size());
            next.m_order.resize(connections.size());
            next.m_targetOffsets.resize(static_cast<std::size_t>(targets) + 1, 0);
            next.m_targetAlias.resize(targets);
            next.m_singletonRepresentative.resize(targets, UINT32_MAX);
            for (std::size_t i = 0; i < connections.size(); ++i) {
                const auto& input = connections[i];
                next.m_connections[i].source = input.source;
                next.m_connections[i].gain.Set(input.level);
                ++next.m_targetOffsets[input.target + 1];
            }
            for (std::size_t i = 1; i < next.m_targetOffsets.size(); ++i)
                next.m_targetOffsets[i] += next.m_targetOffsets[i - 1];
            auto cursor = next.m_targetOffsets;
            for (std::uint32_t i = 0; i < connections.size(); ++i)
                next.m_order[cursor[connections[i].target]++] = i;
            std::vector<std::uint32_t> representative(sources, UINT32_MAX);
            for (std::uint32_t target = 0; target < targets; ++target) {
                const auto first = next.m_targetOffsets[target];
                if (next.m_targetOffsets[target + 1] - first != 1) continue;
                const auto source = next.m_connections[next.m_order[first]].source;
                if (representative[source] == UINT32_MAX) representative[source] = target;
                next.m_singletonRepresentative[target] = representative[source];
            }
            next.m_prepared = true;
            Swap(next);
            return true;
        } catch (...) {
            return false;
        }
    }

    // Commands change a connection at the consumption boundary. The new
    // trajectory applies once to the preceding source block's PCM.
    bool SetLevel(std::uint32_t connection, float level,
                  std::uint64_t rampFrames) noexcept {
        if (m_open || connection >= m_connections.size() || !ValidLevel(level))
            return false;
        m_connections[connection].gain.Set(level, rampFrames);
        return true;
    }

    double ConnectionBound(std::uint32_t connection, std::uint32_t frames) const noexcept {
        if (connection >= m_connections.size()) return std::numeric_limits<double>::infinity();
        return m_connections[connection].gain.AbsoluteBound(frames);
    }

    bool BeginBlock(const SignalBlockIdentity& identity) noexcept {
        if (!m_prepared || m_open || identity.frames == 0 ||
            identity.frames > m_format.capacityFrames ||
            identity.frameBegin > UINT64_MAX - identity.frames) return false;
        const bool read = m_committed &&
            identity.world == m_previous.world &&
            identity.transport == m_previous.transport &&
            identity.deviceFormat == m_previous.deviceFormat &&
            m_previous.epoch != UINT64_MAX &&
            identity.epoch == m_previous.epoch + 1 &&
            identity.frames == m_previous.frames &&
            identity.frameBegin == m_previous.frameBegin + m_previous.frames;
        m_current = identity;
        m_open = true;
        std::fill(m_captured.begin(), m_captured.end(), 0);
        // Compare all trajectories before advancing any connection. Each
        // target retains its independent clock even when it borrows PCM.
        for (std::uint32_t target = 0; target < m_targets; ++target) {
            m_targetAlias[target] = target;
            const auto representative = m_singletonRepresentative[target];
            if (!read || representative == UINT32_MAX || representative == target) continue;
            const auto& a = m_connections[m_order[m_targetOffsets[target]]].gain;
            const auto& b = m_connections[m_order[m_targetOffsets[representative]]].gain;
            if (SameTrajectory(a, b)) m_targetAlias[target] = representative;
        }
        for (std::uint32_t target = 0; target < m_targets; ++target) {
            const auto first = m_targetOffsets[target], end = m_targetOffsets[target + 1];
            if (m_targetAlias[target] != target) {
                m_connections[m_order[first]].gain.Advance(identity.frames);
                continue;
            }
            if (read && end - first == 1) {
                auto& connection = m_connections[m_order[first]];
                const auto* source = Source(m_readBank, connection.source);
                auto* output = m_targetPcm.data() + target * m_planeSamples;
                const auto frames = identity.frames, channels = m_format.channels;
                const auto stride = m_format.capacityFrames;
                // A single input needs no intermediate double accumulation buffer.
                // Keep the initial zero addition and final saturation/rounding.
                const auto write = [&](auto gainAt) {
                    constexpr double limit = std::numeric_limits<float>::max();
                    for (std::uint32_t ch = 0; ch < channels; ++ch) {
                        const auto base = static_cast<std::size_t>(ch) * stride;
                        const auto* samples = source + base;
                        auto* destination = output + base;
                        for (std::uint32_t n = 0; n < frames; ++n) {
                            const double sum = 0.0 + static_cast<double>(samples[n]) * gainAt(n);
                            destination[n] = static_cast<float>(std::clamp(sum, -limit, limit));
                        }
                        std::fill(destination + frames, destination + stride, 0.0f);
                    }
                };
                if (connection.gain.remainingFrames == 0) {
                    const double gain = connection.gain.target;
                    write([gain](std::uint32_t) { return gain; });
                } else {
                    connection.gain.Fill(m_gainScratch.data(), frames, 0);
                    const auto* gains = m_gainScratch.data();
                    write([gains](std::uint32_t n) { return static_cast<double>(gains[n]); });
                }
                connection.gain.Advance(frames);
                continue;
            }
            std::fill(m_accumulator.begin(), m_accumulator.end(), 0.0);
            for (auto p = m_targetOffsets[target]; p < m_targetOffsets[target + 1]; ++p) {
                auto& connection = m_connections[m_order[p]];
                if (read) {
                    const float* source = Source(m_readBank, connection.source);
                    const auto frames = identity.frames, channels = m_format.channels;
                    const auto stride = m_format.capacityFrames;
                    const auto accumulate = [&](auto gainAt) {
                        for (std::uint32_t ch = 0; ch < channels; ++ch) {
                            const auto base = static_cast<std::size_t>(ch) * stride;
                            auto* accumulator = m_accumulator.data() + base;
                            const auto* samples = source + base;
                            for (std::uint32_t n = 0; n < frames; ++n)
                                accumulator[n] += static_cast<double>(samples[n]) * gainAt(n);
                        }
                    };
                    if (connection.gain.remainingFrames == 0) {
                        const double gain = connection.gain.target;
                        accumulate([gain](std::uint32_t) { return gain; });
                    } else {
                        connection.gain.Fill(m_gainScratch.data(), frames, 0);
                        const auto* gains = m_gainScratch.data();
                        accumulate([gains](std::uint32_t n) { return static_cast<double>(gains[n]); });
                    }
                }
                connection.gain.Advance(identity.frames);
            }
            float* output = m_targetPcm.data() + target * m_planeSamples;
            constexpr double limit = std::numeric_limits<float>::max();
            const auto count = m_planeSamples;
            const auto* accumulator = m_accumulator.data();
            for (std::size_t n = 0; n < count; ++n)
                output[n] = static_cast<float>(std::clamp(accumulator[n], -limit, limit));
        }
        return true;
    }

    // Source planes are block-relative. Validate every channel before copying
    // any data, so malformed capture can be retried without a partial source.
    bool Capture(std::uint32_t source, std::span<const float* const> planes,
                 std::uint32_t offset, std::uint32_t frames) noexcept {
        if (!m_open || source >= m_sources || m_captured[source] ||
            planes.size() != m_format.channels || offset > m_current.frames ||
            frames > m_current.frames - offset) return false;
        for (const float* plane : planes) {
            if (plane == nullptr) return false;
            bool finite = true;
            for (std::uint32_t n = offset; n < offset + frames; ++n)
                finite &= std::isfinite(plane[n]);
            if (!finite) return false;
        }
        float* output = Source(m_writeBank, source);
        for (std::uint32_t ch = 0; ch < m_format.channels; ++ch) {
            auto* destination = output + static_cast<std::size_t>(ch) * m_format.capacityFrames;
            std::fill_n(destination, offset, 0.0f);
            std::copy_n(planes[ch] + offset, frames, destination + offset);
            std::fill(destination + offset + frames, destination + m_format.capacityFrames, 0.0f);
        }
        m_captured[source] = 1;
        return true;
    }

    // Borrowed only until CommitBlock, AbortBlock or Invalidate. Repeated
    // consumers observe the same raw PCM without advancing a gain or detector.
    const float* Target(std::uint32_t target, std::uint32_t channel) const noexcept {
        if (!m_open || target >= m_targets || channel >= m_format.channels) return nullptr;
        return m_targetPcm.data() + m_targetAlias[target] * m_planeSamples +
            static_cast<std::size_t>(channel) * m_format.capacityFrames;
    }
    bool HasTargetInput(std::uint32_t target) const noexcept {
        return target < m_targets && m_targetOffsets[target] != m_targetOffsets[target + 1];
    }

    bool CommitBlock() noexcept {
        if (!m_open) return false;
        // Captures overwrite their complete planes. Only absent producers need
        // clearing before this bank becomes visible in the following block.
        for (std::uint32_t source = 0; source < m_sources; ++source)
            if (!m_captured[source]) std::fill_n(Source(m_writeBank, source), m_planeSamples, 0.0f);
        m_previous = m_current;
        m_readBank = m_writeBank;
        m_writeBank = 1 - m_readBank;
        m_committed = true;
        m_open = false;
        return true;
    }

    // A successful Begin consumed audio time, including its send ramps. Abort
    // discards PCM but does not roll that time back. Deferral must keep the
    // open block, not abort it or begin it again.
    void AbortBlock() noexcept { m_open = false; m_committed = false; }
    void Invalidate() noexcept { AbortBlock(); }

private:
    friend class SignalRenderPlan;
    // The plan has validated every finite member before summing in double.
    // Even size_t's maximum member count times FLT_MAX fits in double.
    // Write the rounded sum directly into the bank, without a temporary PCM copy.
    bool CaptureFiniteSum(std::uint32_t source, const double* sum, std::uint32_t frames, std::uint32_t silentChannels) noexcept {
        if (!m_open || source >= m_sources || m_captured[source] || sum == nullptr ||
            frames > m_current.frames) return false;
        auto* output = Source(m_writeBank, source);
        const auto channels = m_format.channels, stride = m_format.capacityFrames;
        constexpr double limit = std::numeric_limits<float>::max();
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            const auto base = static_cast<std::size_t>(ch) * stride;
            const auto* samples = sum + base;
            auto* destination = output + base;
            if ((silentChannels & (1u << ch)) != 0) {
                std::fill_n(destination, stride, 0.0f);
                continue;
            }
            for (std::uint32_t n = 0; n < frames; ++n) {
                assert(std::isfinite(samples[n]));
                destination[n] = static_cast<float>(std::clamp(samples[n], -limit, limit));
            }
            std::fill(destination + frames, destination + stride, 0.0f);
        }
        m_captured[source] = 1;
        return true;
    }
    void Swap(SignalBlockExchange& other) noexcept {
        using std::swap;
        swap(m_format, other.m_format);
        swap(m_previous, other.m_previous);
        swap(m_current, other.m_current);
        swap(m_sources, other.m_sources);
        swap(m_targets, other.m_targets);
        swap(m_readBank, other.m_readBank);
        swap(m_writeBank, other.m_writeBank);
        swap(m_planeSamples, other.m_planeSamples);
        swap(m_prepared, other.m_prepared);
        swap(m_open, other.m_open);
        swap(m_committed, other.m_committed);
        m_sourceBank.swap(other.m_sourceBank);
        m_targetPcm.swap(other.m_targetPcm);
        m_gainScratch.swap(other.m_gainScratch);
        m_accumulator.swap(other.m_accumulator);
        m_connections.swap(other.m_connections);
        m_order.swap(other.m_order);
        m_targetOffsets.swap(other.m_targetOffsets);
        m_targetAlias.swap(other.m_targetAlias);
        m_singletonRepresentative.swap(other.m_singletonRepresentative);
        m_captured.swap(other.m_captured);
    }
    struct Connection { std::uint32_t source = 0; OutputGain gain{}; };
    static bool SameTrajectory(const OutputGain& a, const OutputGain& b) noexcept {
        if (std::bit_cast<std::uint32_t>(a.target) != std::bit_cast<std::uint32_t>(b.target) ||
            a.remainingFrames != b.remainingFrames) return false;
        if (a.remainingFrames == 0) return true;
        return std::bit_cast<std::uint32_t>(a.start) == std::bit_cast<std::uint32_t>(b.start) &&
            std::bit_cast<std::uint64_t>(a.step) == std::bit_cast<std::uint64_t>(b.step) &&
            a.totalFrames == b.totalFrames;
    }
    static bool ValidLevel(float level) noexcept {
        return std::isfinite(level) && level >= 0.0f;
    }
    static bool Multiply(std::size_t a, std::size_t b, std::size_t& result) noexcept {
        if (b != 0 && a > std::numeric_limits<std::size_t>::max() / b) return false;
        result = a * b;
        return true;
    }
    float* Source(std::uint32_t bank, std::uint32_t source) noexcept {
        return m_sourceBank.data() +
            (static_cast<std::size_t>(bank) * m_sources + source) * m_planeSamples;
    }
    SignalBlockFormat m_format{};
    SignalBlockIdentity m_previous{}, m_current{};
    std::uint32_t m_sources = 0, m_targets = 0, m_readBank = 0, m_writeBank = 1;
    std::size_t m_planeSamples = 0;
    bool m_prepared = false, m_open = false, m_committed = false;
    std::vector<float> m_sourceBank, m_targetPcm, m_gainScratch;
    std::vector<double> m_accumulator;
    std::vector<Connection> m_connections;
    std::vector<std::uint32_t> m_order, m_targetOffsets, m_targetAlias, m_singletonRepresentative;
    std::vector<std::uint8_t> m_captured;
};

} // namespace harken

#endif // HARKEN_RENDER_SIGNAL_BLOCK_EXCHANGE_H
