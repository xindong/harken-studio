#include "HarkenEngineSubsystem.h"

#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HarkenCoreRuntime.h"
#include "HarkenEmitterComponent.h"
#include "HarkenListenerComponent.h"
#include "HarkenRuntime.h"
#include "HarkenSettings.h"

void UHarkenEngineSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this, &UHarkenEngineSubsystem::Tick));
    BackgroundHandle = FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddUObject(
        this, &UHarkenEngineSubsystem::EnterBackground);
    ForegroundHandle = FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddUObject(
        this, &UHarkenEngineSubsystem::EnterForeground);

    const UHarkenSettings* Settings = GetDefault<UHarkenSettings>();
    if (Settings->bAutoInitialize)
    {
        const FHarkenResult Result = StartHarken();
        if (!Result.bSuccess)
        {
            UE_LOG(LogHarken, Error, TEXT("%s"), *Result.Message);
        }
    }
}

void UHarkenEngineSubsystem::Deinitialize()
{
    Listeners.Reset();
    if (TickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
        TickerHandle.Reset();
    }
    if (BackgroundHandle.IsValid())
    {
        FCoreDelegates::ApplicationWillEnterBackgroundDelegate.Remove(BackgroundHandle);
        BackgroundHandle.Reset();
    }
    if (ForegroundHandle.IsValid())
    {
        FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Remove(ForegroundHandle);
        ForegroundHandle.Reset();
    }
    StopHarken();
    Super::Deinitialize();
}

bool UHarkenEngineSubsystem::IsHarkenInitialized() const
{
    return FHarkenCoreRuntime::Get().IsInitialized();
}

FHarkenResult UHarkenEngineSubsystem::StartHarken()
{
    const bool bWasInitialized = IsHarkenInitialized();
    const FHarkenResult Result = FHarkenCoreRuntime::Get().Initialize(*GetDefault<UHarkenSettings>());
    if (Result.bSuccess && !bWasInitialized)
    {
        ++SessionGeneration;
        bStartupContentLoaded = false;
        LoadStartupContent();
    }
    return Result;
}

FHarkenResult UHarkenEngineSubsystem::StopHarken()
{
    bStartupContentLoaded = false;
    ClearLoadedPacks();
    return FHarkenCoreRuntime::Get().Shutdown();
}

FHarkenResult UHarkenEngineSubsystem::SuspendHarken()
{
    return FHarkenCoreRuntime::Get().Suspend();
}

FHarkenResult UHarkenEngineSubsystem::ResumeHarken()
{
    return FHarkenCoreRuntime::Get().Resume();
}

FHarkenResult UHarkenEngineSubsystem::RetryRemoteCommunication()
{
    return FHarkenCoreRuntime::Get().RetryRemoteCommunication(*GetDefault<UHarkenSettings>());
}

FHarkenResult UHarkenEngineSubsystem::QueryRemoteCommunication(FHarkenRemoteStatus& Status) const
{
    return FHarkenCoreRuntime::Get().QueryRemoteCommunication(Status);
}

FHarkenResult UHarkenEngineSubsystem::GetRemoteCommunicationResult() const
{
    return FHarkenCoreRuntime::Get().GetRemoteCommunicationResult();
}

FHarkenResult UHarkenEngineSubsystem::LoadCurrentWorld(const FString& Path,
                                                      FHarkenCurrentWorldLoadReport& Report)
{
    const FString FullPath = ResolveContentPath(Path);
    if (!FPaths::FileExists(FullPath))
    {
        Report = FHarkenCurrentWorldLoadReport{};
        return FHarkenCoreRuntime::MakeResult(VN_CURRENT_WORLD_IO_ERROR,
                                             FString::Printf(TEXT("Current World file does not exist: %s"), *FullPath));
    }
    return FHarkenCoreRuntime::Get().LoadCurrentWorld(FullPath, Report);
}

FHarkenResult UHarkenEngineSubsystem::LoadPack(const FString& Path, FHarkenPackHandle& Pack)
{
    const FString FullPath = ResolveContentPath(Path);
    if (!FPaths::FileExists(FullPath))
    {
        Pack = FHarkenPackHandle{};
        return FHarkenCoreRuntime::MakeResult(VN_BAD_PACK,
                                             FString::Printf(TEXT("Pack file does not exist: %s"), *FullPath));
    }
    return FHarkenCoreRuntime::Get().LoadPack(FullPath, SessionGeneration, Pack);
}

FHarkenResult UHarkenEngineSubsystem::UnloadPack(FHarkenPackHandle& Pack)
{
    return FHarkenCoreRuntime::Get().UnloadPack(Pack, SessionGeneration);
}

FHarkenResult UHarkenEngineSubsystem::PostActionById(FGuid ActionId,
                                                   FHarkenEmitterHandle Emitter,
                                                   int64 CallerSeed,
                                                   FHarkenActionFiringHandle& Firing)
{
    return FHarkenCoreRuntime::Get().PostAction(
        ActionId, Emitter, SessionGeneration, CallerSeed, Firing);
}

FHarkenResult UHarkenEngineSubsystem::PostActionByName(const FString& ActionName,
    FHarkenEmitterHandle Emitter, int64 CallerSeed, FHarkenActionFiringHandle& Firing)
{
    return FHarkenCoreRuntime::Get().PostAction(
        ActionName, Emitter, SessionGeneration, CallerSeed, Firing);
}

FHarkenResult UHarkenEngineSubsystem::PostAction(FGuid ActionId,
    FHarkenEmitterHandle Emitter, int64 CallerSeed, FHarkenActionFiringHandle& Firing)
{
    return PostActionById(ActionId, Emitter, CallerSeed, Firing);
}

FHarkenResult UHarkenEngineSubsystem::PostAction(const FString& ActionName,
    FHarkenEmitterHandle Emitter, int64 CallerSeed, FHarkenActionFiringHandle& Firing)
{
    return PostActionByName(ActionName, Emitter, CallerSeed, Firing);
}

FHarkenResult UHarkenEngineSubsystem::StopActionFiring(FHarkenActionFiringHandle Firing,
                                                      int32 FadeMilliseconds)
{
    return FHarkenCoreRuntime::Get().StopActionFiring(
        Firing, SessionGeneration, FadeMilliseconds);
}

FHarkenResult UHarkenEngineSubsystem::QueryActionFiring(FHarkenActionFiringHandle Firing,
                                                       FHarkenActionFiringInfo& Info)
{
    return FHarkenCoreRuntime::Get().QueryActionFiring(
        Firing, SessionGeneration, Info);
}

FHarkenResult UHarkenEngineSubsystem::QueryActionStep(FHarkenActionFiringHandle Firing,
                                                     int32 AuthoredEventIndex,
                                                     FHarkenActionStepResult& Step)
{
    return FHarkenCoreRuntime::Get().QueryActionStep(
        Firing, SessionGeneration, AuthoredEventIndex, Step);
}

FHarkenResult UHarkenEngineSubsystem::ReleaseActionFiring(FHarkenActionFiringHandle& Firing)
{
    return FHarkenCoreRuntime::Get().ReleaseActionFiring(
        Firing, SessionGeneration);
}

FHarkenResult UHarkenEngineSubsystem::StopAll(int32 FadeMilliseconds)
{
    return FHarkenCoreRuntime::Get().StopAll(FadeMilliseconds);
}

FHarkenResult UHarkenEngineSubsystem::SetListenerTransform(const FTransform& Transform)
{
    return FHarkenCoreRuntime::Get().SetListenerTransform(Transform);
}

FHarkenResult UHarkenEngineSubsystem::CreateEmitter(FHarkenEmitterHandle& Emitter)
{
    return FHarkenCoreRuntime::Get().CreateEmitter(SessionGeneration, Emitter);
}

FHarkenResult UHarkenEngineSubsystem::DestroyEmitter(FHarkenEmitterHandle& Emitter)
{
    return FHarkenCoreRuntime::Get().DestroyEmitter(Emitter, SessionGeneration);
}

FHarkenResult UHarkenEngineSubsystem::SetEmitterTransform(FHarkenEmitterHandle Emitter,
                                                        const FTransform& Transform)
{
    return FHarkenCoreRuntime::Get().SetEmitterTransform(
        Emitter, SessionGeneration, Transform);
}

FHarkenResult UHarkenEngineSubsystem::StopEmitter(FHarkenEmitterHandle Emitter,
                                                int32 FadeMilliseconds)
{
    return FHarkenCoreRuntime::Get().StopEmitter(
        Emitter, SessionGeneration, FadeMilliseconds);
}

FHarkenResult UHarkenEngineSubsystem::SetValue(FHarkenVariableId Variable, float Value,
                                             UHarkenEmitterComponent* Emitter)
{
    if (Emitter != nullptr && (!IsValid(Emitter) || !Emitter->HasEmitter()))
        return FHarkenCoreRuntime::MakeResult(VN_STALE_HANDLE, TEXT("Set variable"));
    return SetValue(Variable, Value,
        Emitter == nullptr ? FHarkenEmitterHandle{} : Emitter->GetEmitterHandle());
}

FHarkenResult UHarkenEngineSubsystem::SetValue(FHarkenVariableId Variable, float Value,
                                             FHarkenEmitterHandle Emitter)
{
    return FHarkenCoreRuntime::Get().SetValue(Variable, Value, Emitter, SessionGeneration);
}

FHarkenResult UHarkenEngineSubsystem::SetValue(FGuid VariableId, float Value,
                                             FHarkenEmitterHandle Emitter)
{
    return FHarkenCoreRuntime::Get().SetValue(VariableId, Value, Emitter, SessionGeneration);
}

FHarkenResult UHarkenEngineSubsystem::SetValue(const FString& VariableName, float Value,
    FHarkenEmitterHandle Emitter)
{
    return FHarkenCoreRuntime::Get().SetValue(VariableName, Value, Emitter, SessionGeneration);
}

FHarkenResult UHarkenEngineSubsystem::SetValueById(FGuid VariableId, float Value,
    UHarkenEmitterComponent* Emitter)
{
    if (Emitter != nullptr && (!IsValid(Emitter) || !Emitter->HasEmitter()))
        return FHarkenCoreRuntime::MakeResult(VN_STALE_HANDLE, TEXT("Set variable"));
    return SetValue(VariableId, Value,
        Emitter == nullptr ? FHarkenEmitterHandle{} : Emitter->GetEmitterHandle());
}

FHarkenResult UHarkenEngineSubsystem::SetValueByName(const FString& VariableName, float Value,
    UHarkenEmitterComponent* Emitter)
{
    if (Emitter != nullptr && (!IsValid(Emitter) || !Emitter->HasEmitter()))
        return FHarkenCoreRuntime::MakeResult(VN_STALE_HANDLE, TEXT("Set variable"));
    return SetValue(VariableName, Value,
        Emitter == nullptr ? FHarkenEmitterHandle{} : Emitter->GetEmitterHandle());
}

FHarkenResult UHarkenEngineSubsystem::ResolveVariable(const FString& VariableName, FGuid& VariableId)
{
    return FHarkenCoreRuntime::Get().ResolveVariable(VariableName, VariableId);
}

FHarkenResult UHarkenEngineSubsystem::ResetValue(FHarkenVariableId Variable)
{
    return FHarkenCoreRuntime::Get().ResetValue(Variable);
}

FHarkenResult UHarkenEngineSubsystem::SetValue(FHarkenVariableId Variable, FGuid LabelId,
                                            UHarkenEmitterComponent* Emitter)
{
    if (Emitter != nullptr && (!IsValid(Emitter) || !Emitter->HasEmitter()))
        return FHarkenCoreRuntime::MakeResult(VN_STALE_HANDLE, TEXT("Set variable"));
    return SetValue(Variable, LabelId,
        Emitter == nullptr ? FHarkenEmitterHandle{} : Emitter->GetEmitterHandle());
}

FHarkenResult UHarkenEngineSubsystem::SetValue(FHarkenVariableId Variable, FGuid LabelId,
                                            FHarkenEmitterHandle Emitter)
{
    return FHarkenCoreRuntime::Get().SetValue(Variable, LabelId, nullptr,
                                            Emitter, SessionGeneration);
}

FHarkenResult UHarkenEngineSubsystem::SetValue(FHarkenVariableId Variable, const FString& LabelName,
                                                  UHarkenEmitterComponent* Emitter)
{
    if (Emitter != nullptr && (!IsValid(Emitter) || !Emitter->HasEmitter()))
        return FHarkenCoreRuntime::MakeResult(VN_STALE_HANDLE, TEXT("Set variable"));
    return SetValue(Variable, LabelName,
        Emitter == nullptr ? FHarkenEmitterHandle{} : Emitter->GetEmitterHandle());
}

FHarkenResult UHarkenEngineSubsystem::SetValue(FHarkenVariableId Variable, const FString& LabelName,
                                                  FHarkenEmitterHandle Emitter)
{
    return FHarkenCoreRuntime::Get().SetValue(Variable, {}, &LabelName,
                                            Emitter, SessionGeneration);
}

FHarkenResult UHarkenEngineSubsystem::SetValue(FGuid VariableId, FGuid LabelId,
                                          FHarkenEmitterHandle Emitter)
{
    return FHarkenCoreRuntime::Get().SetValue(VariableId, LabelId, nullptr,
                                           Emitter, SessionGeneration);
}

FHarkenResult UHarkenEngineSubsystem::SetValue(FGuid VariableId, const FString& LabelName,
                                          FHarkenEmitterHandle Emitter)
{
    return FHarkenCoreRuntime::Get().SetValue(VariableId, {}, &LabelName,
                                           Emitter, SessionGeneration);
}

FHarkenResult UHarkenEngineSubsystem::SetValue(const FString& VariableName, FGuid LabelId,
                                          FHarkenEmitterHandle Emitter)
{
    return FHarkenCoreRuntime::Get().SetValue(VariableName, LabelId, nullptr,
                                           Emitter, SessionGeneration);
}

FHarkenResult UHarkenEngineSubsystem::SetValue(const FString& VariableName, const FString& LabelName,
                                          FHarkenEmitterHandle Emitter)
{
    return FHarkenCoreRuntime::Get().SetValue(VariableName, {}, &LabelName,
                                           Emitter, SessionGeneration);
}

FHarkenResult UHarkenEngineSubsystem::SetValueByLabelId(FHarkenVariableId Variable, FGuid LabelId,
                                                   UHarkenEmitterComponent* Emitter)
{
    return SetValue(Variable, LabelId, Emitter);
}

FHarkenResult UHarkenEngineSubsystem::SetValueByLabelName(FHarkenVariableId Variable,
                                                     const FString& Name,
                                                     UHarkenEmitterComponent* Emitter)
{
    return SetValue(Variable, Name, Emitter);
}

FHarkenResult UHarkenEngineSubsystem::QueryVariable(FHarkenVariableId Variable,
                                                   FHarkenVariableQueryResult& Result)
{
    return FHarkenCoreRuntime::Get().QueryVariable(Variable, Result);
}

FHarkenResult UHarkenEngineSubsystem::QueryVariableTicket(
    FHarkenVariableTicket Ticket, FHarkenVariableTicketQueryResult& Result)
{
    return FHarkenCoreRuntime::Get().QueryVariableTicket(Ticket, Result);
}

FHarkenResult UHarkenEngineSubsystem::SetEmitterValue(FHarkenEmitterHandle Emitter,
                                                    FGuid VariableId, float Value)
{
    if (!Emitter.IsValid())
        return FHarkenCoreRuntime::MakeResult(VN_STALE_HANDLE, TEXT("Set variable"));
    return SetValue(VariableId, Value, Emitter);
}

FHarkenResult UHarkenEngineSubsystem::SetEmitterVariable(FHarkenEmitterHandle Emitter,
                                                       FHarkenVariableId Variable,
                                                       float Value)
{
    if (!Emitter.IsValid())
        return FHarkenCoreRuntime::MakeResult(VN_STALE_HANDLE, TEXT("Set variable"));
    return SetValue(Variable, Value, Emitter);
}

FHarkenResult UHarkenEngineSubsystem::QueryEmitterVariable(
    FHarkenEmitterHandle Emitter, FHarkenVariableId Variable,
    FHarkenVariableQueryResult& Result)
{
    return FHarkenCoreRuntime::Get().QueryEmitterVariable(
        Emitter, SessionGeneration, Variable, Result);
}

int64 UHarkenEngineSubsystem::GetSessionGeneration() const
{
    return SessionGeneration;
}

void UHarkenEngineSubsystem::RegisterListener(UHarkenListenerComponent* Listener)
{
    Listeners.RemoveAll([Listener](const TWeakObjectPtr<UHarkenListenerComponent>& Item)
    {
        return !Item.IsValid() || Item.Get() == Listener;
    });
    if (!Listeners.IsEmpty())
    {
        UE_LOG(LogHarken, Warning, TEXT("A Harken listener is already active; the newest listener will be used."));
    }
    Listeners.Add(Listener);
}

void UHarkenEngineSubsystem::UnregisterListener(UHarkenListenerComponent* Listener)
{
    Listeners.RemoveAll([Listener](const TWeakObjectPtr<UHarkenListenerComponent>& Item)
    {
        return !Item.IsValid() || Item.Get() == Listener;
    });
}

bool UHarkenEngineSubsystem::Tick(float)
{
    if (IsHarkenInitialized())
    {
        while (!Listeners.IsEmpty() && !Listeners.Last().IsValid())
        {
            Listeners.Pop();
        }
        if (!Listeners.IsEmpty())
        {
            const UHarkenListenerComponent* Listener = Listeners.Last().Get();
            const FHarkenResult ListenerResult = SetListenerTransform(Listener->GetComponentTransform());
            if (!ListenerResult.bSuccess && ListenerResult.Code != VN_BACKPRESSURE)
            {
                UE_LOG(LogHarken, Warning, TEXT("%s"), *ListenerResult.Message);
            }
        }

        const FHarkenResult UpdateResult = FHarkenCoreRuntime::Get().Update();
        if (!UpdateResult.bSuccess && UpdateResult.Code != VN_BACKPRESSURE)
        {
            UE_LOG(LogHarken, Error, TEXT("%s"), *UpdateResult.Message);
        }
    }
    return true;
}

void UHarkenEngineSubsystem::EnterBackground()
{
    if (GetDefault<UHarkenSettings>()->bSuspendWhenBackgrounded && IsHarkenInitialized())
    {
        const FHarkenResult Result = SuspendHarken();
        if (!Result.bSuccess)
        {
            UE_LOG(LogHarken, Warning, TEXT("%s"), *Result.Message);
        }
    }
}

void UHarkenEngineSubsystem::EnterForeground()
{
    if (GetDefault<UHarkenSettings>()->bSuspendWhenBackgrounded && IsHarkenInitialized())
    {
        const FHarkenResult Result = ResumeHarken();
        if (!Result.bSuccess)
        {
            UE_LOG(LogHarken, Warning, TEXT("%s"), *Result.Message);
        }
    }
}

void UHarkenEngineSubsystem::LoadStartupContent()
{
    if (bStartupContentLoaded || !IsHarkenInitialized())
    {
        return;
    }

    const FString ManifestPath = FPaths::Combine(
        FPaths::ProjectContentDir(), TEXT("Harken/HarkenContent.json"));
    if (FPaths::FileExists(ManifestPath))
    {
        const FHarkenResult Result = LoadContentManifest();
        bStartupContentLoaded = Result.bSuccess;
        if (!Result.bSuccess)
        {
            UE_LOG(LogHarken, Error, TEXT("%s"), *Result.Message);
        }
        return;
    }

    const UHarkenSettings* Settings = GetDefault<UHarkenSettings>();
    TArray<FHarkenPackHandle> StagedPacks;
    for (const FString& PackPath : Settings->StartupPackPaths)
    {
        if (PackPath.IsEmpty())
        {
            continue;
        }
        FHarkenPackHandle Pack;
        const FHarkenResult Result = LoadPack(PackPath, Pack);
        if (!Result.bSuccess)
        {
            for (int32 Index = StagedPacks.Num() - 1; Index >= 0; --Index)
            {
                UnloadPack(StagedPacks[Index]);
            }
            UE_LOG(LogHarken, Error, TEXT("%s"), *Result.Message);
            return;
        }
        StagedPacks.Add(Pack);
    }
    if (!Settings->StartupCurrentWorldPath.IsEmpty())
    {
        UE_LOG(LogHarken, Warning,
               TEXT("StartupCurrentWorldPath is a preview envelope; games load Pack files. Ignoring %s"),
               *Settings->StartupCurrentWorldPath);
    }
    LoadedPacks.Append(StagedPacks);
    bStartupContentLoaded = true;
}

FHarkenResult UHarkenEngineSubsystem::LoadContentManifest()
{
    const FString ManifestPath = FPaths::Combine(
        FPaths::ProjectContentDir(), TEXT("Harken/HarkenContent.json"));
    FString Text;
    if (!FFileHelper::LoadFileToString(Text, *ManifestPath))
    {
        return FHarkenCoreRuntime::MakeResult(
            VN_CURRENT_WORLD_IO_ERROR, TEXT("Runtime content manifest cannot be read"));
    }
    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        return FHarkenCoreRuntime::MakeResult(
            VN_BAD_PACK, TEXT("Runtime content manifest is invalid"));
    }
    double Schema = 0;
    FString PlatformId;
    FString PlatformName;
    FString BasePlatform;
    FString BundleId;
    FGuid ParsedPlatformId;
    const TArray<TSharedPtr<FJsonValue>>* Packs = nullptr;
    if (!Root->TryGetNumberField(TEXT("schema"), Schema) || Schema != 1
        || Root->HasField(TEXT("platform"))
        || !Root->TryGetStringField(TEXT("platformId"), PlatformId)
        || !FGuid::ParseExact(PlatformId, EGuidFormats::DigitsWithHyphensLower, ParsedPlatformId)
        || !ParsedPlatformId.IsValid()
        || !Root->TryGetStringField(TEXT("platformName"), PlatformName) || PlatformName.IsEmpty()
        || !Root->TryGetStringField(TEXT("basePlatform"), BasePlatform)
        || BasePlatform != TEXT("Windows")
        || !Root->TryGetStringField(TEXT("bundleId"), BundleId) || BundleId.Len() != 64
        || BundleId.Contains(TEXT("/")) || BundleId.Contains(TEXT("\\")) || BundleId.Contains(TEXT(".."))
        || !Root->TryGetArrayField(TEXT("orderedPacks"), Packs))
    {
        return FHarkenCoreRuntime::MakeResult(
            VN_BAD_PACK, TEXT("Runtime content manifest contract is unsupported"));
    }

    TArray<FHarkenPackHandle> StagedPacks;
    for (const TSharedPtr<FJsonValue>& Value : *Packs)
    {
        const TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr;
        FString Path;
        FString PackPlatformId;
        if (!Row.IsValid() || !Row->TryGetStringField(TEXT("path"), Path)
            || !Row->TryGetStringField(TEXT("platformId"), PackPlatformId) || PackPlatformId != PlatformId
            || !Path.StartsWith(TEXT("Bundles/") + BundleId + TEXT("/"))
            || Path.Contains(TEXT("\\")) || Path.Contains(TEXT(":"))
            || Path.IsEmpty() || Path.Contains(TEXT("..")))
        {
            for (int32 Index = StagedPacks.Num() - 1; Index >= 0; --Index)
            {
                UnloadPack(StagedPacks[Index]);
            }
            return FHarkenCoreRuntime::MakeResult(
                VN_BAD_PACK, TEXT("Runtime content Pack path is invalid"));
        }
        FHarkenPackHandle Pack;
        const FHarkenResult Result = LoadPack(
            FPaths::Combine(TEXT("Harken"), Path), Pack);
        if (!Result.bSuccess)
        {
            for (int32 Index = StagedPacks.Num() - 1; Index >= 0; --Index)
            {
                UnloadPack(StagedPacks[Index]);
            }
            return Result;
        }
        StagedPacks.Add(Pack);
    }

    FString CurrentWorld;
    if (Root->TryGetStringField(TEXT("currentWorld"), CurrentWorld)
        && !CurrentWorld.IsEmpty())
    {
        UE_LOG(LogHarken, Log,
               TEXT("Ignoring preview envelope %s; games load Pack files"),
               *CurrentWorld);
    }
    LoadedPacks.Append(StagedPacks);
    return FHarkenCoreRuntime::MakeResult(VN_OK, TEXT(""));
}

void UHarkenEngineSubsystem::ClearLoadedPacks()
{
    for (int32 Index = LoadedPacks.Num() - 1; Index >= 0; --Index)
    {
        FHarkenPackHandle Pack = LoadedPacks[Index];
        FHarkenCoreRuntime::Get().UnloadPack(Pack, SessionGeneration);
    }
    LoadedPacks.Reset();
}

FString UHarkenEngineSubsystem::ResolveContentPath(const FString& Path) const
{
    return FPaths::ConvertRelativePathToFull(
        FPaths::IsRelative(Path) ? FPaths::Combine(FPaths::ProjectContentDir(), Path) : Path);
}
