#include "UnitTest/StudioObjectIdTestSupport.h"
#pragma once

#include "Core/ProjectTypes.h"

#include <cstdint>
#include <limits>

namespace HarkenTest {

inline harkenstudio::ObjectId fixtureRuntimeStableId(VNX::ProjectData&)
{
    return VNX::createObjectId();
}

}
