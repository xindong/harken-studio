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
    return {{{std::uint64_t(1) << 20, 0, ConversionFormat::Vorbis, kDefaultVorbisQuality},
             {std::nullopt, 32000, ConversionFormat::Vorbis, kDefaultVorbisQuality}}};
}

ConversionConfig newPreset()
{
    return {{{std::nullopt, 0, ConversionFormat::Vorbis, kDefaultVorbisQuality}}};
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
