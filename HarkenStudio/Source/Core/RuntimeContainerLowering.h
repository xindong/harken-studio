#pragma once

#include "Components/VSelection.h"
#include "ProjectTypes.h"

#include <QString>

#include <cstdint>
#include <vector>

namespace harken::studio {

struct RuntimeContainerChild
{
    harken::studio::types::ObjectId playableId = {};
    uint32_t weight = 1;
    uint32_t matchValue = 0;
    uint32_t stableOccurrenceOrdinal = 0;
};

struct RuntimeContainerPlan
{
    harken::studio::types::ObjectId containerId = {};
    harken::studio::types::ObjectId variableId = {};
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
