#include "harken/core/LogicReasonCode.h"

#include <iterator>

namespace harken {
namespace {

struct Row {
    LogicReasonCode code;
    const char* name;
};

// One row per code, in numeric order. A new code without a row fails the
// completeness check below rather than printing as a bare number, which is the
// failure mode a hand-maintained table otherwise develops.
constexpr Row kRows[] = {
    {LogicReasonCode::None, "none"},
    {LogicReasonCode::OwnerStale, "ownerStale"},
    {LogicReasonCode::OwnerCancelled, "ownerCancelled"},
    {LogicReasonCode::OccurrenceCancelled, "occurrenceCancelled"},
    {LogicReasonCode::OutsideHorizonWindow, "outsideHorizonWindow"},
    {LogicReasonCode::HorizonPageFrozen, "horizonPageFrozen"},
    {LogicReasonCode::CapacityExhausted, "capacityExhausted"},
    {LogicReasonCode::OwnerShareExhausted, "ownerShareExhausted"},
    {LogicReasonCode::BundleReservationRefused, "bundleReservationRefused"},
    {LogicReasonCode::ReferenceSlotUnresolved, "referenceSlotUnresolved"},
    {LogicReasonCode::ReferenceSlotTargetMissing, "referenceSlotTargetMissing"},
    {LogicReasonCode::ReferenceSlotLayoutIncompatible,
     "referenceSlotLayoutIncompatible"},
    {LogicReasonCode::AncestorChainTooDeep, "ancestorChainTooDeep"},
};

static_assert(std::size(kRows) == kLogicReasonCodeCount,
              "every logic reason code needs exactly one name row");

} // namespace

const char* LogicReasonCodeName(LogicReasonCode code) {
    const std::uint32_t index = static_cast<std::uint32_t>(code);
    if (index >= kLogicReasonCodeCount) {
        return "unknown";
    }
    // Rows are in numeric order, asserted by the index check below, so the
    // lookup is a direct index rather than a scan.
    const Row& row = kRows[index];
    return row.code == code ? row.name : "unknown";
}

} // namespace harken
