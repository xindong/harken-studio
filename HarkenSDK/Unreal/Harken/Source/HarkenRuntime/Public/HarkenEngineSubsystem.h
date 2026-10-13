#pragma once

#include "Containers/Ticker.h"
#include "Subsystems/EngineSubsystem.h"
#include "HarkenTypes.h"
#include "HarkenEngineSubsystem.generated.h"

class UHarkenListenerComponent;
class UHarkenEmitterComponent;

UCLASS()
class HARKENRUNTIME_API UHarkenEngineSubsystem final : public UEngineSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    UFUNCTION(BlueprintPure, Category = "Harken")
    bool IsHarkenInitialized() const;

    UFUNCTION(BlueprintCallable, Category = "Harken")
    FHarkenResult StartHarken();

    UFUNCTION(BlueprintCallable, Category = "Harken")
    FHarkenResult StopHarken();

    UFUNCTION(BlueprintCallable, Category = "Harken")
    FHarkenResult SuspendHarken();

    UFUNCTION(BlueprintCallable, Category = "Harken")
    FHarkenResult ResumeHarken();

    UFUNCTION(BlueprintCallable, Category = "Harken|Communication")
    FHarkenResult RetryRemoteCommunication();

    UFUNCTION(BlueprintCallable, Category = "Harken|Communication")
    FHarkenResult QueryRemoteCommunication(FHarkenRemoteStatus& Status) const;

    UFUNCTION(BlueprintPure, Category = "Harken|Communication")
    FHarkenResult GetRemoteCommunicationResult() const;

    UFUNCTION(BlueprintCallable, Category = "Harken")
    FHarkenResult LoadCurrentWorld(const FString& Path, FHarkenCurrentWorldLoadReport& Report);

    UFUNCTION(BlueprintCallable, Category = "Harken")
    FHarkenResult LoadPack(const FString& Path, FHarkenPackHandle& Pack);

    UFUNCTION(BlueprintCallable, Category = "Harken")
    FHarkenResult UnloadPack(UPARAM(ref) FHarkenPackHandle& Pack);

    UFUNCTION(BlueprintCallable, Category = "Harken")
    FHarkenResult PostActionById(FGuid ActionId, FHarkenEmitterHandle Emitter,
                                int64 CallerSeed,
                                FHarkenActionFiringHandle& Firing);
    UFUNCTION(BlueprintCallable, Category = "Harken")
    FHarkenResult PostActionByName(const FString& ActionName, FHarkenEmitterHandle Emitter,
                                  int64 CallerSeed, FHarkenActionFiringHandle& Firing);
    FHarkenResult PostAction(FGuid ActionId, FHarkenEmitterHandle Emitter,
                             int64 CallerSeed, FHarkenActionFiringHandle& Firing);
    FHarkenResult PostAction(const FString& ActionName, FHarkenEmitterHandle Emitter,
                             int64 CallerSeed, FHarkenActionFiringHandle& Firing);

    UFUNCTION(BlueprintCallable, Category = "Harken", meta = (ClampMin = "0"))
    FHarkenResult StopActionFiring(FHarkenActionFiringHandle Firing,
                                  int32 FadeMilliseconds = 0);

    UFUNCTION(BlueprintCallable, Category = "Harken")
    FHarkenResult QueryActionFiring(FHarkenActionFiringHandle Firing,
                                   FHarkenActionFiringInfo& Info);

    UFUNCTION(BlueprintCallable, Category = "Harken", meta = (ClampMin = "0"))
    FHarkenResult QueryActionStep(FHarkenActionFiringHandle Firing,
                                 int32 AuthoredEventIndex,
                                 FHarkenActionStepResult& Step);

    UFUNCTION(BlueprintCallable, Category = "Harken")
    FHarkenResult ReleaseActionFiring(UPARAM(ref) FHarkenActionFiringHandle& Firing);

    UFUNCTION(BlueprintCallable, Category = "Harken", meta = (ClampMin = "0"))
    FHarkenResult StopAll(int32 FadeMilliseconds = 0);

    UFUNCTION(BlueprintCallable, Category = "Harken")
    FHarkenResult SetListenerTransform(const FTransform& Transform);

    UFUNCTION(BlueprintCallable, Category = "Harken")
    FHarkenResult SetValue(FHarkenVariableId Variable, float Value,
                          UHarkenEmitterComponent* Emitter = nullptr);
    FHarkenResult SetValue(FHarkenVariableId Variable, float Value,
                          FHarkenEmitterHandle Emitter);
    FHarkenResult SetValue(FGuid VariableId, float Value,
                          FHarkenEmitterHandle Emitter = FHarkenEmitterHandle());
    FHarkenResult SetValue(const FString& VariableName, float Value,
                          FHarkenEmitterHandle Emitter = FHarkenEmitterHandle());
    UFUNCTION(BlueprintCallable, Category = "Harken")
    FHarkenResult SetValueById(FGuid VariableId, float Value,
                              UHarkenEmitterComponent* Emitter = nullptr);
    UFUNCTION(BlueprintCallable, Category = "Harken")
    FHarkenResult SetValueByName(const FString& VariableName, float Value,
                                UHarkenEmitterComponent* Emitter = nullptr);
    UFUNCTION(BlueprintCallable, Category = "Harken")
    FHarkenResult ResolveVariable(const FString& VariableName, FGuid& VariableId);

    FHarkenResult SetValue(FHarkenVariableId Variable, FGuid LabelId,
                         UHarkenEmitterComponent* Emitter = nullptr);
    FHarkenResult SetValue(FHarkenVariableId Variable, FGuid LabelId,
                         FHarkenEmitterHandle Emitter);
    FHarkenResult SetValue(FHarkenVariableId Variable, const FString& LabelName,
                         UHarkenEmitterComponent* Emitter = nullptr);
    FHarkenResult SetValue(FHarkenVariableId Variable, const FString& LabelName,
                         FHarkenEmitterHandle Emitter);
    FHarkenResult SetValue(FGuid VariableId, FGuid LabelId,
                         FHarkenEmitterHandle Emitter = FHarkenEmitterHandle());
    FHarkenResult SetValue(FGuid VariableId, const FString& LabelName,
                         FHarkenEmitterHandle Emitter = FHarkenEmitterHandle());
    FHarkenResult SetValue(const FString& VariableName, FGuid LabelId,
                         FHarkenEmitterHandle Emitter = FHarkenEmitterHandle());
    FHarkenResult SetValue(const FString& VariableName, const FString& LabelName,
                         FHarkenEmitterHandle Emitter = FHarkenEmitterHandle());

    UFUNCTION(BlueprintCallable, Category = "Harken", meta = (DisplayName = "Set Value"))
    FHarkenResult SetValueByLabelId(FHarkenVariableId Variable, FGuid LabelId,
                                 UHarkenEmitterComponent* Emitter = nullptr);
    UFUNCTION(BlueprintCallable, Category = "Harken", meta = (DisplayName = "Set Value"))
    FHarkenResult SetValueByLabelName(FHarkenVariableId Variable, const FString& Name,
                                   UHarkenEmitterComponent* Emitter = nullptr);

    UFUNCTION(BlueprintCallable, Category = "Harken", meta = (DeprecatedFunction,
        DeprecationMessage = "Use SetValue with the Variable default value."))
    UE_DEPRECATED(5.0, "Use SetValue with the Variable default value.")
    FHarkenResult ResetValue(FHarkenVariableId Variable);

    UFUNCTION(BlueprintCallable, Category = "Harken")
    FHarkenResult QueryVariable(FHarkenVariableId Variable,
                               FHarkenVariableQueryResult& Result);

    UFUNCTION(BlueprintCallable, Category = "Harken")
    FHarkenResult QueryVariableTicket(FHarkenVariableTicket Ticket,
                                     FHarkenVariableTicketQueryResult& Result);

    FHarkenResult CreateEmitter(FHarkenEmitterHandle& Emitter);
    FHarkenResult DestroyEmitter(FHarkenEmitterHandle& Emitter);
    FHarkenResult SetEmitterTransform(FHarkenEmitterHandle Emitter, const FTransform& Transform);
    FHarkenResult StopEmitter(FHarkenEmitterHandle Emitter, int32 FadeMilliseconds);
    UE_DEPRECATED(5.0, "Use SetValue(variable, value, emitter).")
    FHarkenResult SetEmitterValue(FHarkenEmitterHandle Emitter, FGuid VariableId, float Value);
    UFUNCTION(BlueprintCallable, Category = "Harken", meta = (DeprecatedFunction,
        DeprecationMessage = "Use SetValue with an Emitter."))
    UE_DEPRECATED(5.0, "Use SetValue(variable, value, emitter).")
    FHarkenResult SetEmitterVariable(FHarkenEmitterHandle Emitter, FHarkenVariableId Variable,
                                    float Value);
    UFUNCTION(BlueprintCallable, Category = "Harken")
    FHarkenResult QueryEmitterVariable(FHarkenEmitterHandle Emitter, FHarkenVariableId Variable,
                                      FHarkenVariableQueryResult& Result);

    int64 GetSessionGeneration() const;

    void RegisterListener(UHarkenListenerComponent* Listener);
    void UnregisterListener(UHarkenListenerComponent* Listener);

private:
    bool Tick(float DeltaTime);
    void EnterBackground();
    void EnterForeground();
    void LoadStartupContent();
    FString ResolveContentPath(const FString& Path) const;
    FHarkenResult LoadContentManifest();
    void ClearLoadedPacks();

    FTSTicker::FDelegateHandle TickerHandle;
    FDelegateHandle BackgroundHandle;
    FDelegateHandle ForegroundHandle;
    TArray<TWeakObjectPtr<UHarkenListenerComponent>> Listeners;
    TArray<FHarkenPackHandle> LoadedPacks;
    int64 SessionGeneration = 0;
    bool bStartupContentLoaded = false;
};
