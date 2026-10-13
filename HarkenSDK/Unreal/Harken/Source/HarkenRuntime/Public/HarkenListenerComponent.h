#pragma once

#include "Components/SceneComponent.h"
#include "HarkenListenerComponent.generated.h"

UCLASS(ClassGroup = (Harken), meta = (BlueprintSpawnableComponent))
class HARKENRUNTIME_API UHarkenListenerComponent final : public USceneComponent
{
    GENERATED_BODY()

public:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
};
