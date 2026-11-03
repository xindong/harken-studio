#ifndef HARKEN_RENDER_SIGNAL_RENDER_PLAN_H
#define HARKEN_RENDER_SIGNAL_RENDER_PLAN_H

#include "harken/core/CurrentWorld.h"
#include "harken/core/PlaybackPath.h"
#include "harken/render/SignalBlockExchange.h"
#include "harken/render/SignalFeedbackPlan.h"

#include <cassert>
#include <memory>

namespace harken {
class BusTree;
class BlockArena;
struct VoiceRenderContext;

struct SignalRenderNodeInput {
    SignalEndpointKindV1 kind = SignalEndpointKindV1::Bus;
    bool receiver = false;
    float attackMs = kDuckingDefaultAttackMs;
    float releaseMs = kDuckingDefaultReleaseMs;
    std::uint32_t busIndex = UINT32_MAX;
    VnBusId busId{};
    std::uint32_t busGeneration = 0;
    VnObjectId stableId{};
};

struct SignalMemberProof {
    VnObjectId stableId{};
    std::uint64_t propertyFingerprint = 0;
    ProgramSourceIdentityV1 source{};
    std::uint32_t node = 0;
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
    const VoiceRenderContext* voice = nullptr;
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
        std::span<const SignalFeedbackEdge> paths,
        std::span<const SignalMemberProof> membership = {}) noexcept {
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
                node.busId = input.busId;
                node.busGeneration = input.busGeneration;
                node.stableId = input.stableId;
                if (input.kind == SignalEndpointKindV1::Bus && input.busIndex != UINT32_MAX)
                    next->busNodes_.push_back({input.busIndex, static_cast<std::uint32_t>(i)});
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
            std::sort(next->busNodes_.begin(), next->busNodes_.end());
            for (std::size_t i = 1; i < next->busNodes_.size(); ++i)
                if (next->busNodes_[i - 1].first == next->busNodes_[i].first) return {};
            next->membership_.assign(membership.begin(), membership.end());
            std::sort(next->membership_.begin(), next->membership_.end(),
                [](const auto& a, const auto& b) { return a.stableId < b.stableId; });
            for (const auto& proof : next->membership_) {
                if (proof.node >= nodes.size() || proof.propertyFingerprint == 0 ||
                    nodes[proof.node].kind == SignalEndpointKindV1::Bus) return {};
            }
            // One formal endpoint per kind/ID, even when several carriers
            // prove the same structural ancestor snapshot.
            for (std::size_t i = 0; i < next->membership_.size(); ++i)
                for (std::size_t j = i + 1; j < next->membership_.size() &&
                     next->membership_[j].stableId == next->membership_[i].stableId; ++j)
                    if (next->membership_[i].node != next->membership_[j].node &&
                        nodes[next->membership_[i].node].kind == nodes[next->membership_[j].node].kind)
                        return {};
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
            next->relations_.assign(relations.begin(), relations.end());
            next->levelBounds_.resize(relations.size());
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

    // Compare immutable prepared proofs, never query a world/lease on audio.
    // A Pack's unchanged definition revision survives composition changes;
    // a direct-world definition must match its exact installed generation.
    std::uint32_t MatchMember(std::span<const PlaybackPathRow> path,
                              std::span<std::uint32_t> result) const noexcept {
        std::uint32_t count = 0;
        for (const auto& row : path) {
            if (row.rowKind != ResolvedPlayablePathRowKindV1::Definition) continue;
            auto it = std::lower_bound(membership_.begin(), membership_.end(), row.stableId,
                [](const auto& proof, VnObjectId id) { return proof.stableId < id; });
            for (; it != membership_.end() && it->stableId == row.stableId; ++it) {
                const auto kind = nodes_[it->node].kind;
                if (row.propertyFingerprint != it->propertyFingerprint || !SameDefinitionSource(row.source, it->source) ||
                    (kind == SignalEndpointKindV1::Sound && row.contentPropertyKind != playableproperties::ContentPropertyKindV1::Sound) ||
                    (kind == SignalEndpointKindV1::Container && row.contentPropertyKind != playableproperties::ContentPropertyKindV1::Container))
                    continue;
                if (std::find(result.begin(), result.begin() + count, it->node) == result.begin() + count) {
                    if (count == result.size()) return UINT32_MAX;
                    result[count++] = it->node;
                }
                break;
            }
        }
        return count;
    }
    std::uint32_t NodeForBus(std::uint32_t bus, VnBusId id = {},
                             std::uint32_t generation = 0) const noexcept {
        const auto it = std::lower_bound(busNodes_.begin(), busNodes_.end(), bus,
            [](const auto& entry, std::uint32_t value) { return entry.first < value; });
        if (it == busNodes_.end() || it->first != bus) return UINT32_MAX;
        const auto& node = nodes_[it->second];
        return node.busId == VnBusId{} || (node.busId == id && node.busGeneration == generation)
            ? it->second : UINT32_MAX;
    }
    std::size_t RelationCount() const noexcept { return relations_.size(); }
    // Logic compares immutable prepared definitions only. The content identity
    // excludes signal parameters but includes the authoritative content world.
    bool ConfigurePublication(std::uint64_t identity, std::uint64_t contentIdentity,
                              const SignalRenderPlan* predecessor,
                              std::uint64_t rampFrames) noexcept {
        if (publicationIdentity_ != 0 || identity == 0 || contentIdentity == 0 || open_ || committed_)
            return false;
        publicationIdentity_ = worldIdentity_ = identity;
        contentIdentity_ = contentIdentity;
        updateRampFrames_ = rampFrames;
        if (predecessor != nullptr && predecessor->publicationIdentity_ != 0 &&
            contentIdentity_ == predecessor->contentIdentity_ && SameStructure(*predecessor)) {
            predecessorIdentity_ = predecessor->publicationIdentity_;
            worldIdentity_ = predecessor->worldIdentity_;
        }
        return true;
    }
    std::uint64_t WorldIdentity() const noexcept { return worldIdentity_; }

    // Run before retiring the old topology slot. Swapping transfers storage to
    // the retired owner, so neither allocation nor destruction occurs on audio.
    bool AdoptContinuityFrom(SignalRenderPlan& previous) noexcept {
        if (&previous == this || adopted_ || open_ || previous.open_ ||
            predecessorIdentity_ == 0 || predecessorIdentity_ != previous.publicationIdentity_)
            return false;
        exchange_.Swap(previous.exchange_);
        for (std::size_t i = 0; i < nodes_.size(); ++i)
            nodes_[i].receiver.AdoptStateFrom(previous.nodes_[i].receiver);
        for (std::uint32_t i = 0; i < relations_.size(); ++i)
            if (relations_[i].level != previous.relations_[i].level) {
                const bool changed = exchange_.SetLevel(i, relations_[i].level, updateRampFrames_);
                assert(changed);
                (void)changed;
            }
        previous_ = previous.previous_;
        committed_ = previous.committed_;
        adopted_ = true;
        return true;
    }
    std::uint32_t NodeCount() const noexcept { return static_cast<std::uint32_t>(nodes_.size()); }
    // A candidate graph is borrowed only for this quantum. Its slab must not
    // alias a live prior graph; failed compilation is never a usable fallback.
    bool SetBlockRouting(const SignalFeedbackView& routing) noexcept {
        if (!open_ || contentProcessed_ || routing.nodes.size() < nodes_.size() ||
            routing.order.size() != routing.nodes.size()) return false;
        for (const auto& node : nodes_) if (node.processed) return false;
        blockRouting_ = routing;
        return true;
    }
    void EnableActualRouting() noexcept {
        actualRouting_ = true;
        routingBusCount_ = 0;
        while (routingBusCount_ < nodes_.size() &&
               nodes_[routingBusCount_].kind == SignalEndpointKindV1::Bus &&
               routingBusCount_ < busNodes_.size() &&
               busNodes_[routingBusCount_] == std::pair(routingBusCount_, routingBusCount_))
            ++routingBusCount_;
    }
    bool UsesActualRouting() const noexcept { return actualRouting_; }
    static std::uint64_t RoutingScratchBytes(std::uint32_t nodes, std::uint32_t buses,
                                             std::uint64_t edges, std::uint32_t frames) noexcept;
    bool ReserveRouting(BusTree& buses, BlockArena& arena, std::uint32_t members,
                        std::size_t pathRows) noexcept;
    bool BuildRouting(std::span<const SignalMemberImage> members, BusTree& buses) noexcept;
    bool HasContent() const noexcept { return !membership_.empty(); }
    bool SetConnectionLevel(std::uint32_t connection, float level, std::uint64_t frames) noexcept {
        return !open_ && exchange_.SetLevel(connection, level, frames);
    }
    bool UpdateBounds(std::span<const double> bounds) noexcept {
        return !open_ && feedback_.UpdateBounds(bounds);
    }

    bool BeginBlock(const SignalBlockIdentity& identity) noexcept {
        if (open_ || identity.frames > format_.capacityFrames) return false;
        for (std::uint32_t i = 0; i < relations_.size(); ++i)
            levelBounds_[i] = exchange_.ConnectionBound(i, identity.frames);
        if (!exchange_.BeginBlock(identity)) return false;
        const bool continuous = committed_ && identity.world == previous_.world &&
            identity.transport == previous_.transport && identity.deviceFormat == previous_.deviceFormat &&
            previous_.epoch != UINT64_MAX && identity.epoch == previous_.epoch + 1 &&
            identity.frames == previous_.frames &&
            identity.frameBegin == previous_.frameBegin + previous_.frames;
        if (!continuous) for (auto& node : nodes_) node.receiver.Reset();
        for (auto& node : nodes_) node.processed = false;
        contentProcessed_ = false;
        blockRouting_ = {};
        routingStorage_ = {};
        current_ = identity;
        open_ = true;
        return true;
    }

    // Member images are pre-Out Vol and already include their real spatial
    // mapping. A Sound receives before its ancestors. Container stages see
    // member output after deeper stages, without reapplying author properties.
    bool ProcessContent(std::span<const SignalMemberImage> members) noexcept {
        if (!open_ || contentProcessed_) return false;
        for (const auto& member : members) {
            if (member.planes == nullptr || member.outputGain == nullptr ||
                member.offset > current_.frames || member.frames > current_.frames - member.offset)
                return false;
            for (std::uint32_t ch = 0; ch < format_.channels; ++ch)
                if (member.planes[ch] == nullptr) return false;
            for (std::uint32_t n = 0; n < member.frames; ++n) {
                if (!std::isfinite(member.outputGain->At(n))) return false;
                for (std::uint32_t ch = 0; ch < format_.channels; ++ch)
                    if (!std::isfinite(member.planes[ch][member.offset + n])) return false;
            }
            for (auto node : member.nodes)
                if (node >= nodes_.size() || nodes_[node].kind == SignalEndpointKindV1::Bus)
                    return false;
        }
        for (const auto index : blockRouting_.nodes.empty() ? feedback_.NormalOrder() : blockRouting_.order) {
            if (index >= nodes_.size()) continue;
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
                        const double post = Narrow(pre * member.outputGain->At(n));
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
                                Narrow(static_cast<double>(member.planes[ch][member.offset + n]) * member.outputGain->At(n));
                    }
                }
                StoreMain();
                const float* planes[kMaxChannels]{};
                MainPlanes(planes);
                if (!exchange_.Capture(node.source, {planes, format_.channels}, 0, current_.frames)) return false;
            }
            node.processed = true;
        }
        contentProcessed_ = true;
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
        if (!open_) return false;
        for (std::uint32_t index = 0; index < nodes_.size(); ++index) {
            auto& node = nodes_[index];
            if (!node.processed && node.kind == SignalEndpointKindV1::Bus) {
                // A removed or reincarnated Bus contributes fresh silence;
                // it never binds an old endpoint to a reused dense slot.
                ClearSums();
                Receive(index);
                node.processed = true;
            }
            if (node.target != UINT32_MAX && !node.processed) return false;
        }
        if (!exchange_.CommitBlock()) return false;
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
    bool SameStructure(const SignalRenderPlan& other) const noexcept {
        if (format_.channels != other.format_.channels || format_.capacityFrames != other.format_.capacityFrames ||
            format_.sampleRate != other.format_.sampleRate || format_.layout != other.format_.layout ||
            actualRouting_ != other.actualRouting_ || busNodes_ != other.busNodes_ ||
            nodes_.size() != other.nodes_.size() || relations_.size() != other.relations_.size()) return false;
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            const auto& a = nodes_[i]; const auto& b = other.nodes_[i];
            if (a.kind != b.kind || a.stableId != b.stableId || a.busId != b.busId ||
                a.busGeneration != b.busGeneration || a.source != b.source || a.target != b.target) return false;
        }
        for (std::size_t i = 0; i < relations_.size(); ++i) {
            const auto& a = relations_[i]; const auto& b = other.relations_[i];
            if (a.stableId != b.stableId || a.source != b.source || a.target != b.target) return false;
        }
        return true;
    }
    static bool SameDefinitionSource(const ProgramSourceIdentityV1& a,
                                     const ProgramSourceIdentityV1& b) noexcept {
        return a.ownerStableId == b.ownerStableId && a.ownerRevision == b.ownerRevision &&
            a.sourceBodyVintage == b.sourceBodyVintage &&
            (a.ownerStableId != 0 || a.worldGeneration == b.worldGeneration);
    }
    struct Node {
        SignalEndpointKindV1 kind = SignalEndpointKindV1::Bus;
        VnObjectId stableId{};
        VnBusId busId{};
        std::uint32_t busGeneration = 0;
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
        const auto& feedback = blockRouting_.nodes.empty() ? feedback_.Nodes()[index] : blockRouting_.nodes[index];
        const bool processed = node.receiver.Process(main, side, format_.channels, current_.frames,
            feedback.feedbackInput ? pre_.data() : nullptr,
            feedback.feedbackInput ? post_.data() : nullptr, feedback.slope, gain_.data());
        assert(processed);
        (void)processed;
    }
    SignalBlockFormat format_{};
    SignalBlockIdentity previous_{}, current_{};
    SignalBlockExchange exchange_;
    std::vector<SignalConnectionInput> relations_;
    std::vector<double> levelBounds_;
    bool actualRouting_ = false;
    std::uint32_t routingBusCount_ = 0;
    SignalFeedbackPlan feedback_;
    SignalFeedbackView blockRouting_{};
    std::span<std::byte> routingStorage_{};
    std::vector<Node> nodes_;
    std::vector<SignalMemberProof> membership_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> busNodes_;
    std::vector<double> sum_, absolutePre_, absolutePost_, gain_, pre_, post_;
    std::vector<float> main_;
    bool open_ = false, committed_ = false, contentProcessed_ = false;
    std::uint64_t publicationIdentity_ = 0, contentIdentity_ = 0, worldIdentity_ = 1;
    std::uint64_t predecessorIdentity_ = 0, updateRampFrames_ = 0;
    bool adopted_ = false;
};

} // namespace harken
#endif
