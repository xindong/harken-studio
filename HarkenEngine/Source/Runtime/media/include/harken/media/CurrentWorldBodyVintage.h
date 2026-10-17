// Internal layout id of the CurrentWorld playback-program body. Shared by
// CanonicalDefinitionPartition and the DirectWorld preview envelope
// (VNCWORLD). Not a product schema; do not register in catalog.json.
// Change kCurrentWorldShippingBodyVintage when minting a new write table.
#ifndef HARKEN_MEDIA_CURRENT_WORLD_BODY_VINTAGE_H
#define HARKEN_MEDIA_CURRENT_WORLD_BODY_VINTAGE_H

#include <cstdint>

namespace harken {

inline constexpr std::uint32_t kCurrentWorldBodyVintageV5 = 5;
inline constexpr std::uint32_t kCurrentWorldBodyVintageV6 = 6;
inline constexpr std::uint32_t kCurrentWorldBodyVintageV7 = 7;
inline constexpr std::uint32_t kCurrentWorldBodyVintageV8 = 8;
inline constexpr std::uint32_t kCurrentWorldBodyVintageV9 = 9;
inline constexpr std::uint32_t kCurrentWorldBodyVintageV10 = 10;
inline constexpr std::uint32_t kCurrentWorldBodyVintageV11 = 11;
inline constexpr std::uint32_t kCurrentWorldBodyVintageV12 = 12;
inline constexpr std::uint32_t kCurrentWorldBodyVintageV13 = 13;
inline constexpr std::uint32_t kCurrentWorldBodyVintageV14 = 14;
inline constexpr std::uint32_t kCurrentWorldBodyVintageV15 = 15;
inline constexpr std::uint32_t kCurrentWorldBodyVintageV16 = 16;
inline constexpr std::uint32_t kCurrentWorldBodyVintageV17 = 17;
inline constexpr std::uint32_t kCurrentWorldBodyVintageV18 = 18;
inline constexpr std::uint32_t kCurrentWorldBodyVintageV19 = 19;
inline constexpr std::uint32_t kCurrentWorldShippingBodyVintage =
    kCurrentWorldBodyVintageV18;

inline constexpr bool IsReadableCurrentWorldBodyVintage(
    std::uint32_t vintage) noexcept {
    return vintage == kCurrentWorldBodyVintageV18 ||
           vintage == kCurrentWorldBodyVintageV19;
}

} // namespace harken

#endif // HARKEN_MEDIA_CURRENT_WORLD_BODY_VINTAGE_H
