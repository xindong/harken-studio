#pragma once

#include <harken/Harken.h>
#include "harken/core/PackComposition.h"

namespace harken::detail {

struct ActionAdmissionView {
    const ActionProgramViewV1* action = nullptr;
    const PackComposition::PublishedView* publication = nullptr;
    PackPartitionLease lease{};
    bool packBacked = false;
};

// Synchronous, logic-thread-only validation of borrowed immutable data. The
// validator must not reenter APIs, mutate resources, retain pointers/tokens,
// or transfer the lease. External API serialization remains the caller's duty.
using ActionAdmissionValidator = VnResult (*)(
    const ActionAdmissionView&, const void* context) noexcept;

VnResult PostActionWithValidation(
    const VnPostActionDesc* desc, VnActionFiringHandle* outFiring,
    ActionAdmissionValidator validator, const void* context,
    bool journalCall = true);

} // namespace harken::detail
