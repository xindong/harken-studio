#pragma once

#include "CoreMinimal.h"

namespace HarkenCoordinates
{
    HARKENRUNTIME_API FVector ToHarkenPosition(const FVector& UnrealPosition);
    HARKENRUNTIME_API FVector ToHarkenDirection(const FVector& UnrealDirection);
}
