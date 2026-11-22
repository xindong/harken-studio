// Logic-thread authority for Render Voice admission and exact victim claims.
// The manager owns policy and reservations only. It never touches render
// contexts, issues commands, or crosses to the audio thread.
#ifndef HARKEN_CORE_VOICE_MANAGER_H
#define HARKEN_CORE_VOICE_MANAGER_H

#include <harken/Harken.h>

#include <cstdint>
#include <span>
#include "harken/core/StableRowStorage.h"

namespace harken {

class VoiceManager {
public:
    enum class Residency : std::uint8_t {
        ApproximateParkingAllowed,
        RenderRequired
    };

    enum class Protection : std::uint8_t {
        Displaceable,
        Protected
    };

    enum class RequestKind : std::uint8_t {
        Fresh,
        Pending,
        ParkedReturn
    };

    struct RenderRequest {
        RequestKind kind = RequestKind::Fresh;
        VnVoiceHandle publicHandle = 0;
        std::uint32_t ledgerSlot = 0;
        std::uint32_t priority = 128;
        double audibilityDb = 0.0;
        Residency residency = Residency::ApproximateParkingAllowed;
    };

    struct LiveCandidate {
        VnVoiceHandle renderHandle = 0;
        VnVoiceHandle publicHandle = 0;
        std::uint32_t priority = 128;
        std::uint64_t startOrder = 0;
        std::uint64_t sourceCursor = 0;
        std::uint32_t loopsRemaining = 0;
        double audibilityDb = 0.0;
        Residency residency = Residency::ApproximateParkingAllowed;
        Protection protection = Protection::Displaceable;
    };

    struct FreeRender {
        bool available = false;
        std::uint32_t slot = 0;
        std::uint32_t generation = 0;
    };

    enum class DecisionKind : std::uint8_t {
        Refuse,
        DirectVirtual,
        UseFreeRender,
        Steal
    };

    struct Decision {
        DecisionKind kind = DecisionKind::Refuse;
        std::uint32_t renderSlot = 0;
        std::uint32_t renderGeneration = 0;
        VnVoiceHandle victimRenderHandle = 0;
    };

    VoiceManager(std::uint32_t renderCapacity, std::uint32_t logicalCapacity, std::uint32_t initialRender = UINT32_MAX);
    bool PrepareRenderSlots(std::uint32_t count) noexcept;

    Decision Decide(const RenderRequest& request, bool parkingEnabled,
                    bool virtualCapacityAvailable, double parkThresholdDb,
                    std::uint32_t stealPolicy, const FreeRender& freeRender,
                    const LiveCandidate* candidates,
                    std::uint32_t candidateCount) const;

    static bool RequestBefore(const RenderRequest& lhs,
                              const RenderRequest& rhs);
    static bool ParkCandidateBefore(const LiveCandidate& lhs,
                                    const LiveCandidate& rhs);

    void ClearRequests();
    bool AddRequest(const RenderRequest& request);
    void SortRequests();
    std::span<const RenderRequest> Requests() const { return {m_requests.data(), m_requestCount}; }

    void ClearCandidates();
    bool AddCandidate(const LiveCandidate& candidate);
    void SortParkCandidates();
    std::span<const LiveCandidate> Candidates() const {
        return {m_candidates.data(), m_candidateCount};
    }

    bool PublishClaim(VnVoiceHandle victimRenderHandle,
                      VnVoiceHandle publicClaimant);
    bool ClaimFor(VnVoiceHandle publicClaimant,
                  VnVoiceHandle* outVictimRenderHandle) const;
    bool IsSlotClaimed(std::uint32_t renderSlot) const;
    bool IsClaimReady(VnVoiceHandle publicClaimant,
                      std::uint32_t freeGeneration,
                      bool victimStillLive) const;
    void ReleaseClaim(VnVoiceHandle publicClaimant);

    static std::uint32_t SuccessorGeneration(VnVoiceHandle renderHandle);

private:
    struct Claim {
        bool active = false;
        VnVoiceHandle victimRenderHandle = 0;
        VnVoiceHandle publicClaimant = 0;
    };

    StableRowStorage<Claim> m_claims;
    std::uint32_t m_virtualCapacity;
    StableRowStorage<RenderRequest> m_requests;
    std::uint32_t m_requestCount = 0;
    StableRowStorage<LiveCandidate> m_candidates;
    std::uint32_t m_candidateCount = 0;
};

} // namespace harken

#endif // HARKEN_CORE_VOICE_MANAGER_H
