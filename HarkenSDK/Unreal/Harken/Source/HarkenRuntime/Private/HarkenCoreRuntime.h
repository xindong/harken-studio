#pragma once

#include "HarkenTypes.h"
#include "harken/Harken.h"

class UHarkenSettings;

class FHarkenCoreRuntime final
{
public:
    static FHarkenCoreRuntime& Get();

    void SetLibraryReady(bool bReady);
    bool IsInitialized() const;

    FHarkenResult Initialize(const UHarkenSettings& Settings);
    FHarkenResult Shutdown();
    FHarkenResult Update();
    FHarkenResult Suspend();
    FHarkenResult Resume();
    FHarkenResult RetryRemoteCommunication(const UHarkenSettings& Settings);
    FHarkenResult QueryRemoteCommunication(FHarkenRemoteStatus& Status) const;
    FHarkenResult GetRemoteCommunicationResult() const;

    FHarkenResult LoadCurrentWorld(const FString& Path, FHarkenCurrentWorldLoadReport& Report);
    FHarkenResult LoadPack(const FString& Path, int64 SessionGeneration, FHarkenPackHandle& Pack);
    FHarkenResult UnloadPack(FHarkenPackHandle& Pack, int64 SessionGeneration);
    FHarkenResult PostAction(FGuid ActionId, FHarkenEmitterHandle Emitter,
                            int64 SessionGeneration, int64 CallerSeed,
                            FHarkenActionFiringHandle& Firing);
    FHarkenResult PostAction(const FString& ActionName, FHarkenEmitterHandle Emitter,
                            int64 SessionGeneration, int64 CallerSeed,
                            FHarkenActionFiringHandle& Firing);
    FHarkenResult StopActionFiring(FHarkenActionFiringHandle Firing,
                                  int64 SessionGeneration, int32 FadeMilliseconds);
    FHarkenResult QueryActionFiring(FHarkenActionFiringHandle Firing,
                                   int64 SessionGeneration,
                                   FHarkenActionFiringInfo& Info);
    FHarkenResult QueryActionStep(FHarkenActionFiringHandle Firing,
                                 int64 SessionGeneration,
                                 int32 AuthoredEventIndex,
                                 FHarkenActionStepResult& Step);
    FHarkenResult ReleaseActionFiring(FHarkenActionFiringHandle& Firing,
                                     int64 SessionGeneration);
    FHarkenResult StopAll(int32 FadeMilliseconds);

    FHarkenResult CreateEmitter(int64 SessionGeneration, FHarkenEmitterHandle& Emitter);
    FHarkenResult DestroyEmitter(FHarkenEmitterHandle& Emitter, int64 SessionGeneration);
    FHarkenResult SetEmitterTransform(FHarkenEmitterHandle Emitter,
                                     int64 SessionGeneration,
                                     const FTransform& Transform);
    FHarkenResult StopEmitter(FHarkenEmitterHandle Emitter,
                             int64 SessionGeneration, int32 FadeMilliseconds);
    FHarkenResult QueryEmitterVariable(FHarkenEmitterHandle Emitter,
                                      int64 SessionGeneration,
                                      FHarkenVariableId Variable,
                                      FHarkenVariableQueryResult& Result);
    FHarkenResult SetValue(FHarkenVariableId Variable, float Value,
                          FHarkenEmitterHandle Emitter, int64 SessionGeneration);
    FHarkenResult SetValue(FGuid VariableId, float Value,
                          FHarkenEmitterHandle Emitter, int64 SessionGeneration);
    FHarkenResult SetValue(FHarkenVariableId Variable, FGuid LabelId, const FString* LabelName,
                         FHarkenEmitterHandle Emitter, int64 SessionGeneration);
    FHarkenResult SetValue(FGuid VariableId, FGuid LabelId, const FString* LabelName,
                         FHarkenEmitterHandle Emitter, int64 SessionGeneration);
    FHarkenResult SetValue(const FString& VariableName, FGuid LabelId, const FString* LabelName,
                         FHarkenEmitterHandle Emitter, int64 SessionGeneration);
    FHarkenResult SetValue(const FString& VariableName, float Value,
                          FHarkenEmitterHandle Emitter, int64 SessionGeneration);
    FHarkenResult ResolveVariable(const FString& VariableName, FGuid& VariableId);
    FHarkenResult ResetValue(FHarkenVariableId Variable);
    FHarkenResult QueryVariable(FHarkenVariableId Variable,
                               FHarkenVariableQueryResult& Result);
    FHarkenResult QueryVariableTicket(const FHarkenVariableTicket& Ticket,
                                     FHarkenVariableTicketQueryResult& Result);
    FHarkenResult SetListenerTransform(const FTransform& Transform);

    static FHarkenResult MakeResult(VnResult Result, const FString& Context = FString());

private:
    static void NativeLog(void* UserData, uint32 Level, uint32 Code, const char* Text);
    static bool CheckGameThread(FHarkenResult& Result);
    static bool SessionMatches(int64 HandleGeneration, int64 SessionGeneration);
    FHarkenResult GuardTypedCatalog(const TCHAR* Context);
    FHarkenResult QueryTypedVariable(VnEmitterHandle Emitter,
                                    FHarkenVariableId Variable,
                                    FHarkenVariableQueryResult& Result,
                                    const TCHAR* Context);
    FHarkenResult LastRemoteResult;
    bool bLibraryReady = false;
    bool bInitialized = false;
};
