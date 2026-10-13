#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include <harken_bus_compiler/BusCompiler.h>
#include <harken/BusDefinitionFingerprintValue.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <memory>

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

BusCompilerInputV1 masterOnlyInput(std::uint32_t sampleRate = 48000)
{
    BusCompilerInputV1 input;
    input.projectGeneration = 23;
    const AuthoringGuid masterGuid = kProjectMasterId;
    input.identityIndex = BuildRuntimeIdentityIndexV1({
        {7, {kProjectMasterId, RuntimeIdentityKindV1::Bus, masterGuid,
             {masterGuid, RuntimeIdentityKindV1::Bus, 0, kProjectMasterId}}},
    }).index;
    input.compileTargetProfile = MakeCompileTargetProfileV1(
        1, sampleRate, VN_CHANNEL_LAYOUT_STEREO,
        VN_CHANNEL_LAYOUT_STEREO, kKnownCapabilityMaskV1);
    input.buses.push_back({
        kProjectMasterId, harkenstudio::kMasterBusObjectId, 0.0f, 8, VN_CHANNEL_LAYOUT_DEFAULT,
        {masterGuid, "Busses/Master.vp", "/volume"}});
    return input;
}

TEST(BusCompilerTest, UsesFullUuidIdsAndFeedsFirstOrder)
{
    const BusCompileOutcomeV1 outcome = CompileBusProjectV1(validInput());
    ASSERT_TRUE(outcome.ok());
    ASSERT_EQ(outcome.result->buses.size(), 3u);
    EXPECT_EQ(outcome.result->buses[0].busId, harkentest::StudioObjectId(0x1'0000'0002ull));
    EXPECT_EQ(outcome.result->buses[1].busId, harkentest::StudioObjectId(0x1'0000'0001ull));
    EXPECT_EQ(outcome.result->buses[2].busId, kProjectMasterId);
    EXPECT_EQ(outcome.result->buses[2].parentId, harkenstudio::kMasterBusObjectId);
    EXPECT_EQ(outcome.result->identityIndex->find(harkenstudio::kMasterBusObjectId), nullptr);
    EXPECT_EQ(outcome.result->buses[0].parentId, harkentest::StudioObjectId(0x1'0000'0001ull));
    ASSERT_EQ(outcome.result->sends.size(), 1u);
    EXPECT_EQ(outcome.result->sends[0].sendLevelDb, -12.0f);
}

TEST(BusCompilerTest, RejectsDuplicateReservedMissingAndCycle)
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

    auto reserved = validInput();
    reserved.buses[0].busId = harkenstudio::kMasterBusObjectId;
    const auto reservedResult = CompileBusProjectV1(reserved);
    EXPECT_FALSE(reservedResult.ok());
    EXPECT_NE(std::find_if(reservedResult.diagnostics.begin(), reservedResult.diagnostics.end(),
                           [](const auto& diagnostic) {
                               return diagnostic.code == BusDiagnosticCodeV1::ReservedMasterCollision;
                           }), reservedResult.diagnostics.end());

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

TEST(BusCompilerTest, RejectsMissingParentTargetAndNonFiniteValues)
{
    auto input = validInput();
    input.buses[1].parentId = harkentest::StudioObjectId(999);
    input.sends[0].targetBusId = harkentest::StudioObjectId(999);
    input.sends[0].sendLevelDb = std::numeric_limits<float>::quiet_NaN();
    const auto outcome = CompileBusProjectV1(input);
    EXPECT_FALSE(outcome.ok());
    EXPECT_EQ(outcome.result, nullptr);
    EXPECT_GE(outcome.diagnostics.size(), 3u);
}

TEST(BusCompilerTest, OnlyTheInternalOutputRootUsesTheDeviceLayout)
{
    auto input = masterOnlyInput();
    input.compileTargetProfile = MakeCompileTargetProfileV1(
        1, 48000, VN_CHANNEL_LAYOUT_5_1, VN_CHANNEL_LAYOUT_STEREO,
        kKnownCapabilityMaskV1);
    const auto compiled = CompileBusProjectV1(input);
    ASSERT_TRUE(compiled.ok());
    ASSERT_EQ(compiled.result->buses.size(), 1u);
    EXPECT_EQ(compiled.result->buses.front().outputLayout, VN_CHANNEL_LAYOUT_5_1);
    for (const auto capability : {
             BusPublicationCapabilityVintageV1::TopologyAndSendsNeutralProperties,
             BusPublicationCapabilityVintageV1::FullAuthoredProperties}) {
        const auto projection = BuildPublishedBusProjectionV1(*compiled.result, capability);
        ASSERT_EQ(projection.buses.size(), 2u);
        EXPECT_EQ(projection.buses.front().busId, kProjectMasterId);
        EXPECT_EQ(projection.buses.front().outputLayout, VN_CHANNEL_LAYOUT_5_1);
        EXPECT_EQ(projection.buses.back().busId, harkenstudio::kMasterBusObjectId);
        EXPECT_EQ(projection.buses.back().outputLayout, VN_CHANNEL_LAYOUT_STEREO);
        EXPECT_EQ(projection.buses.back().effectCount, 0u);
    }
}

TEST(BusCompilerTest, SerializationAndProjectionAreDeterministic)
{
    auto first = validInput();
    auto second = validInput();
    std::reverse(second.buses.begin(), second.buses.end());
    std::reverse(second.sends.begin(), second.sends.end());
    const auto a = CompileBusProjectV1(first);
    const auto b = CompileBusProjectV1(second);
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(a.result->deterministicBytes, b.result->deterministicBytes);
    EXPECT_EQ(a.result->authoredBusFingerprint, b.result->authoredBusFingerprint);

    const auto neutral = BuildPublishedBusProjectionV1(
        *a.result, BusPublicationCapabilityVintageV1::TopologyAndSendsNeutralProperties);
    ASSERT_EQ(neutral.buses.size(), a.result->buses.size() + 1);
    EXPECT_EQ(neutral.buses.back().busId, harkenstudio::kMasterBusObjectId);
    EXPECT_EQ(neutral.buses.back().parentId, harkenstudio::ObjectId{});
    EXPECT_FLOAT_EQ(neutral.buses[0].faderDb, 0.0f);
    EXPECT_TRUE(neutral.effects.empty());
    EXPECT_EQ(neutral.identityIndex.get(), a.result->identityIndex.get());
}

TEST(BusCompilerTest, CapacityBoundariesRejectWithoutPartialOutput)
{
    auto atCapacity = validInput();
    atCapacity.capacity.maxBusesIncludingOutputRoot = 4;
    EXPECT_TRUE(CompileBusProjectV1(atCapacity).ok());
    auto tooManyBuses = validInput();
    tooManyBuses.capacity.maxBusesIncludingOutputRoot = 3;
    const auto buses = CompileBusProjectV1(tooManyBuses);
    EXPECT_FALSE(buses.ok());
    EXPECT_EQ(buses.result, nullptr);
    EXPECT_NE(std::find_if(buses.diagnostics.begin(), buses.diagnostics.end(),
                           [](const auto& d) {
                               return d.code == BusDiagnosticCodeV1::BusCapacity;
                           }), buses.diagnostics.end());

    auto tooManySends = validInput();
    tooManySends.capacity.maxTotalSends = 0;
    const auto sends = CompileBusProjectV1(tooManySends);
    EXPECT_FALSE(sends.ok());
    EXPECT_EQ(sends.result, nullptr);
    EXPECT_NE(std::find_if(sends.diagnostics.begin(), sends.diagnostics.end(),
                           [](const auto& d) {
                               return d.code == BusDiagnosticCodeV1::TotalSendCapacity;
                           }), sends.diagnostics.end());
}

TEST(BusCompilerTest, RenameMetadataDoesNotChangeIdentityAndReparentOnlyChangesEdge)
{
    auto first = validInput();
    auto renamed = first;
    const auto a = CompileBusProjectV1(first);
    const auto b = CompileBusProjectV1(renamed);
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(a.result->idSet.ids, b.result->idSet.ids);
    EXPECT_EQ(a.result->deterministicBytes, b.result->deterministicBytes);

    auto reparented = first;
    reparented.buses[2].parentId = kProjectMasterId;
    const auto c = CompileBusProjectV1(reparented);
    ASSERT_TRUE(c.ok());
    const auto findBus = [](const CompiledBusResultV1& result, harkenstudio::ObjectId id) {
        return std::find_if(result.buses.begin(), result.buses.end(),
                            [id](const auto& bus) { return bus.busId == id; });
    };
    const auto original = findBus(*a.result, harkentest::StudioObjectId(0x1'0000'0002ull));
    const auto updated = findBus(*c.result, harkentest::StudioObjectId(0x1'0000'0002ull));
    ASSERT_NE(original, a.result->buses.end());
    ASSERT_NE(updated, c.result->buses.end());
    EXPECT_EQ(updated->busId, original->busId);
    EXPECT_NE(updated->parentId, original->parentId);
}

TEST(BusCompilerTest, DistinguishesMalformedTargetProfile)
{
    auto input = validInput();
    auto profile = std::make_shared<CompileTargetProfileV1>(
        *input.compileTargetProfile);
    profile->vintage = 2;
    profile->fingerprint = compileTargetProfileFingerprintV1(*profile);
    input.compileTargetProfile = profile;
    const auto outcome = CompileBusProjectV1(input);
    EXPECT_FALSE(outcome.ok());
    EXPECT_EQ(outcome.result, nullptr);
    EXPECT_NE(std::find_if(outcome.diagnostics.begin(), outcome.diagnostics.end(),
                           [](const auto& d) {
                               return d.code == BusDiagnosticCodeV1::UnsupportedAuthoredProperty;
                           }), outcome.diagnostics.end());
}

TEST(BusCompilerTest, DistinguishesUnsupportedTargetLayout)
{
    auto input = validInput();
    auto profile = std::make_shared<CompileTargetProfileV1>(
        *input.compileTargetProfile);
    profile->deviceLayout = VN_CHANNEL_LAYOUT_7_1;
    profile->fingerprint = compileTargetProfileFingerprintV1(*profile);
    input.compileTargetProfile = profile;

    const auto outcome = CompileBusProjectV1(input);

    EXPECT_FALSE(outcome.ok());
    EXPECT_NE(std::find_if(outcome.diagnostics.begin(), outcome.diagnostics.end(),
                           [](const auto& d) {
                               return d.code == BusDiagnosticCodeV1::InvalidLayoutDownmix;
                           }), outcome.diagnostics.end());
}

TEST(BusCompilerTest, DistinguishesMissingTargetCapability)
{
    auto input = validInput();
    auto profile = std::make_shared<CompileTargetProfileV1>(
        *input.compileTargetProfile);
    profile->capabilityMask &= ~kTargetCapabilityBusDownmixV1;
    profile->fingerprint = compileTargetProfileFingerprintV1(*profile);
    input.compileTargetProfile = profile;

    const auto outcome = CompileBusProjectV1(input);

    EXPECT_FALSE(outcome.ok());
    EXPECT_NE(std::find_if(outcome.diagnostics.begin(), outcome.diagnostics.end(),
                           [](const auto& d) {
                               return d.code == BusDiagnosticCodeV1::TargetCapabilityUnavailable;
                           }), outcome.diagnostics.end());
}

TEST(BusCompilerTest, ProfileFingerprintMatchesFrozenVector)
{
    const auto profile = MakeCompileTargetProfileV1(
        1, 48000, VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO,
        kKnownCapabilityMaskV1);
    ASSERT_NE(profile, nullptr);
    EXPECT_EQ(profile->fingerprint, UINT64_C(0x094beb16959f4bcf));
    EXPECT_EQ(profile->fingerprint,
              compileTargetProfileFingerprintV1(*profile));
}

TEST(BusCompilerTest, FingerprintsMatchFrozenMasterVectors)
{
    const auto neutral = CompileBusProjectV1(masterOnlyInput());
    ASSERT_TRUE(neutral.ok());
    EXPECT_EQ(neutral.result->authoredBusFingerprint,
              UINT64_C(0x657aeae476e8095d));
    EXPECT_EQ(BuildPublishedBusProjectionV1(
                  *neutral.result,
                  BusPublicationCapabilityVintageV1::TopologyAndSendsNeutralProperties)
                  .fingerprint,
              UINT64_C(0x2edf2ccc8361e788));

    auto fullInput = masterOnlyInput();
    const AuthoringGuid masterGuid = kProjectMasterId;
    fullInput.identityIndex = BuildRuntimeIdentityIndexV1({
        {7, {kProjectMasterId, RuntimeIdentityKindV1::Bus, masterGuid,
             {masterGuid, RuntimeIdentityKindV1::Bus, 0, kProjectMasterId}}},
        {0, {harkentest::StudioObjectId(2), RuntimeIdentityKindV1::BusEffect, harkentest::StudioObjectId(2),
             {masterGuid, RuntimeIdentityKindV1::BusEffect, 0, harkentest::StudioObjectId(2)}}},
    }).index;
    fullInput.buses[0].faderDb = -6.0f;
    fullInput.buses[0].faderAfterSlot = 1;
    fullInput.buses[0].effectFirst = 0;
    fullInput.buses[0].effectCount = 1;
    const BusSourceLocationV1 effectSource{
        masterGuid, "Busses/Master.vp", "/effects/0"};
    fullInput.effects.push_back({kProjectMasterId, harkentest::StudioObjectId(2), 0, VN_EFFECT_GAIN,
                                 2, false, 0, 1, effectSource});
    fullInput.effectParameters.push_back({harkentest::StudioObjectId(2), 0, -3.0f, effectSource});
    const auto full = CompileBusProjectV1(fullInput);
    ASSERT_TRUE(full.ok());
    EXPECT_EQ(full.result->authoredBusFingerprint,
              UINT64_C(0x1ffd600aa2fe28ce));
    EXPECT_EQ(BuildPublishedBusProjectionV1(
                  *full.result,
                  BusPublicationCapabilityVintageV1::FullAuthoredProperties)
                  .fingerprint,
              UINT64_C(0xf0c25879cf00a864));

    const auto rate44100 = CompileBusProjectV1(masterOnlyInput(44100));
    ASSERT_TRUE(rate44100.ok());
    EXPECT_EQ(rate44100.result->compileTargetProfileFingerprint,
              UINT64_C(0x8af13b15c37bc05e));
    EXPECT_EQ(BuildPublishedBusProjectionV1(
                  *rate44100.result,
                  BusPublicationCapabilityVintageV1::TopologyAndSendsNeutralProperties)
                  .fingerprint,
              UINT64_C(0x1216a7a716fc6dfb));
    EXPECT_EQ(harken::busfingerprint::Finalize(0),
              UINT64_C(0x0000a5000001185f));
}

TEST(BusCompilerTest, ProofBytesUseFrozenSectionSizesAndSourceRows)
{
    const auto outcome = CompileBusProjectV1(masterOnlyInput());
    ASSERT_TRUE(outcome.ok());
    const std::uint64_t expectedSize = 96 + 96 + 48 + 64 * 4 + 40
        + std::string("Busses/Master.vp").size()
        + std::string("/volume").size();
    EXPECT_EQ(outcome.result->checkedByteSize, expectedSize);
    EXPECT_EQ(outcome.result->deterministicBytes.size(), expectedSize);
    ASSERT_GE(outcome.result->deterministicBytes.size(), 96u);
    std::uint64_t encodedSize = 0;
    for (unsigned index = 0; index < 8; ++index)
        encodedSize |= static_cast<std::uint64_t>(
            outcome.result->deterministicBytes[88 + index]) << (index * 8);
    EXPECT_EQ(encodedSize, expectedSize);

    auto tooSmall = masterOnlyInput();
    tooSmall.capacity.maxCompiledBytes = expectedSize - 1;
    const auto refusal = CompileBusProjectV1(tooSmall);
    EXPECT_FALSE(refusal.ok());
    EXPECT_EQ(refusal.result, nullptr);
    EXPECT_NE(std::find_if(
                  refusal.diagnostics.begin(), refusal.diagnostics.end(),
                  [](const auto& diagnostic) {
                      return diagnostic.code
                          == BusDiagnosticCodeV1::CompiledByteCapacity;
                  }),
              refusal.diagnostics.end());
}

TEST(BusCompilerTest, RejectsNonCanonicalSourceLocations)
{
    auto input = masterOnlyInput();
    input.buses[0].source.documentPath = "C:\\temp\\Master.vp";
    const auto outcome = CompileBusProjectV1(input);
    EXPECT_FALSE(outcome.ok());
    EXPECT_EQ(outcome.result, nullptr);
    EXPECT_NE(std::find_if(
                  outcome.diagnostics.begin(), outcome.diagnostics.end(),
                  [](const auto& diagnostic) {
                      return diagnostic.code
                          == BusDiagnosticCodeV1::UnsupportedAuthoredProperty;
                  }),
              outcome.diagnostics.end());
}

TEST(BusCompilerTest, ValidatesExplicitSixtyFourSlotDownmix)
{
    auto input = validInput();
    input.compileTargetProfile = MakeCompileTargetProfileV1(
        1, 48000, VN_CHANNEL_LAYOUT_7_1, VN_CHANNEL_LAYOUT_STEREO,
        kKnownCapabilityMaskV1);
    ASSERT_NE(input.compileTargetProfile, nullptr);
    input.buses[1].outputLayout = VN_CHANNEL_LAYOUT_STEREO;
    input.buses[1].downmixFirst = 0;
    input.buses[1].downmixCount = 1;
    input.downmix.push_back({harkentest::StudioObjectId(0x1'0000'0001ull),
                             VN_CHANNEL_LAYOUT_7_1,
                             VN_CHANNEL_LAYOUT_STEREO,
                             0,
                             64,
                             {harkentest::StudioObjectId(0x1'0000'0001ull), "downmix", "matrix"}});
    input.downmixCoefficients.assign(64, 0.0f);
    input.downmixCoefficients[0] = 1.0f;
    input.downmixCoefficients[9] = 0.5f;
    const auto outcome = CompileBusProjectV1(input);
    ASSERT_TRUE(outcome.ok());
    const auto row = std::find_if(
        outcome.result->downmix.begin(), outcome.result->downmix.end(),
        [](const auto& value) { return value.busId == harkentest::StudioObjectId(0x1'0000'0001ull); });
    ASSERT_NE(row, outcome.result->downmix.end());
    EXPECT_EQ(row->coefficientCount, 64u);
    EXPECT_EQ(outcome.result->downmixCoefficients.size(), 64u * 3u);
}

TEST(BusCompilerTest, RejectsNonZeroInactiveDownmixCoefficient)
{
    auto input = validInput();
    input.compileTargetProfile = MakeCompileTargetProfileV1(
        1, 48000, VN_CHANNEL_LAYOUT_7_1, VN_CHANNEL_LAYOUT_STEREO,
        kKnownCapabilityMaskV1);
    ASSERT_NE(input.compileTargetProfile, nullptr);
    input.buses[1].outputLayout = VN_CHANNEL_LAYOUT_STEREO;
    input.buses[1].downmixFirst = 0;
    input.buses[1].downmixCount = 1;
    input.downmix.push_back({harkentest::StudioObjectId(0x1'0000'0001ull),
                             VN_CHANNEL_LAYOUT_7_1,
                             VN_CHANNEL_LAYOUT_STEREO,
                             0,
                             64,
                             {harkentest::StudioObjectId(0x1'0000'0001ull), "downmix", "matrix"}});
    input.downmixCoefficients.assign(64, 0.0f);
    input.downmixCoefficients[2 * 8 + 7] = 1.0f;
    const auto outcome = CompileBusProjectV1(input);
    EXPECT_FALSE(outcome.ok());
    EXPECT_NE(std::find_if(outcome.diagnostics.begin(), outcome.diagnostics.end(),
                           [](const auto& value) {
                               return value.code == BusDiagnosticCodeV1::InvalidLayoutDownmix;
                           }), outcome.diagnostics.end());
}

TEST(BusCompilerTest, EffectCapabilityAndTotalCapacityAreTypedRefusals)
{
    auto input = validInput();
    input.effects.push_back({harkentest::StudioObjectId(0x1'0000'0001ull), harkentest::StudioObjectId(99), 0, 0, 2, false, 0, 0,
                             {harkentest::StudioObjectId(0x1'0000'0001ull), "effects", "slot"}});
    const AuthoringGuid masterGuid = kProjectMasterId;
    const AuthoringGuid parentGuid = harkentest::StudioObjectId(0x1'0000'0001ull);
    const AuthoringGuid childGuid = harkentest::StudioObjectId(0x1'0000'0002ull);
    input.identityIndex = BuildRuntimeIdentityIndexV1({
        {1, {kProjectMasterId, RuntimeIdentityKindV1::Bus, masterGuid,
             {masterGuid, RuntimeIdentityKindV1::Bus, 0, kProjectMasterId}}},
        {2, {harkentest::StudioObjectId(0x1'0000'0001ull), RuntimeIdentityKindV1::Bus, parentGuid,
             {parentGuid, RuntimeIdentityKindV1::Bus, 0, harkentest::StudioObjectId(0x1'0000'0001ull)}}},
        {3, {harkentest::StudioObjectId(0x1'0000'0002ull), RuntimeIdentityKindV1::Bus, childGuid,
             {childGuid, RuntimeIdentityKindV1::Bus, 0, harkentest::StudioObjectId(0x1'0000'0002ull)}}},
        {0, {harkentest::StudioObjectId(99), RuntimeIdentityKindV1::BusEffect, harkentest::StudioObjectId(99),
             {parentGuid, RuntimeIdentityKindV1::BusEffect, 0, harkentest::StudioObjectId(99)}}}
    }).index;
    input.capacity.maxEffectsPerBus = 0;
    const auto outcome = CompileBusProjectV1(input);
    EXPECT_FALSE(outcome.ok());
    EXPECT_NE(std::find_if(outcome.diagnostics.begin(), outcome.diagnostics.end(),
                           [](const auto& value) {
                               return value.code == BusDiagnosticCodeV1::EffectCapacity;
                           }), outcome.diagnostics.end());
}

TEST(RuntimeIdentityIndexTest, CrossKindCollisionNamesTheHeldKind)
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
    ASSERT_EQ(outcome.diagnostics.size(), 1u);
    EXPECT_EQ(outcome.diagnostics[0].code,
              RuntimeIdentityIndexDiagnosticCodeV1::DuplicateStableId);
    EXPECT_EQ(outcome.diagnostics[0].kind, Kind::BusEffect);
    EXPECT_EQ(outcome.diagnostics[0].conflictingKind, Kind::Bus);
    EXPECT_EQ(outcome.diagnostics[0].conflictingSourceKey, busSource);
}

TEST(BusCompilerTest, RejectsOutOfRangeFaderSendAndEffectValues)
{
    auto fader = validInput();
    fader.buses[1].faderDb = 24.01f;
    const auto faderOutcome = CompileBusProjectV1(fader);
    EXPECT_FALSE(faderOutcome.ok());
    EXPECT_EQ(faderOutcome.result, nullptr);

    auto send = validInput();
    send.sends[0].sendLevelDb = -96.01f;
    const auto sendOutcome = CompileBusProjectV1(send);
    EXPECT_FALSE(sendOutcome.ok());
    EXPECT_EQ(sendOutcome.result, nullptr);

    auto effect = validInput();
    const AuthoringGuid masterGuid = kProjectMasterId;
    const AuthoringGuid parentGuid = harkentest::StudioObjectId(0x1'0000'0001ull);
    const AuthoringGuid childGuid = harkentest::StudioObjectId(0x1'0000'0002ull);
    const AuthoringGuid effectOwner = parentGuid;
    effect.identityIndex = BuildRuntimeIdentityIndexV1({
        {1, {kProjectMasterId, RuntimeIdentityKindV1::Bus, masterGuid,
             {masterGuid, RuntimeIdentityKindV1::Bus, 0, kProjectMasterId}}},
        {2, {harkentest::StudioObjectId(0x1'0000'0001ull), RuntimeIdentityKindV1::Bus, parentGuid,
             {parentGuid, RuntimeIdentityKindV1::Bus, 0,
              harkentest::StudioObjectId(0x1'0000'0001ull)}}},
        {3, {harkentest::StudioObjectId(0x1'0000'0002ull), RuntimeIdentityKindV1::Bus, childGuid,
             {childGuid, RuntimeIdentityKindV1::Bus, 0,
              harkentest::StudioObjectId(0x1'0000'0002ull)}}},
        {0, {harkentest::StudioObjectId(90), RuntimeIdentityKindV1::BusEffect, harkentest::StudioObjectId(90),
             {effectOwner, RuntimeIdentityKindV1::BusEffect, 0, harkentest::StudioObjectId(90)}}},
    }).index;
    effect.buses[1].effectFirst = 0;
    effect.buses[1].effectCount = 1;
    const BusSourceLocationV1 effectSource{
        parentGuid, "Busses/Parent.vp", "/effects/0"};
    effect.effects.push_back({harkentest::StudioObjectId(0x1'0000'0001ull), harkentest::StudioObjectId(90), 0,
                              VN_EFFECT_THREE_BAND_TONE, 2, false,
                              0, 7, effectSource});
    effect.effectParameters = {
        {harkentest::StudioObjectId(90), 0, 0.0f, effectSource},
        {harkentest::StudioObjectId(90), 1, 24000.0f, effectSource},
        {harkentest::StudioObjectId(90), 2, 0.0f, effectSource},
        {harkentest::StudioObjectId(90), 3, 1000.0f, effectSource},
        {harkentest::StudioObjectId(90), 4, 1.0f, effectSource},
        {harkentest::StudioObjectId(90), 5, 0.0f, effectSource},
        {harkentest::StudioObjectId(90), 6, 8000.0f, effectSource},
    };
    const auto effectOutcome = CompileBusProjectV1(effect);
    EXPECT_FALSE(effectOutcome.ok());
    EXPECT_EQ(effectOutcome.result, nullptr);
    EXPECT_NE(std::find_if(
                  effectOutcome.diagnostics.begin(),
                  effectOutcome.diagnostics.end(),
                  [](const auto& diagnostic) {
                      return diagnostic.code
                          == BusDiagnosticCodeV1::UnsupportedAuthoredProperty;
                  }),
              effectOutcome.diagnostics.end());
}

TEST(BusCompilerTest, CompilesTypedEffectSliceAtTheTargetProfile)
{
    auto input = validInput();
    const AuthoringGuid masterGuid = kProjectMasterId;
    const AuthoringGuid parentGuid = harkentest::StudioObjectId(0x1'0000'0001ull);
    const AuthoringGuid childGuid = harkentest::StudioObjectId(0x1'0000'0002ull);
    input.identityIndex = BuildRuntimeIdentityIndexV1({
        {1, {kProjectMasterId, RuntimeIdentityKindV1::Bus, masterGuid,
             {masterGuid, RuntimeIdentityKindV1::Bus, 0, kProjectMasterId}}},
        {2, {harkentest::StudioObjectId(0x1'0000'0001ull), RuntimeIdentityKindV1::Bus, parentGuid,
             {parentGuid, RuntimeIdentityKindV1::Bus, 0,
              harkentest::StudioObjectId(0x1'0000'0001ull)}}},
        {3, {harkentest::StudioObjectId(0x1'0000'0002ull), RuntimeIdentityKindV1::Bus, childGuid,
             {childGuid, RuntimeIdentityKindV1::Bus, 0,
              harkentest::StudioObjectId(0x1'0000'0002ull)}}},
        {0, {harkentest::StudioObjectId(91), RuntimeIdentityKindV1::BusEffect, harkentest::StudioObjectId(91),
             {parentGuid, RuntimeIdentityKindV1::BusEffect, 0, harkentest::StudioObjectId(91)}}},
    }).index;
    input.buses[1].effectFirst = 0;
    input.buses[1].effectCount = 1;
    const BusSourceLocationV1 effectSource{
        parentGuid, "Busses/Parent.vp", "/effects/0"};
    input.effects.push_back({harkentest::StudioObjectId(0x1'0000'0001ull), harkentest::StudioObjectId(91), 0,
                             VN_EFFECT_GAIN, 2, false, 0, 1,
                             effectSource});
    input.effectParameters.push_back({harkentest::StudioObjectId(91), 0, -12.0f, effectSource});
    const auto outcome = CompileBusProjectV1(input);
    ASSERT_TRUE(outcome.ok());
    ASSERT_EQ(outcome.result->effects.size(), 1u);
    ASSERT_EQ(outcome.result->effectParameters.size(), 1u);
    EXPECT_EQ(outcome.result->effects[0].effectId, harkentest::StudioObjectId(91u));
    EXPECT_FLOAT_EQ(outcome.result->effectParameters[0].value, -12.0f);
}

TEST(BusCompilerTest, VolumeOnlyKeepsThreeGainsAndNeutralizesDeferredProperties)
{
    auto input = validInput();
    const auto parentId = harkentest::StudioObjectId(0x1'0000'0001ull);
    const auto childId = harkentest::StudioObjectId(0x1'0000'0002ull);
    const auto effectId = harkentest::StudioObjectId(91);
    input.compileTargetProfile = MakeCompileTargetProfileV1(
        1, 48000, VN_CHANNEL_LAYOUT_7_1, VN_CHANNEL_LAYOUT_STEREO,
        kKnownCapabilityMaskV1);
    ASSERT_NE(nullptr, input.compileTargetProfile);
    input.identityIndex = BuildRuntimeIdentityIndexV1({
        {1, {kProjectMasterId, RuntimeIdentityKindV1::Bus, kProjectMasterId,
             {kProjectMasterId, RuntimeIdentityKindV1::Bus, 0, kProjectMasterId}}},
        {2, {parentId, RuntimeIdentityKindV1::Bus, parentId,
             {parentId, RuntimeIdentityKindV1::Bus, 0, parentId}}},
        {3, {childId, RuntimeIdentityKindV1::Bus, childId,
             {childId, RuntimeIdentityKindV1::Bus, 0, childId}}},
        {0, {effectId, RuntimeIdentityKindV1::BusEffect, effectId,
             {parentId, RuntimeIdentityKindV1::BusEffect, 0, effectId}}},
    }).index;
    ASSERT_NE(nullptr, input.identityIndex);
    input.buses[1].makeUpGainDb = 2.0f;
    input.buses[1].outputBusVolumeDb = -5.0f;
    input.buses[1].effectFirst = 0;
    input.buses[1].effectCount = 1;
    input.buses[1].outputLayout = VN_CHANNEL_LAYOUT_STEREO;
    input.buses[1].downmixFirst = 0;
    input.buses[1].downmixCount = 1;
    const BusSourceLocationV1 effectSource{
        parentId, "Busses/Parent.vp", "/effects/0"};
    input.effects.push_back({parentId, effectId, 0, VN_EFFECT_GAIN, 2,
                             false, 0, 1, effectSource});
    input.effectParameters.push_back({effectId, 0, -12.0f, effectSource});
    input.downmix.push_back({parentId, VN_CHANNEL_LAYOUT_7_1,
                             VN_CHANNEL_LAYOUT_STEREO, 0, 64,
                             {parentId, "Busses/Parent.vp", "/downmix"}});
    input.downmixCoefficients.assign(64, 0.0f);
    input.downmixCoefficients[0] = 1.0f;
    input.downmixCoefficients[9] = 0.5f;

    const auto compiled = CompileBusProjectV1(input);
    ASSERT_TRUE(compiled.ok());
    ASSERT_EQ(1u, compiled.result->sends.size());
    ASSERT_EQ(1u, compiled.result->effects.size());
    ASSERT_EQ(1u, compiled.result->effectParameters.size());
    ASSERT_EQ(3u, compiled.result->downmix.size());
    EXPECT_FLOAT_EQ(-12.0f, compiled.result->sends.front().sendLevelDb);
    const auto authoredDownmix = std::find_if(compiled.result->downmix.begin(),
        compiled.result->downmix.end(),
        [&](const auto& row) { return row.busId == parentId; });
    ASSERT_NE(authoredDownmix, compiled.result->downmix.end());
    ASSERT_LT(authoredDownmix->coefficientFirst + 9,
              compiled.result->downmixCoefficients.size());
    EXPECT_FLOAT_EQ(0.5f, compiled.result->downmixCoefficients[
        authoredDownmix->coefficientFirst + 9]);
    const auto authored = std::find_if(compiled.result->buses.begin(),
        compiled.result->buses.end(),
        [&](const auto& bus) { return bus.busId == parentId; });
    ASSERT_NE(authored, compiled.result->buses.end());
    EXPECT_EQ(2u, authored->faderAfterSlot);
    EXPECT_EQ(VN_CHANNEL_LAYOUT_STEREO, authored->outputLayout);
    EXPECT_EQ(1u, authored->effectCount);

    const auto projected = BuildPublishedBusProjectionV1(*compiled.result,
        BusPublicationCapabilityVintageV1::TopologyAndVolumeNeutralOtherProperties);
    ASSERT_EQ(compiled.result->buses.size() + 1, projected.buses.size());
    ASSERT_EQ(1u, projected.sends.size());
    EXPECT_EQ(childId, projected.sends.front().sourceBusId);
    EXPECT_EQ(kProjectMasterId, projected.sends.front().targetBusId);
    EXPECT_FLOAT_EQ(0.0f, projected.sends.front().sendLevelDb);
    EXPECT_TRUE(projected.effects.empty());
    EXPECT_TRUE(projected.effectParameters.empty());
    ASSERT_EQ(projected.buses.size(), projected.downmix.size());
    ASSERT_EQ(projected.buses.size() * 64, projected.downmixCoefficients.size());
    const auto parent = std::find_if(projected.buses.begin(), projected.buses.end(),
        [&](const auto& bus) { return bus.busId == parentId; });
    ASSERT_NE(parent, projected.buses.end());
    EXPECT_FLOAT_EQ(-3.0f, parent->faderDb);
    EXPECT_FLOAT_EQ(2.0f, parent->makeUpGainDb);
    EXPECT_FLOAT_EQ(-5.0f, parent->outputBusVolumeDb);
    EXPECT_EQ(8u, parent->faderAfterSlot);
    EXPECT_EQ(0u, parent->effectCount);
    EXPECT_EQ(VN_CHANNEL_LAYOUT_7_1, parent->outputLayout);
    ASSERT_LT(parent->downmixFirst, projected.downmix.size());
    const auto& downmix = projected.downmix[parent->downmixFirst];
    EXPECT_EQ(parentId, downmix.busId);
    EXPECT_EQ(VN_CHANNEL_LAYOUT_7_1, downmix.sourceLayout);
    EXPECT_EQ(VN_CHANNEL_LAYOUT_7_1, downmix.destinationLayout);
    ASSERT_LT(downmix.coefficientFirst + 9, projected.downmixCoefficients.size());
    EXPECT_FLOAT_EQ(1.0f, projected.downmixCoefficients[downmix.coefficientFirst + 9]);
    for (const auto& bus : projected.buses)
        EXPECT_EQ(8u, bus.faderAfterSlot);
    EXPECT_EQ(VN_CHANNEL_LAYOUT_STEREO, projected.buses.back().outputLayout);
    const auto withSends = BuildPublishedBusProjectionV1(*compiled.result,
        BusPublicationCapabilityVintageV1::TopologyVolumeAndSendNeutralOtherProperties);
    ASSERT_EQ(1u, withSends.sends.size());
    EXPECT_FLOAT_EQ(-12.0f, withSends.sends.front().sendLevelDb);
    EXPECT_EQ(projected.buses.size(), withSends.buses.size());
    EXPECT_EQ(projected.buses.front().faderDb, withSends.buses.front().faderDb);
    EXPECT_TRUE(withSends.effects.empty());
    EXPECT_NE(projected.fingerprint, withSends.fingerprint);
    auto altered = *compiled.result;
    altered.sends.front().sendLevelDb = -6.0f;
    const auto alteredProjection = BuildPublishedBusProjectionV1(altered,
        BusPublicationCapabilityVintageV1::TopologyVolumeAndSendNeutralOtherProperties);
    EXPECT_NE(withSends.fingerprint, alteredProjection.fingerprint);
}

} // namespace
