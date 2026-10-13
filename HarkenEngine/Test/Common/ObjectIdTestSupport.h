#pragma once

#include <harken/ObjectId.h>
#include <ostream>

inline std::ostream& operator<<(std::ostream& stream, VnObjectId id)
{
    return stream << harken::ObjectIdString(id);
}

namespace harkentest {

constexpr VnObjectId ObjectId(std::uint64_t value) noexcept
{
    return value == 0 ? VnObjectId{} : VnObjectId{0x3da91a64b9474560ull, value};
}

constexpr VnObjectId ObjectId(VnObjectId value) noexcept
{
    return value;
}

constexpr VnObjectId OffsetObjectId(VnObjectId base, std::uint64_t offset) noexcept
{
    return {base.high, base.low + offset};
}

inline std::string JsonObjectId(VnObjectId id)
{
    return "\"" + harken::ObjectIdString(id) + "\"";
}

}
