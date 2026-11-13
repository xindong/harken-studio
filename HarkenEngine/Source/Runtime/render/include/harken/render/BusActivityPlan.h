#pragma once

#include "harken/render/BusTree.h"
#include "harken/render/SignalRenderPlan.h"

namespace harken {

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
        for (const auto& binding : working) tree.ReleaseWorkRow(binding.work);
    }
    BusTree& tree;
    CommandQueue::BusActivityAttachment attachment{};
    std::atomic<Phase> phase{Phase::Preparing};
    std::vector<BusTree::WorkBinding> working;
    std::vector<BusRenderSlot> directory;
    std::vector<std::uint32_t> order;
    std::unique_ptr<SignalRenderPlan> signals;
    BusTree::SignalTransition signalTransition = BusTree::SignalTransition::Preserve;
    std::uint64_t replacedSignalPublication = 0;
};

} // namespace harken
