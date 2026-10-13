#include "HarkenListenerComponent.h"

#include "Engine/Engine.h"
#include "HarkenEngineSubsystem.h"

void UHarkenListenerComponent::BeginPlay()
{
    Super::BeginPlay();
    if (GEngine != nullptr)
    {
        if (UHarkenEngineSubsystem* Subsystem = GEngine->GetEngineSubsystem<UHarkenEngineSubsystem>())
        {
            Subsystem->RegisterListener(this);
        }
    }
}

void UHarkenListenerComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (GEngine != nullptr)
    {
        if (UHarkenEngineSubsystem* Subsystem = GEngine->GetEngineSubsystem<UHarkenEngineSubsystem>())
        {
            Subsystem->UnregisterListener(this);
        }
    }
    Super::EndPlay(EndPlayReason);
}
