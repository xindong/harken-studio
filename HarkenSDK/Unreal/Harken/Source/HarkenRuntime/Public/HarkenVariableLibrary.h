#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "HarkenTypes.h"
#include "HarkenVariableLibrary.generated.h"

UCLASS()
class HARKENRUNTIME_API UHarkenVariableLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintPure, Category = "Harken")
    static FHarkenVariableId VehicleRPM();
};
