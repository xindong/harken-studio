#ifndef HARKEN_OBJECT_ID_H
#define HARKEN_OBJECT_ID_H

#include <stdint.h>

typedef struct VnObjectId {
    uint64_t high;
    uint64_t low;
#ifdef __cplusplus
    constexpr VnObjectId() noexcept = default;
    constexpr VnObjectId(uint64_t highValue, uint64_t lowValue) noexcept
        : high(highValue), low(lowValue) {}
#endif
} VnObjectId;

#ifdef __cplusplus
#define VN_OBJECT_ID(high_, low_) (VnObjectId{high_, low_})
#else
#define VN_OBJECT_ID(high_, low_) ((VnObjectId){high_, low_})
#endif

#ifdef __cplusplus

#include <array>
#include <bit>
#include <charconv>
#include <compare>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

inline constexpr bool operator==(VnObjectId left, VnObjectId right) noexcept
{
    return left.high == right.high && left.low == right.low;
}

inline constexpr std::strong_ordering operator<=>(VnObjectId left, VnObjectId right) noexcept
{
    if (const auto order = left.high <=> right.high; order != 0) return order;
    return left.low <=> right.low;
}

namespace harken {

inline constexpr VnObjectId kNullObjectId{};
inline constexpr VnObjectId kMasterBusObjectId{0, 1};

inline constexpr bool IsNull(VnObjectId id) noexcept
{
    return id == kNullObjectId;
}

inline constexpr std::array<uint8_t, 16> ObjectIdBytes(VnObjectId id) noexcept
{
    std::array<uint8_t, 16> bytes{};
    for (unsigned i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>(id.high >> ((7 - i) * 8));
        bytes[i + 8] = static_cast<uint8_t>(id.low >> ((7 - i) * 8));
    }
    return bytes;
}

inline constexpr VnObjectId ObjectIdFromBytes(std::span<const uint8_t, 16> bytes) noexcept
{
    VnObjectId id{};
    for (unsigned i = 0; i < 8; ++i) {
        id.high = (id.high << 8) | bytes[i];
        id.low = (id.low << 8) | bytes[i + 8];
    }
    return id;
}

inline bool ParseObjectId(std::string_view text, VnObjectId* output) noexcept
{
    if (output == nullptr) return false;
    if (text.size() == 38 && text.front() == '{' && text.back() == '}')
        text = text.substr(1, 36);
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' ||
        text[18] != '-' || text[23] != '-') return false;
    std::array<char, 32> digits{};
    unsigned count = 0;
    for (unsigned i = 0; i < text.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        const char value = text[i];
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
              (value >= 'A' && value <= 'F'))) return false;
        digits[count++] = value;
    }
    VnObjectId id{};
    const auto high = std::from_chars(digits.data(), digits.data() + 16, id.high, 16);
    const auto low = std::from_chars(digits.data() + 16, digits.data() + 32, id.low, 16);
    if (high.ec != std::errc{} || low.ec != std::errc{}) return false;
    *output = id;
    return true;
}

inline constexpr std::array<char, 37> FormatObjectId(VnObjectId id) noexcept
{
    constexpr char digits[] = "0123456789abcdef";
    const auto bytes = ObjectIdBytes(id);
    std::array<char, 37> text{};
    unsigned position = 0;
    for (unsigned i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) text[position++] = '-';
        text[position++] = digits[bytes[i] >> 4];
        text[position++] = digits[bytes[i] & 15];
    }
    return text;
}

inline std::string ObjectIdString(VnObjectId id)
{
    const auto text = FormatObjectId(id);
    return {text.data(), 36};
}

inline constexpr uint64_t HashObjectId(VnObjectId id) noexcept
{
    uint64_t value = id.high ^ std::rotl(id.low, 23);
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

}

template<> struct std::hash<VnObjectId> {
    size_t operator()(VnObjectId id) const noexcept
    {
        return static_cast<size_t>(harken::HashObjectId(id));
    }
};

static_assert(sizeof(VnObjectId) == 16);
static_assert(alignof(VnObjectId) == alignof(uint64_t));
static_assert(std::is_standard_layout_v<VnObjectId>);
static_assert(std::is_trivially_copyable_v<VnObjectId>);
static_assert(!std::is_convertible_v<VnObjectId, uint64_t>);
static_assert(!std::is_convertible_v<uint64_t, VnObjectId>);

#endif

#endif
