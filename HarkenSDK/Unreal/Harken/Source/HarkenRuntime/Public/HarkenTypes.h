#pragma once

#include "CoreMinimal.h"
#include "HarkenTypes.generated.h"

UENUM(BlueprintType)
enum class EHarkenDeviceMode : uint8
{
    Real,
    Null
};

USTRUCT(BlueprintType)
struct HARKENRUNTIME_API FHarkenResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    bool bSuccess = true;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 Code = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    FString Message;

    bool IsSuccess() const
    {
        return bSuccess;
    }
};

UENUM(BlueprintType)
enum class EHarkenRemoteState : uint8
{
    Stopped,
    Listening,
    Connected,
    Failed
};

USTRUCT(BlueprintType)
struct HARKENRUNTIME_API FHarkenRemoteStatus
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Harken|Communication")
    EHarkenRemoteState State = EHarkenRemoteState::Stopped;

    UPROPERTY(BlueprintReadOnly, Category = "Harken|Communication")
    FHarkenResult LastResult;

    UPROPERTY(BlueprintReadOnly, Category = "Harken|Communication")
    FString BindAddress;

    UPROPERTY(BlueprintReadOnly, Category = "Harken|Communication")
    int32 Port = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken|Communication")
    int32 DiscoveryPort = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken|Communication")
    int32 ProcessId = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken|Communication")
    FString InstanceId;

    UPROPERTY(BlueprintReadOnly, Category = "Harken|Communication")
    FString SessionId;

    UPROPERTY(BlueprintReadOnly, Category = "Harken|Communication")
    int32 ClientCount = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken|Communication")
    int32 ProtocolMajor = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken|Communication")
    int32 ProtocolMinor = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken|Communication")
    FString ApplicationName;

    UPROPERTY(BlueprintReadOnly, Category = "Harken|Communication")
    FString ProjectName;

    UPROPERTY(BlueprintReadOnly, Category = "Harken|Communication")
    FString EngineVersion;

    UPROPERTY(BlueprintReadOnly, Category = "Harken|Communication")
    FString BuildId;

    UPROPERTY(BlueprintReadOnly, Category = "Harken|Communication")
    FString Platform;
};

USTRUCT(BlueprintType)
struct HARKENRUNTIME_API FHarkenPackHandle
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int64 Value = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int64 SessionGeneration = 0;

    bool IsValid() const
    {
        return Value != 0 && SessionGeneration != 0;
    }
};

USTRUCT(BlueprintType)
struct HARKENRUNTIME_API FHarkenEmitterHandle
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int64 Value = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int64 SessionGeneration = 0;

    bool IsValid() const
    {
        return Value != 0 && SessionGeneration != 0;
    }
};

USTRUCT(BlueprintType)
struct HARKENRUNTIME_API FHarkenActionFiringHandle
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int64 Value = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int64 SessionGeneration = 0;

    bool IsValid() const
    {
        return Value != 0 && SessionGeneration != 0;
    }
};

USTRUCT(BlueprintType)
struct HARKENRUNTIME_API FHarkenActionFiringInfo
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 State = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 Outcome = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 TerminalReason = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    FGuid ActionId;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int64 WorldGeneration = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 DirectStepCount = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 DirectStepTerminalCount = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 ChildFiringCount = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 LivePlayableCount = 0;
};

USTRUCT(BlueprintType)
struct HARKENRUNTIME_API FHarkenActionStepResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 State = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 ResultCode = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 Opcode = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    FGuid OwnerActionId;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    FGuid TargetStableId;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 AuthoredEventIndex = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 ExecutionOrdinal = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 Depth = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 TargetKind = 0;
};

USTRUCT(BlueprintType)
struct HARKENRUNTIME_API FHarkenCurrentWorldLoadReport
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 Status = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 ResultCode = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int64 PriorGeneration = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int64 ResultGeneration = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int64 RuntimeSemanticFingerprint = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int64 CapabilityMask = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int64 BodyBytes = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int64 BodyChecksum = 0;
};

USTRUCT(BlueprintType)
struct HARKENRUNTIME_API FHarkenVariableId
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harken")
    FGuid StableId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harken")
    FName CodeName;

    bool IsAssigned() const
    {
        return StableId.IsValid();
    }
};

USTRUCT(BlueprintType)
struct HARKENRUNTIME_API FHarkenVariableTicket
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int64 Sequence = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int64 WorldGeneration = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int64 TransportGeneration = 0;
};

USTRUCT(BlueprintType)
struct HARKENRUNTIME_API FHarkenVariableTicketQueryResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 Status = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 Outcome = 0;
};

USTRUCT(BlueprintType)
struct HARKENRUNTIME_API FHarkenVariableQueryResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 Kind = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 Scope = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 Writers = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    float RangeMin = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    float RangeMax = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    float DefaultValue = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    float CurrentValue = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 HasEmitterOverride = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int32 PendingCount = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Harken")
    int64 ExecutionFingerprint = 0;
};
