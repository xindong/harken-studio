#pragma once

#include "UnitTest/ObjectIdTestSupport.h"
#include <harken/Harken.h>
#include <harken_studio/ObjectId.h>

namespace harken::studio::types {

inline std::ostream& operator<<(std::ostream& stream, ObjectId id)
{
    return stream << ObjectIdString(id);
}

inline constexpr bool operator==(ObjectId left, VnObjectId right) noexcept
{
    return left.high == right.high && left.low == right.low;
}

}

namespace harkentest {

inline constexpr harken::studio::types::ObjectId ToStudio(VnObjectId id) noexcept
{
    return {id.high, id.low};
}

inline constexpr VnObjectId ToRuntime(harken::studio::types::ObjectId id) noexcept
{
    return {id.high, id.low};
}

inline constexpr VnObjectId ToRuntime(VnObjectId id) noexcept { return id; }

inline constexpr harken::studio::types::ObjectId StudioObjectId(std::uint64_t value) noexcept
{
    return ToStudio(ObjectId(value));
}

inline constexpr harken::studio::types::ObjectId StudioObjectId(harken::studio::types::ObjectId value) noexcept
{
    return value;
}

inline constexpr harken::studio::types::ObjectId StudioObjectId(VnObjectId value) noexcept
{
    return ToStudio(value);
}

inline constexpr harken::studio::types::ObjectId OffsetObjectId(harken::studio::types::ObjectId base, std::uint64_t offset) noexcept
{
    return {base.high, base.low + offset};
}

inline std::string JsonObjectId(harken::studio::types::ObjectId id)
{
    return "\"" + harken::studio::types::ObjectIdString(id) + "\"";
}

}
