#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "HarkenCoordinates.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHarkenPositionConversionTest,
                                 "Harken.Coordinates.Position",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHarkenPositionConversionTest::RunTest(const FString&)
{
    const FVector Converted = HarkenCoordinates::ToHarkenPosition(FVector(100.0, 200.0, 300.0));
    TestTrue(TEXT("Position conversion"), Converted.Equals(FVector(2.0, 3.0, -1.0), UE_KINDA_SMALL_NUMBER));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHarkenDirectionConversionTest,
                                 "Harken.Coordinates.Direction",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHarkenDirectionConversionTest::RunTest(const FString&)
{
    const FVector Converted = HarkenCoordinates::ToHarkenDirection(FVector(1.0, 2.0, 3.0));
    TestTrue(TEXT("Direction conversion"), Converted.Equals(FVector(2.0, 3.0, -1.0), UE_KINDA_SMALL_NUMBER));
    return true;
}

#endif
