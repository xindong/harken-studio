#include "harken/core/CompletionOwner.h"

#include <limits>

namespace harken {
namespace {

bool Valid(const CompletionOwnerRef& owner) noexcept {
    if (owner.factsRevision == 0) {
        return false;
    }
    if (owner.kind == CompletionOwnerKind::SingleQueue) {
        return owner.singleDecision.Valid() && !owner.composite.Valid();
    }
    if (owner.kind == CompletionOwnerKind::Composite) {
        return owner.composite.Valid() && !owner.singleDecision.Valid();
    }
    return false;
}

std::uint64_t Pack(CompositeCompletionOwnerToken token) noexcept {
    return (static_cast<std::uint64_t>(token.generation) << 32) | token.slot;
}

std::uint64_t Pack(DecisionToken token) noexcept {
    return (static_cast<std::uint64_t>(token.generation) << 32) | token.slot;
}

CompositeCompletionOwnerToken Unpack(std::uint64_t packed) noexcept {
    return {static_cast<std::uint32_t>(packed),
            static_cast<std::uint32_t>(packed >> 32)};
}

DecisionToken UnpackDecision(std::uint64_t packed) noexcept {
    return {static_cast<std::uint32_t>(packed),
            static_cast<std::uint32_t>(packed >> 32)};
}

constexpr std::uint64_t kStateMask = 3;
constexpr std::uint64_t kMaxEpoch =
    (std::numeric_limits<std::uint64_t>::max)() >> 2;

std::uint64_t MakeControl(std::uint64_t epoch,
                          std::uint64_t state) noexcept {
    return (epoch << 2) | state;
}

std::uint64_t ControlState(std::uint64_t control) noexcept {
    return control & kStateMask;
}

std::uint64_t ControlEpoch(std::uint64_t control) noexcept {
    return control >> 2;
}

} // namespace

bool SameCompletionOwner(const CompletionOwnerRef& a,
                         const CompletionOwnerRef& b) noexcept {
    return a.kind == b.kind &&
           a.composite.slot == b.composite.slot &&
           a.composite.generation == b.composite.generation &&
           a.singleDecision.slot == b.singleDecision.slot &&
           a.singleDecision.generation == b.singleDecision.generation &&
           a.factsRevision == b.factsRevision;
}

bool CompletionOwnerBackref::TryPin(
    const CompletionOwnerRef& owner) noexcept {
    if (!Valid(owner)) {
        return false;
    }
    std::uint64_t expected = control_.load(std::memory_order_seq_cst);
    if (ControlState(expected) != static_cast<std::uint64_t>(State::None)) {
        return false;
    }
    const std::uint64_t epoch = ControlEpoch(expected);
    if (epoch == kMaxEpoch) {
        const std::uint64_t exhausted = MakeControl(
            epoch, static_cast<std::uint64_t>(State::Exhausted));
        control_.compare_exchange_strong(
            expected, exhausted, std::memory_order_seq_cst,
            std::memory_order_seq_cst);
        return false;
    }
    const std::uint64_t claiming = MakeControl(
        epoch + 1, static_cast<std::uint64_t>(State::Claiming));
    if (!control_.compare_exchange_strong(
            expected, claiming, std::memory_order_seq_cst,
            std::memory_order_seq_cst)) {
        return false;
    }
    composite_.store(Pack(owner.composite), std::memory_order_seq_cst);
    singleDecision_.store(Pack(owner.singleDecision),
                          std::memory_order_seq_cst);
    factsRevision_.store(owner.factsRevision, std::memory_order_seq_cst);
    kind_.store(static_cast<std::uint32_t>(owner.kind),
                std::memory_order_seq_cst);
    control_.store(MakeControl(
                       epoch + 1,
                       static_cast<std::uint64_t>(State::Pinned)),
                   std::memory_order_seq_cst);
    return true;
}

bool CompletionOwnerBackref::CaptureWithControl(
    CompletionOwnerRef* out, std::uint64_t* control) const noexcept {
    if (out == nullptr || control == nullptr) {
        return false;
    }
    const std::uint64_t before = control_.load(std::memory_order_seq_cst);
    if (ControlState(before) != static_cast<std::uint64_t>(State::Pinned)) {
        return false;
    }
    CompletionOwnerRef captured{};
    captured.kind = static_cast<CompletionOwnerKind>(
        kind_.load(std::memory_order_seq_cst));
    captured.composite = Unpack(composite_.load(std::memory_order_seq_cst));
    captured.singleDecision = UnpackDecision(
        singleDecision_.load(std::memory_order_seq_cst));
    captured.factsRevision =
        factsRevision_.load(std::memory_order_seq_cst);
    if (control_.load(std::memory_order_seq_cst) != before ||
        !Valid(captured)) {
        return false;
    }
    *out = captured;
    *control = before;
    return true;
}

bool CompletionOwnerBackref::Capture(
    CompletionOwnerRef* out) const noexcept {
    std::uint64_t control = 0;
    return CaptureWithControl(out, &control);
}

bool CompletionOwnerBackref::ReplaceExact(
    const CompletionOwnerRef& expected,
    const CompletionOwnerRef& replacement) noexcept {
    if (!Valid(replacement)) {
        return false;
    }
    CompletionOwnerRef current{};
    std::uint64_t pinned = 0;
    if (!CaptureWithControl(&current, &pinned) ||
        !SameCompletionOwner(current, expected)) {
        return false;
    }
    const std::uint64_t epoch = ControlEpoch(pinned);
    if (epoch == kMaxEpoch) {
        return false;
    }
    const std::uint64_t claiming = MakeControl(
        epoch + 1, static_cast<std::uint64_t>(State::Claiming));
    if (!control_.compare_exchange_strong(
            pinned, claiming, std::memory_order_seq_cst,
            std::memory_order_seq_cst)) {
        return false;
    }
    composite_.store(Pack(replacement.composite),
                     std::memory_order_seq_cst);
    singleDecision_.store(Pack(replacement.singleDecision),
                          std::memory_order_seq_cst);
    factsRevision_.store(replacement.factsRevision,
                         std::memory_order_seq_cst);
    kind_.store(static_cast<std::uint32_t>(replacement.kind),
                std::memory_order_seq_cst);
    control_.store(MakeControl(
                       epoch + 1,
                       static_cast<std::uint64_t>(State::Pinned)),
                   std::memory_order_seq_cst);
    return true;
}

bool CompletionOwnerBackref::ClearExact(
    const CompletionOwnerRef& owner) noexcept {
    CompletionOwnerRef current{};
    std::uint64_t pinned = 0;
    if (!CaptureWithControl(&current, &pinned) ||
        !SameCompletionOwner(current, owner)) {
        return false;
    }
    const std::uint64_t epoch = ControlEpoch(pinned);
    const std::uint64_t claiming = MakeControl(
        epoch, static_cast<std::uint64_t>(State::Claiming));
    if (!control_.compare_exchange_strong(
            pinned, claiming, std::memory_order_seq_cst,
            std::memory_order_seq_cst)) {
        return false;
    }
    kind_.store(0, std::memory_order_seq_cst);
    composite_.store(0, std::memory_order_seq_cst);
    singleDecision_.store(0, std::memory_order_seq_cst);
    factsRevision_.store(0, std::memory_order_seq_cst);
    control_.store(MakeControl(
                       epoch,
                       static_cast<std::uint64_t>(
                           epoch == kMaxEpoch ? State::Exhausted
                                              : State::None)),
                   std::memory_order_seq_cst);
    return true;
}

bool CompletionOwnerBackref::IsUnpinnedForRelease() const noexcept {
    const std::uint64_t state =
        ControlState(control_.load(std::memory_order_seq_cst));
    return state == static_cast<std::uint64_t>(State::None) ||
           state == static_cast<std::uint64_t>(State::Exhausted);
}

bool CompletionOwnerBackref::Exhausted() const noexcept {
    return ControlState(control_.load(std::memory_order_seq_cst)) ==
           static_cast<std::uint64_t>(State::Exhausted);
}

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

} // namespace harken
