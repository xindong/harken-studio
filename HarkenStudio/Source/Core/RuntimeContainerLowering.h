#pragma once

#include "Components/VSelection.h"
#include "ProjectTypes.h"

#include <QString>

#include <cstdint>
#include <vector>

namespace VNX {

struct RuntimeContainerChild
{
    harkenstudio::ObjectId playableId = {};
    uint32_t weight = 1;
    uint32_t matchValue = 0;
    uint32_t stableOccurrenceOrdinal = 0;
};

struct RuntimeContainerPlan
{
    harkenstudio::ObjectId containerId = {};
    harkenstudio::ObjectId variableId = {};
    bool weightedRandom = false;
    uint32_t avoidRepeat = 0;
    std::vector<RuntimeContainerChild> children;
};

struct RuntimeContainerLoweringResult
{
    RuntimeContainerPlan plan;
    QString diagnostic;

    bool ok() const { return diagnostic.isEmpty(); }
};

namespace RuntimeContainerLowering {

RuntimeContainerLoweringResult lower(
    const ProjectItem& container,
    const ProjectData& project, PlatformScope scope = std::nullopt);

}
}
