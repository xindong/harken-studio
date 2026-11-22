#pragma once

#include <harken/ObjectId.h>
#include <harken_studio/ObjectId.h>

namespace harken::studio::bridge {

inline constexpr VnObjectId toNative(harken::studio::types::ObjectId id) noexcept
{
    return {id.high, id.low};
}

inline constexpr harken::studio::types::ObjectId fromNative(VnObjectId id) noexcept
{
    return {id.high, id.low};
}

}
