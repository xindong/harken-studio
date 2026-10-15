#ifndef HARKEN_API_ENGINE_PENDING_SELECTION_BATCH_H
#define HARKEN_API_ENGINE_PENDING_SELECTION_BATCH_H

#include "harken/core/CurrentSelection.h"

#include <cassert>
#include <array>
#include <algorithm>
#include <optional>
#include <utility>

namespace harken {

// Owns the original selection request until final publication or cancellation.
// Acceptance transfers cancellation rights; it never consumes selection history.
class EnginePendingSelectionBatch {
public:
    EnginePendingSelectionBatch() noexcept = default;
    EnginePendingSelectionBatch(const EnginePendingSelectionBatch&) = delete;
    EnginePendingSelectionBatch& operator=(const EnginePendingSelectionBatch&) = delete;
    EnginePendingSelectionBatch(EnginePendingSelectionBatch&& other) noexcept {
        const bool moved = other.MoveTo(*this);
        assert(moved);
        (void)moved;
    }
    EnginePendingSelectionBatch& operator=(EnginePendingSelectionBatch&& other) noexcept {
        if (this != &other) {
            const bool moved = other.MoveTo(*this);
            assert(moved);
            (void)moved;
        }
        return *this;
    }

    bool Empty() const noexcept { return !batch_.has_value(); }
    bool Live() const noexcept {
        return !Empty() && table_ != nullptr && table_->ReadPendingBatch(*batch_).live;
    }
    bool Accepted() const noexcept { return accepted_; }
    bool OwnsTable(const SelectionMemoryTableV1& table) const noexcept {
        return !Empty() && table_ == &table;
    }
    bool ReplacementPending() const noexcept { return replacement_.has_value(); }
    bool MatchesOwner(const SelectionMemoryTableV1* table,
        const SelectionPendingBatchIdentityV1& identity) const noexcept {
        return table != nullptr && table_ == table && Live() &&
            identity_.world == identity.world &&
            identity_.worldSemanticFingerprint == identity.worldSemanticFingerprint &&
            identity_.rootStableId == identity.rootStableId && identity_.callerSeed == identity.callerSeed &&
            identity_.acceptedPostOrdinal == identity.acceptedPostOrdinal && identity_.ticket == identity.ticket &&
            identity_.emitterSlot == identity.emitterSlot && identity_.emitterGeneration == identity.emitterGeneration &&
            identity_.eventBegin == identity.eventBegin && identity_.eventCount == identity.eventCount;
    }
    bool MatchesEmitter(std::uint32_t slot, std::uint32_t generation) const noexcept {
        return !Empty() && identity_.emitterSlot == slot && identity_.emitterGeneration == generation;
    }
    std::uint64_t MaterializedRevision() const noexcept { return revision_; }
    std::uint64_t MaterializedFrame() const noexcept { return frame_; }
    bool MatchesMaterialized() const noexcept {
        if (Empty() || table_ == nullptr || ReplacementPending()) return false;
        const auto view = table_->ReadPendingBatch(*batch_);
        if (!view.live || view.revision != revision_ || view.memberCount != memberCount_) return false;
        for (std::uint32_t index = 0; index < memberCount_; ++index) {
            const auto member = table_->ReadPendingBatchMember(*batch_, index);
            if (!member.delta.live || member.revision != revision_ ||
                member.delta.status != SelectionMemoryStatusV1::Ok ||
                member.delta.selectedChildOrdinal != materializedOrdinals_[index]) return false;
        }
        return true;
    }

    // The caller has already certified the actual tree, source, cut and anchor.
    // A follower install can advance this batch's revision without changing its tree.
    bool CertifyMaterialized(std::uint64_t frame, const std::uint32_t* actualOrdinals,
        std::uint32_t count) noexcept {
        if (Empty() || table_ == nullptr || ReplacementPending() || frame != frame_ || count != memberCount_ ||
            (count != 0 && actualOrdinals == nullptr)) return false;
        const auto view = table_->ReadPendingBatch(*batch_);
        if (!view.live || view.memberCount != count) return false;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto member = table_->ReadPendingBatchMember(*batch_, index);
            if (actualOrdinals[index] != materializedOrdinals_[index] || !member.delta.live ||
                member.revision != view.revision || member.delta.status != SelectionMemoryStatusV1::Ok ||
                member.delta.selectedChildOrdinal != actualOrdinals[index]) return false;
        }
        revision_ = view.revision;
        return true;
    }

    SelectionMemoryStatusV1 BeginReplacement() noexcept {
        if (!Live() || ReplacementPending()) return SelectionMemoryStatusV1::InvalidInput;
        const auto view = table_->ReadPendingBatch(*batch_);
        replacement_.emplace();
        const auto status = table_->BeginPendingBatchReplacement(*batch_, view.revision, &*replacement_);
        if (status != SelectionMemoryStatusV1::Ok && !replacement_->Valid()) replacement_.reset();
        return status;
    }
    SelectionMemoryStatusV1 PrepareReplacementPlay(const SelectionPlayRequestV1& request,
        std::uint32_t* selectedOrdinal) noexcept {
        if (!ReplacementPending() || table_ == nullptr) return SelectionMemoryStatusV1::InvalidInput;
        return table_->PrepareReplacementPlay(&*replacement_, request, selectedOrdinal);
    }
    SelectionMemoryStatusV1 SealReplacement(std::uint64_t frame, const std::uint32_t* actualOrdinals,
        std::uint32_t count) noexcept {
        if (!ReplacementPending() || table_ == nullptr || replacementSealed_ ||
            count > replacementOrdinals_.size() || (count != 0 && actualOrdinals == nullptr))
            return SelectionMemoryStatusV1::InvalidInput;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto member = table_->ReadReplacementPlay(*replacement_, index);
            if (member.status != SelectionMemoryStatusV1::Ok) return member.status;
            if (member.selectedChildOrdinal != actualOrdinals[index]) return SelectionMemoryStatusV1::InvalidInput;
        }
        const auto status = table_->SealReplacement(&*replacement_, count, frame);
        if (status != SelectionMemoryStatusV1::Ok) return status;
        replacementFrame_ = frame;
        replacementCount_ = count;
        if (count != 0) std::copy_n(actualOrdinals, count, replacementOrdinals_.begin());
        replacementSealed_ = true;
        return status;
    }
    SelectionMemoryStatusV1 CanInstallReplacement() const noexcept {
        return !Empty() && table_ != nullptr && ReplacementPending() && replacementSealed_
            ? table_->CanInstallReplacement(*replacement_) : SelectionMemoryStatusV1::InvalidInput;
    }
    SelectionMemoryStatusV1 InstallReplacement() noexcept {
        const auto ready = CanInstallReplacement();
        if (ready != SelectionMemoryStatusV1::Ok) return ready;
        const auto installed = table_->InstallReplacement(&*replacement_, &*batch_);
        if (installed != SelectionMemoryStatusV1::Ok) return installed;
        const auto view = table_->ReadPendingBatch(*batch_);
        revision_ = view.revision;
        frame_ = replacementFrame_;
        memberCount_ = replacementCount_;
        materializedOrdinals_ = replacementOrdinals_;
        ClearReplacement();
        return MatchesMaterialized() ? SelectionMemoryStatusV1::Ok : SelectionMemoryStatusV1::RuntimeRecreateRequired;
    }
    bool CancelReplacement() noexcept {
        if (!ReplacementPending()) return true;
        if (table_ == nullptr || table_->CancelReplacement(&*replacement_) != SelectionMemoryStatusV1::Ok)
            return false;
        ClearReplacement();
        return true;
    }

    SelectionMemoryStatusV1 Issue(SelectionMemoryTableV1& table,
        const SelectionPendingBatchIdentityV1& identity,
        PreparedSelectionPlayV1* const* prepared, std::uint32_t count,
        std::uint64_t frame, const std::uint32_t* materializedOrdinals) noexcept {
        if (!Empty() || count > materializedOrdinals_.size() ||
            (count != 0 && materializedOrdinals == nullptr)) return SelectionMemoryStatusV1::InvalidInput;
        batch_.emplace();
        const auto status = table.IssueProducerPendingSelectionBatch(identity, prepared, count, &*batch_);
        if (status != SelectionMemoryStatusV1::Ok) {
            assert(!batch_->Valid());
            if (!batch_->Valid()) batch_.reset();
            return status;
        }
        table_ = &table;
        identity_ = identity;
        frame_ = frame;
        const auto view = table.ReadPendingBatch(*batch_);
        revision_ = view.revision;
        memberCount_ = view.memberCount;
        if (count != 0) std::copy_n(materializedOrdinals, count, materializedOrdinals_.begin());
        return view.live && view.memberCount == count ? SelectionMemoryStatusV1::Ok
            : SelectionMemoryStatusV1::RuntimeRecreateRequired;
    }

    bool Accept(std::uint64_t ticket) noexcept {
        if (!Live() || ticket == 0 || ticket != identity_.ticket) return false;
        accepted_ = true;
        return true;
    }
    bool MoveTo(EnginePendingSelectionBatch& receiver) noexcept {
        if (&receiver == this || !receiver.Empty() || ReplacementPending()) return false;
        if (Empty()) return true;
        receiver.batch_.emplace(std::move(*batch_));
        receiver.table_ = std::exchange(table_, nullptr);
        receiver.identity_ = std::exchange(identity_, {});
        receiver.revision_ = std::exchange(revision_, 0);
        receiver.frame_ = std::exchange(frame_, 0);
        receiver.memberCount_ = std::exchange(memberCount_, 0);
        receiver.materializedOrdinals_ = std::exchange(materializedOrdinals_, {});
        receiver.accepted_ = std::exchange(accepted_, false);
        batch_.reset();
        return true;
    }
    bool Cancel() noexcept {
        if (Empty()) return true;
        if (!CancelReplacement()) return false;
        if (table_ == nullptr || table_->CancelPendingBatch(&*batch_) != SelectionMemoryStatusV1::Ok)
            return false;
        ClearConsumed();
        return true;
    }
    PendingSelectionBatchCommitV1 CommitInput() noexcept {
        return {MatchesMaterialized() ? &*batch_ : nullptr, revision_};
    }
    bool ReturnAfterCommit() noexcept {
        if (Empty() || batch_->Valid()) return false;
        if (ReplacementPending()) {
            if (table_ == nullptr ||
                table_->ReadReplacementPlay(*replacement_, 0).status != SelectionMemoryStatusV1::NotFound)
                return false;
            // Core returns associated candidates when it clears this exact original token.
            ClearReplacement();
        }
        ClearConsumed();
        return true;
    }

private:
    void ClearReplacement() noexcept {
        assert(replacement_.has_value() && (!replacement_->Valid() ||
            (batch_.has_value() && !batch_->Valid() && table_ != nullptr &&
             table_->ReadReplacementPlay(*replacement_, 0).status == SelectionMemoryStatusV1::NotFound)));
        replacement_.reset();
        replacementOrdinals_ = {};
        replacementFrame_ = 0;
        replacementCount_ = 0;
        replacementSealed_ = false;
    }
    void ClearConsumed() noexcept {
        assert(batch_.has_value() && !batch_->Valid() && !ReplacementPending());
        batch_.reset();
        table_ = nullptr;
        identity_ = {};
        revision_ = frame_ = 0;
        memberCount_ = 0;
        materializedOrdinals_ = {};
        accepted_ = false;
    }
    std::optional<PendingSelectionBatchV1> batch_;
    std::optional<PendingSelectionReplacementV1> replacement_;
    SelectionMemoryTableV1* table_ = nullptr;
    SelectionPendingBatchIdentityV1 identity_{};
    std::uint64_t revision_ = 0;
    std::uint64_t frame_ = 0;
    std::uint32_t memberCount_ = 0;
    std::array<std::uint32_t, kSelectionPathNodeCapacity> materializedOrdinals_{};
    std::array<std::uint32_t, kSelectionPathNodeCapacity> replacementOrdinals_{};
    std::uint64_t replacementFrame_ = 0;
    std::uint32_t replacementCount_ = 0;
    bool replacementSealed_ = false;
    bool accepted_ = false;
};

enum class EngineSelectionPreparationMode : std::uint8_t {
    Legacy,
    PendingInitial,
    PendingReplacement,
};

// Borrowed by the complete factory traversal; replacement cancellation belongs
// to the original batch rather than an individual legacy prepared token.
struct EngineSelectionPreparation {
    EngineSelectionPreparationMode mode = EngineSelectionPreparationMode::Legacy;
    EnginePendingSelectionBatch* replacementOwner = nullptr;

    SelectionMemoryStatusV1 Prepare(SelectionMemoryTableV1& table,
        const SelectionPlayRequestV1& request, PreparedSelectionPlayV1* prepared,
        std::uint32_t* selectedOrdinal) const noexcept {
        if (selectedOrdinal == nullptr) return SelectionMemoryStatusV1::InvalidInput;
        switch (mode) {
        case EngineSelectionPreparationMode::Legacy:
            if (replacementOwner != nullptr || prepared == nullptr)
                return SelectionMemoryStatusV1::InvalidInput;
            return table.PreparePlay(request, prepared, selectedOrdinal);
        case EngineSelectionPreparationMode::PendingInitial:
            if (replacementOwner != nullptr || prepared == nullptr)
                return SelectionMemoryStatusV1::InvalidInput;
            return table.PreparePlayForPendingBatch(request, prepared, selectedOrdinal);
        case EngineSelectionPreparationMode::PendingReplacement:
            if (replacementOwner == nullptr || !replacementOwner->OwnsTable(table) ||
                !replacementOwner->ReplacementPending() ||
                (prepared != nullptr && prepared->Valid())) return SelectionMemoryStatusV1::InvalidInput;
            return replacementOwner->PrepareReplacementPlay(request, selectedOrdinal);
        }
        return SelectionMemoryStatusV1::InvalidInput;
    }

    bool Cancel(SelectionMemoryTableV1& table, PreparedSelectionPlayV1* prepared) const noexcept {
        if (mode == EngineSelectionPreparationMode::PendingReplacement)
            return replacementOwner != nullptr && replacementOwner->OwnsTable(table) &&
                replacementOwner->CancelReplacement();
        return (mode == EngineSelectionPreparationMode::Legacy ||
                mode == EngineSelectionPreparationMode::PendingInitial) &&
            replacementOwner == nullptr && prepared != nullptr &&
            table.CancelPreparedPlay(prepared) == SelectionMemoryStatusV1::Ok;
    }
};

} // namespace harken

#endif
