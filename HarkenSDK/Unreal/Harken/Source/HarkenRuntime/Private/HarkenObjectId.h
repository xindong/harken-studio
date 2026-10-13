#pragma once

#include "CoreMinimal.h"
#include "harken/ObjectId.h"

inline VnObjectId ToHarkenObjectId(const FGuid& Id)
{
    return {(static_cast<uint64>(Id.A) << 32) | Id.B,
            (static_cast<uint64>(Id.C) << 32) | Id.D};
}

inline FGuid FromHarkenObjectId(VnObjectId Id)
{
    return FGuid(static_cast<uint32>(Id.high >> 32), static_cast<uint32>(Id.high),
                 static_cast<uint32>(Id.low >> 32), static_cast<uint32>(Id.low));
}
