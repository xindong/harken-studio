#ifndef HARKEN_CORE_PLAYBACK_PATH_H
#define HARKEN_CORE_PLAYBACK_PATH_H

#include <harken/ObjectId.h>
#include <cstdint>

namespace harken {

inline constexpr std::uint32_t kPlaybackPathRowCapacity = 64;

struct PlaybackPathRow {
    VnObjectId stableId{};
    std::uint32_t occurrenceOrdinal = UINT32_MAX;
};

struct PlaybackPath {
    std::uint32_t rowCount = 0;
    PlaybackPathRow rows[kPlaybackPathRowCapacity]{};
};

}

#endif
