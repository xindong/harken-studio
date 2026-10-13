#include "HarkenVariableLibrary.h"

#include "Generated/HarkenVariables.g.h"

FHarkenVariableId UHarkenVariableLibrary::VehicleRPM()
{
    FHarkenVariableId Id;
    Id.StableId = Harken::Generated::VehicleRPM;
    Id.CodeName = TEXT("VehicleRPM");
    return Id;
}
