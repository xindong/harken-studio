#ifndef HARKEN_CORE_PRIVATE_CATALOG_STAGE_H
#define HARKEN_CORE_PRIVATE_CATALOG_STAGE_H

#include "LogicCommitCoordinator.h"
#include "OriginalCatalogStageLoan.h"
#include "QueueRuntime.h"
#include "ScheduledTransactionPool.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <utility>

namespace harken {

// A preparation-only admission kernel. It cannot commit a new catalog, consume
// a sequence or publish a command head. All native participants remain Old.
// Reserve, owner close and reclamation run on one serialized logic producer.
// Distinct activity caps can cross threads. Receivers outlive all issued caps.
class PrivateCatalogStagePool {
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "Catalog activity requires a lock-free native 64-bit word");
    enum class Kind : std::uint8_t { Owner, Loan, Read, Mutation, Snapshot, Compare, Receipt,
                                    OriginBaseline, OriginalOldActivity };
    enum class BorrowedOriginMode : std::uint8_t { None, Normal, NativeCounted, NativeOverflow };
public:
    // An abort bit preserves the real count and epoch in a single atomic RMW.
    enum class State : std::uint8_t { Free = 0, PreparingOld = 1, AbortedOld = 3, Quarantined = 4 };
    enum class Result : std::uint8_t { Ok, Invalid, Busy, Exhausted };
    static constexpr unsigned kCountShift = 3;
    static constexpr unsigned kEpochShift = 11;
    static constexpr std::uint64_t kStateMask = 7;
    static constexpr std::uint64_t kCountMask = UINT64_C(255) << kCountShift;
    static constexpr std::uint64_t kEpochMax = UINT64_MAX >> kEpochShift;

    template<Kind K> class Cap {
    public:
        Cap() = default;
        Cap(const Cap&) = delete;
        Cap& operator=(const Cap&) = delete;
        Cap(Cap&& other) noexcept { Move(other); }
        Cap& operator=(Cap&& other) noexcept {
            if (this != &other) { Release(); Move(other); }
            return *this;
        }
        ~Cap() { Release(); }
        bool Valid() const noexcept { return pool_ != nullptr && pool_->Matches(*this); }
        void Release() noexcept {
            if (pool_ != nullptr) pool_->Return(*this);
        }
    private:
        friend class PrivateCatalogStagePool;
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
        friend struct PrivateCatalogStagePoolTestAccess;
#endif
        void Move(Cap& other) noexcept {
            pool_ = std::exchange(other.pool_, nullptr);
            index_ = std::exchange(other.index_, 0);
            generation_ = std::exchange(other.generation_, 0);
            marker_ = std::exchange(other.marker_, 0);
            expected_ = std::exchange(other.expected_, 0);
            originMode_ = std::exchange(other.originMode_, BorrowedOriginMode::None);
        }
        void Clear() noexcept {
            pool_ = nullptr; index_ = 0; generation_ = marker_ = expected_ = 0;
            originMode_ = BorrowedOriginMode::None;
        }
        PrivateCatalogStagePool* pool_ = nullptr;
        std::uint32_t index_ = 0;
        std::uint64_t generation_ = 0, marker_ = 0, expected_ = 0;
        BorrowedOriginMode originMode_ = BorrowedOriginMode::None;
    };
    using Owner = Cap<Kind::Owner>;
    using Loan = Cap<Kind::Loan>;
    using Read = Cap<Kind::Read>;
    using Mutation = Cap<Kind::Mutation>;
    using Snapshot = Cap<Kind::Snapshot>;
    using CompareProof = Cap<Kind::Compare>;
    using QuiescentComparison = Cap<Kind::Receipt>;
    // These retain the actual preparation kernel only. No Source/Control/Current
    // relationship, physical operation, Redirect or New selection is issued here.
    using OriginBaseline = OriginalCatalogStageBaselineLoan;
    using OriginalOldActivity = Cap<Kind::OriginalOldActivity>;
    using OriginalOldSibling = OriginalCatalogOldSiblingLoan;

    explicit PrivateCatalogStagePool(std::uint32_t capacity)
        : capacity_(capacity), rows_(capacity == 0 ? nullptr : std::make_unique<Row[]>(capacity)) {}
    PrivateCatalogStagePool(const PrivateCatalogStagePool&) = delete;
    PrivateCatalogStagePool& operator=(const PrivateCatalogStagePool&) = delete;
    PrivateCatalogStagePool(PrivateCatalogStagePool&&) = delete;
    PrivateCatalogStagePool& operator=(PrivateCatalogStagePool&&) = delete;
    ~PrivateCatalogStagePool() {
        ReclaimClosed();
        for (std::uint32_t i = 0; i < capacity_; ++i) {
            assert(rows_[i].structural.load(std::memory_order_acquire) == 0);
            assert(!rows_[i].scope.Valid() && !rows_[i].issue.Valid());
            assert(!rows_[i].first.Valid() && !rows_[i].second.Valid());
        }
    }

    // This retains authentic, independently issued inputs. It does not certify
    // their Queue/Current association or borrow finalization permissions.
    Result Reserve(QueueInstanceTable& queues,
                   QueueInstanceTable::PreparedCatalogResetScope* actualScope,
                   LogicCommitCoordinator::PreparedIssue* actualSequence,
                   ScheduledTransactionPool& groups,
                   const OriginalQueueFutureGroupRegistration& first,
                   const OriginalQueueFutureGroupRegistration& second,
                   Owner* out) noexcept {
        if (out == nullptr || out->pool_ != nullptr || actualScope == nullptr ||
            !actualScope->Valid() || actualScope->MatchedCount() == 0 ||
            actualSequence == nullptr || !actualSequence->Valid() ||
            !groups.IsOriginalQueueFutureGroupFamily(first, second)) return Result::Invalid;
        for (std::uint32_t i = 0; i < actualScope->MatchedCount(); ++i) {
            QueueInstanceToken identity{};
            QueueInstanceSnapshot observation{};
            if (!queues.CaptureCatalogResetScopeQueue(*actualScope, i, &identity, &observation))
                return Result::Invalid;
        }
        ReclaimClosed();
        if (nextMarker_ == UINT64_MAX) return Result::Exhausted;
        std::uint32_t index = capacity_;
        for (std::uint32_t i = 0; i < capacity_; ++i) {
            if (DecodeState(rows_[i].word.load(std::memory_order_acquire)) != State::Free) continue;
            if (rows_[i].generation.load(std::memory_order_relaxed) == UINT64_MAX) {
                rows_[i].word.store(Encode(State::Quarantined, 0, 0), std::memory_order_release);
                continue;
            }
            index = i; break;
        }
        if (index == capacity_) return Result::Busy;
        OriginalQueueFutureGroupLifetimePin firstLoan, secondLoan;
        if (!first.BorrowLifetime(&firstLoan) || !second.BorrowLifetime(&secondLoan)) return Result::Busy;
        auto& row = rows_[index];
        assert(row.structural.load(std::memory_order_acquire) == 0);
        row.scope = std::move(*actualScope);
        row.issue = std::move(*actualSequence);
        row.first = std::move(firstLoan);
        row.second = std::move(secondLoan);
        row.marker = ++nextMarker_;
        row.sequence = row.issue.Sequence();
        row.ownerPresent = true;
        row.normalOriginCount.store(0, std::memory_order_relaxed);
        assert(!row.nativeParent.Valid());
        row.nativeParentWord.store(0, std::memory_order_relaxed);
        row.nativeParentWrote.store(false, std::memory_order_relaxed);
        row.structural.store(1, std::memory_order_relaxed);
        const auto generation = row.generation.load(std::memory_order_relaxed) + 1;
        row.word.store(Encode(State::PreparingOld, 0, 1), std::memory_order_relaxed);
        row.generation.store(generation, std::memory_order_release);
        Set(out, index, generation, row.marker, 0);
        return Result::Ok;
    }

    template<Kind K> bool Borrow(const Cap<K>& actual, Loan* out) noexcept {
        if (!CanOutput(actual, out) || !AddStructural(rows_[actual.index_])) return false;
        Set(out, actual.index_, actual.generation_, actual.marker_, 0);
        return true;
    }
    // Serialized producer. A genuine owner supplies the structural baseline;
    // the later Engine joint issuer must independently certify each origin.
    bool BorrowOriginBaseline(const Owner& actual, OriginBaseline* out) noexcept {
        if (out == nullptr || out->pool_ != nullptr || !Matches(actual) ||
            !AddStructural(rows_[actual.index_])) return false;
        SetBaseline(out, actual.index_, actual.generation_, actual.marker_);
        return true;
    }
    bool BorrowOriginBaseline(const OriginBaseline& actual, OriginBaseline* out) noexcept {
        if (out == nullptr || out->pool_ != nullptr || !Matches(actual) ||
            !AddStructural(rows_[actual.index_])) return false;
        SetBaseline(out, actual.index_, actual.generation_, actual.marker_);
        return true;
    }
    // Native Old operations continue at the real 8-bit activity maximum. The
    // same word permanently aborts the candidate before an uncounted Old loan
    // is issued. Its independently retained structural cap still prevents reuse.
    Result BorrowOriginalOldActivity(const OriginBaseline& actual,
                                     OriginalOldActivity* out) noexcept {
        if (!CanOutput(actual, out)) return Result::Invalid;
        auto& row = rows_[actual.index_];
        if (!TryAddNativeStructural(row)) { AbortWord(row); return Result::Exhausted; }
        auto before = row.word.load(std::memory_order_acquire);
        for (unsigned attempt = 0; attempt < kNativeAttempts; ++attempt) {
            const auto state = DecodeState(before);
            if (state != State::PreparingOld && state != State::AbortedOld) {
                DropStructural(row); return Result::Invalid;
            }
            const auto count = DecodeCount(before);
            if (count == 255u) break;
            if (row.word.compare_exchange_strong(before,
                    Encode(state, count + 1u, DecodeEpoch(before)),
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                Set(out, actual.index_, actual.generation_, actual.marker_, 0,
                    BorrowedOriginMode::NativeCounted);
                return Result::Ok;
            }
        }
        // A genuine structural loan was acquired above. Real count saturation
        // or contention aborts the candidate before this retained Old fallback.
        AbortWord(row);
        Set(out, actual.index_, actual.generation_, actual.marker_, 0,
            BorrowedOriginMode::NativeOverflow);
        return Result::Ok;
    }
    bool BeforeOriginalOldWrite(OriginalOldActivity& actual) noexcept {
        if (!Matches(actual)) return false;
        if (actual.expected_ != 0) return true;
        if (!AdvanceOriginalOldWord(rows_[actual.index_])) return false;
        actual.expected_ = 1;
        return true;
    }
    // Continuous same-episode children retain one shared Catalog activity count.
    // Parent preparation and closing never make another callback wait or spin.
    Result BorrowOriginalOldSibling(const OriginBaseline& actual,
                                    OriginalOldSibling* out) noexcept {
        if (out == nullptr || out->pool_ != nullptr || !Matches(actual)) return Result::Invalid;
        auto& row = rows_[actual.index_];
        auto before = row.nativeParentWord.load(std::memory_order_acquire);
        const auto children = NativeChildren(before);
        if ((before & kNativeParentPhaseMask) == kNativeParentActive && children != 0 &&
            children < kNativeParentChildMax) {
            if (!TryAddNativeStructural(row)) { AbortWord(row); return Result::Exhausted; }
            // The serial is part of this same CAS word. A closed/reopened parent
            // with the same child count cannot lend a previous episode's child.
            if (row.nativeParentWord.compare_exchange_strong(before,
                    before + kNativeParentChild, std::memory_order_acq_rel, std::memory_order_acquire)) {
                SetSibling(out, actual, NativeSerial(before));
                return Result::Ok;
            }
            DropStructural(row);
        } else if ((before & kNativeParentLowMask) == 0) {
            if (!TryAddNativeStructural(row)) { AbortWord(row); return Result::Exhausted; }
            const auto serial = NativeSerial(before);
            if (row.nativeParentWord.compare_exchange_strong(before,
                    (before & ~kNativeParentLowMask) | kNativeParentInitializing,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                if (serial != UINT32_MAX) {
                    OriginalOldActivity parent;
                    const auto result = BorrowOriginalOldActivity(actual, &parent);
                    if (result == Result::Ok) {
                        row.nativeParent = std::move(parent);
                        row.nativeParentWrote.store(false, std::memory_order_relaxed);
                        SetSibling(out, actual, serial + 1u);
                        row.nativeParentWord.store(NativeParentWord(serial + 1u, 1),
                                                   std::memory_order_release);
                        return Result::Ok;
                    }
                    row.nativeParentWord.store(static_cast<std::uint64_t>(serial) << 32,
                                               std::memory_order_release);
                    DropStructural(row);
                    return result;
                }
                AbortWord(row);
                row.nativeParentWord.store(static_cast<std::uint64_t>(serial) << 32,
                                           std::memory_order_release);
            }
            DropStructural(row);
        }
        if ((before & kNativeParentPhaseMask) == kNativeParentActive &&
            NativeChildren(before) == kNativeParentChildMax) AbortWord(row);
        // The actual same-baseline retained activity is the bounded fallback.
        OriginalOldActivity fallback;
        const auto result = BorrowOriginalOldActivity(actual, &fallback);
        if (result != Result::Ok) return result;
        SetSibling(out, actual, 0);
        out->mode_ = fallback.originMode_ == BorrowedOriginMode::NativeCounted
            ? OriginalOldSibling::Mode::Counted : OriginalOldSibling::Mode::AbortedRetained;
        out->wrote_ = fallback.expected_ != 0;
        // A direct move of this genuine loan's counter ownership, not a new mint.
        fallback.Clear();
        return Result::Ok;
    }
    bool BeforeOriginalOldSiblingWrite(OriginalOldSibling& actual) noexcept {
        if (!MatchesSibling(actual)) return false;
        if (actual.parentSerial_ == 0) {
            if (actual.wrote_) return true;
            if (!AdvanceOriginalOldWord(rows_[actual.index_])) return false;
            actual.wrote_ = true;
            return true;
        }
        auto& row = rows_[actual.index_];
        if (row.nativeParentWrote.load(std::memory_order_acquire)) return true;
        // Concurrent first writers can both advance the epoch. Every writer
        // still follows a successful same-word change before its own payload.
        if (!AdvanceOriginalOldWord(row)) return false;
        row.nativeParentWrote.store(true, std::memory_order_release);
        return true;
    }
    template<Kind K> Result BorrowRead(const Cap<K>& actual, Read* out) noexcept {
        return AcquireActivity(actual, out, false);
    }
    template<Kind K> Result BorrowMutation(const Cap<K>& actual, Mutation* out) noexcept {
        // The native Old write occurs only after this returns Ok. Epoch rises
        // in the same CAS as count acquisition, even during an active snapshot.
        return AcquireActivity(actual, out, true);
    }
    bool Upgrade(Read* actual, Mutation* out) noexcept {
        if (actual == nullptr || !CanOutput(*actual, out)) return false;
        auto& row = rows_[actual->index_];
        auto before = row.word.load(std::memory_order_acquire);
        for (;;) {
            auto state = DecodeState(before);
            auto epoch = DecodeEpoch(before);
            if ((state != State::PreparingOld && state != State::AbortedOld) || DecodeCount(before) == 0)
                return false;
            if (state == State::PreparingOld) {
                if (epoch == kEpochMax) state = State::AbortedOld;
                else ++epoch;
            }
            const auto next = Encode(state, DecodeCount(before), epoch);
            if (row.word.compare_exchange_weak(before, next, std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                Transfer(actual, out, 0);
                out->originMode_ = BorrowedOriginMode::Normal;
                return true;
            }
        }
    }
    Result BeginSnapshot(const Owner& actual, Snapshot* out) noexcept {
        if (!CanOutput(actual, out)) return Result::Invalid;
        auto& row = rows_[actual.index_];
        if (!AddStructural(row)) return Result::Busy;
        auto before = row.word.load(std::memory_order_acquire);
        for (;;) {
            if (DecodeState(before) != State::PreparingOld || DecodeCount(before) != 0) {
                DropStructural(row); return Result::Busy;
            }
            const auto held = Encode(State::PreparingOld, 1, DecodeEpoch(before));
            if (row.word.compare_exchange_weak(before, held, std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                Set(out, actual.index_, actual.generation_, actual.marker_, before);
                return Result::Ok;
            }
        }
    }
    bool FinishSnapshot(Snapshot* actual, CompareProof* out) noexcept {
        if (actual == nullptr || !CanOutput(*actual, out)) return false;
        auto& row = rows_[actual->index_];
        auto expectedHeld = actual->expected_ + (UINT64_C(1) << kCountShift);
        if (!row.word.compare_exchange_strong(expectedHeld, actual->expected_,
                std::memory_order_acq_rel, std::memory_order_acquire)) return false;
        Transfer(actual, out, actual->expected_);
        return true;
    }
    bool TryCompare(CompareProof* actual, QuiescentComparison* out) noexcept {
        if (actual == nullptr || !CanOutput(*actual, out)) return false;
        auto expected = actual->expected_;
        // A historical, exact quiescent comparison only. No New visibility bit
        // is set; later native Old mutators remain free to advance the epoch.
        if (!rows_[actual->index_].word.compare_exchange_strong(expected, actual->expected_,
                std::memory_order_acq_rel, std::memory_order_acquire)) return false;
        Transfer(actual, out, actual->expected_);
        return true;
    }
    bool Abort(const Owner& actual) noexcept {
        if (!Matches(actual)) return false;
        AbortWord(rows_[actual.index_]);
        // Return producer exclusivity before retained cleanup services Old.
        rows_[actual.index_].issue.Release();
        return true;
    }
    template<Kind K> bool CaptureState(const Cap<K>& actual, State* out) const noexcept {
        if (out == nullptr || !Matches(actual)) return false;
        *out = DecodeState(rows_[actual.index_].word.load(std::memory_order_acquire));
        return true;
    }
    template<Kind K> LogicCommitSequence ReservedSequence(const Cap<K>& actual) const noexcept {
        return Matches(actual) ? rows_[actual.index_].sequence : 0;
    }

    // No native callback releases group/Queue capabilities. This producer-only
    // sweep runs after real foreign activity and structural loans are returned.
    std::uint32_t ReclaimClosed() noexcept {
        std::uint32_t reclaimed = 0;
        for (std::uint32_t i = 0; i < capacity_; ++i) {
            auto& row = rows_[i];
            if (DecodeState(row.word.load(std::memory_order_acquire)) != State::AbortedOld ||
                row.ownerPresent || row.structural.load(std::memory_order_acquire) != 0) continue;
            assert(DecodeCount(row.word.load(std::memory_order_acquire)) == 0);
            assert(row.normalOriginCount.load(std::memory_order_acquire) == 0);
            assert((row.nativeParentWord.load(std::memory_order_acquire) & kNativeParentLowMask) == 0);
            assert(!row.nativeParent.Valid());
            row.issue.Release();
            row.second.Release(); row.first.Release();
            row.scope.Release();
            row.sequence = 0; row.marker = 0;
            row.word.store(Encode(row.generation.load(std::memory_order_relaxed) == UINT64_MAX ?
                State::Quarantined : State::Free, 0, 0), std::memory_order_release);
            ++reclaimed;
        }
        return reclaimed;
    }
private:
    friend class OriginalCatalogStageBaselineLoan;
    friend class OriginalCatalogOldSiblingLoan;
    struct Row {
        std::atomic<std::uint64_t> word{0}, generation{0};
        std::atomic<std::uint32_t> structural{0}, normalOriginCount{0};
        // Stable storage; this move-only parent never enters copy-reset rows.
        std::atomic<std::uint64_t> nativeParentWord{0};
        std::atomic<bool> nativeParentWrote{false};
        OriginalOldActivity nativeParent;
        std::uint64_t marker = 0;
        LogicCommitSequence sequence = 0;
        bool ownerPresent = false;
        QueueInstanceTable::PreparedCatalogResetScope scope;
        LogicCommitCoordinator::PreparedIssue issue;
        OriginalQueueFutureGroupLifetimePin first, second;
    };
    static constexpr unsigned kNativeAttempts = 8;
    static constexpr std::uint64_t kAbortBit = 2;
    static constexpr unsigned kNativeParentCountShift = 2;
    static constexpr std::uint64_t kNativeParentPhaseMask = 3;
    static constexpr std::uint64_t kNativeParentLowMask = UINT32_MAX;
    static constexpr std::uint64_t kNativeParentInitializing = 1;
    static constexpr std::uint64_t kNativeParentActive = 2;
    static constexpr std::uint64_t kNativeParentChild = UINT64_C(1) << kNativeParentCountShift;
    static constexpr std::uint32_t kNativeParentChildMax = UINT32_MAX >> kNativeParentCountShift;
    static std::uint32_t NativeChildren(std::uint64_t word) noexcept {
        return static_cast<std::uint32_t>(word) >> kNativeParentCountShift;
    }
    static std::uint32_t NativeSerial(std::uint64_t word) noexcept {
        return static_cast<std::uint32_t>(word >> 32);
    }
    static std::uint64_t NativeParentWord(std::uint32_t serial, std::uint32_t children) noexcept {
        return (static_cast<std::uint64_t>(serial) << 32) |
            (static_cast<std::uint64_t>(children) << kNativeParentCountShift) | kNativeParentActive;
    }
    bool MatchesSibling(const OriginalOldSibling& actual) const noexcept {
        if (actual.pool_ != this || actual.index_ >= capacity_ || actual.generation_ == 0 ||
            rows_[actual.index_].generation.load(std::memory_order_acquire) != actual.generation_ ||
            rows_[actual.index_].marker != actual.marker_) return false;
        if (actual.parentSerial_ == 0)
            return actual.mode_ == OriginalOldSibling::Mode::Counted ||
                (actual.mode_ == OriginalOldSibling::Mode::AbortedRetained &&
                 DecodeState(rows_[actual.index_].word.load(std::memory_order_acquire)) == State::AbortedOld);
        const auto word = rows_[actual.index_].nativeParentWord.load(std::memory_order_acquire);
        return NativeSerial(word) == actual.parentSerial_ &&
            (word & kNativeParentPhaseMask) == kNativeParentActive && NativeChildren(word) != 0;
    }
    void SetSibling(OriginalOldSibling* out, const OriginBaseline& actual,
                    std::uint64_t parentSerial) noexcept {
        out->pool_ = this; out->index_ = actual.index_; out->generation_ = actual.generation_;
        out->marker_ = actual.marker_; out->parentSerial_ = parentSerial;
        out->mode_ = parentSerial == 0 ? OriginalOldSibling::Mode::None : OriginalOldSibling::Mode::Shared;
        out->wrote_ = false;
    }
    void ReturnSibling(OriginalOldSibling& actual) noexcept {
        const bool matches = MatchesSibling(actual);
        assert(matches);
        if (!matches) return;
        auto& row = rows_[actual.index_];
        const auto mode = actual.mode_;
        const bool shared = actual.parentSerial_ != 0;
        actual.Clear();
        if (!shared) {
            if (mode == OriginalOldSibling::Mode::Counted) {
                const auto previous = row.word.fetch_sub(UINT64_C(1) << kCountShift,
                                                        std::memory_order_acq_rel);
                assert(DecodeCount(previous) != 0);
            } else {
                assert(mode == OriginalOldSibling::Mode::AbortedRetained);
                assert(DecodeState(row.word.load(std::memory_order_acquire)) == State::AbortedOld);
            }
            DropStructural(row);
            return;
        }
        assert(mode == OriginalOldSibling::Mode::Shared);
        // Active/count0 is closing. New borrowers use their own retained loan.
        const auto previous = row.nativeParentWord.fetch_sub(kNativeParentChild,
                                                            std::memory_order_acq_rel);
        assert((previous & kNativeParentPhaseMask) == kNativeParentActive);
        assert(NativeChildren(previous) != 0);
        if (NativeChildren(previous) == 1u) {
            // Preclear Record-owned fields before the last native return. This
            // child's genuine structural loan still retains the actual Row.
            OriginalOldActivity local(std::move(row.nativeParent));
            row.nativeParentWrote.store(false, std::memory_order_relaxed);
            local.Release();
            row.nativeParentWord.store(static_cast<std::uint64_t>(NativeSerial(previous)) << 32,
                                       std::memory_order_release);
        }
        DropStructural(row);
    }
    static bool TryAddNativeStructural(Row& row) noexcept {
        auto count = row.structural.load(std::memory_order_acquire);
        for (unsigned attempt = 0; attempt < kNativeAttempts; ++attempt) {
            if (count == 0 || count == UINT32_MAX) return false;
            if (row.structural.compare_exchange_strong(count, count + 1u,
                    std::memory_order_acq_rel, std::memory_order_acquire)) return true;
        }
        return false;
    }
    static bool AdvanceOriginalOldWord(Row& row) noexcept {
        auto before = row.word.load(std::memory_order_acquire);
        for (unsigned attempt = 0; attempt < kNativeAttempts; ++attempt) {
            const auto state = DecodeState(before);
            if (state == State::AbortedOld) return true;
            if (state != State::PreparingOld) return false;
            const auto epoch = DecodeEpoch(before);
            if (epoch == kEpochMax) break;
            if (row.word.compare_exchange_strong(before,
                    Encode(State::PreparingOld, DecodeCount(before), epoch + 1u),
                    std::memory_order_acq_rel, std::memory_order_acquire)) return true;
        }
        AbortWord(row);
        return true;
    }
    static State DecodeState(std::uint64_t value) noexcept { return static_cast<State>(value & kStateMask); }
    static std::uint32_t DecodeCount(std::uint64_t value) noexcept {
        return static_cast<std::uint32_t>((value & kCountMask) >> kCountShift);
    }
    static std::uint64_t DecodeEpoch(std::uint64_t value) noexcept { return value >> kEpochShift; }
    static std::uint64_t Encode(State state, std::uint32_t count, std::uint64_t epoch) noexcept {
        return static_cast<std::uint64_t>(state) | (static_cast<std::uint64_t>(count) << kCountShift) |
            (epoch << kEpochShift);
    }
    template<Kind K> bool Matches(const Cap<K>& actual) const noexcept {
        return actual.pool_ == this && actual.index_ < capacity_ && actual.generation_ != 0 &&
            rows_[actual.index_].generation.load(std::memory_order_acquire) == actual.generation_ &&
            rows_[actual.index_].marker == actual.marker_;
    }
    bool Matches(const OriginBaseline& actual) const noexcept {
        return actual.pool_ == this && actual.index_ < capacity_ && actual.generation_ != 0 &&
            rows_[actual.index_].generation.load(std::memory_order_acquire) == actual.generation_ &&
            rows_[actual.index_].marker == actual.marker_;
    }
    template<Kind B> bool CanOutput(const OriginBaseline& actual, const Cap<B>* out) const noexcept {
        return out != nullptr && out->pool_ == nullptr && Matches(actual);
    }
    void SetBaseline(OriginBaseline* out, std::uint32_t index, std::uint64_t generation,
                     std::uint64_t marker) noexcept {
        out->pool_ = this; out->index_ = index; out->generation_ = generation; out->marker_ = marker;
    }
    void ReturnBaseline(OriginBaseline& actual) noexcept {
        const bool matches = Matches(actual);
        assert(matches);
        if (!matches) return;
        auto& row = rows_[actual.index_];
        actual.Clear();
        DropStructural(row);
    }
    template<Kind A, Kind B> bool CanOutput(const Cap<A>& actual, const Cap<B>* out) const noexcept {
        return out != nullptr && out->pool_ == nullptr && Matches(actual);
    }
    template<Kind K> void Set(Cap<K>* out, std::uint32_t index, std::uint64_t generation,
                             std::uint64_t marker, std::uint64_t expected,
                             BorrowedOriginMode originMode = BorrowedOriginMode::None) noexcept {
        out->pool_ = this; out->index_ = index; out->generation_ = generation;
        out->marker_ = marker; out->expected_ = expected;
        out->originMode_ = originMode;
    }
    template<Kind A, Kind B> void Transfer(Cap<A>* from, Cap<B>* to, std::uint64_t expected) noexcept {
        Set(to, from->index_, from->generation_, from->marker_, expected);
        from->Clear();
    }
    static bool AddStructural(Row& row) noexcept {
        auto count = row.structural.load(std::memory_order_acquire);
        while (count != 0 && count != UINT32_MAX) {
            if (row.structural.compare_exchange_weak(count, count + 1, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) return true;
        }
        return false;
    }
    static void DropStructural(Row& row) noexcept {
        const auto previous = row.structural.fetch_sub(1, std::memory_order_acq_rel);
        assert(previous != 0);
        // No Row access after this final structural return.
    }
    static bool AddNormalOrigin(Row& row) noexcept {
        auto count = row.normalOriginCount.load(std::memory_order_acquire);
        while (count != UINT32_MAX) {
            if (row.normalOriginCount.compare_exchange_weak(count, count + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire)) return true;
        }
        return false;
    }
    template<Kind A, Kind B> Result AcquireActivity(const Cap<A>& actual, Cap<B>* out, bool mutation) noexcept {
        if (!CanOutput(actual, out)) return Result::Invalid;
        auto& row = rows_[actual.index_];
        if (!AddStructural(row)) return Result::Busy;
        if (!AddNormalOrigin(row)) { DropStructural(row); return Result::Busy; }
        auto before = row.word.load(std::memory_order_acquire);
        for (;;) {
            const auto state = DecodeState(before);
            if ((state != State::PreparingOld && state != State::AbortedOld) || DecodeCount(before) == 255) {
                row.normalOriginCount.fetch_sub(1, std::memory_order_acq_rel);
                DropStructural(row); return Result::Busy;
            }
            auto nextState = state;
            auto epoch = DecodeEpoch(before);
            if (mutation && state == State::PreparingOld) {
                if (epoch == kEpochMax) nextState = State::AbortedOld;
                else ++epoch;
            }
            const auto next = Encode(nextState, DecodeCount(before) + 1, epoch);
            if (row.word.compare_exchange_weak(before, next, std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                Set(out, actual.index_, actual.generation_, actual.marker_, 0, BorrowedOriginMode::Normal);
                return Result::Ok;
            }
        }
    }
    static void AbortWord(Row& row) noexcept {
        // A live factory-issued structural cap retains this full-generation
        // episode. Free, quarantine and a reused Row cannot race this RMW.
        const auto previous = row.word.fetch_or(kAbortBit, std::memory_order_acq_rel);
        assert(DecodeState(previous) == State::PreparingOld ||
               DecodeState(previous) == State::AbortedOld);
    }
    template<Kind K> void Return(Cap<K>& actual) noexcept {
        assert(Matches(actual));
        if (!Matches(actual)) return;
        auto& row = rows_[actual.index_];
        const auto originMode = actual.originMode_;
        actual.Clear();
        if constexpr (K == Kind::Owner) {
            AbortWord(row); row.issue.Release(); row.ownerPresent = false;
        } else if constexpr (K == Kind::OriginalOldActivity) {
            if (originMode == BorrowedOriginMode::NativeCounted) {
                const auto previous = row.word.fetch_sub(UINT64_C(1) << kCountShift,
                                                        std::memory_order_acq_rel);
                assert(DecodeCount(previous) != 0);
            } else {
                assert(originMode == BorrowedOriginMode::NativeOverflow);
                assert(DecodeState(row.word.load(std::memory_order_acquire)) == State::AbortedOld);
            }
        } else if constexpr (K == Kind::Read || K == Kind::Mutation || K == Kind::Snapshot) {
            const auto previous = row.word.fetch_sub(UINT64_C(1) << kCountShift, std::memory_order_acq_rel);
            assert(DecodeCount(previous) != 0);
            if constexpr (K != Kind::Snapshot) {
                // Return the mode recorded by the actual normal-origin borrow.
                // This kernel deliberately has no Redirect issuer or selector.
                assert(originMode == BorrowedOriginMode::Normal);
                const auto origins = row.normalOriginCount.fetch_sub(1, std::memory_order_acq_rel);
                assert(origins != 0);
            }
        }
        DropStructural(row);
    }
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
    friend struct PrivateCatalogStagePoolTestAccess;
#endif
    const std::uint32_t capacity_;
    std::unique_ptr<Row[]> rows_;
    std::uint64_t nextMarker_ = 0;
};

} // namespace harken
#endif
