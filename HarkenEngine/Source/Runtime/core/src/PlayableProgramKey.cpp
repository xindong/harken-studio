#include "harken/core/PlayableProgramKey.h"

namespace harken {

bool ValidPlayableProgramKey(const PlayableProgramKey& key) noexcept {
    const std::uint32_t kind = static_cast<std::uint32_t>(key.kind);
    if (kind > static_cast<std::uint32_t>(PlayableKindV1::Piece) ||
        key.stableId == VnObjectId{} || key.world == kNoWorldGeneration ||
        key.runtimeSemanticFingerprint == 0) {
        return false;
    }
    if (key.kind != PlayableKindV1::Container) {
        return key.selectionAlgorithm == 0 && key.gridAlgorithm == 0 &&
               key.transitionAlgorithm == 0;
    }
    return key.selectionAlgorithm != 0;
}

bool SamePlayableProgramKey(const PlayableProgramKey& a,
                            const PlayableProgramKey& b) noexcept {
    return a.kind == b.kind && a.stableId == b.stableId &&
           a.world == b.world &&
           a.runtimeSemanticFingerprint == b.runtimeSemanticFingerprint &&
           a.selectionAlgorithm == b.selectionAlgorithm &&
           a.gridAlgorithm == b.gridAlgorithm &&
           a.transitionAlgorithm == b.transitionAlgorithm;
}

} // namespace harken
