#include "HarkenCoreRuntime.h"
#include "HarkenObjectId.h"

#include "Generated/HarkenVariables.g.h"
#include "Containers/UnrealString.h"
#include "Misc/App.h"
#include "HarkenCoordinates.h"
#include "HarkenRuntime.h"
#include "HarkenSettings.h"

namespace
{
void CopyQueryResult(const VnVariableQueryResultV1& Native,
                     FHarkenVariableQueryResult& Result)
{
    Result.Kind = static_cast<int32>(Native.kind);
    Result.Scope = static_cast<int32>(Native.scope);
    Result.Writers = static_cast<int32>(Native.writers);
    Result.RangeMin = Native.rangeMin;
    Result.RangeMax = Native.rangeMax;
    Result.DefaultValue = Native.defaultValue;
    Result.CurrentValue = Native.currentValue;
    Result.HasEmitterOverride = static_cast<int32>(Native.hasEmitterOverride);
    Result.PendingCount = static_cast<int32>(Native.pendingCount);
    Result.ExecutionFingerprint = static_cast<int64>(Native.executionFingerprint);
}
}

namespace
{
FString ResultName(VnResult Result)
{
    switch (Result)
    {
    case VN_OK: return TEXT("OK");
    case VN_INVALID_ARG: return TEXT("Invalid argument");
    case VN_INVALID_CONFIG: return TEXT("Invalid configuration");
    case VN_UNKNOWN_ID: return TEXT("Unknown ID");
    case VN_STALE_HANDLE: return TEXT("Stale handle");
    case VN_NO_VOICE: return TEXT("Voice capacity exhausted");
    case VN_BACKPRESSURE: return TEXT("Backpressure");
    case VN_IN_USE: return TEXT("Resource is in use");
    case VN_NOT_INITIALIZED: return TEXT("HarkenCore is not initialized");
    case VN_ALREADY_INITIALIZED: return TEXT("HarkenCore is already initialized");
    case VN_DEVICE_ERROR: return TEXT("Audio device error");
    case VN_NO_PACK: return TEXT("Pack capacity exhausted");
    case VN_BAD_PACK: return TEXT("Invalid pack");
    case VN_NO_ACTION: return TEXT("Action firing capacity exhausted");
    case VN_NO_EMITTER: return TEXT("Emitter capacity exhausted");
    case VN_CAPABILITY_UNAVAILABLE: return TEXT("Capability is unavailable");
    case VN_RUNTIME_RECREATE_REQUIRED: return TEXT("Runtime recreation is required");
    case VN_CURRENT_WORLD_CAPACITY: return TEXT("Current World capacity exceeded");
    case VN_CURRENT_WORLD_IO_ERROR: return TEXT("Current World I/O error");
    case VN_BAD_CURRENT_WORLD: return TEXT("Invalid Current World file");
    case VN_VARIABLE_CATALOG_MISMATCH: return TEXT("Variable catalog mismatch");
    case VN_NO_VARIABLE_LABEL: return TEXT("Variable label not found");
    default: return FString::Printf(TEXT("HarkenCore error %d"), Result);
    }
}

FHarkenResult MakeCommunicationResult(VnResult Result, const TCHAR* Context)
{
    FHarkenResult Value = FHarkenCoreRuntime::MakeResult(Result, Context);
    if (Result == VN_DEVICE_ERROR)
    {
        Value.Message = FString::Printf(TEXT("%s: Communication service error (%d)"),
                                        Context, Result);
    }
    return Value;
}

VnVec3 ToNativeVector(const FVector& Vector)
{
    return VnVec3{
        static_cast<float>(Vector.X),
        static_cast<float>(Vector.Y),
        static_cast<float>(Vector.Z)
    };
}
}

FHarkenCoreRuntime& FHarkenCoreRuntime::Get()
{
    static FHarkenCoreRuntime Runtime;
    return Runtime;
}

void FHarkenCoreRuntime::SetLibraryReady(bool bReady)
{
    bLibraryReady = bReady;
}

bool FHarkenCoreRuntime::IsInitialized() const
{
    return bInitialized;
}

FHarkenResult FHarkenCoreRuntime::Initialize(const UHarkenSettings& Settings)
{
    FHarkenResult ThreadResult;
    if (!CheckGameThread(ThreadResult))
    {
        return ThreadResult;
    }
    if (!bLibraryReady)
    {
        return MakeResult(VN_NOT_INITIALIZED, TEXT("HarkenCore library is unavailable"));
    }
    if (bInitialized)
    {
        return MakeResult(VN_OK);
    }

    VnConfig Config{};
    Config.structSize = sizeof(Config);
    Config.sampleRate = static_cast<uint32>(FMath::Max(Settings.SampleRate, 0));
    Config.blockFrames = static_cast<uint32>(FMath::Max(Settings.BlockFrames, 0));
    Config.deviceMode = Settings.DeviceMode == EHarkenDeviceMode::Null ? VN_DEVICE_NULL : VN_DEVICE_REAL;
    Config.maxEmitters = static_cast<uint32>(FMath::Max(Settings.MaxEmitters, 0));
    Config.emitterVariableSlots =
        static_cast<uint32>(FMath::Max(Settings.EmitterVariableSlots, 0));
    Config.logCallback = &FHarkenCoreRuntime::NativeLog;

    const VnResult Result = Vn_Init(&Config);
    bInitialized = Result == VN_OK;
    if (bInitialized)
    {
        LastRemoteResult = MakeResult(VN_OK);
        if (Settings.IsCommunicationEnabled())
        {
            const FHarkenResult RemoteResult = RetryRemoteCommunication(Settings);
            if (!RemoteResult.bSuccess)
            {
                UE_LOG(LogHarken, Warning, TEXT("%s"), *RemoteResult.Message);
            }
        }
    }
    return MakeResult(Result, TEXT("Initialize"));
}

FHarkenResult FHarkenCoreRuntime::RetryRemoteCommunication(const UHarkenSettings& Settings)
{
    if (!CheckGameThread(LastRemoteResult))
    {
        return LastRemoteResult;
    }
    if (!bInitialized)
    {
        LastRemoteResult = MakeResult(VN_NOT_INITIALIZED, TEXT("Start remote communication"));
        return LastRemoteResult;
    }
    if (!Settings.IsCommunicationEnabled())
    {
        LastRemoteResult = MakeResult(VN_CAPABILITY_UNAVAILABLE, TEXT("Communication is disabled for this build"));
        return LastRemoteResult;
    }
    if (Settings.RemotePort < 0 || Settings.RemotePort > 65535 ||
        Settings.RemoteDiscoveryPort < 0 || Settings.RemoteDiscoveryPort > 65535)
    {
        LastRemoteResult = MakeResult(VN_INVALID_ARG, TEXT("Start remote communication"));
        return LastRemoteResult;
    }
    const FString ApplicationName = Settings.RemoteApplicationName.IsEmpty()
        ? FString(FApp::GetProjectName()) : Settings.RemoteApplicationName;
    const FTCHARToUTF8 BindAddress(*Settings.RemoteBindAddress);
    const FTCHARToUTF8 Application(*ApplicationName);
    const FTCHARToUTF8 ProjectName(FApp::GetProjectName());
    VnRemoteConfig Config{};
    Config.structSize = sizeof(Config);
    Config.tcpPort = static_cast<uint32>(Settings.RemotePort);
    Config.discoveryPort = static_cast<uint32>(Settings.RemoteDiscoveryPort);
    Config.flags = Settings.bEnableRemoteDiscovery ? VN_REMOTE_DISCOVERY_ENABLED : 0u;
    Config.bindAddress = BindAddress.Get();
    Config.applicationName = Application.Get();
    Config.projectName = ProjectName.Get();
    LastRemoteResult = MakeCommunicationResult(Vn_RemoteStart(&Config), TEXT("Start remote communication"));
    return LastRemoteResult;
}

FHarkenResult FHarkenCoreRuntime::QueryRemoteCommunication(FHarkenRemoteStatus& Status) const
{
    Status = FHarkenRemoteStatus{};
    FHarkenResult ThreadResult;
    if (!CheckGameThread(ThreadResult))
    {
        return ThreadResult;
    }
    if (!bLibraryReady)
    {
        return MakeResult(VN_NOT_INITIALIZED, TEXT("Query remote communication"));
    }
    VnRemoteStatus Native{};
    Native.structSize = sizeof(Native);
    const VnResult Result = Vn_RemoteGetStatus(&Native);
    if (Result == VN_OK)
    {
        Status.State = static_cast<EHarkenRemoteState>(Native.state);
        Status.LastResult = MakeCommunicationResult(Native.lastResult, TEXT("Remote communication"));
        Status.BindAddress = UTF8_TO_TCHAR(Native.bindAddress);
        Status.Port = static_cast<int32>(Native.tcpPort);
        Status.DiscoveryPort = static_cast<int32>(Native.discoveryPort);
        Status.ProcessId = static_cast<int32>(Native.processId);
        Status.InstanceId = LexToString(Native.instanceId);
        Status.SessionId = LexToString(Native.sessionId);
        Status.ClientCount = static_cast<int32>(Native.clientCount);
        Status.ProtocolMajor = static_cast<int32>(Native.protocolMajor);
        Status.ProtocolMinor = static_cast<int32>(Native.protocolMinor);
        Status.ApplicationName = UTF8_TO_TCHAR(Native.applicationName);
        Status.ProjectName = UTF8_TO_TCHAR(Native.projectName);
        Status.EngineVersion = UTF8_TO_TCHAR(Native.engineVersion);
        Status.BuildId = UTF8_TO_TCHAR(Native.buildId);
        Status.Platform = UTF8_TO_TCHAR(Native.platform);
    }
    return MakeCommunicationResult(Result, TEXT("Query remote communication"));
}

FHarkenResult FHarkenCoreRuntime::GetRemoteCommunicationResult() const
{
    return LastRemoteResult;
}

FHarkenResult FHarkenCoreRuntime::Shutdown()
{
    if (!bInitialized)
    {
        return MakeResult(VN_OK);
    }
    const VnResult Result = Vn_Term();
    bInitialized = false;
    return MakeResult(Result, TEXT("Shutdown"));
}

FHarkenResult FHarkenCoreRuntime::Update()
{
    if (!bInitialized)
    {
        return MakeResult(VN_OK);
    }
    return MakeResult(Vn_Update(), TEXT("Update"));
}

FHarkenResult FHarkenCoreRuntime::Suspend()
{
    return MakeResult(bInitialized ? Vn_Suspend() : VN_NOT_INITIALIZED, TEXT("Suspend"));
}

FHarkenResult FHarkenCoreRuntime::Resume()
{
    return MakeResult(bInitialized ? Vn_Resume() : VN_NOT_INITIALIZED, TEXT("Resume"));
}

FHarkenResult FHarkenCoreRuntime::LoadCurrentWorld(const FString& Path, FHarkenCurrentWorldLoadReport& Report)
{
    Report = FHarkenCurrentWorldLoadReport{};
    if (!bInitialized)
    {
        return MakeResult(VN_NOT_INITIALIZED, TEXT("Load Current World"));
    }

    const FTCHARToUTF8 Utf8Path(*Path);
    VnCurrentWorldLoadDesc Desc{};
    Desc.structSize = sizeof(Desc);
    Desc.utf8Path = Utf8Path.Get();
    VnCurrentWorldLoadReport NativeReport{};
    NativeReport.structSize = sizeof(NativeReport);
    const VnResult Result = Vn_LoadCurrentWorld(&Desc, &NativeReport);

    Report.Status = static_cast<int32>(NativeReport.status);
    Report.ResultCode = NativeReport.result;
    Report.PriorGeneration = static_cast<int64>(NativeReport.priorGeneration);
    Report.ResultGeneration = static_cast<int64>(NativeReport.resultGeneration);
    Report.RuntimeSemanticFingerprint = static_cast<int64>(NativeReport.runtimeSemanticFingerprint);
    Report.CapabilityMask = static_cast<int64>(NativeReport.capabilityMask);
    Report.BodyBytes = static_cast<int64>(NativeReport.bodyBytes);
    Report.BodyChecksum = static_cast<int64>(NativeReport.bodyChecksum);
    return MakeResult(Result, TEXT("Load Current World"));
}

FHarkenResult FHarkenCoreRuntime::LoadPack(const FString& Path, int64 SessionGeneration,
                                         FHarkenPackHandle& Pack)
{
    Pack = FHarkenPackHandle{};
    if (!bInitialized)
    {
        return MakeResult(VN_NOT_INITIALIZED, TEXT("Load pack"));
    }

    const FTCHARToUTF8 Utf8Path(*Path);
    VnPackDesc Desc{};
    Desc.structSize = sizeof(Desc);
    Desc.utf8Path = Utf8Path.Get();
    VnPackHandle NativeHandle = 0;
    const VnResult Result = Vn_LoadPack(&Desc, &NativeHandle);
    if (Result == VN_OK)
    {
        Pack.Value = static_cast<int64>(NativeHandle);
        Pack.SessionGeneration = SessionGeneration;
    }
    return MakeResult(Result, TEXT("Load pack"));
}

FHarkenResult FHarkenCoreRuntime::UnloadPack(FHarkenPackHandle& Pack, int64 SessionGeneration)
{
    if (!bInitialized)
    {
        return MakeResult(VN_NOT_INITIALIZED, TEXT("Unload pack"));
    }
    if (!SessionMatches(Pack.SessionGeneration, SessionGeneration))
    {
        return MakeResult(VN_STALE_HANDLE, TEXT("Unload pack"));
    }
    const VnResult Result = Vn_UnloadPack(static_cast<VnPackHandle>(Pack.Value));
    if (Result == VN_OK || Result == VN_STALE_HANDLE)
    {
        Pack = FHarkenPackHandle{};
    }
    return MakeResult(Result, TEXT("Unload pack"));
}

FHarkenResult FHarkenCoreRuntime::PostAction(FGuid ActionId, FHarkenEmitterHandle Emitter,
                                           int64 SessionGeneration, int64 CallerSeed,
                                           FHarkenActionFiringHandle& Firing)
{
    Firing = FHarkenActionFiringHandle{};
    FHarkenResult ThreadResult;
    if (!CheckGameThread(ThreadResult)) return ThreadResult;
    if (!bInitialized)
    {
        return MakeResult(VN_NOT_INITIALIZED, TEXT("Post Action"));
    }
    if (!ActionId.IsValid())
    {
        return MakeResult(VN_INVALID_ARG, TEXT("Post Action"));
    }
    if ((Emitter.Value != 0 || Emitter.SessionGeneration != 0) &&
        (!Emitter.IsValid() || !SessionMatches(Emitter.SessionGeneration, SessionGeneration)))
    {
        return MakeResult(VN_STALE_HANDLE, TEXT("Post Action"));
    }

    VnPostActionDesc Desc{};
    Desc.structSize = sizeof(Desc);
    Desc.flags = 0;
    Desc.actionId = ToHarkenObjectId(ActionId);
    Desc.emitter = static_cast<VnEmitterHandle>(Emitter.Value);
    Desc.callerSeed = static_cast<uint64>(CallerSeed);
    Desc.reserved0 = 0;
    VnActionFiringHandle NativeHandle = 0;
    const VnResult Result = Vn_PostAction(&Desc, &NativeHandle);
    if (Result == VN_OK)
    {
        Firing.Value = static_cast<int64>(NativeHandle);
        Firing.SessionGeneration = SessionGeneration;
    }
    return MakeResult(Result, TEXT("Post Action"));
}

FHarkenResult FHarkenCoreRuntime::PostAction(const FString& ActionName,
    FHarkenEmitterHandle Emitter, int64 SessionGeneration, int64 CallerSeed,
    FHarkenActionFiringHandle& Firing)
{
    Firing = FHarkenActionFiringHandle{};
    FHarkenResult ThreadResult;
    if (!CheckGameThread(ThreadResult)) return ThreadResult;
    if (!bInitialized) return MakeResult(VN_NOT_INITIALIZED, TEXT("Post Action"));
    if ((Emitter.Value != 0 || Emitter.SessionGeneration != 0) &&
        (!Emitter.IsValid() || !SessionMatches(Emitter.SessionGeneration, SessionGeneration)))
        return MakeResult(VN_STALE_HANDLE, TEXT("Post Action"));
    FTCHARToUTF8 Name(*ActionName);
    if (Name.Length() != ActionName.Len())
        return MakeResult(VN_INVALID_ARG, TEXT("Post Action"));
    VnPostActionByNameDesc Desc{};
    Desc.structSize = sizeof(Desc);
    Desc.utf8Name = Name.Get();
    Desc.emitter = static_cast<VnEmitterHandle>(Emitter.Value);
    Desc.callerSeed = static_cast<uint64>(CallerSeed);
    VnActionFiringHandle NativeHandle = 0;
    const VnResult Result = Vn_PostActionByName(&Desc, &NativeHandle);
    if (Result == VN_OK)
    {
        Firing.Value = static_cast<int64>(NativeHandle);
        Firing.SessionGeneration = SessionGeneration;
    }
    return MakeResult(Result, TEXT("Post Action"));
}

FHarkenResult FHarkenCoreRuntime::StopActionFiring(FHarkenActionFiringHandle Firing,
                                                  int64 SessionGeneration,
                                                  int32 FadeMilliseconds)
{
    if (!bInitialized)
    {
        return MakeResult(VN_NOT_INITIALIZED, TEXT("Stop Action firing"));
    }
    if (!SessionMatches(Firing.SessionGeneration, SessionGeneration))
    {
        return MakeResult(VN_STALE_HANDLE, TEXT("Stop Action firing"));
    }
    const uint32 Fade = static_cast<uint32>(FMath::Max(FadeMilliseconds, 0));
    return MakeResult(Vn_StopActionFiring(static_cast<VnActionFiringHandle>(Firing.Value), Fade),
                      TEXT("Stop Action firing"));
}

FHarkenResult FHarkenCoreRuntime::QueryActionFiring(FHarkenActionFiringHandle Firing,
                                                   int64 SessionGeneration,
                                                   FHarkenActionFiringInfo& Info)
{
    Info = FHarkenActionFiringInfo{};
    if (!bInitialized)
    {
        return MakeResult(VN_NOT_INITIALIZED, TEXT("Query Action firing"));
    }
    if (!SessionMatches(Firing.SessionGeneration, SessionGeneration))
    {
        return MakeResult(VN_STALE_HANDLE, TEXT("Query Action firing"));
    }
    VnActionFiringInfo Native{};
    Native.structSize = sizeof(Native);
    const VnResult Result = Vn_QueryActionFiring(
        static_cast<VnActionFiringHandle>(Firing.Value), &Native);
    if (Result == VN_OK)
    {
        Info.State = Native.state;
        Info.Outcome = Native.outcome;
        Info.TerminalReason = Native.terminalReason;
        Info.ActionId = FromHarkenObjectId(Native.actionId);
        Info.WorldGeneration = static_cast<int64>(Native.worldGeneration);
        Info.DirectStepCount = static_cast<int32>(Native.directStepCount);
        Info.DirectStepTerminalCount = static_cast<int32>(Native.directStepTerminalCount);
        Info.ChildFiringCount = static_cast<int32>(Native.childFiringCount);
        Info.LivePlayableCount = static_cast<int32>(Native.livePlayableCount);
    }
    return MakeResult(Result, TEXT("Query Action firing"));
}

FHarkenResult FHarkenCoreRuntime::QueryActionStep(FHarkenActionFiringHandle Firing,
                                                 int64 SessionGeneration,
                                                 int32 AuthoredEventIndex,
                                                 FHarkenActionStepResult& Step)
{
    Step = FHarkenActionStepResult{};
    if (!bInitialized)
    {
        return MakeResult(VN_NOT_INITIALIZED, TEXT("Query Action step"));
    }
    if (!SessionMatches(Firing.SessionGeneration, SessionGeneration))
    {
        return MakeResult(VN_STALE_HANDLE, TEXT("Query Action step"));
    }
    if (AuthoredEventIndex < 0)
    {
        return MakeResult(VN_INVALID_ARG, TEXT("Query Action step"));
    }
    VnActionStepResult Native{};
    Native.structSize = sizeof(Native);
    const VnResult Result = Vn_QueryActionStep(
        static_cast<VnActionFiringHandle>(Firing.Value),
        static_cast<uint32>(AuthoredEventIndex), &Native);
    if (Result == VN_OK)
    {
        Step.State = Native.state;
        Step.ResultCode = Native.result;
        Step.Opcode = Native.opcode;
        Step.OwnerActionId = FromHarkenObjectId(Native.ownerActionId);
        Step.TargetStableId = FromHarkenObjectId(Native.targetStableId);
        Step.AuthoredEventIndex = static_cast<int32>(Native.authoredEventIndex);
        Step.ExecutionOrdinal = static_cast<int32>(Native.executionOrdinal);
        Step.Depth = static_cast<int32>(Native.depth);
        Step.TargetKind = static_cast<int32>(Native.targetKind);
    }
    return MakeResult(Result, TEXT("Query Action step"));
}

FHarkenResult FHarkenCoreRuntime::ReleaseActionFiring(FHarkenActionFiringHandle& Firing,
                                                     int64 SessionGeneration)
{
    if (!bInitialized)
    {
        return MakeResult(VN_NOT_INITIALIZED, TEXT("Release Action firing"));
    }
    if (!SessionMatches(Firing.SessionGeneration, SessionGeneration))
    {
        return MakeResult(VN_STALE_HANDLE, TEXT("Release Action firing"));
    }
    const VnResult Result = Vn_ReleaseActionFiring(
        static_cast<VnActionFiringHandle>(Firing.Value));
    if (Result == VN_OK || Result == VN_STALE_HANDLE)
    {
        Firing = FHarkenActionFiringHandle{};
    }
    return MakeResult(Result, TEXT("Release Action firing"));
}

FHarkenResult FHarkenCoreRuntime::StopAll(int32 FadeMilliseconds)
{
    const uint32 Fade = static_cast<uint32>(FMath::Max(FadeMilliseconds, 0));
    return MakeResult(bInitialized ? Vn_StopAll(Fade) : VN_NOT_INITIALIZED, TEXT("Stop all"));
}

FHarkenResult FHarkenCoreRuntime::CreateEmitter(int64 SessionGeneration,
                                              FHarkenEmitterHandle& Emitter)
{
    Emitter = FHarkenEmitterHandle{};
    if (!bInitialized)
    {
        return MakeResult(VN_NOT_INITIALIZED, TEXT("Create emitter"));
    }
    VnEmitterHandle NativeHandle = 0;
    const VnResult Result = Vn_CreateEmitter(&NativeHandle);
    if (Result == VN_OK)
    {
        Emitter.Value = static_cast<int64>(NativeHandle);
        Emitter.SessionGeneration = SessionGeneration;
    }
    return MakeResult(Result, TEXT("Create emitter"));
}

FHarkenResult FHarkenCoreRuntime::DestroyEmitter(FHarkenEmitterHandle& Emitter,
                                               int64 SessionGeneration)
{
    if (!bInitialized)
    {
        return MakeResult(VN_NOT_INITIALIZED, TEXT("Destroy emitter"));
    }
    if (!SessionMatches(Emitter.SessionGeneration, SessionGeneration))
    {
        return MakeResult(VN_STALE_HANDLE, TEXT("Destroy emitter"));
    }
    const VnResult Result = Vn_DestroyEmitter(static_cast<VnEmitterHandle>(Emitter.Value));
    if (Result == VN_OK || Result == VN_STALE_HANDLE)
    {
        Emitter = FHarkenEmitterHandle{};
    }
    return MakeResult(Result, TEXT("Destroy emitter"));
}

FHarkenResult FHarkenCoreRuntime::SetEmitterTransform(FHarkenEmitterHandle Emitter,
                                                     int64 SessionGeneration,
                                                     const FTransform& Transform)
{
    if (!bInitialized)
    {
        return MakeResult(VN_NOT_INITIALIZED, TEXT("Set emitter transform"));
    }
    if (!SessionMatches(Emitter.SessionGeneration, SessionGeneration))
    {
        return MakeResult(VN_STALE_HANDLE, TEXT("Set emitter transform"));
    }
    const VnEmitterHandle Handle = static_cast<VnEmitterHandle>(Emitter.Value);
    const VnVec3 Position = ToNativeVector(HarkenCoordinates::ToHarkenPosition(Transform.GetLocation()));
    const VnVec3 Forward = ToNativeVector(HarkenCoordinates::ToHarkenDirection(Transform.GetUnitAxis(EAxis::X)));
    const VnResult PositionResult = Vn_SetEmitterPosition(Handle, Position);
    if (PositionResult != VN_OK)
    {
        return MakeResult(PositionResult, TEXT("Set emitter position"));
    }
    return MakeResult(Vn_SetEmitterOrientation(Handle, Forward), TEXT("Set emitter orientation"));
}

FHarkenResult FHarkenCoreRuntime::StopEmitter(FHarkenEmitterHandle Emitter,
                                             int64 SessionGeneration,
                                             int32 FadeMilliseconds)
{
    if (!bInitialized)
    {
        return MakeResult(VN_NOT_INITIALIZED, TEXT("Stop emitter"));
    }
    if (!SessionMatches(Emitter.SessionGeneration, SessionGeneration))
    {
        return MakeResult(VN_STALE_HANDLE, TEXT("Stop emitter"));
    }
    const uint32 Fade = static_cast<uint32>(FMath::Max(FadeMilliseconds, 0));
    return MakeResult(Vn_StopEmitter(static_cast<VnEmitterHandle>(Emitter.Value), Fade),
                      TEXT("Stop emitter"));
}

FHarkenResult FHarkenCoreRuntime::SetValue(FGuid VariableId, float Value,
                                         FHarkenEmitterHandle Emitter,
                                         int64 SessionGeneration)
{
    FHarkenResult ThreadResult;
    if (!CheckGameThread(ThreadResult))
    {
        return ThreadResult;
    }
    if (!bInitialized)
    {
        return MakeResult(VN_NOT_INITIALIZED, TEXT("Set variable"));
    }
    const bool bEmitter = Emitter.Value != 0 || Emitter.SessionGeneration != 0;
    if (bEmitter && (!Emitter.IsValid() ||
        !SessionMatches(Emitter.SessionGeneration, SessionGeneration)))
    {
        return MakeResult(VN_STALE_HANDLE, TEXT("Set variable"));
    }
    if (!VariableId.IsValid())
    {
        return MakeResult(VN_INVALID_ARG, TEXT("Set variable"));
    }
    const VnResult Result = bEmitter
        ? Vn_SetEmitterValue(static_cast<VnEmitterHandle>(Emitter.Value),
                             ToHarkenObjectId(VariableId), Value)
        : Vn_SetValue(ToHarkenObjectId(VariableId), Value);
    return MakeResult(Result, TEXT("Set variable"));
}

FHarkenResult FHarkenCoreRuntime::SetValue(FHarkenVariableId Variable, FGuid LabelId, const FString* LabelName,
                                       FHarkenEmitterHandle Emitter, int64 SessionGeneration)
{
    FHarkenResult ThreadResult;
    if (!CheckGameThread(ThreadResult)) return ThreadResult;
    if (!bInitialized) return MakeResult(VN_NOT_INITIALIZED, TEXT("Set variable"));
    const bool bEmitter = Emitter.Value != 0 || Emitter.SessionGeneration != 0;
    if (bEmitter && (!Emitter.IsValid() ||
        !SessionMatches(Emitter.SessionGeneration, SessionGeneration)))
        return MakeResult(VN_STALE_HANDLE, TEXT("Set variable"));
    if (!Variable.IsAssigned() || (LabelName == nullptr && !LabelId.IsValid()))
        return MakeResult(VN_INVALID_ARG, TEXT("Set variable"));
    const FHarkenResult Guard = GuardTypedCatalog(TEXT("Set variable"));
    if (!Guard.bSuccess) return Guard;
    return SetValue(Variable.StableId, LabelId, LabelName, Emitter, SessionGeneration);
}

FHarkenResult FHarkenCoreRuntime::SetValue(FGuid VariableId, FGuid LabelId, const FString* LabelName,
                                       FHarkenEmitterHandle Emitter, int64 SessionGeneration)
{
    FHarkenResult ThreadResult;
    if (!CheckGameThread(ThreadResult)) return ThreadResult;
    if (!bInitialized) return MakeResult(VN_NOT_INITIALIZED, TEXT("Set variable"));
    const bool bEmitter = Emitter.Value != 0 || Emitter.SessionGeneration != 0;
    if (bEmitter && (!Emitter.IsValid() ||
        !SessionMatches(Emitter.SessionGeneration, SessionGeneration)))
        return MakeResult(VN_STALE_HANDLE, TEXT("Set variable"));
    if (!VariableId.IsValid() || (LabelName == nullptr && !LabelId.IsValid()))
        return MakeResult(VN_INVALID_ARG, TEXT("Set variable"));
    if (LabelName != nullptr) {
        for (int32 Index = 0; Index < LabelName->Len(); ++Index)
            if ((*LabelName)[Index] == 0) return MakeResult(VN_INVALID_ARG, TEXT("Set variable"));
        const FTCHARToUTF8 Utf8Name(**LabelName);
        return MakeResult(Vn_SetValueByLabelName(ToHarkenObjectId(VariableId), Utf8Name.Get(),
            static_cast<VnEmitterHandle>(Emitter.Value)), TEXT("Set variable"));
    }
    return MakeResult(Vn_SetValueByLabelId(ToHarkenObjectId(VariableId), ToHarkenObjectId(LabelId),
        static_cast<VnEmitterHandle>(Emitter.Value)), TEXT("Set variable"));
}

FHarkenResult FHarkenCoreRuntime::SetValue(const FString& VariableName, FGuid LabelId,
                                       const FString* LabelName, FHarkenEmitterHandle Emitter,
                                       int64 SessionGeneration)
{
    FHarkenResult ThreadResult;
    if (!CheckGameThread(ThreadResult)) return ThreadResult;
    if (!bInitialized) return MakeResult(VN_NOT_INITIALIZED, TEXT("Set variable"));
    if ((Emitter.Value != 0 || Emitter.SessionGeneration != 0) &&
        (!Emitter.IsValid() || !SessionMatches(Emitter.SessionGeneration, SessionGeneration)))
        return MakeResult(VN_STALE_HANDLE, TEXT("Set variable"));
    FGuid VariableId;
    const FHarkenResult Result = ResolveVariable(VariableName, VariableId);
    return Result.bSuccess ? SetValue(VariableId, LabelId, LabelName, Emitter, SessionGeneration) : Result;
}

FHarkenResult FHarkenCoreRuntime::ResolveVariable(const FString& VariableName, FGuid& VariableId)
{
    VariableId = FGuid{};
    FHarkenResult ThreadResult;
    if (!CheckGameThread(ThreadResult)) return ThreadResult;
    if (!bInitialized) return MakeResult(VN_NOT_INITIALIZED, TEXT("Resolve variable"));
    FTCHARToUTF8 Name(*VariableName);
    if (Name.Length() != VariableName.Len())
        return MakeResult(VN_INVALID_ARG, TEXT("Resolve variable"));
    VnPostActionByNameDesc Desc{};
    Desc.structSize = sizeof(Desc);
    Desc.utf8Name = Name.Get();
    VnVariableResolved Resolved{};
    Resolved.structSize = sizeof(Resolved);
    const VnResult Result = Vn_ResolveVariableByName(&Desc, &Resolved);
    if (Result == VN_OK) VariableId = FromHarkenObjectId(Resolved.variableId);
    return MakeResult(Result, TEXT("Resolve variable"));
}

FHarkenResult FHarkenCoreRuntime::SetValue(const FString& VariableName, float Value,
    FHarkenEmitterHandle Emitter, int64 SessionGeneration)
{
    FHarkenResult ThreadResult;
    if (!CheckGameThread(ThreadResult)) return ThreadResult;
    if (!bInitialized) return MakeResult(VN_NOT_INITIALIZED, TEXT("Set variable"));
    if ((Emitter.Value != 0 || Emitter.SessionGeneration != 0) &&
        (!Emitter.IsValid() || !SessionMatches(Emitter.SessionGeneration, SessionGeneration)))
        return MakeResult(VN_STALE_HANDLE, TEXT("Set variable"));
    FGuid VariableId;
    const FHarkenResult Result = ResolveVariable(VariableName, VariableId);
    return Result.bSuccess ? SetValue(VariableId, Value, Emitter, SessionGeneration) : Result;
}

FHarkenResult FHarkenCoreRuntime::QueryEmitterVariable(FHarkenEmitterHandle Emitter,
                                                      int64 SessionGeneration,
                                                      FHarkenVariableId Variable,
                                                      FHarkenVariableQueryResult& Result)
{
    FHarkenResult ThreadResult;
    if (!CheckGameThread(ThreadResult))
    {
        Result = FHarkenVariableQueryResult{};
        return ThreadResult;
    }
    if (!bInitialized)
    {
        Result = FHarkenVariableQueryResult{};
        return MakeResult(VN_NOT_INITIALIZED, TEXT("Query emitter variable"));
    }
    if (!SessionMatches(Emitter.SessionGeneration, SessionGeneration))
    {
        Result = FHarkenVariableQueryResult{};
        return MakeResult(VN_STALE_HANDLE, TEXT("Query emitter variable"));
    }
    return QueryTypedVariable(static_cast<VnEmitterHandle>(Emitter.Value),
                              Variable, Result, TEXT("Query emitter variable"));
}

FHarkenResult FHarkenCoreRuntime::SetValue(FHarkenVariableId Variable, float Value,
                                         FHarkenEmitterHandle Emitter,
                                         int64 SessionGeneration)
{
    FHarkenResult ThreadResult;
    if (!CheckGameThread(ThreadResult))
    {
        return ThreadResult;
    }
    if (!Variable.IsAssigned())
        return MakeResult(VN_INVALID_ARG, TEXT("Set variable"));
    const FHarkenResult Guard = GuardTypedCatalog(TEXT("Set variable"));
    if (!Guard.bSuccess) return Guard;
    return SetValue(Variable.StableId, Value, Emitter, SessionGeneration);
}

FHarkenResult FHarkenCoreRuntime::ResetValue(FHarkenVariableId Variable)
{
    FHarkenResult ThreadResult;
    if (!CheckGameThread(ThreadResult))
    {
        return ThreadResult;
    }
    if (!Variable.IsAssigned())
        return MakeResult(VN_INVALID_ARG, TEXT("Reset variable"));
    const FHarkenResult Guard = GuardTypedCatalog(TEXT("Reset variable"));
    if (!Guard.bSuccess) return Guard;
    VnVariableWriteV1 Write{};
    Write.variableId = ToHarkenObjectId(Variable.StableId);
    VnVariableBatchDescV1 Desc{};
    Desc.structSize = sizeof(Desc);
    Desc.writeCount = 1;
    Desc.writes = &Write;
    return MakeResult(Vn_ResetValues(&Desc, nullptr), TEXT("Reset variable"));
}

FHarkenResult FHarkenCoreRuntime::QueryVariable(FHarkenVariableId Variable,
                                               FHarkenVariableQueryResult& Result)
{
    FHarkenResult ThreadResult;
    if (!CheckGameThread(ThreadResult))
    {
        Result = FHarkenVariableQueryResult{};
        return ThreadResult;
    }
    return QueryTypedVariable(0, Variable, Result, TEXT("Query variable"));
}

FHarkenResult FHarkenCoreRuntime::QueryVariableTicket(const FHarkenVariableTicket& Ticket,
                                                     FHarkenVariableTicketQueryResult& Result)
{
    Result = FHarkenVariableTicketQueryResult{};
    FHarkenResult ThreadResult;
    if (!CheckGameThread(ThreadResult))
    {
        return ThreadResult;
    }
    if (!bInitialized)
    {
        return MakeResult(VN_NOT_INITIALIZED, TEXT("Query variable ticket"));
    }
    const FHarkenResult Guard = GuardTypedCatalog(TEXT("Query variable ticket"));
    if (!Guard.bSuccess)
    {
        return Guard;
    }
    VnVariableTicketQueryDescV1 Desc{};
    Desc.structSize = sizeof(Desc);
    Desc.ticket.sequence = static_cast<uint64>(Ticket.Sequence);
    Desc.ticket.worldGeneration = static_cast<uint64>(Ticket.WorldGeneration);
    Desc.ticket.transportGeneration = static_cast<uint64>(Ticket.TransportGeneration);
    VnVariableTicketQueryResultV1 Native{};
    Native.structSize = sizeof(Native);
    const VnResult QueryResult = Vn_QueryVariableTicket(&Desc, &Native);
    if (QueryResult == VN_OK)
    {
        Result.Status = static_cast<int32>(Native.status);
        Result.Outcome = static_cast<int32>(Native.outcome);
    }
    return MakeResult(QueryResult, TEXT("Query variable ticket"));
}

FHarkenResult FHarkenCoreRuntime::SetListenerTransform(const FTransform& Transform)
{
    if (!bInitialized)
    {
        return MakeResult(VN_NOT_INITIALIZED, TEXT("Set listener"));
    }
    const VnVec3 Position = ToNativeVector(HarkenCoordinates::ToHarkenPosition(Transform.GetLocation()));
    const VnVec3 Forward = ToNativeVector(HarkenCoordinates::ToHarkenDirection(Transform.GetUnitAxis(EAxis::X)));
    const VnVec3 Up = ToNativeVector(HarkenCoordinates::ToHarkenDirection(Transform.GetUnitAxis(EAxis::Z)));
    return MakeResult(Vn_SetListener(Position, Forward, Up), TEXT("Set listener"));
}

FHarkenResult FHarkenCoreRuntime::MakeResult(VnResult Result, const FString& Context)
{
    FHarkenResult Value;
    Value.bSuccess = Result == VN_OK;
    Value.Code = Result;
    const FString Name = ResultName(Result);
    Value.Message = Context.IsEmpty() ? Name : FString::Printf(TEXT("%s: %s"), *Context, *Name);
    return Value;
}

FHarkenResult FHarkenCoreRuntime::GuardTypedCatalog(const TCHAR* Context)
{
    if (!bInitialized)
    {
        return MakeResult(VN_NOT_INITIALIZED, Context);
    }
    VnVariableCatalogInfoV1 Info{};
    Info.structSize = sizeof(Info);
    const VnResult Queried = Vn_QueryVariableCatalog(&Info);
    if (Queried != VN_OK)
    {
        return MakeResult(Queried, Context);
    }
    if (Info.executionCatalogFingerprint !=
        Harken::Generated::ExecutionCatalogFingerprint)
    {
        return MakeResult(VN_VARIABLE_CATALOG_MISMATCH, Context);
    }
    if (Info.sdkCatalogFingerprint != 0 &&
        Harken::Generated::CatalogFingerprint != 0 &&
        Info.sdkCatalogFingerprint != Harken::Generated::CatalogFingerprint)
    {
        UE_LOG(LogHarken, Warning,
               TEXT("Harken Variable SDK catalog is stale; write still proceeds."));
    }
    return MakeResult(VN_OK);
}

FHarkenResult FHarkenCoreRuntime::QueryTypedVariable(VnEmitterHandle Emitter,
                                                    FHarkenVariableId Variable,
                                                    FHarkenVariableQueryResult& Result,
                                                    const TCHAR* Context)
{
    Result = FHarkenVariableQueryResult{};
    if (!Variable.IsAssigned())
    {
        return MakeResult(VN_INVALID_ARG, Context);
    }
    const FHarkenResult Guard = GuardTypedCatalog(Context);
    if (!Guard.bSuccess)
    {
        return Guard;
    }
    VnVariableQueryDescV1 Desc{};
    Desc.structSize = sizeof(Desc);
    Desc.variableId = ToHarkenObjectId(Variable.StableId);
    Desc.emitter = Emitter;
    VnVariableQueryResultV1 Native{};
    Native.structSize = sizeof(Native);
    const VnResult QueryResult = Vn_QueryVariable(&Desc, &Native);
    if (QueryResult == VN_OK)
    {
        CopyQueryResult(Native, Result);
    }
    return MakeResult(QueryResult, Context);
}

void FHarkenCoreRuntime::NativeLog(void*, uint32 Level, uint32 Code, const char* Text)
{
    UE_LOG(LogHarken, Log, TEXT("Core[%u:%u] %s"), Level, Code,
           Text != nullptr ? UTF8_TO_TCHAR(Text) : TEXT(""));
}

bool FHarkenCoreRuntime::CheckGameThread(FHarkenResult& Result)
{
    if (IsInGameThread())
    {
        return true;
    }
    Result = MakeResult(VN_INVALID_ARG, TEXT("Harken calls must run on the game thread"));
    return false;
}

bool FHarkenCoreRuntime::SessionMatches(int64 HandleGeneration, int64 SessionGeneration)
{
    return HandleGeneration != 0 && HandleGeneration == SessionGeneration;
}
