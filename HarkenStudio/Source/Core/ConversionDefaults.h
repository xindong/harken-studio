#pragma once

#include "Components/VConversion.h"
#include "FieldGroups.h"
#include "ProjectPlatform.h"

namespace harken::studio { struct ProjectData; }

namespace harken::studio::ConversionDefaults {

QString lowQualityName();
ConversionConfig lowQuality();
ConversionConfig newPreset();
bool usesLowQuality(BasePlatform platform);
void applyRootDefaults(const ProjectData& project, AuthoredGroup<ConversionFieldGroup>& conversion);

}
