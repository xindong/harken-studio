#include "HarkenSettings.h"

UHarkenSettings::UHarkenSettings()
{
#if PLATFORM_LINUX
    DeviceMode = EHarkenDeviceMode::Null;
#else
    DeviceMode = EHarkenDeviceMode::Real;
#endif
}

FName UHarkenSettings::GetCategoryName() const
{
    return TEXT("Plugins");
}

bool UHarkenSettings::IsCommunicationEnabled() const
{
#if WITH_EDITOR
    return true;
#else
    return bEnableCommunicationInBuilds;
#endif
}
