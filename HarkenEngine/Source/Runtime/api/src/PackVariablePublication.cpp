#include "PackVariablePublication.h"
#include "Engine.h"

#include <algorithm>
#include <vector>

namespace harken {

VnResult PreparePackVariables(
    Engine& engine, const PackComposition::PublishedView& next,
    VariableServiceV1::PreparedPackDefinitions* out) {
    return PreparePackVariables(engine, next, PackVariablePublicationReason::ContentMutation, out);
}

VnResult PreparePackVariables(
    Engine& engine, const PackComposition::PublishedView& next,
    PackVariablePublicationReason reason,
    VariableServiceV1::PreparedPackDefinitions* out) {
    if (out == nullptr) {
        return VN_INVALID_ARG;
    }
    *out = VariableServiceV1::PreparedPackDefinitions{};
    std::vector<VariableDefinitionV1> merged;
    for (const auto& owner : next.owners) {
        if (owner.readiness != PackOwnerReadiness::Active ||
            owner.view.variableCount == 0 || owner.view.variables == nullptr) {
            continue;
        }
        for (std::uint32_t i = 0; i < owner.view.variableCount; ++i) {
            merged.push_back(owner.view.variables[i]);
        }
    }
    std::sort(merged.begin(), merged.end(),
              [](const VariableDefinitionV1& a, const VariableDefinitionV1& b) {
                  return a.variableId < b.variableId;
              });
    merged.erase(std::unique(merged.begin(), merged.end(),
        [](const auto& a, const auto& b) { return a.variableId == b.variableId; }),
        merged.end());
    std::vector<VariablePolicySidecarV1> overlays(merged.size());
    const auto findOverlay =
        [&next](VnObjectId variableId)
            -> const PackCanonicalImage::VariablePolicyOverlay* {
            for (const auto& overlay : next.catalog.variablePolicyOverlays) {
                if (overlay.variableId == variableId) {
                    return &overlay;
                }
            }
            for (const auto& owner : next.owners) {
                if (!owner.hasVariablePolicyOverlay) {
                    continue;
                }
                for (const auto& overlay : owner.variablePolicyOverlays) {
                    if (overlay.variableId == variableId) {
                        return &overlay;
                    }
                }
            }
            return nullptr;
        };
    bool foundAnyOverlay = false;
    for (std::size_t i = 0; i < merged.size(); ++i) {
        const auto* overlay = findOverlay(merged[i].variableId);
        if (overlay == nullptr) {
            continue;
        }
        foundAnyOverlay = true;
        overlays[i].writers = overlay->writers;
        overlays[i].executionFingerprint = overlay->executionFingerprint;
        overlays[i].catalogFingerprint = overlay->catalogFingerprint;
        overlays[i].labels = overlay->labels.empty() ? nullptr : overlay->labels.data();
        overlays[i].labelCount = static_cast<std::uint32_t>(overlay->labels.size());
    }
    bool initDeclaredWithoutOverlay = false;
    if (next.initPackStableId != 0) {
        for (const auto& owner : next.owners) {
            if (owner.packStableId != next.initPackStableId) {
                continue;
            }
            if (owner.view.variableCount > 0 &&
                !next.catalog.hasVariablePolicyOverlay &&
                !owner.hasVariablePolicyOverlay) {
                initDeclaredWithoutOverlay = true;
            }
            break;
        }
    }
    const VariablePolicySidecarV1* overlayPtr =
        !overlays.empty() && (foundAnyOverlay || initDeclaredWithoutOverlay)
            ? overlays.data()
            : nullptr;
    if (!engine.params.PrepareVariableStorage(static_cast<std::uint32_t>(merged.size())))
        return VN_PACK_RESIDENT_CAPACITY;
    return engine.variables.PreparePackDefinitions(
        merged.empty() ? nullptr : merged.data(),
        static_cast<std::uint32_t>(merged.size()), overlayPtr, reason,
        engine.serviceFrameClock, out);
}

void CommitPackVariables(
    Engine& engine,
    VariableServiceV1::PreparedPackDefinitions* prepared) noexcept {
    engine.variables.CommitPreparedPackDefinitions(
        prepared, engine.params, engine.emitters, engine.bindings,
        engine.busTargets);
}

} // namespace harken
