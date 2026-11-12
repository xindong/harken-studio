#ifndef HARKEN_CORE_PLAYBACK_PATH_H
#define HARKEN_CORE_PLAYBACK_PATH_H

#include <harken/ObjectId.h>
#include "harken/core/ProgramHandle.h"
#include <cstdint>

namespace harken {

inline constexpr std::uint32_t kPlaybackPathRowCapacity = 64;

enum class ResolvedPlayablePathRowKindV1 : std::uint8_t {
    OrganizationLayer = 0,
    Definition = 1,
    Occurrence = 2,
    Unspecified = 3,
};

struct PlaybackPathRow {
    VnObjectId stableId{};
    std::uint32_t occurrenceOrdinal = UINT32_MAX;
    // Formal provenance is captured with the selected path, never reconstructed
    // from a newer world or the logic-only playback observation table. Legacy
    // ID-only paths deliberately carry no definition membership proof.
    ResolvedPlayablePathRowKindV1 rowKind = ResolvedPlayablePathRowKindV1::Unspecified;
    playableproperties::ContentPropertyKindV1 contentPropertyKind =
        playableproperties::ContentPropertyKindV1::Sound;
    std::uint64_t propertyFingerprint = 0;
    ProgramSourceIdentityV1 source{};

};

struct PlaybackPath {
    std::uint32_t rowCount = 0;
    PlaybackPathRow rows[kPlaybackPathRowCapacity]{};
};

}

#endif
