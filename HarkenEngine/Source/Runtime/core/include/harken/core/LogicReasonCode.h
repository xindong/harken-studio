// Reason codes for the logic-side seams: one vocabulary for why something was
// refused, dropped, or arrived too late.
//
// The seams already return their own typed refusals where a caller has to branch
// on the answer. This vocabulary is for the other direction — reporting. Without
// it, each seam invents its own wording for the same situation and a reader
// cannot tell whether two reports describe one condition or two.
//
// Deliberately internal and deliberately not a log vocabulary. Codes are stable
// numbers with stable names for diagnostics; nothing here reserves a log record
// or a capture surface, because a compile-time or logic-side reason has no
// business occupying the runtime's log numbering.
#ifndef HARKEN_CORE_LOGIC_REASON_CODE_H
#define HARKEN_CORE_LOGIC_REASON_CODE_H

#include <cstdint>

namespace harken {

// Append-only. A code's number is what a stored diagnostic keeps, so renumbering
// would silently change the meaning of records already written.
enum class LogicReasonCode : std::uint32_t {
    None = 0,
    // The owning instance no longer exists, so the work has nobody to belong to.
    OwnerStale = 1,
    // The owning instance cancelled, superseding the stamp this work carried.
    OwnerCancelled = 2,
    // A single occurrence was cancelled while its host continued.
    OccurrenceCancelled = 3,
    // The target frame is behind or beyond the schedulable window.
    OutsideHorizonWindow = 4,
    // The page covering the target frame had already been published.
    HorizonPageFrozen = 5,
    // A fixed pool or table was full. Refused rather than evicted.
    CapacityExhausted = 6,
    // One owner had already taken its share of a shared pool.
    OwnerShareExhausted = 7,
    // A participating service refused room, so the whole bundle was abandoned.
    BundleReservationRefused = 8,
    // A reference slot resolved to nothing and the slot has no default.
    ReferenceSlotUnresolved = 9,
    // A resolved target does not exist in this session.
    ReferenceSlotTargetMissing = 10,
    // A resolved bus exists but cannot take this material's channel layout.
    ReferenceSlotLayoutIncompatible = 11,
    // An ancestor chain ran past its depth ceiling, so no resolution can be
    // claimed for it.
    AncestorChainTooDeep = 12
};

inline constexpr std::uint32_t kLogicReasonCodeCount = 13;

// Stable diagnostic spelling of a code, or "unknown" for a number this build
// does not define. Never null, so a report cannot lose its reason to a null
// check.
const char* LogicReasonCodeName(LogicReasonCode code);

} // namespace harken

#endif // HARKEN_CORE_LOGIC_REASON_CODE_H
