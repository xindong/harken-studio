#ifndef HARKEN_CORE_OCCURRENCE_TOKEN_H
#define HARKEN_CORE_OCCURRENCE_TOKEN_H

#include <cstdint>

namespace harken {

struct OccurrenceToken {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;
};

} // namespace harken

#endif // HARKEN_CORE_OCCURRENCE_TOKEN_H
