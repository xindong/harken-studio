#include "harken_project_schema/FixedSlotLowerer.h"

#include <cstddef>
#include <utility>

namespace harkenprojectschema {

LowerEffectChainResult LowerEffectChainToFixedSlots(
    const CompiledEffectChain& chain, const AuthoringGuid& owner) {
    LowerEffectChainResult result;

    if (chain.entries.size() > kFixedVoiceFxSlotCount) {
        Diagnostic d;
        d.code = DiagnosticCode::EffectChainExceedsSlotLimit;
        d.primary = owner;
        d.detail = chain.entries.size();  // the length that did not fit
        result.diagnostics.push_back(d);
        return result;
    }

    LoweredEffectChain lowered;
    lowered.slots.reserve(chain.entries.size());
    for (std::size_t i = 0; i < chain.entries.size(); ++i) {
        lowered.slots.push_back(LoweredEffectSlot{
            static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i)});
    }
    result.chain = std::move(lowered);
    return result;
}

}  // namespace harkenprojectschema
