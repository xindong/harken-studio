#pragma once

#include "UnitTest/ObjectIdTestSupport.h"
#include <harken/Harken.h>
#include <harken_studio/ObjectId.h>

namespace harkenstudio {

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

inline constexpr harkenstudio::ObjectId ToStudio(VnObjectId id) noexcept
{
    return {id.high, id.low};
}

inline constexpr VnObjectId ToRuntime(harkenstudio::ObjectId id) noexcept
{
    return {id.high, id.low};
}

inline constexpr VnObjectId ToRuntime(VnObjectId id) noexcept { return id; }

inline constexpr harkenstudio::ObjectId StudioObjectId(std::uint64_t value) noexcept
{
    return ToStudio(ObjectId(value));
}

inline constexpr harkenstudio::ObjectId StudioObjectId(harkenstudio::ObjectId value) noexcept
{
    return value;
}

inline constexpr harkenstudio::ObjectId StudioObjectId(VnObjectId value) noexcept
{
    return ToStudio(value);
}

inline constexpr harkenstudio::ObjectId OffsetObjectId(harkenstudio::ObjectId base, std::uint64_t offset) noexcept
{
    return {base.high, base.low + offset};
}

inline std::string JsonObjectId(harkenstudio::ObjectId id)
{
    return "\"" + harkenstudio::ObjectIdString(id) + "\"";
}

}
