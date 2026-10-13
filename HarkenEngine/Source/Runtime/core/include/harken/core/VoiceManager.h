// Logic-thread authority for Render Voice admission and exact victim claims.
// The manager owns policy and reservations only. It never touches render
// contexts, issues commands, or crosses to the audio thread.
#ifndef HARKEN_CORE_VOICE_MANAGER_H
#define HARKEN_CORE_VOICE_MANAGER_H

#include <harken/Harken.h>

#include <cstdint>
#include <vector>

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

    VoiceManager(std::uint32_t renderCapacity, std::uint32_t logicalCapacity);

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
    const std::vector<RenderRequest>& Requests() const { return m_requests; }

    void ClearCandidates();
    bool AddCandidate(const LiveCandidate& candidate);
    void SortParkCandidates();
    const std::vector<LiveCandidate>& Candidates() const {
        return m_candidates;
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

    std::vector<Claim> m_claims;
    std::vector<RenderRequest> m_requests;
    std::vector<LiveCandidate> m_candidates;
};

} // namespace harken

#endif // HARKEN_CORE_VOICE_MANAGER_H
