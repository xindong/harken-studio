#include "harken/core/LogicCommitCoordinator.h"

namespace harken {

ActionCommitBundle::ActionCommitBundle(std::uint32_t maxSteps)
    : m_maxSteps(maxSteps) {
    m_steps.reserve(maxSteps);
}

bool ActionCommitBundle::AddStep(const CommitParticipant& participant,
                                 std::uint32_t count) {
    if (m_state != BundleState::Open ||
        m_steps.size() >= static_cast<std::size_t>(m_maxSteps)) {
        return false;
    }
    Step step;
    step.participant = participant;
    step.count = count;
    m_steps.push_back(step);
    return true;
}

void ActionCommitBundle::ReleasePrefix(std::uint32_t count) {
    // Reverse order, so a service that reserved last gives its room back first.
    // Services are independent here, but unwinding in the order things were
    // taken is the only order that stays correct once one of them is not.
    for (std::uint32_t i = count; i > 0; --i) {
        const Step& step = m_steps[i - 1];
        if (step.participant.release != nullptr) {
            step.participant.release(step.participant.ctx, step.count);
        }
    }
}

bool ActionCommitBundle::Reserve() {
    if (m_state != BundleState::Open) {
        return false;
    }
    for (std::uint32_t i = 0; i < m_steps.size(); ++i) {
        const Step& step = m_steps[i];
        // An incomplete participant cannot promise an atomic owner operation.
        const bool held =
            step.participant.reserve != nullptr &&
            step.participant.release != nullptr &&
            step.participant.commit != nullptr &&
            step.participant.reserve(step.participant.ctx, step.count);
        if (!held) {
            ReleasePrefix(i);
            m_state = BundleState::Abandoned;
            return false;
        }
    }
    m_state = BundleState::Reserved;
    return true;
}

bool ActionCommitBundle::Commit(LogicCommitCoordinator& coordinator) {
    if (m_state != BundleState::Reserved) {
        return false;
    }
    LogicCommitSequence issued = 0;
    if (coordinator.TryIssue(&issued) !=
        LogicCommitCoordinator::IssueStatus::Issued) {
        ReleasePrefix(static_cast<std::uint32_t>(m_steps.size()));
        m_state = BundleState::Abandoned;
        return false;
    }
    return CommitReserved(issued);
}

bool ActionCommitBundle::CommitReserved(LogicCommitSequence sequence) {
    if (m_state != BundleState::Reserved || sequence == 0) {
        return false;
    }
    m_sequence = sequence;
    for (const Step& step : m_steps) {
        if (step.participant.commit != nullptr) {
            step.participant.commit(step.participant.ctx, step.count,
                                    m_sequence);
        }
    }
    m_state = BundleState::Committed;
    return true;
}

bool ActionCommitBundle::Reset() {
    if (m_state == BundleState::Reserved) {
        return false;
    }
    m_steps.clear();
    m_state = BundleState::Open;
    m_sequence = 0;
    return true;
}

void ActionCommitBundle::Abandon() {
    if (m_state != BundleState::Reserved) {
        return;
    }
    ReleasePrefix(static_cast<std::uint32_t>(m_steps.size()));
    m_state = BundleState::Abandoned;
}

} // namespace harken
