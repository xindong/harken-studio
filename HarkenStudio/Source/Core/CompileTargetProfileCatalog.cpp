#include "CompileTargetProfileCatalog.h"
#include "ProjectTypes.h"

#include <harken_studio/AudioValues.h>
#include <harken_project_schema/CompileTargetProfile.h>

namespace harken::studio {

namespace {

harken::studio::types::ChannelLayout runtimeLayout(AudioChannelConfig config) noexcept
{
    switch (config) {
    case AudioChannelConfig::Mono: return harken::studio::types::kChannelLayoutMono;
    case AudioChannelConfig::Stereo_2_0: return harken::studio::types::kChannelLayoutStereo;
    case AudioChannelConfig::Surround_5_1: return harken::studio::types::kChannelLayout5_1;
    case AudioChannelConfig::Surround_7_1: return harken::studio::types::kChannelLayout7_1;
    case AudioChannelConfig::Atmos_7_1_4: return harken::studio::types::kChannelLayoutDefault;
    }
    return harken::studio::types::kChannelLayoutDefault;
}

}

std::shared_ptr<const harken::studio::project_schema::CompileTargetProfileV1>
CompileTargetProfileCatalog::resolve(const ProjectData& project) noexcept
{
    if (project.sampleRate <= 0) return {};
    const harken::studio::types::ChannelLayout layout = runtimeLayout(project.channelConfig);
    if (layout == harken::studio::types::kChannelLayoutDefault) return {};
    return harken::studio::project_schema::MakeCompileTargetProfileV1(
        1, static_cast<std::uint32_t>(project.sampleRate), layout, layout,
        harken::studio::project_schema::kKnownCapabilityMaskV1);
}

}
