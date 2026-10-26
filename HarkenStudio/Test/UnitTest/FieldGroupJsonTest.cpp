#include "FieldGroupJson.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace VNX {
namespace {

using namespace harkenprojectschema;

constexpr char kInherit[] = R"({"inherit":true})";
constexpr char kMainOutput[] = R"({"mainOutputBusRef":"12345678-9abc-def0-0123-456789abcdef","outputBusVolume":0})";
constexpr char kSend[] = R"({"sends":[{"targetBusID":"fedcba98-7654-3210-ffff-ffffffffffff","sendLevel":-6}]})";
constexpr char kRating[] = R"({"spatialPanning":0,"spatialIs3DEnabled":false,"spatialAttenuationRef":"00000000-0000-0000-0000-000000000000","spatialAttenuationDistanceScaling":1})";
constexpr AuthoringGuid kBus{UINT64_C(0x123456789abcdef0), UINT64_C(0x0123456789abcdef)};
constexpr AuthoringGuid kSendBus{UINT64_C(0xfedcba9876543210), UINT64_C(0xffffffffffffffff)};

std::string State(std::string_view mode, std::string_view history)
{
    const std::string inherit = mode == "UseShared" ? ""
        : "\"inherit\":" + (mode == "Local" ? std::string("false")
            : mode == "Inherit" ? std::string("true") : "\"" + std::string(mode) + "\"") + ",";
    return "{" + inherit + "\"values\":" + std::string(history) + "}";
}

std::string Block(std::string_view mainOutput = kInherit,
                  std::string_view send = kInherit,
                  std::string_view rating = kInherit,
                  std::string_view platforms = {})
{
    std::string result = "{\"shared\":{\"mainOutput\":" + std::string(mainOutput)
        + ",\"send\":" + std::string(send) + ",\"spatialRating\":" + std::string(rating) + "}";
    if (!platforms.empty()) result += ",\"platforms\":" + std::string(platforms);
    return result + "}";
}

std::string Replace(std::string text, std::string_view oldText, std::string_view newText)
{
    const auto offset = text.find(oldText);
    EXPECT_NE(offset, std::string::npos);
    if (offset != std::string::npos) text.replace(offset, oldText.size(), newText);
    return text;
}

void ExpectRejected(const std::string& text)
{
    SCOPED_TRACE(text);
    DefinitionFieldGroups groups;
    groups.mainOutput.shared = {FieldGroupMode::Local, MainOutputFieldGroup{kBus, -12.0f}};
    const auto before = groups;
    std::string error;
    EXPECT_FALSE(ReadFieldGroupsJson(text, groups, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(groups, before);
}

void ExpectWriteRejected(const DefinitionFieldGroups& groups)
{
    std::string text = "unchanged output";
    std::string error;
    EXPECT_FALSE(WriteFieldGroupsJson(groups, text, error));
    EXPECT_EQ(text, "unchanged output");
    EXPECT_FALSE(error.empty());
}

TEST(FieldGroupJson, DormantReferencesPreserveAll128BitsWithoutTargetLookup)
{
    const auto rating = Replace(kRating, "00000000-0000-0000-0000-000000000000",
                               "ffffffff-ffff-ffff-ffff-ffffffffffff");
    DefinitionFieldGroups groups;
    std::string error;
    ASSERT_TRUE(ReadFieldGroupsJson(Block(State("Inherit", kMainOutput),
                                           State("Inherit", kSend),
                                           State("Inherit", rating)), groups, error)) << error;
    ASSERT_TRUE(groups.send.shared.localHistory);
    ASSERT_EQ(groups.send.shared.localHistory->sends.size(), 1u);
    EXPECT_EQ(groups.send.shared.localHistory->sends[0].targetBusID, kSendBus);
    ASSERT_TRUE(groups.spatialRating.shared.localHistory);
    EXPECT_EQ(groups.spatialRating.shared.localHistory->spatialAttenuationRef,
              (AuthoringGuid{UINT64_MAX, UINT64_MAX}));
    std::string text;
    ASSERT_TRUE(WriteFieldGroupsJson(groups, text, error)) << error;
    DefinitionFieldGroups roundTrip;
    ASSERT_TRUE(ReadFieldGroupsJson(text, roundTrip, error)) << error;
    EXPECT_EQ(roundTrip, groups);
}

TEST(FieldGroupJson, UuidCaseNormalizesWithoutChangingIdentity)
{
    const auto mainOutput = Replace(kMainOutput, "12345678-9abc-def0-0123-456789abcdef",
                                    "12345678-9ABC-DEF0-0123-456789ABCDEF");
    DefinitionFieldGroups groups;
    std::string error;
    ASSERT_TRUE(ReadFieldGroupsJson(Block(State("Local", mainOutput)), groups, error)) << error;
    EXPECT_EQ(groups.mainOutput.shared.localHistory->mainOutputBusRef, kBus);
}

TEST(FieldGroupJson, FiniteFloatBitsAndSendRowOrderRoundTripDeterministically)
{
    DefinitionFieldGroups groups;
    groups.mainOutput.shared = {FieldGroupMode::Local, MainOutputFieldGroup{kBus, -0.0f}};
    groups.send.shared = {FieldGroupMode::Local,
                         SendFieldGroup{{{kSendBus, 0.1f}, {kBus, -0.0f}}}};
    SpatialRatingFieldGroup rating;
    rating.spatialPanning = std::numeric_limits<float>::denorm_min();
    rating.spatialAttenuationDistanceScaling = 3.333333f;
    groups.spatialRating.shared = {FieldGroupMode::Local, rating};
    std::string text;
    std::string error;
    ASSERT_TRUE(WriteFieldGroupsJson(groups, text, error)) << error;
    DefinitionFieldGroups roundTrip;
    ASSERT_TRUE(ReadFieldGroupsJson(text, roundTrip, error)) << error;
    EXPECT_EQ(roundTrip, groups);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(roundTrip.mainOutput.shared.localHistory->outputBusVolume),
              std::bit_cast<std::uint32_t>(-0.0f));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(roundTrip.spatialRating.shared.localHistory->spatialPanning), 1u);
    ASSERT_EQ(roundTrip.send.shared.localHistory->sends.size(), 2u);
    EXPECT_EQ(roundTrip.send.shared.localHistory->sends[0].targetBusID, kSendBus);
    EXPECT_EQ(roundTrip.send.shared.localHistory->sends[1].targetBusID, kBus);
    std::string second;
    ASSERT_TRUE(WriteFieldGroupsJson(roundTrip, second, error)) << error;
    EXPECT_EQ(text, second);
}

TEST(FieldGroupJson, RejectsInvalidFloatTypesOverflowAndOutOfRangeDormantHistory)
{
    for (const auto* value : {"null", "true", "\"0\"", "1e400", "1e-400", "1e39"})
        ExpectRejected(Block(State("Inherit", Replace(kMainOutput, "\"outputBusVolume\":0",
            "\"outputBusVolume\":" + std::string(value)))));
}

TEST(FieldGroupJson, RejectsDuplicateMembersAndMalformedJson)
{
    ExpectRejected(Block() + "{}");
    ExpectRejected(Replace(Block(), "\"inherit\":true", "\"inherit\":true,\"inherit\":false"));
    ExpectRejected(Block(kInherit, kInherit, kInherit, R"({"7e69e0e2-8d15-52df-a609-ab96fff38750":{},"7e69e0e2-8d15-52df-a609-ab96fff38750":{}})"));
    ExpectRejected(Replace(Block(), "\"shared\":", "\"shared\":{},\"shared\":"));
    ExpectRejected(Replace(Block(), "\"shared\":", "/*comment*/\"shared\":"));
    ExpectRejected(Replace(Block(), "\"mainOutput\":", "\"mainOutput\":,"));
    ExpectRejected("[]");
}

TEST(FieldGroupJson, WriterRejectsInvalidStatesWithoutChangingOutput)
{
    DefinitionFieldGroups groups;
    groups.mainOutput.shared.localHistory = MainOutputFieldGroup{kBus, std::numeric_limits<float>::quiet_NaN()};
    ExpectWriteRejected(groups);
    groups.mainOutput.shared.localHistory = MainOutputFieldGroup{kBus, std::numeric_limits<float>::infinity()};
    ExpectWriteRejected(groups);
}

}
}
