#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include <harken_bus_compiler/BusCompiler.h>

#include <gtest/gtest.h>

#include <algorithm>

namespace {

using namespace harkenbuscompiler;
using namespace harkenprojectschema;

constexpr harkenstudio::ObjectId kProjectMasterId{0x4d4153544552, 1};

BusCompilerInputV1 validInput()
{
    BusCompilerInputV1 input;
    input.projectGeneration = 17;
    std::vector<RuntimeIdentityIndexBuildRowV1> claims;
    const AuthoringGuid masterGuid = kProjectMasterId;
    const AuthoringGuid parentGuid = harkentest::StudioObjectId(0x1'0000'0001ull);
    const AuthoringGuid childGuid = harkentest::StudioObjectId(0x1'0000'0002ull);
    claims.push_back({1, {kProjectMasterId, RuntimeIdentityKindV1::Bus, masterGuid,
                          {masterGuid, RuntimeIdentityKindV1::Bus, 0, kProjectMasterId}}});
    claims.push_back({2, {harkentest::StudioObjectId(0x1'0000'0001ull), RuntimeIdentityKindV1::Bus,
                          parentGuid,
                          {parentGuid, RuntimeIdentityKindV1::Bus, 0,
                           harkentest::StudioObjectId(0x1'0000'0001ull)}}});
    claims.push_back({3, {harkentest::StudioObjectId(0x1'0000'0002ull), RuntimeIdentityKindV1::Bus,
                          childGuid,
                          {childGuid, RuntimeIdentityKindV1::Bus, 0,
                           harkentest::StudioObjectId(0x1'0000'0002ull)}}});
    input.identityIndex = BuildRuntimeIdentityIndexV1(claims).index;
    input.compileTargetProfile = MakeCompileTargetProfileV1(
        1, 48000, VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO,
        kKnownCapabilityMaskV1);
    const BusSourceLocationV1 masterSource{
        masterGuid, "Busses/Master.vp", "/volume"};
    const BusSourceLocationV1 parentSource{
        parentGuid, "Busses/Parent.vp", "/volume"};
    const BusSourceLocationV1 childSource{
        childGuid, "Busses/Child.vp", "/volume"};
    input.buses = {
        {kProjectMasterId, harkenstudio::kMasterBusObjectId, 0.0f, 8, 0, masterSource},
        {harkentest::StudioObjectId(0x1'0000'0001ull), kProjectMasterId, -3.0f, 2, 0, parentSource},
        {harkentest::StudioObjectId(0x1'0000'0002ull), harkentest::StudioObjectId(0x1'0000'0001ull), -6.0f, 1, 0, childSource},
    };
    input.sends.push_back(
        {harkentest::StudioObjectId(0x1'0000'0002ull), kProjectMasterId, -12.0f, childSource});
    return input;
}

TEST(BusCompilerTest, RejectsDuplicateIdentityAndCycle)
{
    auto duplicate = validInput();
    duplicate.buses.push_back({harkentest::StudioObjectId(0x1'0000'0001ull), kProjectMasterId, 0.0f, 8, 0, {}});
    const auto duplicateResult = CompileBusProjectV1(duplicate);
    EXPECT_FALSE(duplicateResult.ok());
    EXPECT_EQ(duplicateResult.result, nullptr);
    EXPECT_NE(std::find_if(duplicateResult.diagnostics.begin(), duplicateResult.diagnostics.end(),
                           [](const auto& diagnostic) {
                               return diagnostic.code == BusDiagnosticCodeV1::DuplicateIdentity;
                           }), duplicateResult.diagnostics.end());

    auto cycle = validInput();
    cycle.buses[1].parentId = cycle.buses[2].busId;
    cycle.buses[2].parentId = cycle.buses[1].busId;
    const auto cycleResult = CompileBusProjectV1(cycle);
    EXPECT_FALSE(cycleResult.ok());
    EXPECT_NE(std::find_if(cycleResult.diagnostics.begin(), cycleResult.diagnostics.end(),
                           [](const auto& diagnostic) {
                               return diagnostic.code == BusDiagnosticCodeV1::CombinedTreeSendCycle;
                           }), cycleResult.diagnostics.end());
}

TEST(BusCompilerTest, SerializationIsIndependentOfInputOrder)
{
    auto first = validInput();
    auto second = validInput();
    std::reverse(second.buses.begin(), second.buses.end());
    const auto a = CompileBusProjectV1(first);
    const auto b = CompileBusProjectV1(second);
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(a.result->deterministicBytes, b.result->deterministicBytes);
    EXPECT_EQ(a.result->authoredBusFingerprint, b.result->authoredBusFingerprint);
}

TEST(RuntimeIdentityIndexTest, CrossKindCollisionIsRejected)
{
    using Kind = RuntimeIdentityKindV1;
    const AuthoringGuid busGuid = harkentest::StudioObjectId(42);
    const AuthoringGuid ownerGuid = harkentest::StudioObjectId(43);
    const RuntimeIdentitySourceKeyV1 busSource{
        busGuid, Kind::Bus, 0, harkentest::StudioObjectId(42)};
    const RuntimeIdentitySourceKeyV1 effectSource{
        ownerGuid, Kind::BusEffect, 0, harkentest::StudioObjectId(42)};
    const auto outcome = BuildRuntimeIdentityIndexV1({
        {9, {harkentest::StudioObjectId(42), Kind::Bus, busGuid, busSource}},
        {0, {harkentest::StudioObjectId(42), Kind::BusEffect, harkentest::StudioObjectId(42), effectSource}},
    });
    EXPECT_FALSE(outcome.ok());
    ASSERT_FALSE(outcome.diagnostics.empty());
    EXPECT_EQ(outcome.diagnostics[0].code,
              RuntimeIdentityIndexDiagnosticCodeV1::DuplicateStableId);
}

} // namespace
