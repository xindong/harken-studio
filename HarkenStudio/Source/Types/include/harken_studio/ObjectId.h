#pragma once

#include <array>
#include <bit>
#include <charconv>
#include <compare>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace harkenstudio {

struct ObjectId {
    std::uint64_t high = 0;
    std::uint64_t low = 0;
    constexpr auto operator<=>(const ObjectId&) const noexcept = default;
};

inline constexpr ObjectId kNullObjectId{};
inline constexpr ObjectId kMasterBusObjectId{0, 1};

inline constexpr bool IsNull(ObjectId id) noexcept { return id == kNullObjectId; }

inline constexpr std::array<std::uint8_t, 16> ObjectIdBytes(ObjectId id) noexcept
{
    std::array<std::uint8_t, 16> bytes{};
    for (unsigned i = 0; i < 8; ++i) {
        bytes[i] = static_cast<std::uint8_t>(id.high >> ((7 - i) * 8));
        bytes[i + 8] = static_cast<std::uint8_t>(id.low >> ((7 - i) * 8));
    }
    return bytes;
}

inline constexpr ObjectId ObjectIdFromBytes(std::span<const std::uint8_t, 16> bytes) noexcept
{
    ObjectId id{};
    for (unsigned i = 0; i < 8; ++i) {
        id.high = (id.high << 8) | bytes[i];
        id.low = (id.low << 8) | bytes[i + 8];
    }
    return id;
}

inline bool ParseObjectId(std::string_view text, ObjectId* output) noexcept
{
    if (!output) return false;
    if (text.size() == 38 && text.front() == '{' && text.back() == '}')
        text = text.substr(1, 36);
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' ||
        text[18] != '-' || text[23] != '-') return false;
    std::array<char, 32> digits{};
    unsigned count = 0;
    for (unsigned i = 0; i < text.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        const char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
        digits[count++] = c;
    }
    ObjectId id{};
    if (std::from_chars(digits.data(), digits.data() + 16, id.high, 16).ec != std::errc{} ||
        std::from_chars(digits.data() + 16, digits.data() + 32, id.low, 16).ec != std::errc{})
        return false;
    *output = id;
    return true;
}

inline constexpr std::array<char, 37> FormatObjectId(ObjectId id) noexcept
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

inline std::string ObjectIdString(ObjectId id)
{
    const auto text = FormatObjectId(id);
    return {text.data(), 36};
}

inline constexpr std::uint64_t HashObjectId(ObjectId id) noexcept
{
    auto value = id.high ^ std::rotl(id.low, 23);
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

}

template<> struct std::hash<harkenstudio::ObjectId> {
    std::size_t operator()(harkenstudio::ObjectId id) const noexcept
    {
        return static_cast<std::size_t>(harkenstudio::HashObjectId(id));
    }
};
