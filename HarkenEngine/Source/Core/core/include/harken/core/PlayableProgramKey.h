#ifndef HARKEN_CORE_PLAYABLE_PROGRAM_KEY_H
#define HARKEN_CORE_PLAYABLE_PROGRAM_KEY_H

#include <harken/ObjectId.h>
#include "harken/core/CurrentWorld.h"

#include <cstdint>

namespace harken {

struct PlayableProgramKey {
    PlayableKindV1 kind = PlayableKindV1::Sound;
    VnObjectId stableId{};
    WorldGeneration world = 0;
    std::uint64_t runtimeSemanticFingerprint = 0;
    std::uint32_t selectionAlgorithm = 0;
    std::uint32_t gridAlgorithm = 0;
    std::uint32_t transitionAlgorithm = 0;
};

bool ValidPlayableProgramKey(const PlayableProgramKey& key) noexcept;
bool SamePlayableProgramKey(const PlayableProgramKey& a,
                            const PlayableProgramKey& b) noexcept;

} // namespace harken

#endif // HARKEN_CORE_PLAYABLE_PROGRAM_KEY_H
