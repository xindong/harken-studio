#ifndef HARKEN_API_SIGNAL_PUBLICATION_H
#define HARKEN_API_SIGNAL_PUBLICATION_H

#include "harken/render/SignalWorldPlan.h"
#include "harken/core/CommandQueue.h"
#include "harken/core/PackComposition.h"

namespace harken {
struct Engine;
struct TopologyPlanView;
class TopologyPlanPool;

struct PreparedSignalPublication {
    std::shared_ptr<SignalRenderPlan> owner;
    std::uint64_t predecessor = 0;
    bool changed = false;
};

WorldInstallStatus PrepareSignalPublication(Engine& engine,
    std::span<const SignalRelationV1> relations, std::span<const SignalReceiverV1> receivers,
    std::span<const SignalPropertyInput> properties, std::uint64_t contentIdentity,
    const TopologyPlanView* topology, PreparedSignalPublication& out,
    std::span<const SignalRetainedDefinitionInput> retained = {}) noexcept;
WorldInstallStatus PrepareWorldSignals(Engine& engine, const ContainerWorldInstallViewV1& world,
    std::uint64_t generation, const TopologyPlanView* topology, PreparedSignalPublication& out) noexcept;
bool CanCommitSignals(const Engine& engine, const PreparedSignalPublication& prepared) noexcept;
void AttachPreparedSignals(TopologyPlanPool& pool, std::uint32_t index,
    const PreparedSignalPublication& prepared);
void CommitSignals(Engine& engine, PreparedSignalPublication& prepared) noexcept;
WorldInstallStatus PreparePackSignals(Engine& engine, const PackComposition::PublishedView* published,
    const TopologyPlanView* topology, PreparedSignalPublication& out) noexcept;
void RefreshRetainedPackSignals(Engine& engine) noexcept;
struct BusActivityRoots;
// Logic-owned routing, including the private starts in the prospective batch.
// Membership is matched against the complete definition, not its active projection.
bool CaptureBusActivityOwners(Engine& engine, const TopologyPlanView& topology,
    std::span<const CommandRecord> prospective, BusActivityRoots& out) noexcept;
struct BusActivitySelection;
bool PrepareTopologyActivity(Engine& engine, const TopologyPlanView& topology,
    CommandQueue::BatchReservation& reservation, std::span<const CommandRecord> commands,
    CommandQueue::PreparedBatchProof& proof, const BusActivitySelection* selected = nullptr,
    bool adoptsTopology = true) noexcept;
bool PrepareBusActivityWake(Engine& engine, CommandQueue::BatchReservation& reservation,
    std::span<const CommandRecord> commands, CommandQueue::PreparedBatchProof& proof) noexcept;
bool AcknowledgeBusSleep(Engine& engine) noexcept;

// For an owner transaction without an existing topology publication. Its
// reservation must not span cancellation/drain work that can enqueue commands.
class PrivateSignalTopology {
public:
    explicit PrivateSignalTopology(Engine& engine) noexcept : engine_(engine) {}
    ~PrivateSignalTopology();
    PrivateSignalTopology(const PrivateSignalTopology&) = delete;
    PrivateSignalTopology& operator=(const PrivateSignalTopology&) = delete;
    bool Prepare(const PreparedSignalPublication& signals) noexcept;
    void Commit() noexcept;
private:
    Engine& engine_;
    std::uint32_t index_ = UINT32_MAX;
    CommandQueue::BatchReservation reservation_{};
    CommandQueue::PreparedBatchProof proof_;
    std::uint64_t acceptedRevision_ = 0;
};
}
#endif
