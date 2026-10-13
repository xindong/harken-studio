#ifndef HARKEN_PROJECT_SCHEMA_DETAIL_FNV1A_H
#define HARKEN_PROJECT_SCHEMA_DETAIL_FNV1A_H

#include <cstdint>

namespace harkenprojectschema::detail {

inline constexpr std::uint64_t kFnv1a64OffsetBasis =
    14695981039346656037ull;
inline constexpr std::uint64_t kFnv1a64Prime = 1099511628211ull;

inline void Fnv1a64HashByte(std::uint64_t* state, std::uint8_t byte) {
    *state ^= byte;
    *state *= kFnv1a64Prime;
}

}  // namespace harkenprojectschema::detail

#endif  // HARKEN_PROJECT_SCHEMA_DETAIL_FNV1A_H
