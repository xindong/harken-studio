#pragma once

#include <harken/BusDefinitionFingerprintValue.h>
#include <harken/param/BindingTable.h>
#include <harken/param/ParamSystem.h>

#include <cstdint>

namespace harken {

inline std::uint64_t ComputeBusBindingSourceStamp(
    const BindingTable& bindings, const ParamSystem& params) noexcept {
    std::uint64_t hash = busfingerprint::kOffsetBasis;
    const auto u64 = [&](std::uint64_t value) {
        busfingerprint::AppendU64([&](std::uint8_t byte) {
            hash = busfingerprint::Step(hash, byte);
        }, value);
    };
    bindings.ForEachBusAttachment([&](const auto& attachment, const auto& definition) {
        float input = 0.0f;
        const bool hasValue = !attachment.meterFrozen &&
            params.ScopedVariableValue(nullptr, 0, definition.variableId, &input);
        u64(attachment.bindingId.high);
        u64(attachment.bindingId.low);
        u64(attachment.objectId.high);
        u64(attachment.objectId.low);
        u64(attachment.busIndex);
        u64(attachment.ownerCookie);
        u64(attachment.lastHadValue);
        u64(busfingerprint::CanonicalFloatBits(attachment.lastContribution));
        u64(definition.variableId.high);
        u64(definition.variableId.low);
        u64(definition.targetId);
        u64(definition.revision);
        u64(definition.pendingInstall);
        u64(definition.transitionMs);
        u64(hasValue);
        if (hasValue) {
            u64(busfingerprint::CanonicalFloatBits(input));
            u64(busfingerprint::CanonicalFloatBits(definition.Evaluate(input)));
        }
    });
    return busfingerprint::Finalize(hash);
}

} // namespace harken
