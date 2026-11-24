#pragma once

#include "Components/VConversion.h"
#include "FieldGroups.h"

namespace harken::studio { struct ProjectData; }

namespace harken::studio::ConversionDefaults {

void createProjectPresets(ProjectData& project);
ConversionConfig newPreset();
void applyRootDefaults(const ProjectData& project, AuthoredGroup<ConversionFieldGroup>& conversion);

}
