#include "CompileTargetProfileCatalog.h"
#include "ProjectTypes.h"

#include <harken_studio/AudioValues.h>
#include <harken_project_schema/CompileTargetProfile.h>

namespace VNX {

namespace {

harkenstudio::ChannelLayout runtimeLayout(AudioChannelConfig config) noexcept
{
    switch (config) {
    case AudioChannelConfig::Mono: return harkenstudio::kChannelLayoutMono;
    case AudioChannelConfig::Stereo_2_0: return harkenstudio::kChannelLayoutStereo;
    case AudioChannelConfig::Surround_5_1: return harkenstudio::kChannelLayout5_1;
    case AudioChannelConfig::Surround_7_1: return harkenstudio::kChannelLayout7_1;
    case AudioChannelConfig::Atmos_7_1_4: return harkenstudio::kChannelLayoutDefault;
    }
    return harkenstudio::kChannelLayoutDefault;
}

}

std::shared_ptr<const harkenprojectschema::CompileTargetProfileV1>
CompileTargetProfileCatalog::resolve(const ProjectData& project) noexcept
{
    if (project.sampleRate <= 0) return {};
    const harkenstudio::ChannelLayout layout = runtimeLayout(project.channelConfig);
    if (layout == harkenstudio::kChannelLayoutDefault) return {};
    return harkenprojectschema::MakeCompileTargetProfileV1(
        1, static_cast<std::uint32_t>(project.sampleRate), layout, layout,
        harkenprojectschema::kKnownCapabilityMaskV1);
}

}
