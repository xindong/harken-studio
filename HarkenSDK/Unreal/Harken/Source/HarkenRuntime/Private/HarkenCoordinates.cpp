#include "HarkenCoordinates.h"

namespace HarkenCoordinates
{
FVector ToHarkenPosition(const FVector& UnrealPosition)
{
    return FVector(UnrealPosition.Y, UnrealPosition.Z, -UnrealPosition.X) * 0.01;
}

FVector ToHarkenDirection(const FVector& UnrealDirection)
{
    return FVector(UnrealDirection.Y, UnrealDirection.Z, -UnrealDirection.X);
}
}
