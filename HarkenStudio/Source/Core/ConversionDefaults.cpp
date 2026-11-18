#include "ConversionDefaults.h"

#include "FieldGroupProjectAdapter.h"
#include "ProjectTypes.h"

namespace VNX::ConversionDefaults {

QString lowQualityName()
{
    return QStringLiteral("Low Quality");
}

ConversionConfig lowQuality()
{
    ConversionConfig config;
    config.format = ConversionFormat::Vorbis;
    config.tiers = {{std::uint64_t(1) << 20, 0}, {std::nullopt, 32000}};
    return config;
}

ConversionConfig newPreset()
{
    ConversionConfig config;
    config.format = ConversionFormat::Vorbis;
    return config;
}

bool usesLowQuality(BasePlatform platform)
{
    return platform == BasePlatform::Android || platform == BasePlatform::iOS;
}

void applyRootDefaults(const ProjectData& project, AuthoredGroup<ConversionFieldGroup>& conversion)
{
    conversion.shared = {FieldGroupMode::Local, ConversionFieldGroup{}};
    conversion.platforms.clear();
    for (auto it = project.defaultConversionRefs.cbegin(); it != project.defaultConversionRefs.cend(); ++it)
        if (ProjectPlatformSemantics::find(project.platforms, it.key()))
            conversion.platforms[toFieldGroupGuid(it.key())] = {FieldGroupMode::Local,
                ConversionFieldGroup{toFieldGroupGuid(it.value())}, FieldGroupPropertyMask(FieldGroupId::Conversion)};
}

}
