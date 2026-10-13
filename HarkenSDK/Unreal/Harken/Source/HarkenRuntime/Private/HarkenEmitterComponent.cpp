#include "HarkenEmitterComponent.h"

#include "Engine/Engine.h"
#include "HarkenCoreRuntime.h"
#include "HarkenEngineSubsystem.h"
#include "HarkenRuntime.h"

namespace
{
FHarkenResult EmitterUnavailable()
{
    return FHarkenCoreRuntime::MakeResult(
        VN_NOT_INITIALIZED, TEXT("Harken emitter is unavailable"));
}
}

UHarkenEmitterComponent::UHarkenEmitterComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
}

void UHarkenEmitterComponent::BeginPlay()
{
    Super::BeginPlay();

    UHarkenEngineSubsystem* Subsystem = GetHarkenSubsystem();
    if (Subsystem == nullptr)
    {
        UE_LOG(LogHarken, Error, TEXT("Harken engine subsystem is unavailable."));
        return;
    }
    if (!Subsystem->IsHarkenInitialized())
    {
        return;
    }

    const FHarkenResult CreateResult = EnsureEmitter(*Subsystem);
    if (!CreateResult.bSuccess)
    {
        UE_LOG(LogHarken, Error, TEXT("%s"), *CreateResult.Message);
        return;
    }
    UpdateEmitterPose();
}

void UHarkenEmitterComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (EmitterHandle.IsValid())
    {
        if (UHarkenEngineSubsystem* Subsystem = GetHarkenSubsystem();
            Subsystem != nullptr && Subsystem->IsHarkenInitialized())
        {
            if (bStopOnEndPlay)
            {
                Subsystem->StopEmitter(EmitterHandle, EndPlayFadeMilliseconds);
            }
            Subsystem->DestroyEmitter(EmitterHandle);
        }
        EmitterHandle = FHarkenEmitterHandle{};
    }
    Super::EndPlay(EndPlayReason);
}

void UHarkenEmitterComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                           FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (UHarkenEngineSubsystem* Subsystem = GetHarkenSubsystem();
        Subsystem != nullptr && Subsystem->IsHarkenInitialized())
    {
        const FHarkenResult Result = EnsureEmitter(*Subsystem);
        if (Result.bSuccess)
        {
            UpdateEmitterPose();
        }
    }
}

bool UHarkenEmitterComponent::HasEmitter() const
{
    return EmitterHandle.IsValid();
}

FHarkenEmitterHandle UHarkenEmitterComponent::GetEmitterHandle() const
{
    return EmitterHandle;
}

FHarkenResult UHarkenEmitterComponent::PostActionById(
    FGuid ActionId, int64 CallerSeed, FHarkenActionFiringHandle& Firing)
{
    if (UHarkenEngineSubsystem* Subsystem = GetHarkenSubsystem())
    {
        const FHarkenResult ReadyResult = EnsureEmitter(*Subsystem);
        if (!ReadyResult.bSuccess)
        {
            Firing = FHarkenActionFiringHandle{};
            return ReadyResult;
        }
        return Subsystem->PostActionById(
            ActionId, EmitterHandle, CallerSeed, Firing);
    }
    Firing = FHarkenActionFiringHandle{};
    return EmitterUnavailable();
}

FHarkenResult UHarkenEmitterComponent::PostActionByName(
    const FString& ActionName, int64 CallerSeed, FHarkenActionFiringHandle& Firing)
{
    Firing = FHarkenActionFiringHandle{};
    if (UHarkenEngineSubsystem* Subsystem = GetHarkenSubsystem())
    {
        const FHarkenResult ReadyResult = EnsureEmitter(*Subsystem);
        if (!ReadyResult.bSuccess) return ReadyResult;
        return Subsystem->PostActionByName(ActionName, EmitterHandle, CallerSeed, Firing);
    }
    return EmitterUnavailable();
}

FHarkenResult UHarkenEmitterComponent::PostAction(FGuid ActionId, int64 CallerSeed,
    FHarkenActionFiringHandle& Firing)
{
    return PostActionById(ActionId, CallerSeed, Firing);
}

FHarkenResult UHarkenEmitterComponent::PostAction(const FString& ActionName, int64 CallerSeed,
    FHarkenActionFiringHandle& Firing)
{
    return PostActionByName(ActionName, CallerSeed, Firing);
}

FHarkenResult UHarkenEmitterComponent::StopEmitter(int32 FadeMilliseconds)
{
    if (UHarkenEngineSubsystem* Subsystem = GetHarkenSubsystem())
    {
        const FHarkenResult ReadyResult = EnsureEmitter(*Subsystem);
        if (!ReadyResult.bSuccess)
        {
            return ReadyResult;
        }
        return Subsystem->StopEmitter(EmitterHandle, FadeMilliseconds);
    }
    return EmitterUnavailable();
}

FHarkenResult UHarkenEmitterComponent::SetValue(FGuid VariableId, float Value)
{
    if (UHarkenEngineSubsystem* Subsystem = GetHarkenSubsystem())
    {
        const FHarkenResult ReadyResult = EnsureEmitter(*Subsystem);
        if (!ReadyResult.bSuccess)
        {
            return ReadyResult;
        }
        return Subsystem->SetValue(VariableId, Value, EmitterHandle);
    }
    return EmitterUnavailable();
}

FHarkenResult UHarkenEmitterComponent::SetVariable(FHarkenVariableId Variable, float Value)
{
    if (UHarkenEngineSubsystem* Subsystem = GetHarkenSubsystem())
    {
        const FHarkenResult ReadyResult = EnsureEmitter(*Subsystem);
        if (!ReadyResult.bSuccess)
        {
            return ReadyResult;
        }
        return Subsystem->SetValue(Variable, Value, EmitterHandle);
    }
    return EmitterUnavailable();
}

FHarkenResult UHarkenEmitterComponent::QueryVariable(FHarkenVariableId Variable,
                                                    FHarkenVariableQueryResult& Result)
{
    if (UHarkenEngineSubsystem* Subsystem = GetHarkenSubsystem())
    {
        const FHarkenResult ReadyResult = EnsureEmitter(*Subsystem);
        if (!ReadyResult.bSuccess)
        {
            Result = FHarkenVariableQueryResult{};
            return ReadyResult;
        }
        return Subsystem->QueryEmitterVariable(EmitterHandle, Variable, Result);
    }
    Result = FHarkenVariableQueryResult{};
    return EmitterUnavailable();
}

UHarkenEngineSubsystem* UHarkenEmitterComponent::GetHarkenSubsystem() const
{
    return GEngine != nullptr ? GEngine->GetEngineSubsystem<UHarkenEngineSubsystem>() : nullptr;
}

FHarkenResult UHarkenEmitterComponent::EnsureEmitter(UHarkenEngineSubsystem& Subsystem)
{
    if (!Subsystem.IsHarkenInitialized())
    {
        return EmitterUnavailable();
    }
    const int64 CurrentGeneration = Subsystem.GetSessionGeneration();
    if (EmitterHandle.IsValid() &&
        EmitterHandle.SessionGeneration == CurrentGeneration)
    {
        return FHarkenCoreRuntime::MakeResult(VN_OK);
    }

    EmitterHandle = FHarkenEmitterHandle{};
    return Subsystem.CreateEmitter(EmitterHandle);
}

void UHarkenEmitterComponent::UpdateEmitterPose()
{
    if (!EmitterHandle.IsValid())
    {
        return;
    }
    if (UHarkenEngineSubsystem* Subsystem = GetHarkenSubsystem())
    {
        const FHarkenResult Result = Subsystem->SetEmitterTransform(EmitterHandle, GetComponentTransform());
        if (!Result.bSuccess && Result.Code != VN_BACKPRESSURE)
        {
            UE_LOG(LogHarken, Warning, TEXT("%s"), *Result.Message);
        }
    }
}
