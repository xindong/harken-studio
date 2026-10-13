// Lowering a compiled effect chain onto a fixed per-voice slot array.
//
// A compiled chain is host-independent: it says what runs in what order and
// names no slot. A backend that executes chains has to place each entry
// somewhere, and this is the backend that places them in the runtime's fixed
// per-voice insert-effect slots. The mapping is the only one a fixed array
// admits: entry i executes in slot i, because the slots are processed in
// ascending order and the chain is a series.
//
// The backend's own ceiling is how many slots that array has (kMaxVoiceFxSlots,
// the published layout bound). A chain longer than the ceiling cannot be
// represented at all, so lowering refuses it deterministically instead of
// truncating — dropping the tail of a chain would silently change what a sound is.
// This is a distinct gate from the load-time one: this refusal is about what the
// backend can express, while a loader separately gates the same chain against the
// budget and pools a particular session resolved. Passing here therefore does not
// promise a load will succeed.
//
// Slot numbers are produced here and nowhere else, and they are deliberately not
// part of the chain artifact or of any semantic fingerprint: the same chain
// lowered by a different backend must stay the same chain.
#ifndef HARKEN_PROJECT_SCHEMA_FIXED_SLOT_LOWERER_H
#define HARKEN_PROJECT_SCHEMA_FIXED_SLOT_LOWERER_H

#include "harken_project_schema/Diagnostics.h"
#include "harken_project_schema/EffectChain.h"

#include <harken_studio_build/BuildLimits.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace harkenprojectschema {

// The fixed per-voice slot count this backend lowers onto — the published layout
// bound, resolved from the shared ABI contract rather than restated here.
inline constexpr std::uint32_t kFixedVoiceFxSlotCount = harkenstudiobuild::kMaxVoiceFxSlots;

// One lowered chain step: which chain entry runs in which physical slot. slot is
// the index into the voice's insert-effect array; entryIndex indexes the chain's
// entries. They are equal by this backend's mapping, and both are named anyway so
// a consumer never has to rely on that coincidence.
struct LoweredEffectSlot {
    std::uint32_t slot = 0;
    std::uint32_t entryIndex = 0;
};

// A chain lowered onto fixed slots: the steps in ascending slot order. An empty
// chain lowers to no steps, which is a successful lowering of a real chain (the
// declared-empty case), not a failure.
struct LoweredEffectChain {
    std::vector<LoweredEffectSlot> slots;
};

// The result of lowering. On success chain holds the slot sequence and
// diagnostics is empty; a chain past the backend's ceiling yields a single
// EffectChainExceedsSlotLimit and no chain, all-or-nothing like the compile
// stages before it.
struct LowerEffectChainResult {
    std::vector<Diagnostic> diagnostics;
    std::optional<LoweredEffectChain> chain;

    bool ok() const { return diagnostics.empty(); }
};

// Lowers one compiled chain onto the fixed slot array. owner is the authoring
// identity a refusal is attributed to.
LowerEffectChainResult LowerEffectChainToFixedSlots(
    const CompiledEffectChain& chain, const AuthoringGuid& owner);

}  // namespace harkenprojectschema

#endif  // HARKEN_PROJECT_SCHEMA_FIXED_SLOT_LOWERER_H
