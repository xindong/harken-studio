#ifndef HARKEN_RENDER_SIGNAL_RENDER_PLAN_H
#define HARKEN_RENDER_SIGNAL_RENDER_PLAN_H

#include "harken/core/CurrentWorld.h"
#include "harken/render/SignalBlockExchange.h"
#include "harken/render/SignalFeedbackPlan.h"

#include <cassert>
#include <memory>

namespace harken {

struct SignalRenderNodeInput {
    SignalEndpointKindV1 kind = SignalEndpointKindV1::Bus;
    bool receiver = false;
    float attackMs = kDuckingDefaultAttackMs;
    float releaseMs = kDuckingDefaultReleaseMs;
};

// The selected definition membership is prepared with the Voice/Arm, not
// discovered from the world on audio. Plan-local node indices may repeat in
// an authored path; an actual member contributes once to each definition.
struct SignalMemberImage {
    float* const* planes = nullptr;
    const OutputGain* outputGain = nullptr;
    std::span<const std::uint32_t> nodes{};
    std::uint32_t offset = 0;
    std::uint32_t frames = 0;
};

// Prepared off audio and exclusively executed by the audio coordinator.
// Publication and retirement belong to the existing topology-plan owner.
// All participating Voice cascades finish before ProcessContent; Bus stages
// then execute in their existing ordinary routing order. Workers never mutate
// this object. Structural compilation and member identity are owner inputs.
class SignalRenderPlan {
public:
    static std::unique_ptr<SignalRenderPlan> Prepare(
        const SignalBlockFormat& format, std::span<const SignalRenderNodeInput> nodes,
        std::span<const SignalConnectionInput> relations,
        std::span<const SignalFeedbackEdge> paths) noexcept {
        if (nodes.size() > UINT32_MAX) return {};
        try {
            auto next = std::make_unique<SignalRenderPlan>();
            if (!next->feedback_.Prepare(static_cast<std::uint32_t>(nodes.size()), paths)) return {};
            next->nodes_.resize(nodes.size());
            std::uint32_t sources = 0, targets = 0;
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                const auto& input = nodes[i];
                auto& node = next->nodes_[i];
                node.kind = input.kind;
                if (input.kind != SignalEndpointKindV1::Bus &&
                    input.kind != SignalEndpointKindV1::Sound &&
                    input.kind != SignalEndpointKindV1::Container) return {};
                if (input.receiver) {
                    DuckingPreparedParameters parameters;
                    if (!PrepareDuckingParameters(format.sampleRate, input.attackMs,
                                                   input.releaseMs, parameters)) return {};
                    node.receiver.SetParameters(parameters);
                    node.target = targets++;
                }
            }
            std::vector<SignalConnectionInput> connections;
            connections.reserve(relations.size());
            for (const auto& relation : relations) {
                if (relation.source >= nodes.size() || relation.target >= nodes.size()) return {};
                auto& source = next->nodes_[relation.source];
                const auto target = next->nodes_[relation.target].target;
                if (target == UINT32_MAX) return {};
                if (source.source == UINT32_MAX) source.source = sources++;
                connections.push_back({source.source, target, relation.level});
            }
            if (!next->exchange_.Prepare(format, sources, targets, connections)) return {};
            next->format_ = format;
            const auto samples = static_cast<std::size_t>(format.channels) * format.capacityFrames;
            next->sum_.resize(samples);
            next->absolutePre_.resize(samples);
            next->absolutePost_.resize(samples);
            next->main_.resize(samples);
            next->gain_.resize(format.capacityFrames);
            next->pre_.resize(format.capacityFrames);
            next->post_.resize(format.capacityFrames);
            return next;
        } catch (...) { return {}; }
    }

    bool UpdateBounds(std::span<const double> bounds) noexcept {
        return !open_ && feedback_.UpdateBounds(bounds);
    }

    bool BeginBlock(const SignalBlockIdentity& identity) noexcept {
        if (open_ || !exchange_.BeginBlock(identity)) return false;
        const bool continuous = committed_ && identity.world == previous_.world &&
            identity.transport == previous_.transport && identity.deviceFormat == previous_.deviceFormat &&
            previous_.epoch != UINT64_MAX && identity.epoch == previous_.epoch + 1 &&
            identity.frames == previous_.frames &&
            identity.frameBegin == previous_.frameBegin + previous_.frames;
        if (!continuous) for (auto& node : nodes_) node.receiver.Reset();
        for (auto& node : nodes_) node.processed = false;
        current_ = identity;
        open_ = true;
        return true;
    }

    // Member images are pre-Out Vol and already include their real spatial
    // mapping. A Sound receives before its ancestors. Container stages see
    // member output after deeper stages, without reapplying author properties.
    bool ProcessContent(std::span<const SignalMemberImage> members) noexcept {
        if (!open_) return false;
        for (const auto& member : members) {
            if (member.planes == nullptr || member.outputGain == nullptr ||
                member.offset > current_.frames || member.frames > current_.frames - member.offset)
                return false;
            for (std::uint32_t ch = 0; ch < format_.channels; ++ch)
                if (member.planes[ch] == nullptr) return false;
            for (auto node : member.nodes)
                if (node >= nodes_.size() || nodes_[node].kind == SignalEndpointKindV1::Bus)
                    return false;
        }
        for (const auto index : feedback_.NormalOrder()) {
            auto& node = nodes_[index];
            if (node.kind == SignalEndpointKindV1::Bus || node.processed) continue;
            ClearSums();
            for (const auto& member : members) {
                if (!Contains(member.nodes, index)) continue;
                for (std::uint32_t ch = 0; ch < format_.channels; ++ch) {
                    const auto base = static_cast<std::size_t>(ch) * format_.capacityFrames;
                    for (std::uint32_t n = 0; n < member.frames; ++n) {
                        const auto frame = member.offset + n;
                        const double pre = member.planes[ch][frame];
                        const double post = pre * member.outputGain->At(n);
                        sum_[base + frame] += node.kind == SignalEndpointKindV1::Sound ? pre : post;
                        absolutePre_[base + frame] += std::abs(pre);
                        absolutePost_[base + frame] += std::abs(post);
                    }
                }
            }
            Receive(index);
            if (node.target != UINT32_MAX) {
                for (const auto& member : members) {
                    if (!Contains(member.nodes, index)) continue;
                    for (std::uint32_t ch = 0; ch < format_.channels; ++ch)
                        for (std::uint32_t n = member.offset; n < member.offset + member.frames; ++n)
                            member.planes[ch][n] = Narrow(static_cast<double>(member.planes[ch][n]) * gain_[n]);
                }
            }
            if (node.source != UINT32_MAX) {
                // Freeze this node's actual post-receiving/post-Out image before
                // any ancestor changes its members. Preserve float rounding at
                // the actual member multiplication, then sum in double.
                std::fill(sum_.begin(), sum_.end(), 0.0);
                for (const auto& member : members) {
                    if (!Contains(member.nodes, index)) continue;
                    for (std::uint32_t ch = 0; ch < format_.channels; ++ch) {
                        const auto base = static_cast<std::size_t>(ch) * format_.capacityFrames;
                        for (std::uint32_t n = 0; n < member.frames; ++n)
                            sum_[base + member.offset + n] +=
                                static_cast<double>(member.planes[ch][member.offset + n]) * member.outputGain->At(n);
                    }
                }
                StoreMain();
                const float* planes[kMaxChannels]{};
                MainPlanes(planes);
                if (!exchange_.Capture(node.source, {planes, format_.channels}, 0, current_.frames)) return false;
            }
            node.processed = true;
        }
        return true;
    }

    bool ProcessBus(std::uint32_t index, float* const* planes, const OutputGain& output) noexcept {
        if (!open_ || index >= nodes_.size() || nodes_[index].kind != SignalEndpointKindV1::Bus ||
            nodes_[index].processed || planes == nullptr) return false;
        for (std::uint32_t ch = 0; ch < format_.channels; ++ch)
            if (planes[ch] == nullptr) return false;
        ClearSums();
        for (std::uint32_t ch = 0; ch < format_.channels; ++ch) {
            const auto base = static_cast<std::size_t>(ch) * format_.capacityFrames;
            for (std::uint32_t n = 0; n < current_.frames; ++n) {
                const double sample = planes[ch][n];
                sum_[base + n] = sample;
                absolutePre_[base + n] = std::abs(sample);
                absolutePost_[base + n] = std::abs(sample * output.At(n));
            }
        }
        Receive(index);
        if (nodes_[index].target != UINT32_MAX)
            DuckingReceiver::Apply(planes, format_.channels, current_.frames, gain_.data());
        nodes_[index].processed = true;
        return true;
    }

    // Called at the Bus's actual post-Out Vol/output-filter port.
    bool CaptureBus(std::uint32_t index, std::span<const float* const> planes) noexcept {
        if (!open_ || index >= nodes_.size() || nodes_[index].kind != SignalEndpointKindV1::Bus ||
            !nodes_[index].processed) return false;
        const auto source = nodes_[index].source;
        return source == UINT32_MAX || exchange_.Capture(source, planes, 0, current_.frames);
    }

    bool CommitBlock() noexcept {
        if (!open_ || !exchange_.CommitBlock()) return false;
        previous_ = current_;
        committed_ = true;
        open_ = false;
        return true;
    }
    void Invalidate() noexcept {
        exchange_.Invalidate();
        open_ = committed_ = false;
        for (auto& node : nodes_) node.receiver.Reset();
    }

private:
    struct Node {
        SignalEndpointKindV1 kind = SignalEndpointKindV1::Bus;
        std::uint32_t source = UINT32_MAX, target = UINT32_MAX;
        DuckingReceiver receiver;
        bool processed = false;
    };
    static bool Contains(std::span<const std::uint32_t> nodes, std::uint32_t node) noexcept {
        return std::find(nodes.begin(), nodes.end(), node) != nodes.end();
    }
    static float Narrow(double sample) noexcept {
        constexpr double limit = std::numeric_limits<float>::max();
        return static_cast<float>(std::clamp(sample, -limit, limit));
    }
    void ClearSums() noexcept {
        std::fill(sum_.begin(), sum_.end(), 0.0);
        std::fill(absolutePre_.begin(), absolutePre_.end(), 0.0);
        std::fill(absolutePost_.begin(), absolutePost_.end(), 0.0);
    }
    void StoreMain() noexcept {
        for (std::size_t n = 0; n < main_.size(); ++n) main_[n] = Narrow(sum_[n]);
    }
    void MainPlanes(const float** planes) const noexcept {
        for (std::uint32_t ch = 0; ch < format_.channels; ++ch)
            planes[ch] = main_.data() + static_cast<std::size_t>(ch) * format_.capacityFrames;
    }
    void Receive(std::uint32_t index) noexcept {
        auto& node = nodes_[index];
        if (node.target == UINT32_MAX) return;
        StoreMain();
        const float* main[kMaxChannels]{}, *side[kMaxChannels]{};
        MainPlanes(main);
        for (std::uint32_t ch = 0; ch < format_.channels; ++ch)
            side[ch] = exchange_.Target(node.target, ch);
        for (std::uint32_t n = 0; n < current_.frames; ++n) {
            pre_[n] = post_[n] = 0;
            for (std::uint32_t ch = 0; ch < format_.channels; ++ch) {
                const auto p = static_cast<std::size_t>(ch) * format_.capacityFrames + n;
                pre_[n] = std::max(pre_[n], absolutePre_[p]);
                post_[n] = std::max(post_[n], absolutePost_[p]);
            }
        }
        const auto& feedback = feedback_.Nodes()[index];
        const bool processed = node.receiver.Process(main, side, format_.channels, current_.frames,
            feedback.feedbackInput ? pre_.data() : nullptr,
            feedback.feedbackInput ? post_.data() : nullptr, feedback.slope, gain_.data());
        assert(processed);
        (void)processed;
    }
    SignalBlockFormat format_{};
    SignalBlockIdentity previous_{}, current_{};
    SignalBlockExchange exchange_;
    SignalFeedbackPlan feedback_;
    std::vector<Node> nodes_;
    std::vector<double> sum_, absolutePre_, absolutePost_, gain_, pre_, post_;
    std::vector<float> main_;
    bool open_ = false, committed_ = false;
};

} // namespace harken
#endif
