#pragma once

#include "Components/SceneComponent.h"
#include "HarkenTypes.h"
#include "HarkenEmitterComponent.generated.h"

UCLASS(ClassGroup = (Harken), meta = (BlueprintSpawnableComponent))
class HARKENRUNTIME_API UHarkenEmitterComponent final : public USceneComponent
{
    GENERATED_BODY()

public:
    UHarkenEmitterComponent();

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
                               FActorComponentTickFunction* ThisTickFunction) override;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harken")
    bool bStopOnEndPlay = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harken", meta = (ClampMin = "0"))
    int32 EndPlayFadeMilliseconds = 0;

    UFUNCTION(BlueprintPure, Category = "Harken")
    bool HasEmitter() const;

    UFUNCTION(BlueprintPure, Category = "Harken")
    FHarkenEmitterHandle GetEmitterHandle() const;

    UFUNCTION(BlueprintCallable, Category = "Harken")
    FHarkenResult PostActionById(FGuid ActionId, int64 CallerSeed,
                                FHarkenActionFiringHandle& Firing);
    UFUNCTION(BlueprintCallable, Category = "Harken")
    FHarkenResult PostActionByName(const FString& ActionName, int64 CallerSeed,
                                  FHarkenActionFiringHandle& Firing);
    FHarkenResult PostAction(FGuid ActionId, int64 CallerSeed, FHarkenActionFiringHandle& Firing);
    FHarkenResult PostAction(const FString& ActionName, int64 CallerSeed,
                             FHarkenActionFiringHandle& Firing);

    UFUNCTION(BlueprintCallable, Category = "Harken", meta = (ClampMin = "0"))
    FHarkenResult StopEmitter(int32 FadeMilliseconds = 0);

    UFUNCTION(BlueprintCallable, Category = "Harken", meta = (DeprecatedFunction,
        DeprecationMessage = "Use the engine subsystem SetValue with an Emitter."))
    UE_DEPRECATED(5.0, "Use the engine subsystem SetValue with an Emitter.")
    FHarkenResult SetValue(FGuid VariableId, float Value);

    UFUNCTION(BlueprintCallable, Category = "Harken", meta = (DeprecatedFunction,
        DeprecationMessage = "Use the engine subsystem SetValue with an Emitter."))
    UE_DEPRECATED(5.0, "Use the engine subsystem SetValue with an Emitter.")
    FHarkenResult SetVariable(FHarkenVariableId Variable, float Value);

    UFUNCTION(BlueprintCallable, Category = "Harken")
    FHarkenResult QueryVariable(FHarkenVariableId Variable,
                               FHarkenVariableQueryResult& Result);

private:
    class UHarkenEngineSubsystem* GetHarkenSubsystem() const;
    FHarkenResult EnsureEmitter(UHarkenEngineSubsystem& Subsystem);
    void UpdateEmitterPose();

    FHarkenEmitterHandle EmitterHandle;
};
