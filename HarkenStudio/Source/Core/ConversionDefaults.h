#pragma once

#include "Components/VConversion.h"
#include "FieldGroups.h"
#include "ProjectPlatform.h"

namespace VNX { struct ProjectData; }

namespace VNX::ConversionDefaults {

QString lowQualityName();
ConversionConfig lowQuality();
ConversionConfig newPreset();
bool usesLowQuality(BasePlatform platform);
void applyRootDefaults(const ProjectData& project, AuthoredGroup<ConversionFieldGroup>& conversion);

}
