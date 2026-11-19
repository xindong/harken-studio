#ifndef HARKEN_RENDER_ORIGINAL_SOURCE_INSTALLATION_LOAN_H
#define HARKEN_RENDER_ORIGINAL_SOURCE_INSTALLATION_LOAN_H

#include "harken/render/OriginalSourceMutationActor.h"
#include <cstdint>

namespace harken {
class OriginalSourceSubmissionRegistry;
class VoicePool;

// Only the joint Source factory issues this pre-owned installation loan.
// It preserves the native Source record, never a PCM or effect payload.
class OriginalSourceInstallationLoan {
public:
    OriginalSourceInstallationLoan() noexcept = default;
    OriginalSourceInstallationLoan(const OriginalSourceInstallationLoan&) = delete;
    OriginalSourceInstallationLoan& operator=(const OriginalSourceInstallationLoan&) = delete;
    OriginalSourceInstallationLoan(OriginalSourceInstallationLoan&& other) noexcept;
    OriginalSourceInstallationLoan& operator=(OriginalSourceInstallationLoan&& other) noexcept;
    ~OriginalSourceInstallationLoan() { Release(); }
    bool Valid() const noexcept { return owner_ != nullptr; }
    void Release() noexcept;
private:
    friend class OriginalSourceSubmissionRegistry;
    friend class VoicePool;
    // Called only while the real native installation or rendering standing
    // prevents this self-loan from returning. Numeric lookup cannot issue it.
    // The actual callback immediately begins its first write under this loan.
    bool BorrowMutationForFirstWrite(OriginalSourceMutationActor* out) const noexcept;
    bool Matches(const void* actualSubmission, TimelineArmToken token,
        VnVoiceHandle sourceIdentity) const noexcept;
    OriginalSourceSubmissionRegistry* owner_ = nullptr;
    TimelineArmToken token_{};
    std::uint64_t marker_ = 0;
};
} // namespace harken
#endif
