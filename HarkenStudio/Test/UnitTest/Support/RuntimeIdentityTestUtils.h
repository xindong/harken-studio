#include "UnitTest/StudioObjectIdTestSupport.h"
#pragma once

#include "Core/ProjectTypes.h"

#include <cstdint>
#include <limits>

namespace HarkenTest {

inline harken::studio::types::ObjectId fixtureRuntimeStableId(harken::studio::ProjectData&)
{
    return harken::studio::createObjectId();
}

}
