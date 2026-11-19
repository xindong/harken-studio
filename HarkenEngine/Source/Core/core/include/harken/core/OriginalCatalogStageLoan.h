#ifndef HARKEN_CORE_ORIGINAL_CATALOG_STAGE_LOAN_H
#define HARKEN_CORE_ORIGINAL_CATALOG_STAGE_LOAN_H
#include <cstdint>

namespace harken {
class PrivateCatalogStagePool;
class OriginalSourceSubmissionRegistry;
class CommandQueue;
class OriginalCatalogOldSiblingLoan;

// Structural retention from an actual Stage owner. This carries no Source,
// Current, Control, calendar, selector or physical-operation permission.
class OriginalCatalogStageBaselineLoan {
public:
    OriginalCatalogStageBaselineLoan() noexcept = default;
    OriginalCatalogStageBaselineLoan(const OriginalCatalogStageBaselineLoan&) = delete;
    OriginalCatalogStageBaselineLoan& operator=(const OriginalCatalogStageBaselineLoan&) = delete;
    OriginalCatalogStageBaselineLoan(OriginalCatalogStageBaselineLoan&& other) noexcept;
    OriginalCatalogStageBaselineLoan& operator=(OriginalCatalogStageBaselineLoan&& other) noexcept;
    ~OriginalCatalogStageBaselineLoan();
    bool Valid() const noexcept;
    bool Borrow(OriginalCatalogStageBaselineLoan* out) const noexcept;
    void Release() noexcept;
private:
    friend class PrivateCatalogStagePool;
    friend class OriginalSourceSubmissionRegistry;
    friend class CommandQueue;
    bool BorrowNativeOldSibling(OriginalCatalogOldSiblingLoan* out) const noexcept;
    bool AbortNativeOldCandidate() const noexcept;
    bool AbortedForNativeDetach() const noexcept;
    void Take(OriginalCatalogStageBaselineLoan& other) noexcept;
    void Clear() noexcept;
    PrivateCatalogStagePool* pool_ = nullptr;
    std::uint32_t index_ = 0;
    std::uint64_t generation_ = 0, marker_ = 0;
};

// An authentic retained native Old activity, or one child of its genuine shared
// parent. Only the Stage issuer can transfer the actual count into this object.
class OriginalCatalogOldSiblingLoan {
public:
    OriginalCatalogOldSiblingLoan() noexcept = default;
    OriginalCatalogOldSiblingLoan(const OriginalCatalogOldSiblingLoan&) = delete;
    OriginalCatalogOldSiblingLoan& operator=(const OriginalCatalogOldSiblingLoan&) = delete;
    OriginalCatalogOldSiblingLoan(OriginalCatalogOldSiblingLoan&& other) noexcept;
    OriginalCatalogOldSiblingLoan& operator=(OriginalCatalogOldSiblingLoan&& other) noexcept;
    ~OriginalCatalogOldSiblingLoan();
    bool Valid() const noexcept;
    void Release() noexcept;
private:
    friend class PrivateCatalogStagePool;
    friend class OriginalSourceSubmissionRegistry;
    friend class CommandQueue;
    bool BeforeFirstNativeOldWrite() noexcept;
    enum class Mode : std::uint8_t { None, Shared, Counted, AbortedRetained };
    void Take(OriginalCatalogOldSiblingLoan& other) noexcept;
    void Clear() noexcept;
    PrivateCatalogStagePool* pool_ = nullptr;
    std::uint32_t index_ = 0;
    std::uint64_t generation_ = 0, marker_ = 0, parentSerial_ = 0;
    Mode mode_ = Mode::None;
    bool wrote_ = false;
};
} // namespace harken
#endif
