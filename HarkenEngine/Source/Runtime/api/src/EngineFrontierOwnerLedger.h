#ifndef HARKEN_API_ENGINE_FRONTIER_OWNER_LEDGER_H
#define HARKEN_API_ENGINE_FRONTIER_OWNER_LEDGER_H

#include <array>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace harken {

enum class FrontierOwnerKind : std::uint8_t {
    Selection, Decision, Occurrence, Composite, Voice, Mode, Service,
    Timeline, Group, Transition, Count
};

enum class FrontierOwnerCommitPhase : std::uint8_t {
    Selection, Decision, Completion, Voice, Resource, Mode, Publication, Count
};

enum class FrontierOwnerCancelPhase : std::uint8_t {
    FrontierBinding, Exposure, Voice, Completion, InitialDecision, Mode, Selection, Count
};

struct FrontierOwnerRef {
    FrontierOwnerKind kind = FrontierOwnerKind::Count;
    std::uint32_t slot = UINT32_MAX;
    std::uint64_t generation = 0;
    bool operator==(const FrontierOwnerRef&) const noexcept = default;
};

// Contexts remain in stable attempt/service storage. Only cancel rights live here.
class EngineFrontierOwnerLedger {
public:
    using ValidateFn = bool (*)(void*) noexcept;
    using CommitFn = bool (*)(void*, std::uint64_t) noexcept;
    using CancelFn = bool (*)(void*) noexcept;
    static constexpr std::size_t kDomainCount =
        static_cast<std::size_t>(FrontierOwnerKind::Count);
    using Limits = std::array<std::uint32_t, kDomainCount>;

    EngineFrontierOwnerLedger() = default;
    EngineFrontierOwnerLedger(const EngineFrontierOwnerLedger&) = delete;
    EngineFrontierOwnerLedger& operator=(const EngineFrontierOwnerLedger&) = delete;
    EngineFrontierOwnerLedger(EngineFrontierOwnerLedger&& other) noexcept
        : entries_(std::move(other.entries_)), limits_(other.limits_),
          domainCounts_(other.domainCounts_), generation_(other.generation_),
          count_(other.count_), unresolved_(other.unresolved_), phase_(other.phase_) {
        other.count_ = 0;
        other.unresolved_ = 0;
        other.generation_ = UINT64_MAX;
    }
    EngineFrontierOwnerLedger& operator=(EngineFrontierOwnerLedger&&) = delete;

    void Initialize(const Limits& limits) {
        if (generation_ != 0 || !entries_.empty()) throw std::bad_alloc{};
        std::uint64_t count = 0;
        for (const auto limit : limits) count += limit;
        if (count > UINT32_MAX ||
            count > std::numeric_limits<std::size_t>::max() / sizeof(Entry))
            throw std::bad_alloc{};
        entries_.resize(static_cast<std::size_t>(count));
        limits_ = limits;
    }

    bool Begin(std::uint64_t generation) noexcept {
        if (unresolved_ != 0 || generation == 0 || generation <= generation_)
            return false;
        generation_ = generation;
        count_ = 0;
        domainCounts_.fill(0);
        phase_ = Phase::Preparing;
        return true;
    }

    bool Add(std::uint64_t generation, FrontierOwnerRef identity, void* context,
             FrontierOwnerCommitPhase commitPhase,
             FrontierOwnerCancelPhase cancelPhase,
             ValidateFn validate, CommitFn commit, CancelFn cancel) noexcept {
        const auto domain = static_cast<std::size_t>(identity.kind);
        if (!Matches(generation) || phase_ != Phase::Preparing ||
            domain >= kDomainCount || identity.slot == UINT32_MAX ||
            identity.generation == 0 || context == nullptr || validate == nullptr ||
            commit == nullptr || cancel == nullptr ||
            commitPhase >= FrontierOwnerCommitPhase::Count ||
            cancelPhase >= FrontierOwnerCancelPhase::Count ||
            count_ >= entries_.size() || domainCounts_[domain] >= limits_[domain])
            return false;
        for (std::uint32_t index = 0; index < count_; ++index) {
            if (entries_[index].identity == identity || entries_[index].context == context)
                return false;
        }
        entries_[count_++] = {identity, context, validate, commit, cancel,
            commitPhase, cancelPhase, State::Prepared};
        ++domainCounts_[domain];
        ++unresolved_;
        return true;
    }

    bool Validate(std::uint64_t generation) const noexcept {
        if (!Matches(generation) || phase_ != Phase::Preparing) return false;
        for (std::uint32_t index = 0; index < count_; ++index) {
            const auto& entry = entries_[index];
            if (entry.state != State::Prepared || !entry.validate(entry.context))
                return false;
        }
        return true;
    }

    bool Commit(std::uint64_t generation, std::uint64_t ticket) noexcept {
        if (!Validate(generation)) return false;
        phase_ = Phase::Committing;
        for (std::uint8_t phase = 0;
             phase < static_cast<std::uint8_t>(FrontierOwnerCommitPhase::Count); ++phase) {
            for (std::uint32_t index = 0; index < count_; ++index) {
                auto& entry = entries_[index];
                if (static_cast<std::uint8_t>(entry.commitPhase) != phase) continue;
                entry.state = State::Committing;
                if (!entry.commit(entry.context, ticket)) {
                    phase_ = Phase::CancelPending;
                    return false;
                }
                entry.state = State::Committed;
            }
        }
        phase_ = Phase::Committed;
        return true;
    }

    bool Cancel(std::uint64_t generation) noexcept {
        if (!Matches(generation) || phase_ == Phase::Transferred) return false;
        phase_ = Phase::CancelPending;
        for (std::uint8_t phase = 0;
             phase < static_cast<std::uint8_t>(FrontierOwnerCancelPhase::Count); ++phase) {
            for (std::uint32_t index = count_; index-- > 0;) {
                auto& entry = entries_[index];
                if (entry.state == State::Cancelled ||
                    static_cast<std::uint8_t>(entry.cancelPhase) != phase) continue;
                // A failed owner retains every not-yet-cancelled dependency.
                if (!entry.cancel(entry.context)) {
                    return false;
                }
                entry.state = State::Cancelled;
                --unresolved_;
            }
        }
        return unresolved_ == 0;
    }

    bool CanTransfer(std::uint64_t generation) const noexcept {
        return Matches(generation) && phase_ == Phase::Committed;
    }
    bool Transfer(std::uint64_t generation) noexcept {
        if (!CanTransfer(generation)) return false;
        for (std::uint32_t index = 0; index < count_; ++index)
            entries_[index].state = State::Transferred;
        unresolved_ = 0;
        phase_ = Phase::Transferred;
        return true;
    }

    bool Empty() const noexcept { return unresolved_ == 0; }
    std::uint32_t UnresolvedCount() const noexcept { return unresolved_; }
    std::uint32_t NextContextSlot(FrontierOwnerKind kind) const noexcept {
        const auto domain = static_cast<std::size_t>(kind);
        return domain < kDomainCount ? domainCounts_[domain] : UINT32_MAX;
    }
    std::uint32_t Capacity() const noexcept {
        return static_cast<std::uint32_t>(entries_.size());
    }
    std::size_t StorageBytes() const noexcept { return entries_.capacity() * sizeof(Entry); }

private:
    enum class State : std::uint8_t { Prepared, Committing, Committed, Cancelled, Transferred };
    enum class Phase : std::uint8_t { Preparing, Committing, Committed, CancelPending, Transferred };
    struct Entry {
        FrontierOwnerRef identity{};
        void* context = nullptr;
        ValidateFn validate = nullptr;
        CommitFn commit = nullptr;
        CancelFn cancel = nullptr;
        FrontierOwnerCommitPhase commitPhase = FrontierOwnerCommitPhase::Selection;
        FrontierOwnerCancelPhase cancelPhase = FrontierOwnerCancelPhase::Selection;
        State state = State::Prepared;
    };
    bool Matches(std::uint64_t generation) const noexcept {
        return generation != 0 && generation == generation_;
    }
    std::vector<Entry> entries_;
    Limits limits_{};
    Limits domainCounts_{};
    std::uint64_t generation_ = 0;
    std::uint32_t count_ = 0;
    std::uint32_t unresolved_ = 0;
    Phase phase_ = Phase::Preparing;
};

struct EngineFrontierOwnerBatch {
    EngineFrontierOwnerLedger* ledger = nullptr;
    std::uint64_t generation = 0;
    static bool Validate(void* context) noexcept {
        const auto* batch = static_cast<EngineFrontierOwnerBatch*>(context);
        return batch != nullptr && batch->ledger != nullptr &&
            batch->ledger->Validate(batch->generation);
    }
    static bool Commit(void* context, std::uint64_t ticket) noexcept {
        auto* batch = static_cast<EngineFrontierOwnerBatch*>(context);
        return batch != nullptr && batch->ledger != nullptr &&
            batch->ledger->Commit(batch->generation, ticket);
    }
    static bool Cancel(void* context) noexcept {
        auto* batch = static_cast<EngineFrontierOwnerBatch*>(context);
        return batch != nullptr && batch->ledger != nullptr &&
            batch->ledger->Cancel(batch->generation);
    }
};

} // namespace harken

#endif
