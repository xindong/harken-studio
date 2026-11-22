#include "harken/core/PrivateCatalogStage.h"
#include <utility>

namespace harken {
OriginalCatalogStageBaselineLoan::OriginalCatalogStageBaselineLoan(
    OriginalCatalogStageBaselineLoan&& other) noexcept { Take(other); }
OriginalCatalogStageBaselineLoan& OriginalCatalogStageBaselineLoan::operator=(
    OriginalCatalogStageBaselineLoan&& other) noexcept {
    if (this != &other) { Release(); Take(other); }
    return *this;
}
OriginalCatalogStageBaselineLoan::~OriginalCatalogStageBaselineLoan() { Release(); }
bool OriginalCatalogStageBaselineLoan::Valid() const noexcept {
    return pool_ != nullptr && pool_->Matches(*this);
}
bool OriginalCatalogStageBaselineLoan::Borrow(OriginalCatalogStageBaselineLoan* out) const noexcept {
    return pool_ != nullptr && pool_->BorrowOriginBaseline(*this, out);
}
bool OriginalCatalogStageBaselineLoan::BorrowNativeOldSibling(
    OriginalCatalogOldSiblingLoan* out) const noexcept {
    return pool_ != nullptr && pool_->BorrowOriginalOldSibling(*this, out) ==
        PrivateCatalogStagePool::Result::Ok;
}
bool OriginalCatalogStageBaselineLoan::AbortNativeOldCandidate() const noexcept {
    if (pool_ == nullptr || !pool_->Matches(*this)) return false;
    pool_->AbortWord(pool_->rows_[index_]);
    return true;
}
bool OriginalCatalogStageBaselineLoan::AbortedForNativeDetach() const noexcept {
    return pool_ != nullptr && pool_->Matches(*this) &&
        pool_->DecodeState(pool_->rows_[index_].word.load(std::memory_order_acquire)) ==
            PrivateCatalogStagePool::State::AbortedOld;
}
void OriginalCatalogStageBaselineLoan::Release() noexcept {
    if (pool_ != nullptr) pool_->ReturnBaseline(*this);
}
void OriginalCatalogStageBaselineLoan::Take(OriginalCatalogStageBaselineLoan& other) noexcept {
    pool_ = std::exchange(other.pool_, nullptr);
    index_ = std::exchange(other.index_, 0);
    generation_ = std::exchange(other.generation_, 0);
    marker_ = std::exchange(other.marker_, 0);
}
void OriginalCatalogStageBaselineLoan::Clear() noexcept {
    pool_ = nullptr; index_ = 0; generation_ = marker_ = 0;
}

OriginalCatalogOldSiblingLoan::OriginalCatalogOldSiblingLoan(
    OriginalCatalogOldSiblingLoan&& other) noexcept { Take(other); }
OriginalCatalogOldSiblingLoan& OriginalCatalogOldSiblingLoan::operator=(
    OriginalCatalogOldSiblingLoan&& other) noexcept {
    if (this != &other) { Release(); Take(other); }
    return *this;
}
OriginalCatalogOldSiblingLoan::~OriginalCatalogOldSiblingLoan() { Release(); }
bool OriginalCatalogOldSiblingLoan::Valid() const noexcept {
    return pool_ != nullptr && pool_->MatchesSibling(*this);
}
bool OriginalCatalogOldSiblingLoan::BeforeFirstNativeOldWrite() noexcept {
    return pool_ != nullptr && pool_->BeforeOriginalOldSiblingWrite(*this);
}
void OriginalCatalogOldSiblingLoan::Release() noexcept {
    if (pool_ != nullptr) pool_->ReturnSibling(*this);
}
void OriginalCatalogOldSiblingLoan::Take(OriginalCatalogOldSiblingLoan& other) noexcept {
    pool_ = std::exchange(other.pool_, nullptr);
    index_ = std::exchange(other.index_, 0);
    generation_ = std::exchange(other.generation_, 0);
    marker_ = std::exchange(other.marker_, 0);
    parentSerial_ = std::exchange(other.parentSerial_, 0);
    mode_ = std::exchange(other.mode_, Mode::None);
    wrote_ = std::exchange(other.wrote_, false);
}
void OriginalCatalogOldSiblingLoan::Clear() noexcept {
    pool_ = nullptr; index_ = 0; generation_ = marker_ = parentSerial_ = 0;
    mode_ = Mode::None; wrote_ = false;
}
} // namespace harken
