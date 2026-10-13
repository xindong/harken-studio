#pragma once

#include <harken/ObjectId.h>
#include <harken_studio/ObjectId.h>

namespace HarkenEditor::Bridge {

inline constexpr VnObjectId toNative(harkenstudio::ObjectId id) noexcept
{
    return {id.high, id.low};
}

inline constexpr harkenstudio::ObjectId fromNative(VnObjectId id) noexcept
{
    return {id.high, id.low};
}

}
