#if WITH_DEV_AUTOMATION_TESTS

#include "Generated/HarkenVariables.g.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"
#include "HarkenCoreRuntime.h"
#include "HarkenObjectId.h"
#include "HarkenSettings.h"
#include "HarkenVariableDataAsset.h"
#include "HarkenVariableLibrary.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHarkenSettingsDefaultsTest,
                                 "Harken.Runtime.SettingsDefaults",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHarkenSettingsDefaultsTest::RunTest(const FString&)
{
    const UHarkenSettings* Settings = NewObject<UHarkenSettings>();
    TestTrue(TEXT("Remote communication defaults on in packaged builds"), Settings->bEnableCommunicationInBuilds);
    TestEqual(TEXT("Remote listener selects an available port"), Settings->RemotePort, 0);
#if PLATFORM_LINUX
    TestTrue(TEXT("Linux defaults to the Null device"),
             Settings->DeviceMode == EHarkenDeviceMode::Null);
#else
    TestTrue(TEXT("Supported hosts default to the Real device"),
             Settings->DeviceMode == EHarkenDeviceMode::Real);
#endif
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHarkenRemoteLifecycleTest,
                                 "Harken.Runtime.RemoteLifecycle",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHarkenRemoteLifecycleTest::RunTest(const FString&)
{
    FHarkenCoreRuntime& Runtime = FHarkenCoreRuntime::Get();
    Runtime.Shutdown();
    UHarkenSettings* Settings = NewObject<UHarkenSettings>();
    Settings->DeviceMode = EHarkenDeviceMode::Null;
    Settings->bEnableCommunicationInBuilds = false;
    Settings->bEnableRemoteDiscovery = false;
    Settings->RemoteBindAddress = TEXT("not-an-ip-address");
    Settings->RemoteApplicationName = TEXT("Remote lifecycle fixture");
#if WITH_EDITOR
    TestTrue(TEXT("Editor communication is independent of the build setting"), Settings->IsCommunicationEnabled());
#else
    TestFalse(TEXT("Packaged runtime honors the disabled setting"), Settings->IsCommunicationEnabled());
    TestTrue(TEXT("Disabled communication preserves audio initialization"), Runtime.Initialize(*Settings).bSuccess);
    FHarkenRemoteStatus DisabledStatus;
    TestTrue(TEXT("Read disabled status"), Runtime.QueryRemoteCommunication(DisabledStatus).bSuccess);
    TestTrue(TEXT("Disabled service stays stopped"), DisabledStatus.State == EHarkenRemoteState::Stopped);
    TestFalse(TEXT("Retry cannot bypass the build setting"), Runtime.RetryRemoteCommunication(*Settings).bSuccess);
    TestTrue(TEXT("Shutdown disabled runtime"), Runtime.Shutdown().bSuccess);
    Settings->bEnableCommunicationInBuilds = true;
#endif
    if (!TestTrue(TEXT("Communication refusal preserves audio initialization"), Runtime.Initialize(*Settings).bSuccess))
    {
        return false;
    }
    TestTrue(TEXT("Audio stays initialized"), Runtime.IsInitialized());
    TestFalse(TEXT("Separate communication failure is observable"), Runtime.GetRemoteCommunicationResult().bSuccess);
    TestTrue(TEXT("Audio pump survives communication failure"), Runtime.Update().bSuccess);
    Settings->RemoteBindAddress = TEXT("127.0.0.1");
    TestTrue(TEXT("Explicit communication retry"), Runtime.RetryRemoteCommunication(*Settings).bSuccess);
    FHarkenRemoteStatus Status;
    TestTrue(TEXT("Read actual listening status"), Runtime.QueryRemoteCommunication(Status).bSuccess);
    TestTrue(TEXT("Service is listening"), Status.State == EHarkenRemoteState::Listening);
    TestTrue(TEXT("Actual ephemeral port"), Status.Port > 0);
    TestTrue(TEXT("Engine version is available"), !Status.EngineVersion.IsEmpty());
    TestEqual(TEXT("Configured application identity is preserved"), Status.ApplicationName, Settings->RemoteApplicationName);
    TestTrue(TEXT("Project identity is available"), !Status.ProjectName.IsEmpty());
    TestEqual(TEXT("Project identity is independent of the application name"), Status.ProjectName, FString(FApp::GetProjectName()));
    TestTrue(TEXT("Engine shutdown closes communication"), Runtime.Shutdown().bSuccess);
    TestTrue(TEXT("Read stopped status"), Runtime.QueryRemoteCommunication(Status).bSuccess);
    TestTrue(TEXT("Service is stopped"), Status.State == EHarkenRemoteState::Stopped);
    Settings->RemoteApplicationName.Reset();
    TestTrue(TEXT("Engine restart restores communication automatically"), Runtime.Initialize(*Settings).bSuccess);
    TestTrue(TEXT("Read restarted status"), Runtime.QueryRemoteCommunication(Status).bSuccess);
    TestTrue(TEXT("Restarted service is listening"), Status.State == EHarkenRemoteState::Listening);
    TestEqual(TEXT("Default application identity uses the project name"), Status.ApplicationName, FString(FApp::GetProjectName()));
    TestEqual(TEXT("Project identity survives restart"), Status.ProjectName, FString(FApp::GetProjectName()));
    TestTrue(TEXT("Shutdown restarted runtime"), Runtime.Shutdown().bSuccess);
    TestTrue(TEXT("Read terminated status"), Runtime.QueryRemoteCommunication(Status).bSuccess);
    TestTrue(TEXT("Terminated service is stopped"), Status.State == EHarkenRemoteState::Stopped);
    TestEqual(TEXT("Terminated service has no endpoint"), Status.Port, 0);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHarkenRuntimeLifecycleTest,
                                 "Harken.Runtime.Lifecycle",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHarkenRuntimeLifecycleTest::RunTest(const FString&)
{
    FHarkenCoreRuntime& Runtime = FHarkenCoreRuntime::Get();
    Runtime.Shutdown();

    UHarkenSettings* Settings = NewObject<UHarkenSettings>();
    Settings->DeviceMode = EHarkenDeviceMode::Null;
    const FHarkenResult InitializeResult = Runtime.Initialize(*Settings);
    if (!TestTrue(TEXT("Initialize HarkenCore"), InitializeResult.bSuccess))
    {
        return false;
    }

    FHarkenEmitterHandle Emitter;
    const FHarkenResult CreateResult = Runtime.CreateEmitter(1, Emitter);
    TestTrue(TEXT("Create emitter"), CreateResult.bSuccess);
    TestTrue(TEXT("Emitter session"), Emitter.SessionGeneration == 1);
    TestTrue(TEXT("Stop emitter"), Runtime.StopEmitter(Emitter, 1, 0).bSuccess);
    TestTrue(TEXT("Destroy emitter"), Runtime.DestroyEmitter(Emitter, 1).bSuccess);
    TestFalse(TEXT("Destroy clears emitter"), Emitter.IsValid());
    TestTrue(TEXT("Stop all"), Runtime.StopAll(0).bSuccess);
    TestTrue(TEXT("Shutdown HarkenCore"), Runtime.Shutdown().bSuccess);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHarkenSessionHandleGuardTest,
                                 "Harken.Runtime.SessionHandleGuard",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHarkenSessionHandleGuardTest::RunTest(const FString&)
{
    FHarkenCoreRuntime& Runtime = FHarkenCoreRuntime::Get();
    Runtime.Shutdown();

    UHarkenSettings* Settings = NewObject<UHarkenSettings>();
    Settings->DeviceMode = EHarkenDeviceMode::Null;
    if (!TestTrue(TEXT("Initialize HarkenCore"), Runtime.Initialize(*Settings).bSuccess))
    {
        return false;
    }

    FHarkenEmitterHandle Emitter;
    if (!TestTrue(TEXT("Create emitter"), Runtime.CreateEmitter(7, Emitter).bSuccess))
    {
        Runtime.Shutdown();
        return false;
    }
    const FHarkenResult WrongSession = Runtime.StopEmitter(Emitter, 8, 0);
    TestEqual(TEXT("Wrong session is stale"), WrongSession.Code,
              static_cast<int32>(VN_STALE_HANDLE));
    TestTrue(TEXT("Wrong session preserves handle"), Emitter.IsValid());
    TestTrue(TEXT("Correct session destroys emitter"),
             Runtime.DestroyEmitter(Emitter, 7).bSuccess);

    FHarkenActionFiringHandle Firing;
    Firing.Value = 1;
    Firing.SessionGeneration = 7;
    FHarkenActionFiringInfo Info;
    const FHarkenResult FiringGuard = Runtime.QueryActionFiring(Firing, 8, Info);
    TestEqual(TEXT("Wrong firing session is stale"), FiringGuard.Code,
              static_cast<int32>(VN_STALE_HANDLE));
    TestTrue(TEXT("Wrong firing session preserves handle"), Firing.IsValid());

    TestTrue(TEXT("Shutdown HarkenCore"), Runtime.Shutdown().bSuccess);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHarkenVariableGeneratedIdTest,
                                 "Harken.Runtime.VariableGeneratedId",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHarkenVariableGeneratedIdTest::RunTest(const FString&)
{
    const FGuid Expected(0x67e0c65b, 0xf36e40d9, 0x80ee75af, 0x00ecf712);
    TestEqual(TEXT("generated VehicleRPM UUID"),
              FromHarkenObjectId(Harken::Generated::VehicleRPM), Expected);
    const FHarkenVariableId LibraryId = UHarkenVariableLibrary::VehicleRPM();
    TestEqual(TEXT("library VehicleRPM stable id"), LibraryId.StableId,
              Expected);
    TestEqual(TEXT("library VehicleRPM code name"), LibraryId.CodeName,
              FName(TEXT("VehicleRPM")));
    UHarkenVariableDataAsset* Asset = NewObject<UHarkenVariableDataAsset>();
    Asset->Variable = LibraryId;
    TestEqual(TEXT("DataAsset VehicleRPM stable id"), Asset->Variable.StableId,
              Expected);
    TestEqual(TEXT("DataAsset VehicleRPM code name"), Asset->Variable.CodeName,
              FName(TEXT("VehicleRPM")));
    TestTrue(TEXT("library matches generated id"),
             ToHarkenObjectId(LibraryId.StableId) == Harken::Generated::VehicleRPM);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHarkenVariableCatalogMismatchNameTest,
                                 "Harken.Runtime.VariableCatalogMismatchName",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHarkenVariableCatalogMismatchNameTest::RunTest(const FString&)
{
    const FHarkenResult Result =
        FHarkenCoreRuntime::MakeResult(VN_VARIABLE_CATALOG_MISMATCH);
    TestEqual(TEXT("catalog mismatch code is -1205"), Result.Code,
              static_cast<int32>(VN_VARIABLE_CATALOG_MISMATCH));
    TestEqual(TEXT("catalog mismatch result name"), Result.Message,
              FString(TEXT("Variable catalog mismatch")));
    TestFalse(TEXT("catalog mismatch is not success"), Result.bSuccess);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHarkenVariableWriteValidationTest,
                                 "Harken.Runtime.VariableWriteValidation",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHarkenVariableWriteValidationTest::RunTest(const FString&)
{
    FHarkenCoreRuntime& Runtime = FHarkenCoreRuntime::Get();
    Runtime.Shutdown();
    UHarkenSettings* Settings = NewObject<UHarkenSettings>();
    Settings->DeviceMode = EHarkenDeviceMode::Null;
    if (!TestTrue(TEXT("Initialize"), Runtime.Initialize(*Settings).bSuccess)) return false;
    FHarkenEmitterHandle Emitter;
    TestTrue(TEXT("Create emitter"), Runtime.CreateEmitter(7, Emitter).bSuccess);
    const FGuid Variable(0x67e0c65b, 0xf36e40d9, 0x80ee75af, 0x00ecf712);
    TestEqual(TEXT("Raw global write reaches native validation"),
              Runtime.SetValue(Variable, 0.5f, FHarkenEmitterHandle(), 7).Code,
              static_cast<int32>(VN_NO_VARIABLE));
    TestEqual(TEXT("Raw local write reaches native validation"),
              Runtime.SetValue(Variable, 0.5f, Emitter, 7).Code,
              static_cast<int32>(VN_NO_VARIABLE));
    TestEqual(TEXT("Wrong emitter session never becomes global"),
              Runtime.SetValue(Variable, 0.5f, Emitter, 8).Code,
              static_cast<int32>(VN_STALE_HANDLE));
    const FHarkenVariableId TypedVariable = UHarkenVariableLibrary::VehicleRPM();
    const FGuid LabelId(0, 0, 0, 721);
    TestEqual(TEXT("Typed label identity refuses mismatched catalog"),
              Runtime.SetValue(TypedVariable, LabelId, nullptr, FHarkenEmitterHandle(), 7).Code,
              static_cast<int32>(VN_VARIABLE_CATALOG_MISMATCH));
    const FString LabelName(TEXT("Active"));
    TestEqual(TEXT("Typed label name refuses mismatched catalog"),
              Runtime.SetValue(TypedVariable, {}, &LabelName, FHarkenEmitterHandle(), 7).Code,
              static_cast<int32>(VN_VARIABLE_CATALOG_MISMATCH));
    TestEqual(TEXT("Typed emitter label identity refuses mismatched catalog"),
              Runtime.SetValue(TypedVariable, LabelId, nullptr, Emitter, 7).Code,
              static_cast<int32>(VN_VARIABLE_CATALOG_MISMATCH));
    TestEqual(TEXT("Typed emitter label name refuses mismatched catalog"),
              Runtime.SetValue(TypedVariable, {}, &LabelName, Emitter, 7).Code,
              static_cast<int32>(VN_VARIABLE_CATALOG_MISMATCH));
    TestEqual(TEXT("Named write requires a published catalog"),
              Runtime.SetValue(TEXT("Speed"), 0.5f, FHarkenEmitterHandle(), 7).Code,
              static_cast<int32>(VN_CAPABILITY_UNAVAILABLE));
    TestEqual(TEXT("Named wrong-session emitter never becomes global"),
              Runtime.SetValue(TEXT("Speed"), 0.5f, Emitter, 8).Code,
              static_cast<int32>(VN_STALE_HANDLE));
    FGuid Resolved = Variable;
    TestEqual(TEXT("Variable resolve requires a publication"),
              Runtime.ResolveVariable(TEXT("Speed"), Resolved).Code,
              static_cast<int32>(VN_CAPABILITY_UNAVAILABLE));
    TestFalse(TEXT("Failed resolve clears the output GUID"), Resolved.IsValid());
    FHarkenActionFiringHandle Firing;
    TestEqual(TEXT("Named Action requires a publication"),
              Runtime.PostAction(TEXT("Play_Footstep"), Emitter, 7, 0, Firing).Code,
              static_cast<int32>(VN_CAPABILITY_UNAVAILABLE));
    const FHarkenEmitterHandle Stale = Emitter;
    TestTrue(TEXT("Destroy emitter"), Runtime.DestroyEmitter(Emitter, 7).bSuccess);
    TestEqual(TEXT("Destroyed emitter never becomes global"),
              Runtime.SetValue(Variable, 0.5f, Stale, 7).Code,
              static_cast<int32>(VN_STALE_HANDLE));
    TestTrue(TEXT("Shutdown"), Runtime.Shutdown().bSuccess);
    return !HasAnyErrors();
}

#endif
