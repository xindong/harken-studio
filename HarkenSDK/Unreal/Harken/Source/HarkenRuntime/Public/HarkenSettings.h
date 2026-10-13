#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "HarkenTypes.h"
#include "HarkenSettings.generated.h"

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Harken"))
class HARKENRUNTIME_API UHarkenSettings final : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UHarkenSettings();

    virtual FName GetCategoryName() const override;
    bool IsCommunicationEnabled() const;

    UPROPERTY(Config, EditAnywhere, Category = "Startup")
    bool bAutoInitialize = true;

    UPROPERTY(Config, EditAnywhere, Category = "Startup")
    EHarkenDeviceMode DeviceMode;

    UPROPERTY(Config, EditAnywhere, Category = "Startup", meta = (ClampMin = "0"))
    int32 SampleRate = 0;

    UPROPERTY(Config, EditAnywhere, Category = "Startup", meta = (ClampMin = "0"))
    int32 BlockFrames = 0;

    // Preview envelope path. Startup ignores it; games load StartupPackPaths.
    UPROPERTY(Config, EditAnywhere, Category = "Startup")
    FString StartupCurrentWorldPath;

    UPROPERTY(Config, EditAnywhere, Category = "Startup")
    TArray<FString> StartupPackPaths;

    UPROPERTY(Config, EditAnywhere, Category = "Lifecycle")
    bool bSuspendWhenBackgrounded = true;

    UPROPERTY(Config, EditAnywhere, Category = "Communication", meta = (DisplayName = "Enable Communication in Builds"))
    bool bEnableCommunicationInBuilds = true;

    UPROPERTY(Config, EditAnywhere, Category = "Communication")
    FString RemoteBindAddress = TEXT("0.0.0.0");

    UPROPERTY(Config, EditAnywhere, Category = "Communication", meta = (ClampMin = "0", ClampMax = "65535"))
    int32 RemotePort = 0;

    UPROPERTY(Config, EditAnywhere, Category = "Communication")
    bool bEnableRemoteDiscovery = true;

    UPROPERTY(Config, EditAnywhere, Category = "Communication", meta = (ClampMin = "0", ClampMax = "65535"))
    int32 RemoteDiscoveryPort = 0;

    UPROPERTY(Config, EditAnywhere, Category = "Communication")
    FString RemoteApplicationName;

    UPROPERTY(Config, EditAnywhere, Category = "Capacity", meta = (ClampMin = "0"))
    int32 MaxEmitters = 0;

    UPROPERTY(Config, EditAnywhere, Category = "Capacity", meta = (ClampMin = "0"))
    int32 EmitterVariableSlots = 0;
};
