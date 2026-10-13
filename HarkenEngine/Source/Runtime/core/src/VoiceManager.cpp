#include "harken/core/VoiceManager.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace harken {

namespace {

double OrderedAudibility(double value) {
    return std::isfinite(value)
               ? value
               : -(std::numeric_limits<double>::infinity)();
}

} // namespace

VoiceManager::VoiceManager(std::uint32_t renderCapacity,
                           std::uint32_t logicalCapacity)
    : m_claims(renderCapacity) {
    m_requests.reserve(logicalCapacity);
    m_candidates.reserve(renderCapacity);
}

VoiceManager::Decision VoiceManager::Decide(
    const RenderRequest& request, bool parkingEnabled,
    bool virtualCapacityAvailable, double parkThresholdDb,
    std::uint32_t stealPolicy, const FreeRender& freeRender,
    const LiveCandidate* candidates, std::uint32_t candidateCount) const {
    Decision decision;
    const bool finiteRequest = std::isfinite(request.audibilityDb);
    if (request.kind != RequestKind::ParkedReturn &&
        request.residency == Residency::ApproximateParkingAllowed &&
        parkingEnabled && virtualCapacityAvailable && finiteRequest &&
        request.audibilityDb < parkThresholdDb) {
        decision.kind = DecisionKind::DirectVirtual;
        return decision;
    }
    if (freeRender.available) {
        decision.kind = DecisionKind::UseFreeRender;
        decision.renderSlot = freeRender.slot;
        decision.renderGeneration = freeRender.generation;
        return decision;
    }
    if (!finiteRequest || stealPolicy == 0 || candidates == nullptr) {
        return decision;
    }

    const LiveCandidate* victim = nullptr;
    for (std::uint32_t i = 0; i < candidateCount; ++i) {
        const LiveCandidate& candidate = candidates[i];
        if (candidate.renderHandle == 0 ||
            candidate.protection == Protection::Protected ||
            candidate.priority > request.priority ||
            !std::isfinite(candidate.audibilityDb)) {
            continue;
        }
        if (victim == nullptr) {
            victim = &candidate;
            continue;
        }
        const std::uint32_t candidateSlot =
            static_cast<std::uint32_t>(candidate.renderHandle);
        const std::uint32_t victimSlot =
            static_cast<std::uint32_t>(victim->renderHandle);
        const bool better =
            stealPolicy == 1
                ? (candidate.audibilityDb < victim->audibilityDb ||
                   (candidate.audibilityDb == victim->audibilityDb &&
                    candidateSlot < victimSlot))
                : (candidate.startOrder < victim->startOrder ||
                   (candidate.startOrder == victim->startOrder &&
                    candidateSlot < victimSlot));
        if (better) {
            victim = &candidate;
        }
    }
    if (victim != nullptr) {
        decision.kind = DecisionKind::Steal;
        decision.victimRenderHandle = victim->renderHandle;
    }
    return decision;
}

bool VoiceManager::RequestBefore(const RenderRequest& lhs,
                                 const RenderRequest& rhs) {
    if (lhs.priority != rhs.priority) {
        return lhs.priority > rhs.priority;
    }
    const double lhsAudibility = OrderedAudibility(lhs.audibilityDb);
    const double rhsAudibility = OrderedAudibility(rhs.audibilityDb);
    if (lhsAudibility != rhsAudibility) {
        return lhsAudibility > rhsAudibility;
    }
    return lhs.publicHandle < rhs.publicHandle;
}

bool VoiceManager::ParkCandidateBefore(const LiveCandidate& lhs,
                                       const LiveCandidate& rhs) {
    if (lhs.priority != rhs.priority) {
        return lhs.priority < rhs.priority;
    }
    const double lhsAudibility = OrderedAudibility(lhs.audibilityDb);
    const double rhsAudibility = OrderedAudibility(rhs.audibilityDb);
    if (lhsAudibility != rhsAudibility) {
        return lhsAudibility < rhsAudibility;
    }
    return static_cast<std::uint32_t>(lhs.renderHandle) <
           static_cast<std::uint32_t>(rhs.renderHandle);
}

void VoiceManager::ClearRequests() {
    m_requests.clear();
}

bool VoiceManager::AddRequest(const RenderRequest& request) {
    if (m_requests.size() == m_requests.capacity()) {
        return false;
    }
    m_requests.push_back(request);
    return true;
}

void VoiceManager::SortRequests() {
    std::sort(m_requests.begin(), m_requests.end(), RequestBefore);
}

void VoiceManager::ClearCandidates() {
    m_candidates.clear();
}

bool VoiceManager::AddCandidate(const LiveCandidate& candidate) {
    if (m_candidates.size() == m_candidates.capacity()) {
        return false;
    }
    m_candidates.push_back(candidate);
    return true;
}

void VoiceManager::SortParkCandidates() {
    std::sort(m_candidates.begin(), m_candidates.end(), ParkCandidateBefore);
}

bool VoiceManager::PublishClaim(VnVoiceHandle victimRenderHandle,
                                VnVoiceHandle publicClaimant) {
    if (victimRenderHandle == 0 || publicClaimant == 0) {
        return false;
    }
    const std::uint32_t slot =
        static_cast<std::uint32_t>(victimRenderHandle);
    if (slot >= m_claims.size() || m_claims[slot].active) {
        return false;
    }
    for (const Claim& claim : m_claims) {
        if (claim.active && claim.publicClaimant == publicClaimant) {
            return false;
        }
    }
    Claim& claim = m_claims[slot];
    claim.active = true;
    claim.victimRenderHandle = victimRenderHandle;
    claim.publicClaimant = publicClaimant;
    return true;
}

bool VoiceManager::ClaimFor(
    VnVoiceHandle publicClaimant,
    VnVoiceHandle* outVictimRenderHandle) const {
    for (const Claim& claim : m_claims) {
        if (claim.active && claim.publicClaimant == publicClaimant) {
            if (outVictimRenderHandle != nullptr) {
                *outVictimRenderHandle = claim.victimRenderHandle;
            }
            return true;
        }
    }
    return false;
}

bool VoiceManager::IsSlotClaimed(std::uint32_t renderSlot) const {
    return renderSlot < m_claims.size() && m_claims[renderSlot].active;
}

bool VoiceManager::IsClaimReady(VnVoiceHandle publicClaimant,
                                std::uint32_t freeGeneration,
                                bool victimStillLive) const {
    VnVoiceHandle victim = 0;
    return !victimStillLive && ClaimFor(publicClaimant, &victim) &&
           freeGeneration == SuccessorGeneration(victim);
}

void VoiceManager::ReleaseClaim(VnVoiceHandle publicClaimant) {
    for (Claim& claim : m_claims) {
        if (claim.active && claim.publicClaimant == publicClaimant) {
            claim = Claim{};
            return;
        }
    }
}

std::uint32_t VoiceManager::SuccessorGeneration(
    VnVoiceHandle renderHandle) {
    std::uint32_t next = static_cast<std::uint32_t>(renderHandle >> 32) + 1u;
    return next == 0 ? 1u : next;
}

} // namespace harken
