#include "ConversionDefaults.h"

#include "FieldGroupProjectAdapter.h"
#include "ProjectTypes.h"

#include <memory>
#include <utility>

namespace harken::studio::ConversionDefaults {

void createProjectPresets(ProjectData& project)
{
    const auto addPreset = [&project](const QString& name, ConversionConfig config) {
        auto item = std::make_shared<ProjectItem>(name, ProjectItem::Type::Conversion);
        item->attachDefaultComponents();
        item->objectID = project.generateObjectID();
        item->guid = QUuid::createUuid();
        item->comp<VConversion>()->config = std::move(config);

        ProjectFileBinding binding;
        binding.parentFolder = QStringLiteral("Conversions");
        binding.rootItem = item;
        project.conversionItems.push_back(std::move(binding));
        project.setObjectFileLocation(item.get(), nullptr, {});
        return item->guid;
    };

    constexpr std::uint64_t KiB = 1024;
    constexpr std::uint64_t MiB = 1024 * KiB;
    const auto defaultQuality = addPreset(QStringLiteral("Default Quality"), {});
    const auto mediumQuality = addPreset(QStringLiteral("Medium Quality"), {{
        {MiB, 48000, ConversionFormat::Vorbis, 6},
        {std::nullopt, 44100, ConversionFormat::Vorbis, 4}
    }});
    addPreset(QStringLiteral("Multiple Quality"), {{
        {64 * KiB, 44100, ConversionFormat::Vorbis, 10},
        {256 * KiB, 44100, ConversionFormat::Vorbis, 8},
        {MiB, 44100, ConversionFormat::Vorbis, 6},
        {4 * MiB, 44100, ConversionFormat::Vorbis, 4},
        {std::nullopt, 44100, ConversionFormat::Vorbis, 2}
    }});

    for (const auto& platform : project.platforms) {
        if (platform.basePlatform == BasePlatform::Windows || platform.basePlatform == BasePlatform::macOS)
            project.defaultConversionRefs.insert(platform.id, defaultQuality);
        else if (platform.basePlatform == BasePlatform::Android || platform.basePlatform == BasePlatform::iOS)
            project.defaultConversionRefs.insert(platform.id, mediumQuality);
    }
}

ConversionConfig newPreset()
{
    return {{{std::nullopt, 0, ConversionFormat::Vorbis, kDefaultVorbisQuality}}};
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
