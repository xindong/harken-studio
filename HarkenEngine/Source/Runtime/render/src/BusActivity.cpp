#include "harken/render/BusActivityPlan.h"

namespace harken {

bool PrepareBusActivitySelection(const TopologyPlanView& topology,
    const SignalRenderPlan* signals, std::span<const BusSlotRef> busRoots,
    std::span<const std::uint32_t> signalRoots,
    std::span<const BusActivityDetectorDependency> detectors,
    std::span<const BusActivityMemberRoute> members,
    BusActivitySelection& out) noexcept {
    if (!topology.valid || topology.slotCount == 0 || topology.alive == nullptr ||
        topology.generations == nullptr || topology.parents == nullptr || topology.busIds == nullptr ||
        topology.order == nullptr || !topology.alive[0]) return false;
    const auto signalCount = signals != nullptr ? signals->NodeCount() : 0u;
    if (signalCount > UINT32_MAX - topology.slotCount) return false;
    const auto count = topology.slotCount + signalCount;
    const auto validBus = [&](BusSlotRef bus) {
        return bus.Valid() && bus.index < topology.slotCount && topology.alive[bus.index] &&
            topology.generations[bus.index] == bus.generation;
    };
    try {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
        std::vector<std::size_t> offsets(static_cast<std::size_t>(count) + 1);
        const auto edge = [&](std::uint32_t from, std::uint32_t to) {
            edges.emplace_back(from, to);
            ++offsets[from + 1];
        };
        std::vector<std::uint8_t> live(topology.slotCount);
        for (std::uint32_t i = 0; i < topology.orderCount; ++i) {
            const auto bus = topology.order[i];
            if (bus >= topology.slotCount || !topology.alive[bus] || live[bus]++) return false;
            if (bus != 0) {
                const auto parent = topology.parents[bus];
                if (parent >= topology.slotCount || !topology.alive[parent]) return false;
                edge(bus, parent);
            }
            const auto sends = topology.Sends(bus);
            for (std::uint32_t s = 0; s < sends.count; ++s) {
                const auto target = topology.sendTargets[sends.first + s];
                if (target == kEmptySendTarget) continue;
                if (target >= topology.slotCount || !topology.alive[target]) return false;
                edge(bus, target);
            }
        }
        if (!live[0]) return false;
        std::vector<std::uint8_t> formal(signalCount);
        if (signals != nullptr) {
            for (const auto& relation : signals->Relations()) {
                if (relation.source >= signalCount || relation.target >= signalCount) return false;
                formal[relation.source] = formal[relation.target] = 1;
                edge(topology.slotCount + relation.source, topology.slotCount + relation.target);
                edge(topology.slotCount + relation.target, topology.slotCount + relation.source);
            }
            for (const auto& path : signals->DefinitionPaths()) {
                if (path.source >= signalCount || path.target >= signalCount) return false;
                edge(topology.slotCount + path.source, topology.slotCount + path.target);
            }
            for (std::uint32_t i = 0; i < signalCount; ++i) {
                const auto node = signals->NodeDefinition(i);
                if (node.kind != SignalEndpointKindV1::Bus || node.busIndex >= topology.slotCount ||
                    !topology.alive[node.busIndex]) continue;
                // Formal deleted endpoints stay connected silence, rather than
                // silently attaching to a new incarnation in the same slot.
                if ((formal[i] || node.receiver) &&
                    (node.busId != topology.busIds[node.busIndex] ||
                     node.busGeneration != topology.generations[node.busIndex])) continue;
                edge(node.busIndex, topology.slotCount + i);
                edge(topology.slotCount + i, node.busIndex);
            }
        }
        for (const auto& detector : detectors) {
            if (!validBus(detector.target) || !validBus(detector.source)) return false;
            // A live detector advances a silent Compressor's release state.
            // Conversely an active receiver needs its connected zero source.
            edge(detector.target.index, detector.source.index);
            edge(detector.source.index, detector.target.index);
        }
        for (const auto& member : members) {
            if (member.signalNode >= signalCount || !validBus(member.bus)) return false;
            edge(topology.slotCount + member.signalNode, member.bus.index);
        }
        for (std::uint32_t i = 0; i < count; ++i) offsets[i + 1] += offsets[i];
        auto cursor = offsets;
        std::vector<std::uint32_t> outgoing(edges.size());
        for (const auto& [from, to] : edges) outgoing[cursor[from]++] = to;
        std::vector<std::uint8_t> selected(count);
        std::vector<std::uint32_t> queue;
        queue.reserve(count);
        const auto select = [&](std::uint32_t node) {
            if (!selected[node]) { selected[node] = 1; queue.push_back(node); }
        };
        for (const auto bus : busRoots) {
            if (!validBus(bus)) return false;
            select(bus.index);
        }
        for (const auto node : signalRoots) {
            if (node >= signalCount) return false;
            select(topology.slotCount + node);
        }
        for (std::size_t i = 0; i < queue.size(); ++i)
            for (auto j = offsets[queue[i]]; j < offsets[queue[i] + 1]; ++j) select(outgoing[j]);
        // Plane zero remains available even for an entirely silent engine.
        // Its existence alone must not keep every Master sidechain awake.
        selected[0] = 1;
        BusActivitySelection next;
        for (std::uint32_t i = 0; i < topology.orderCount; ++i)
            if (selected[topology.order[i]]) next.buses.push_back(topology.order[i]);
        for (std::uint32_t i = 0; i < topology.slotCount; ++i)
            if (selected[i] && !live[i]) return false;
        for (std::uint32_t i = 0; i < signalCount; ++i)
            if (selected[topology.slotCount + i]) next.signalNodes.push_back(i);
        out = std::move(next);
        return true;
    } catch (...) { return false; }
}

BusActivityPlan* BusTree::PrepareActivity(const TopologyPlanView& topology,
    std::span<const WorkBinding> working, std::unique_ptr<SignalRenderPlan> signals,
    SignalTransition signalTransition, const SignalRenderPlan* replacedSignals, bool adoptsTopology) noexcept {
    if (!topology.valid || topology.slotCount == 0 || topology.slotCount > m_capacity ||
        topology.parents == nullptr || topology.generations == nullptr || topology.alive == nullptr || topology.busIds == nullptr ||
        working.empty() || working.size() > topology.orderCount ||
        (topology.signals != nullptr && signals == nullptr)) return nullptr;
    if (signalTransition == SignalTransition::ReplaceContent) {
        if (replacedSignals != nullptr && (replacedSignals->PublicationIdentity() == 0 ||
            (signals != nullptr && signals->SameContinuityDomain(*replacedSignals)))) return nullptr;
    } else if (replacedSignals != nullptr) return nullptr;
    try {
        auto plan = std::make_unique<BusActivityPlan>(*this);
        plan->signalTransition = signalTransition;
        plan->replacedSignalPublication = replacedSignals != nullptr ? replacedSignals->PublicationIdentity() : 0;
        plan->working.reserve(working.size());
        plan->directory.resize(topology.slotCount);
        plan->order.reserve(working.size());
        for (std::uint32_t i = 0; i < topology.slotCount; ++i)
            plan->directory[i] = {UINT32_MAX, topology.generations[i], topology.parents[i]};
        // Master always occupies plane zero, independent of feeds-first order.
        for (const auto& binding : working) {
            const auto bus = binding.bus;
            if (!bus.Valid() || bus.index >= topology.slotCount || topology.alive[bus.index] == 0 ||
                topology.generations[bus.index] != bus.generation ||
                plan->directory[bus.index].plane != UINT32_MAX) return nullptr;
            auto* row = ResolveWorkRow(binding.work);
            if (row != nullptr && row->phase.load(std::memory_order_acquire) == WorkPhase::Preparing) {
                row->host = {row->bus.busIndex, row->bus.generation};
                row->hostId = row->bus.id;
            }
            if (row == nullptr || row->host != bus || row->hostId != topology.busIds[bus.index] ||
                !RetainWorkRow(binding.work)) return nullptr;
            plan->working.push_back(binding);
            plan->directory[bus.index].plane = bus.index == 0 ? 0 : 1;
        }
        if (plan->directory[0].plane != 0) return nullptr;
        const auto master = std::find_if(plan->working.begin(), plan->working.end(),
            [](const auto& binding) { return binding.bus.index == 0; });
        std::rotate(plan->working.begin(), master, master + 1);
        std::uint32_t plane = 1;
        for (auto& binding : plan->working)
            if (binding.bus.index != 0) plan->directory[binding.bus.index].plane = plane++;
        for (const auto& binding : plan->working) {
            const auto bus = binding.bus.index;
            if (bus != 0 && (topology.parents[bus] >= topology.slotCount ||
                plan->directory[topology.parents[bus]].plane == UINT32_MAX)) return nullptr;
            const auto sends = topology.Sends(bus);
            for (std::uint32_t s = 0; s < sends.count; ++s) {
                const auto target = topology.sendTargets[sends.first + s];
                if (target != kEmptySendTarget && (target >= topology.slotCount ||
                    plan->directory[target].plane == UINT32_MAX)) return nullptr;
            }
        }
        std::vector<std::uint8_t> seen(topology.slotCount);
        for (std::uint32_t i = 0; i < topology.orderCount; ++i) {
            const auto bus = topology.order[i];
            if (bus >= topology.slotCount || seen[bus]++) return nullptr;
            if (plan->directory[bus].plane != UINT32_MAX) plan->order.push_back(bus);
        }
        if (plan->order.size() != working.size()) return nullptr;
        if (signals != nullptr) {
            if (signals->PublicationIdentity() == 0 || !signals->UsesActualRouting() ||
                signals->RoutingBusSlotBound() != topology.slotCount) return nullptr;
            for (std::uint32_t i = 0; i < signals->NodeCount(); ++i) {
                const auto node = signals->NodeDefinition(i);
                if (node.kind != SignalEndpointKindV1::Bus || node.busIndex == UINT32_MAX) continue;
                if (node.busIndex >= topology.slotCount || plan->directory[node.busIndex].plane == UINT32_MAX ||
                    node.busId != topology.busIds[node.busIndex] || node.busGeneration != topology.generations[node.busIndex])
                    return nullptr;
            }
            for (const auto& binding : plan->working)
                if (signals->NodeForBus(binding.bus.index, topology.busIds[binding.bus.index],
                        binding.bus.generation) == UINT32_MAX) return nullptr;
        }
        plan->beforeDetach.reserve(adoptsTopology ? topology.effectUpdateCount : 0);
        for (std::uint32_t i = 0; adoptsTopology && i < topology.effectUpdateCount; ++i) {
            const auto& adoption = topology.effectUpdates[i];
            const auto host = BusSlotRef::FromPacked(adoption.host);
            if (!host.Valid() || (host.index < topology.slotCount &&
                topology.alive[host.index] != 0 && topology.generations[host.index] == host.generation)) continue;
            const auto work = AcceptedWorkRef(host);
            if (!RetainWorkRow(work)) return nullptr;
            plan->beforeDetach.push_back({adoption, work});
        }
        plan->signals = std::move(signals);
        auto* result = plan.get();
        m_activities.push_back(std::move(plan));
        return result;
    } catch (...) { return nullptr; }
}

CommandQueue::BusActivityAttachment* BusTree::ActivityAttachment(BusActivityPlan* plan) noexcept {
    return plan != nullptr && &plan->tree == this &&
        plan->phase.load(std::memory_order_acquire) == BusActivityPlan::Phase::Preparing
        ? &plan->attachment : nullptr;
}

void BusTree::CancelActivity(BusActivityPlan* plan) noexcept {
    if (plan == nullptr || &plan->tree != this) return;
    auto expected = BusActivityPlan::Phase::Preparing;
    plan->phase.compare_exchange_strong(expected, BusActivityPlan::Phase::Retired,
        std::memory_order_release, std::memory_order_relaxed);
}

void BusTree::QueueActivity(BusActivityPlan& plan) noexcept {
    assert(plan.phase.load(std::memory_order_relaxed) == BusActivityPlan::Phase::Preparing);
    for (const auto& binding : plan.working) {
        if (PreparedWorkRow(binding.work) != nullptr) {
            const bool published = PublishWorkRow(binding.work);
            assert(published);
            (void)published;
        }
    }
    m_lastQueuedActivity = &plan;
    plan.phase.store(BusActivityPlan::Phase::Queued, std::memory_order_release);
}

bool BusTree::AdoptActivity(BusActivityPlan& plan) noexcept {
    if (plan.phase.load(std::memory_order_acquire) != BusActivityPlan::Phase::Queued) return false;
    for (const auto& binding : plan.working) {
        const auto* row = ResolveWorkRow(binding.work);
        if (row == nullptr || row->phase.load(std::memory_order_acquire) != WorkPhase::Published ||
            row->references.load(std::memory_order_relaxed) >= UINT32_MAX - 1 ||
            row->bus.busIndex != binding.bus.index || row->bus.generation != binding.bus.generation) return false;
    }
    for (const auto& entry : plan.beforeDetach) {
        const auto host = BusSlotRef::FromPacked(entry.adoption.host);
        if (AudioWorkRef(host.index) != entry.work || !AudioRefMatches(host)) return false;
    }
    auto* previousSignals = InstalledSignals();
    if (plan.signalTransition == SignalTransition::ReplaceContent) {
        // Content replacement is an explicit prepared transition, never a
        // fallback for a failed same-content continuity mapping.
        if ((previousSignals != nullptr ? previousSignals->PublicationIdentity() : 0) !=
            plan.replacedSignalPublication) return false;
    } else if (previousSignals != nullptr) {
        if (plan.signals == nullptr || !plan.signals->AdoptContinuityFrom(*previousSignals)) return false;
    }
    for (const auto& entry : plan.beforeDetach) ApplyPreviewEffectChain(entry.adoption);
    // Only the coordinator mutates the directory, after all prior workers join.
    // Every incoming candidate already owns a lease; none can retire here.
    for (const auto& binding : plan.working) {
        const auto bus = binding.bus.index;
        if (m_workSlots[bus] == binding.work) continue;
        const bool retained = RetainWorkRow(binding.work);
        assert(retained);
        if (!retained) std::terminate();
        const auto old = m_workSlots[bus];
        m_workSlots[bus] = binding.work;
        AudioReleaseWorkRow(old);
    }
    const auto remove = [&](std::uint32_t bus) {
        if (bus < plan.directory.size() && plan.directory[bus].plane != UINT32_MAX) return;
        const auto old = m_workSlots[bus];
        m_workSlots[bus] = {};
        AudioReleaseWorkRow(old);
    };
    auto* previous = m_activeActivity;
    if (previous != nullptr) {
        for (auto bus : previous->order) remove(bus);
    } else {
        // Bootstrap only: subsequent adoptions visit the previous work set.
        for (std::uint32_t bus = 0; bus < m_audioCount; ++bus) remove(bus);
    }
    for (const auto& entry : plan.beforeDetach) AudioReleaseWorkRow(entry.work);
    plan.beforeDetachReleased = true;
    if (previous != nullptr) {
        for (const auto& binding : previous->working) AudioReleaseWorkRow(binding.work);
        previous->workingReleased = true;
    }
    m_activeActivity = &plan;
    plan.phase.store(BusActivityPlan::Phase::Active, std::memory_order_release);
    if (previous != nullptr)
        previous->phase.store(BusActivityPlan::Phase::Retired, std::memory_order_release);
    return true;
}

void BusTree::DrainActivities() noexcept {
    std::erase_if(m_activities, [&](const auto& plan) {
        if (plan->phase.load(std::memory_order_acquire) != BusActivityPlan::Phase::Retired) return false;
        if (m_lastQueuedActivity == plan.get()) m_lastQueuedActivity = nullptr;
        return true;
    });
    DrainWorkRows();
}

void BusTree::ShutdownActivities() noexcept {
    m_activeActivity = m_lastQueuedActivity = nullptr;
    m_activities.clear();
}

std::uint32_t BusTree::WorkingPlaneCount() const noexcept {
    return m_activeActivity != nullptr ? static_cast<std::uint32_t>(m_activeActivity->working.size()) : m_audioCount;
}

BusRenderView BusTree::RenderView(float* const* planes) noexcept {
    return m_activeActivity != nullptr ? BusRenderView(planes, m_activeActivity->directory, &m_activityFallbacks)
        : BusRenderView(planes);
}

std::span<const std::uint32_t> BusTree::WorkingOrder() const noexcept {
    if (m_activeActivity != nullptr) return m_activeActivity->order;
    const auto plan = m_plans.View(m_currentPlanIndex);
    return plan.valid ? std::span<const std::uint32_t>(plan.order, plan.orderCount)
        : std::span<const std::uint32_t>{};
}

SignalRenderPlan* BusTree::InstalledSignals() const {
    return m_activeActivity != nullptr ? m_activeActivity->signals.get() : m_plans.View(m_currentPlanIndex).signals;
}

BusTree::WorkRef BusTree::AcceptedWorkRef(BusSlotRef bus) const noexcept {
    if (!bus.Valid()) return {};
    if (m_lastQueuedActivity != nullptr) {
        const auto& plan = *m_lastQueuedActivity;
        if (bus.index >= plan.directory.size()) return {};
        const auto plane = plan.directory[bus.index].plane;
        if (plane >= plan.working.size() || plan.working[plane].bus != bus) return {};
        return plan.working[plane].work;
    }
    // Bootstrap rows have fixed physical slots. Do not read the audio-owned
    // installed directory or mutable DSP identity from the logic thread.
    const auto* row = m_workRows.TryAt(bus.index);
    return row != nullptr && row->generation == 1 && row->host == bus
        ? WorkRef{bus.index, 1} : WorkRef{};
}

const SignalRenderPlan* BusTree::AcceptedActivitySignals() const noexcept {
    return m_lastQueuedActivity != nullptr ? m_lastQueuedActivity->signals.get() : nullptr;
}

std::uint64_t BusTree::NextActivitySignalPublication(const SignalRenderPlan* previous) noexcept {
    if (previous != nullptr) m_activitySignalPublication =
        std::max(m_activitySignalPublication, previous->PublicationIdentity());
    return m_activitySignalPublication == UINT64_MAX ? 0 : ++m_activitySignalPublication;
}

} // namespace harken
