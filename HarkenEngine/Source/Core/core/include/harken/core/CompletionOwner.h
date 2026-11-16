#ifndef HARKEN_CORE_COMPLETION_OWNER_H
#define HARKEN_CORE_COMPLETION_OWNER_H

#include "harken/core/DecisionToken.h"

#include <atomic>
#include <cstdint>

namespace harken {

enum class CompletionOwnerKind : std::uint8_t {
    None = 0,
    SingleQueue = 1,
    Composite = 2,
};

struct CompositeCompletionOwnerToken {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;

    bool Valid() const noexcept { return generation != 0; }
};

struct CompletionOwnerRef {
    CompletionOwnerKind kind = CompletionOwnerKind::None;
    CompositeCompletionOwnerToken composite{};
    DecisionToken singleDecision{};
    std::uint64_t factsRevision = 0;

    static CompletionOwnerRef Single(
        DecisionToken decision,
        std::uint64_t revision) noexcept {
        CompletionOwnerRef ref{};
        ref.kind = CompletionOwnerKind::SingleQueue;
        ref.singleDecision = decision;
        ref.factsRevision = revision;
        return ref;
    }

    static CompletionOwnerRef Composite(
        CompositeCompletionOwnerToken token,
        std::uint64_t revision) noexcept {
        CompletionOwnerRef ref{};
        ref.kind = CompletionOwnerKind::Composite;
        ref.composite = token;
        ref.factsRevision = revision;
        return ref;
    }
};

bool SameCompletionOwner(const CompletionOwnerRef& a,
                         const CompletionOwnerRef& b) noexcept;

class CompletionOwnerBackref {
public:
    bool TryPin(const CompletionOwnerRef& owner) noexcept;
    bool Capture(CompletionOwnerRef* out) const noexcept;
    bool ReplaceExact(const CompletionOwnerRef& expected,
                      const CompletionOwnerRef& replacement) noexcept;
    bool ClearExact(const CompletionOwnerRef& owner) noexcept;
    bool IsUnpinnedForRelease() const noexcept;
    bool Exhausted() const noexcept;

private:
    enum class State : std::uint64_t {
        None = 0,
        Claiming = 1,
        Pinned = 2,
        Exhausted = 3,
    };

    bool CaptureWithControl(CompletionOwnerRef* out,
                            std::uint64_t* control) const noexcept;

    std::atomic<std::uint64_t> control_{0};
    std::atomic<std::uint64_t> composite_{0};
    std::atomic<std::uint64_t> singleDecision_{0};
    std::atomic<std::uint64_t> factsRevision_{0};
    std::atomic<std::uint32_t> kind_{0};
};

} // namespace harken

#endif // HARKEN_CORE_COMPLETION_OWNER_H
