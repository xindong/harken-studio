#pragma once

#include "Engine/DataAsset.h"
#include "HarkenTypes.h"
#include "HarkenVariableDataAsset.generated.h"

UCLASS(BlueprintType)
class HARKENRUNTIME_API UHarkenVariableDataAsset : public UDataAsset
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Harken")
    FHarkenVariableId Variable;
};
