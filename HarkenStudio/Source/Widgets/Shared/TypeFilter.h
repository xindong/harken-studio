
#pragma once

#include "Core/ProjectTypes.h"

#include <algorithm>
#include <vector>

namespace VNX::TypeFilter {

inline bool matches(ProjectItem::Type type,
                    ProjectItem::Type singleFilter,
                    const std::vector<ProjectItem::Type>& multiFilter)
{
    if (multiFilter.empty())
        return type == singleFilter;
    return std::find(multiFilter.begin(), multiFilter.end(), type) != multiFilter.end();
}

}
