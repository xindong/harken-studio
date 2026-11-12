#ifndef HARKEN_CORE_STREAM_PARK_TOKEN_H
#define HARKEN_CORE_STREAM_PARK_TOKEN_H

#include <cstdint>

namespace harken {

inline constexpr std::uint32_t kStreamParkTokenBytes = 32u * 1024u;

struct StreamParkTokenRef {
    std::uint32_t index = UINT32_MAX;
    std::uint32_t generation = 0;

    bool IsValid() const {
        return index != UINT32_MAX && generation != 0;
    }
};

} // namespace harken

#endif // HARKEN_CORE_STREAM_PARK_TOKEN_H
