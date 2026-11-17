#include "ConversionDefaults.h"

namespace VNX {

ConversionPlatformConfig defaultConfigFor(BasePlatform platform)
{
    ConversionPlatformConfig config;
    if (platform == BasePlatform::Android || platform == BasePlatform::iOS)
        config.format = ConversionFormat::Vorbis;
    return config;
}

}
