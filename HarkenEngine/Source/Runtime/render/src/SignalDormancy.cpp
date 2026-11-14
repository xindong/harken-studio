#include "harken/render/SignalRenderPlan.h"

#include <atomic>

namespace harken {

struct SignalRenderPlan::DormantState {
    struct Node {
        SignalRenderNodeInput definition;
        std::vector<SignalMemberProof> membership;
        std::uint32_t prior = UINT32_MAX, source = UINT32_MAX;
        bool receiver = false;
        DuckingDormantState saved;
    };
    struct Connection {
        std::uint32_t prior = UINT32_MAX, source = UINT32_MAX, target = UINT32_MAX;
        VnObjectId id{};
        float level = 1;
        OutputGain gain;
        bool hasPrevious = false;
    };
    SignalBlockFormat format;
    std::uint64_t sourcePublication = 0, sleepPublication = 0, content = 0, world = 0;
    std::vector<Node> nodes;
    std::vector<Connection> connections;
    // Only audio writes the payload, once. The control owner can read after
    // the sleep ACK; queued wakes use this independent publication fence.
    std::uint64_t historyRevision = 0;
    std::atomic<bool> captured{false};
};

std::shared_ptr<SignalRenderPlan::DormantState> SignalRenderPlan::PrepareDormantState(
    const SignalRenderPlan& previous) const noexcept {
    if (publicationIdentity_ == 0 || adopted_ || open_ || committed_ ||
        !SameContinuityDomain(previous)) return {};
    const Continuity* mapping = nullptr;
    for (const auto& candidate : continuity_)
        if (candidate.identity == previous.publicationIdentity_) mapping = &candidate;
    if (mapping == nullptr || mapping->identity == 0) return {};
    try {
        auto result = std::make_shared<DormantState>();
        result->format = format_;
        result->sourcePublication = previous.publicationIdentity_;
        result->sleepPublication = publicationIdentity_;
        result->content = contentIdentity_;
        result->world = worldIdentity_;
        std::vector<std::uint8_t> retained(previous.nodes_.size());
        std::vector<std::uint32_t> cold(previous.nodes_.size(), UINT32_MAX);
        for (std::size_t i = 0; i < nodes_.size(); ++i)
            for (std::size_t j = 0; j < previous.nodes_.size(); ++j)
                if (SameNodeDefinition(i, previous, j)) { retained[j] = 1; break; }
        for (std::uint32_t i = 0; i < previous.nodes_.size(); ++i) {
            const auto& old = previous.nodes_[i];
            if (retained[i] || (old.source == UINT32_MAX && old.target == UINT32_MAX)) continue;
            DormantState::Node node;
            node.definition = previous.NodeDefinition(i);
            node.prior = i;
            node.source = old.source;
            node.receiver = old.target != UINT32_MAX;
            for (const auto& proof : previous.membership_)
                if (proof.node == i) node.membership.push_back(proof);
            cold[i] = static_cast<std::uint32_t>(result->nodes.size());
            result->nodes.push_back(std::move(node));
        }
        for (std::uint32_t i = 0; i < previous.relations_.size(); ++i) {
            if (std::find(mapping->connections.begin(), mapping->connections.end(), i) !=
                mapping->connections.end()) continue;
            const auto& old = previous.relations_[i];
            // Activity cannot turn a connected-zero input into no input.
            if (retained[old.source] || retained[old.target] ||
                cold[old.source] == UINT32_MAX || cold[old.target] == UINT32_MAX) return {};
            DormantState::Connection connection;
            connection.prior = i;
            connection.source = cold[old.source];
            connection.target = cold[old.target];
            connection.id = old.stableId;
            connection.level = old.level;
            result->connections.push_back(connection);
        }
        return result;
    } catch (...) { return {}; }
}

bool SignalRenderPlan::CaptureDormantState(SignalRenderPlan& previous,
    DormantState& state) const noexcept {
    if (state.captured.load(std::memory_order_acquire) || open_ || committed_ || adopted_ ||
        state.sleepPublication != publicationIdentity_ ||
        state.sourcePublication != previous.publicationIdentity_ ||
        previous.open_ || !previous.committed_ || !previous.exchange_.m_committed ||
        previous.historyRevision_ == UINT64_MAX) return false;
    for (auto& saved : state.nodes) {
        if (saved.prior >= previous.nodes_.size()) return false;
        const auto& old = previous.nodes_[saved.prior];
        if (saved.source != UINT32_MAX) {
            if (saved.source >= previous.exchange_.m_sources) return false;
            const auto* pcm = previous.exchange_.Source(previous.exchange_.m_readBank, saved.source);
            for (std::size_t i = 0; i < previous.exchange_.m_planeSamples; ++i)
                if (std::bit_cast<std::uint32_t>(pcm[i]) != 0) return false;
        }
        if (saved.receiver && (!old.processed || !old.dormantInputsQuiet ||
            !old.receiver.CaptureDormantState(format_.channels,
                previous.exchange_.HasTargetInput(old.target), old.dormantFeedback,
                old.dormantSlope, old.dormantBoundMain, saved.saved))) return false;
    }
    for (auto& saved : state.connections) {
        if (saved.prior >= previous.exchange_.m_connections.size()) return false;
        const auto& old = previous.exchange_.m_connections[saved.prior];
        if (old.gain.remainingFrames != 0 || !std::isfinite(old.gain.current) ||
            !std::isfinite(old.gain.target)) return false;
        saved.gain = old.gain;
        saved.hasPrevious = old.hasPrevious;
    }
    state.historyRevision = previous.historyRevision_;
    state.captured.store(true, std::memory_order_release);
    return true;
}

bool SignalRenderPlan::PrepareDormantContinuity(const SignalRenderPlan& previous,
    std::shared_ptr<const DormantState> state) noexcept {
    if (!state || adopted_ || open_ || committed_ || state->content != contentIdentity_ ||
        state->world != worldIdentity_ || state->format.channels != format_.channels ||
        state->format.capacityFrames != format_.capacityFrames ||
        state->format.sampleRate != format_.sampleRate || state->format.layout != format_.layout) return false;
    const Continuity* mapping = nullptr;
    for (const auto& candidate : continuity_)
        if (candidate.identity != 0 && candidate.identity == previous.publicationIdentity_) mapping = &candidate;
    if (mapping == nullptr) return false;
    try {
        DormantRestore restore;
        restore.predecessor = previous.publicationIdentity_;
        restore.receivers.assign(nodes_.size(), UINT32_MAX);
        restore.connections.assign(relations_.size(), UINT32_MAX);
        std::vector<std::uint32_t> matches(nodes_.size(), UINT32_MAX);
        const auto sameProof = [](const SignalMemberProof& a, const SignalMemberProof& b) {
            return a.stableId == b.stableId && a.propertyFingerprint == b.propertyFingerprint &&
                SameDefinitionSource(a.source, b.source);
        };
        for (std::uint32_t i = 0; i < nodes_.size(); ++i) {
            const auto& node = nodes_[i];
            if (node.stableId == VnObjectId{}) continue;
            for (std::uint32_t j = 0; j < state->nodes.size(); ++j) {
                const auto& saved = state->nodes[j];
                const auto& old = saved.definition;
                if (node.kind != old.kind || node.stableId != old.stableId ||
                    node.busId != old.busId || node.busGeneration != old.busGeneration) continue;
                bool same = true;
                for (const auto& proof : membership_)
                    if (proof.node == i && std::none_of(saved.membership.begin(), saved.membership.end(),
                        [&](const auto& other) { return sameProof(proof, other); })) same = false;
                for (const auto& proof : saved.membership)
                    if (std::none_of(membership_.begin(), membership_.end(),
                        [&](const auto& other) { return other.node == i && sameProof(proof, other); })) same = false;
                if (!same) continue;
                matches[i] = j;
                if (node.target != UINT32_MAX && saved.receiver && mapping->receivers[i] == UINT32_MAX)
                    restore.receivers[i] = j;
                break;
            }
        }
        for (std::uint32_t i = 0; i < relations_.size(); ++i) {
            if (mapping->connections[i] != UINT32_MAX || relations_[i].stableId == VnObjectId{}) continue;
            const auto& relation = relations_[i];
            for (std::uint32_t j = 0; j < state->connections.size(); ++j) {
                const auto& old = state->connections[j];
                if (relation.stableId == old.id && matches[relation.source] == old.source &&
                    matches[relation.target] == old.target) { restore.connections[i] = j; break; }
            }
        }
        for (const auto& existing : dormantRestores_) {
            if (existing.predecessor != restore.predecessor) continue;
            for (std::size_t i = 0; i < nodes_.size(); ++i)
                if (restore.receivers[i] != UINT32_MAX && existing.receivers[i] != UINT32_MAX) return false;
            for (std::size_t i = 0; i < relations_.size(); ++i)
                if (restore.connections[i] != UINT32_MAX && existing.connections[i] != UINT32_MAX) return false;
        }
        if (std::all_of(restore.receivers.begin(), restore.receivers.end(),
                [](auto row) { return row == UINT32_MAX; }) &&
            std::all_of(restore.connections.begin(), restore.connections.end(),
                [](auto row) { return row == UINT32_MAX; })) return true;
        restore.state = std::move(state);
        dormantRestores_.push_back(std::move(restore));
        return true;
    } catch (...) { return false; }
}

bool SignalRenderPlan::PruneDormantState(std::shared_ptr<const DormantState> state,
    std::shared_ptr<const DormantState>& remaining) const noexcept {
    if (!state || !state->captured.load(std::memory_order_acquire)) return false;
    if (state->content != contentIdentity_ || state->world != worldIdentity_ ||
        state->format.channels != format_.channels || state->format.sampleRate != format_.sampleRate ||
        state->format.capacityFrames != format_.capacityFrames || state->format.layout != format_.layout) {
        remaining.reset();
        return true;
    }
    try {
        std::vector<std::uint32_t> remap(state->nodes.size(), UINT32_MAX);
        std::uint32_t kept = 0;
        const auto sameProof = [](const SignalMemberProof& a, const SignalMemberProof& b) {
            return a.stableId == b.stableId && a.propertyFingerprint == b.propertyFingerprint &&
                SameDefinitionSource(a.source, b.source);
        };
        for (std::uint32_t i = 0; i < state->nodes.size(); ++i) {
            const auto& saved = state->nodes[i];
            const auto& old = saved.definition;
            bool restored = false;
            for (std::uint32_t j = 0; j < nodes_.size() && !restored; ++j) {
                const auto& node = nodes_[j];
                if (node.kind != old.kind || node.stableId != old.stableId ||
                    node.busId != old.busId || node.busGeneration != old.busGeneration) continue;
                restored = true;
                for (const auto& proof : membership_)
                    if (proof.node == j && std::none_of(saved.membership.begin(), saved.membership.end(),
                        [&](const auto& other) { return sameProof(proof, other); })) restored = false;
                for (const auto& proof : saved.membership)
                    if (std::none_of(membership_.begin(), membership_.end(),
                        [&](const auto& other) { return other.node == j && sameProof(proof, other); })) restored = false;
            }
            if (!restored) remap[i] = kept++;
        }
        if (kept == 0) { remaining.reset(); return true; }
        if (kept == state->nodes.size()) { remaining = std::move(state); return true; }
        auto next = std::make_shared<DormantState>();
        next->format = state->format;
        next->sourcePublication = state->sourcePublication;
        next->sleepPublication = state->sleepPublication;
        next->content = state->content;
        next->world = state->world;
        next->historyRevision = state->historyRevision;
        next->nodes.reserve(kept);
        for (std::uint32_t i = 0; i < remap.size(); ++i)
            if (remap[i] != UINT32_MAX) next->nodes.push_back(state->nodes[i]);
        for (auto connection : state->connections) {
            const auto source = remap[connection.source], target = remap[connection.target];
            // A wake must preserve each formal connected component as a whole.
            if ((source == UINT32_MAX) != (target == UINT32_MAX)) return false;
            if (source == UINT32_MAX) continue;
            connection.source = source;
            connection.target = target;
            next->connections.push_back(connection);
        }
        next->captured.store(true, std::memory_order_release);
        remaining = std::move(next);
        return true;
    } catch (...) { return false; }
}

bool SignalRenderPlan::AdoptContinuityFrom(SignalRenderPlan& previous) noexcept {
    if (&previous == this || adopted_ || open_ || committed_ || previous.open_) return false;
    const Continuity* mapping = nullptr;
    for (const auto& candidate : continuity_)
        if (candidate.identity != 0 && candidate.identity == previous.publicationIdentity_) mapping = &candidate;
    if (mapping == nullptr || mapping->receivers.size() != nodes_.size()) return false;
    for (auto receiver : mapping->receivers)
        if (receiver != UINT32_MAX && receiver >= previous.nodes_.size()) return false;
    // Validate every cold restore before moving any live exchange state.
    for (const auto& restore : dormantRestores_) {
        if (restore.predecessor != previous.publicationIdentity_) continue;
        const auto& state = *restore.state;
        if (!state.captured.load(std::memory_order_acquire)) return false;
        for (auto receiver : restore.receivers) {
            if (receiver == UINT32_MAX) continue;
            if (receiver >= state.nodes.size() || !state.nodes[receiver].receiver ||
                state.nodes[receiver].saved.channels != format_.channels ||
                state.nodes[receiver].saved.sampleRate != format_.sampleRate) return false;
        }
        for (auto connection : restore.connections)
            if (connection != UINT32_MAX && connection >= state.connections.size()) return false;
    }
    if (mapping->exchangeContinuous) exchange_.Swap(previous.exchange_);
    else if (!exchange_.AdoptMappedContinuityFrom(previous.exchange_, mapping->sources, mapping->connections))
        return false;
    for (std::size_t i = 0; i < mapping->receivers.size(); ++i)
        if (mapping->receivers[i] != UINT32_MAX)
            nodes_[i].receiver.AdoptStateFrom(previous.nodes_[mapping->receivers[i]].receiver);
    for (std::uint32_t i = 0; i < relations_.size(); ++i) {
        const auto prior = mapping->exchangeContinuous ? i : mapping->connections[i];
        if (prior != UINT32_MAX && relations_[i].level != previous.relations_[prior].level) {
            const bool changed = exchange_.SetLevel(i, relations_[i].level, updateRampFrames_);
            assert(changed); (void)changed;
        }
    }
    for (const auto& restore : dormantRestores_) {
        if (restore.predecessor != previous.publicationIdentity_) continue;
        const auto& state = *restore.state;
        const bool historyValid = state.historyRevision != UINT64_MAX &&
            state.historyRevision == previous.historyRevision_;
        for (std::uint32_t i = 0; i < restore.receivers.size(); ++i) {
            if (restore.receivers[i] == UINT32_MAX) continue;
            if (!historyValid) { nodes_[i].receiver.Reset(); continue; }
            const bool restored = nodes_[i].receiver.RestoreDormantState(state.nodes[restore.receivers[i]].saved);
            assert(restored); (void)restored;
        }
        for (std::uint32_t i = 0; i < restore.connections.size(); ++i) {
            if (restore.connections[i] == UINT32_MAX) continue;
            const auto& saved = state.connections[restore.connections[i]];
            auto& next = exchange_.m_connections[i];
            next.gain = saved.gain;
            next.hasPrevious = saved.hasPrevious;
            if (relations_[i].level != saved.level) {
                const bool changed = exchange_.SetLevel(i, relations_[i].level, updateRampFrames_);
                assert(changed); (void)changed;
            }
        }
    }
    // A reset while sleeping invalidates receiver history, not connection gain.
    // The current clock always comes from the actual live predecessor.
    historyRevision_ = previous.historyRevision_;
    previous_ = previous.previous_;
    committed_ = previous.committed_;
    adopted_ = true;
    return true;
}

} // namespace harken
