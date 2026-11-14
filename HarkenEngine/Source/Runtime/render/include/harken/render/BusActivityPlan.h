#pragma once

#include "harken/render/BusTree.h"
#include "harken/core/MeterBlockRing.h"
#include "harken/render/SignalRenderPlan.h"
#include "harken/render/BuiltinEffectDormancy.h"

namespace harken {

struct BusActivitySelection {
    std::vector<std::uint32_t> buses;
    std::vector<std::uint32_t> signalNodes;
};

struct BusActivityDetectorDependency { BusSlotRef target, source; };
struct BusActivityMemberRoute { std::uint32_t signalNode; BusSlotRef bus; };

struct BusActivityRoots {
    std::vector<BusSlotRef> buses;
    std::vector<std::uint32_t> signals;
    std::vector<BusActivityMemberRoute> members;
};

// Audio writes into fully prepared capture storage. It is released after ACK.
struct BusDormantCapture {
    BusSlotRef bus;
    std::array<BusTree::DormantToneSection, 4> filters;
    std::array<BuiltinEffectDormantState, kMaxFxSlotsPerBus> effects;
    std::array<EffectObjectIdentity, kMaxFxSlotsPerBus> identities;
    std::array<ToneDormantState, kMaxFxSlotsPerBus> tones;
};

struct BusDormantWork {
    struct Filter { std::uint32_t section; BusTree::DormantToneSection state; };
    struct Effect { std::uint32_t slot; BuiltinEffectDormantState state; EffectObjectIdentity identity; };
    struct Tone { std::uint32_t slot; ToneDormantState state; };
    BusSlotRef bus;
    std::vector<Filter> filters;
    std::vector<Effect> effects;
    std::vector<Tone> tones;
};

// Control-owned immutable residuals, independent of retired work-row storage.
struct BusDormantRegistry {
    std::vector<std::shared_ptr<const BusDormantWork>> buses;
    std::vector<std::shared_ptr<const SignalRenderPlan::DormantState>> signals;
};

// Logic-only reachability over a prospective definition. Roots come from
// actual accepted owners or coherent audio recovery facts, never from the
// previous working set. Unrooted feedback components therefore do not self-retain.
bool PrepareBusActivitySelection(const TopologyPlanView& topology,
    const SignalRenderPlan* signals, std::span<const BusSlotRef> busRoots,
    std::span<const std::uint32_t> signalRoots,
    std::span<const BusActivityDetectorDependency> detectors,
    std::span<const BusActivityMemberRoute> members,
    BusActivitySelection& out) noexcept;

// Control-owned immutable projection, with one audio publication lifecycle.
// The installed directory separately retains the exact work-row incarnations.
struct BusActivityPlan {
    enum class Phase : std::uint8_t { Preparing, Queued, Active, Retired };
    explicit BusActivityPlan(BusTree& owner) noexcept : tree(owner) {
        attachment.context = this;
        attachment.queued = [](void* p) noexcept {
            auto& plan = *static_cast<BusActivityPlan*>(p);
            plan.tree.QueueActivity(plan);
        };
        attachment.cancel = [](void* p) noexcept {
            auto& plan = *static_cast<BusActivityPlan*>(p);
            plan.tree.CancelActivity(&plan);
        };
        attachment.adopt = [](void* p) noexcept {
            auto& plan = *static_cast<BusActivityPlan*>(p);
            return plan.tree.AdoptActivity(plan);
        };
        attachment.discard = [](void* p) noexcept {
            static_cast<BusActivityPlan*>(p)->phase.store(Phase::Retired, std::memory_order_release);
        };
    }
    ~BusActivityPlan() {
        ReleaseSleepHolds();
        if (!workingReleased)
            for (const auto& binding : working) tree.ReleaseWorkRow(binding.work);
        if (!beforeDetachReleased)
            for (const auto& entry : beforeDetach) tree.ReleaseWorkRow(entry.work);
    }
    void ReleaseSleepHolds() noexcept {
        for (const auto& row : sleeping) tree.ReleaseWorkRow(row.expected.work);
        sleeping.clear();
        if (sleepPrevious != nullptr) { --sleepPrevious->logicPins; sleepPrevious = nullptr; }
    }
    struct SleepingWork {
        BusTree::WorkObservation expected;
        std::shared_ptr<BusDormantCapture> saved;
    };
    enum class SleepDecision : std::uint8_t { None, Pending, Accepted, Rejected };
    std::atomic<SleepDecision> sleepDecision{SleepDecision::None};
    std::vector<SleepingWork> sleeping;
    std::shared_ptr<SignalRenderPlan::DormantState> dormantSignals;
    std::vector<std::uint32_t> sleepSignalScratch;
    std::unique_ptr<BusDormantRegistry> remainingDormant;
    struct PreparedEffects {
        BusSlotRef bus;
        BusTree::WorkRef work;
        std::array<EffectInstanceRef, kMaxFxSlotsPerBus> effects;
    };
    std::vector<PreparedEffects> preparedEffects;
    void* logicContext = nullptr;
    void (*logicQueued)(void*, const BusActivityPlan&) noexcept = nullptr;
    BusActivityPlan* sleepPrevious = nullptr;
    VoicePool* sleepVoices = nullptr;
    std::uint32_t logicPins = 0;
    std::uint64_t publication = 0;
    bool reclaimCandidate = false; // logic-only, captured before the completed receipt
    BusTree& tree;
    CommandQueue::BusActivityAttachment attachment{};
    std::atomic<Phase> phase{Phase::Preparing};
    std::vector<BusTree::WorkBinding> working;
    std::vector<float*> planes;
    std::vector<MeterBlockRing::Lease> meterLanes;
    std::vector<BusRenderSlot> directory;
    std::vector<std::uint32_t> order;
    std::unique_ptr<SignalRenderPlan> signals;
    TopologyPlanView topologyDefinition;
    bool adoptsTopology = false;
    BusTree::SignalTransition signalTransition = BusTree::SignalTransition::Preserve;
    std::uint64_t replacedSignalPublication = 0, replacedSignalAlternate = 0;
    struct BeforeDetach { BusEffectChainAdoption adoption; BusTree::WorkRef work; };
    std::vector<BeforeDetach> beforeDetach;
    bool workingReleased = false;
    bool beforeDetachReleased = false;
};

} // namespace harken
