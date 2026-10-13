#include "Support/FieldGroupTestUtils.h"
#include "FieldGroupJson.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

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
    groups.send.platforms[HarkenTest::FieldGroupPlatformIds::iOS] = {FieldGroupMode::Inherit, SendFieldGroup{}};
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

TEST(FieldGroupJson, NewDefinitionHasThreeInheritedGroupsAndNoHistory)
{
    DefinitionFieldGroups groups;
    std::string error = "old error";
    ASSERT_TRUE(ReadFieldGroupsJson(Block(), groups, error)) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(groups, DefinitionFieldGroups{});
    std::string text;
    ASSERT_TRUE(WriteFieldGroupsJson(groups, text, error)) << error;
    EXPECT_EQ(text, "{\n  \"shared\": {\n    \"mainOutput\": {\n      \"inherit\": true\n    },\n"
                    "    \"send\": {\n      \"inherit\": true\n    },\n"
                    "    \"spatialRating\": {\n      \"inherit\": true\n    }\n  }\n}\n");
}

TEST(FieldGroupJson, ExplicitZeroFalseNoneAndEmptySendRemainLocal)
{
    DefinitionFieldGroups groups;
    std::string error;
    ASSERT_TRUE(ReadFieldGroupsJson(Block(State("Local", kMainOutput),
                                           State("Local", R"({"sends":[]})"),
                                           State("Local", kRating)), groups, error)) << error;
    ASSERT_TRUE(groups.mainOutput.shared.localHistory);
    EXPECT_EQ(groups.mainOutput.shared.localHistory->mainOutputBusRef, kBus);
    EXPECT_EQ(groups.mainOutput.shared.localHistory->outputBusVolume, 0.0f);
    ASSERT_TRUE(groups.send.shared.localHistory);
    EXPECT_TRUE(groups.send.shared.localHistory->sends.empty());
    ASSERT_TRUE(groups.spatialRating.shared.localHistory);
    EXPECT_FALSE(groups.spatialRating.shared.localHistory->spatialIs3DEnabled);
    EXPECT_EQ(groups.spatialRating.shared.localHistory->spatialAttenuationRef, kNullGuid);
    std::string text;
    ASSERT_TRUE(WriteFieldGroupsJson(groups, text, error)) << error;
    DefinitionFieldGroups roundTrip;
    ASSERT_TRUE(ReadFieldGroupsJson(text, roundTrip, error)) << error;
    EXPECT_EQ(roundTrip, groups);
    EXPECT_EQ(text.find("spatialSpread"), std::string::npos);
    EXPECT_EQ(roundTrip.send.shared.mode, FieldGroupMode::Local);
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
    EXPECT_NE(text.find("ffffffff-ffff-ffff-ffff-ffffffffffff"), std::string::npos);
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
    std::string text;
    ASSERT_TRUE(WriteFieldGroupsJson(groups, text, error)) << error;
    EXPECT_NE(text.find("12345678-9abc-def0-0123-456789abcdef"), std::string::npos);
}

TEST(FieldGroupJson, EveryTargetPlatformPreservesEachModeAndItsHistory)
{
    const std::array modes{FieldGroupMode::UseShared, FieldGroupMode::Inherit, FieldGroupMode::Local};
    const std::array modeNames{"UseShared", "Inherit", "Local"};
    for (const auto platform : HarkenTest::fieldGroupPlatforms) {
        for (std::size_t index = 0; index < modes.size(); ++index) {
            SCOPED_TRACE(std::string(HarkenTest::fieldGroupPlatformIdString(platform)) + ":" + modeNames[index]);
            const std::string platformJson = "{\"" + std::string(HarkenTest::fieldGroupPlatformIdString(platform))
                + "\":{\"mainOutput\":" + State(modeNames[index], kMainOutput)
                + ",\"send\":" + State(modeNames[index], kSend)
                + ",\"spatialRating\":" + State(modeNames[index], kRating) + "}}";
            DefinitionFieldGroups groups;
            std::string error;
            ASSERT_TRUE(ReadFieldGroupsJson(Block(kInherit, kInherit, kInherit, platformJson),
                                              groups, error)) << error;
            ASSERT_EQ(groups.mainOutput.platforms.size(), 1u);
            EXPECT_EQ(groups.mainOutput.platforms.at(platform).mode, modes[index]);
            EXPECT_TRUE(groups.mainOutput.platforms.at(platform).localHistory);
            EXPECT_EQ(groups.send.platforms.at(platform).mode, modes[index]);
            EXPECT_TRUE(groups.send.platforms.at(platform).localHistory);
            EXPECT_EQ(groups.spatialRating.platforms.at(platform).mode, modes[index]);
            EXPECT_TRUE(groups.spatialRating.platforms.at(platform).localHistory);
            std::string text;
            ASSERT_TRUE(WriteFieldGroupsJson(groups, text, error)) << error;
            DefinitionFieldGroups roundTrip;
            ASSERT_TRUE(ReadFieldGroupsJson(text, roundTrip, error)) << error;
            EXPECT_EQ(roundTrip, groups);
        }
    }
}

TEST(FieldGroupJson, AbsentPlatformGroupsAndExplicitUseSharedHaveNoInventedHistory)
{
    DefinitionFieldGroups groups;
    std::string error;
    ASSERT_TRUE(ReadFieldGroupsJson(Block(kInherit, kInherit, kInherit,
        R"({"7e69e0e2-8d15-52df-a609-ab96fff38750":{"mainOutput":{}},"19587786-cb98-5146-814a-088cee6f47c9":{}})"), groups, error)) << error;
    EXPECT_EQ(groups.mainOutput.platforms.size(), 1u);
    EXPECT_EQ(groups.mainOutput.platforms.at(HarkenTest::FieldGroupPlatformIds::Windows).mode, FieldGroupMode::UseShared);
    EXPECT_FALSE(groups.mainOutput.platforms.at(HarkenTest::FieldGroupPlatformIds::Windows).localHistory);
    EXPECT_FALSE(groups.mainOutput.platforms.contains(HarkenTest::FieldGroupPlatformIds::iOS));
    EXPECT_TRUE(groups.send.platforms.empty());
    EXPECT_TRUE(groups.spatialRating.platforms.empty());
    std::string text;
    ASSERT_TRUE(WriteFieldGroupsJson(groups, text, error)) << error;
    DefinitionFieldGroups roundTrip;
    ASSERT_TRUE(ReadFieldGroupsJson(text, roundTrip, error)) << error;
    EXPECT_EQ(roundTrip, groups);
}

TEST(FieldGroupJson, EqualPlatformLocalPayloadDoesNotCollapseIntoUseShared)
{
    const std::string platforms = "{\"7e69e0e2-8d15-52df-a609-ab96fff38750\":{\"mainOutput\":" + State("Local", kMainOutput) + "}}";
    DefinitionFieldGroups groups;
    std::string error;
    ASSERT_TRUE(ReadFieldGroupsJson(Block(State("Local", kMainOutput), kInherit, kInherit, platforms),
                                      groups, error)) << error;
    std::string text;
    ASSERT_TRUE(WriteFieldGroupsJson(groups, text, error)) << error;
    DefinitionFieldGroups roundTrip;
    ASSERT_TRUE(ReadFieldGroupsJson(text, roundTrip, error)) << error;
    ASSERT_TRUE(roundTrip.mainOutput.platforms.contains(HarkenTest::FieldGroupPlatformIds::Windows));
    EXPECT_EQ(roundTrip.mainOutput.platforms.at(HarkenTest::FieldGroupPlatformIds::Windows).mode, FieldGroupMode::Local);
    EXPECT_EQ(roundTrip, groups);
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

TEST(FieldGroupJson, RejectsLegacyEmptyAndPerFieldGroupBlocksAtomically)
{
    for (const auto* text : {R"({})", R"({"presence":"0000000000000000"})",
                             R"({"presence":"0000000000000040","mainOutputBusRef":"42"})",
                             R"({"overrideBits":0,"sends":[]})",
                             R"({"volume":0,"spatial":{"panning":0}})"})
        ExpectRejected(text);
    ExpectRejected(Block(State("Local", Replace(kMainOutput, "\"outputBusVolume\":0",
                                               "\"outputBusVolume\":0,\"presence\":\"0000000000000020\""))));
    ExpectRejected(Block(R"({"mode":"Inherit","overrideBits":0})"));
}

TEST(FieldGroupJson, RejectsUnknownMissingAndCrossGroupMembers)
{
    for (const auto& text : {
             Replace(Block(), "\"shared\":", "\"shared\":{},\"other\":"),
             Replace(Block(), "\"mainOutput\":{\"inherit\":true},", ""),
             Replace(Block(), "\"spatialRating\"", "\"attenuation\""),
             Block(R"({"mode":"Inherit","scope":"Shared"})"),
             Block(State("Local", R"({"mainOutputBusRef":"12345678-9abc-def0-0123-456789abcdef","outputBusVolume":0,"sends":[]})")),
             Block(kInherit, State("Local", R"({"sends":[],"spatialPanning":0})")),
             Block(kInherit, kInherit, State("Local", Replace(kRating, "\"spatialPanning\":0", "\"spatialPanning\":0,\"volume\":0")))})
        ExpectRejected(text);
}

TEST(FieldGroupJson, RejectsMissingOrMalformedInheritanceAndScope)
{
    for (const auto* state : {"{}", "null", "[]", R"({"inherit":null})", R"({"inherit":1})",
                              R"({"mode":""})", R"({"mode":"inherit"})", R"({"mode":"Unknown"})",
                              R"({})", R"({"mode":"Local"})"})
        ExpectRejected(Block(state));
    for (const auto* platforms : {"null", "[]", R"({"Universal":{}})", R"({"Shared":{}})",
                                  R"({"windows":{}})", R"({"Unknown":{}})", R"({"7e69e0e2-8d15-52df-a609-ab96fff38750":null})",
                                  R"({"7e69e0e2-8d15-52df-a609-ab96fff38750":{"unknown":{"mode":"Inherit"}}})",
                                  R"({"7e69e0e2-8d15-52df-a609-ab96fff38750":{"mainOutput":{"inherit":null}}})",
                                  R"({"7e69e0e2-8d15-52df-a609-ab96fff38750":{"mainOutput":{"mode":"Local"}}})"})
        ExpectRejected(Block(kInherit, kInherit, kInherit, platforms));
}

TEST(FieldGroupJson, EveryHistoryRequiresCompleteTypedPayloadEvenWhenDormant)
{
    for (const auto* mode : {"Inherit", "Local"}) {
        ExpectRejected(Block(State(mode, "null")));
        ExpectRejected(Block(State(mode, "{}")));
        ExpectRejected(Block(State(mode, R"({"mainOutputBusRef":"12345678-9abc-def0-0123-456789abcdef"})")));
        ExpectRejected(Block(State(mode, R"({"outputBusVolume":0})")));
        ExpectRejected(Block(kInherit, State(mode, "{}")));
        ExpectRejected(Block(kInherit, State(mode, R"({"sends":null})")));
        ExpectRejected(Block(kInherit, State(mode, R"({"sends":[{"targetBusID":"12345678-9abc-def0-0123-456789abcdef"}]})")));
        ExpectRejected(Block(kInherit, State(mode, R"({"sends":[{"sendLevel":0}]})")));
        for (const auto* field : {"spatialPanning", "spatialIs3DEnabled", "spatialAttenuationRef",
                                  "spatialAttenuationDistanceScaling"}) {
            const std::string fieldText = "\"" + std::string(field) + "\":";
            auto history = std::string(kRating);
            const auto start = history.find(fieldText);
            const auto end = history.find(',', start);
            if (end == std::string::npos) history.erase(start - 1, history.size() - start);
            else history.erase(start, end - start + 1);
            ExpectRejected(Block(kInherit, kInherit, State(mode, history)));
        }
    }
    ExpectRejected(Block(kInherit, kInherit, kInherit,
        R"({"7e69e0e2-8d15-52df-a609-ab96fff38750":{"mainOutput":{"mode":"UseShared","localHistory":{}}}})"));
    ExpectRejected(Block(kInherit, kInherit,
                         State("Inherit", Replace(kRating, "false", "0"))));
}

TEST(FieldGroupJson, RejectsInvalidGuidTypesAndLostIdentity)
{
    const std::string marker = "\"12345678-9abc-def0-0123-456789abcdef\"";
    for (const auto* reference : {"0", "18446744073709551615", "null", "\"42\"", "\"\"",
                                  "\"123456789abcdef00123456789abcdef\"",
                                  "\"{12345678-9abc-def0-0123-456789abcdef}\"",
                                  "\"12345678x9abc-def0-0123-456789abcdef\"",
                                  "\"12345678-9abc-def0-0123-456789abcdeg\"",
                                  "\"00000000-0000-0000-0000-000000000000\""})
        ExpectRejected(Block(State("Inherit", Replace(kMainOutput, marker, reference))));
    ExpectRejected(Block(kInherit, State("Inherit", Replace(kSend,
        "fedcba98-7654-3210-ffff-ffffffffffff", "00000000-0000-0000-0000-000000000000"))));
}

TEST(FieldGroupJson, RejectsInvalidFloatTypesOverflowAndOutOfRangeDormantHistory)
{
    for (const auto* value : {"null", "true", "\"0\"", "1e400", "1e-400", "1e39"})
        ExpectRejected(Block(State("Inherit", Replace(kMainOutput, "\"outputBusVolume\":0",
            "\"outputBusVolume\":" + std::string(value)))));
    ExpectRejected(Block(kInherit, State("Inherit", Replace(kSend, "-6", "25"))));
    ExpectRejected(Block(kInherit, kInherit, State("Inherit", Replace(kRating, "\"spatialPanning\":0", "\"spatialPanning\":2"))));
    ExpectRejected(Block(kInherit, kInherit, State("Inherit", Replace(kRating, "\"spatialAttenuationDistanceScaling\":1", "\"spatialAttenuationDistanceScaling\":0"))));
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
    groups.mainOutput.shared.mode = FieldGroupMode::Local;
    ExpectWriteRejected(groups);
    groups = {};
    groups.send.shared.mode = FieldGroupMode::UseShared;
    ExpectWriteRejected(groups);
    groups = {};
    groups.send.shared.mode = static_cast<FieldGroupMode>(255);
    ExpectWriteRejected(groups);
    groups = {};
    groups.send.platforms[HarkenTest::FieldGroupPlatformIds::Universal] = {};
    ExpectWriteRejected(groups);
    groups = {};
    groups.send.platforms[harkenprojectschema::AuthoringGuid{}] = {};
    ExpectWriteRejected(groups);
    groups = {};
    groups.mainOutput.shared.localHistory = MainOutputFieldGroup{};
    ExpectWriteRejected(groups);
    groups.mainOutput.shared.localHistory = MainOutputFieldGroup{kBus, std::numeric_limits<float>::quiet_NaN()};
    ExpectWriteRejected(groups);
    groups.mainOutput.shared.localHistory = MainOutputFieldGroup{kBus, std::numeric_limits<float>::infinity()};
    ExpectWriteRejected(groups);
    groups = {};
    groups.send.shared.localHistory = SendFieldGroup{{{kNullGuid, 0.0f}}};
    ExpectWriteRejected(groups);
}

TEST(FieldGroupJson, DuplicateDormantSendTargetsPersistUntilActivationValidation)
{
    DefinitionFieldGroups groups;
    groups.send.shared = {FieldGroupMode::Inherit,
                         SendFieldGroup{{{kBus, -6.0f}, {kBus, -12.0f}}}};
    std::string text;
    std::string error;
    ASSERT_TRUE(WriteFieldGroupsJson(groups, text, error)) << error;
    DefinitionFieldGroups roundTrip;
    ASSERT_TRUE(ReadFieldGroupsJson(text, roundTrip, error)) << error;
    EXPECT_EQ(roundTrip, groups);
    const std::array targets{FieldGroupReferenceTarget{kBus, FieldGroupReferenceKind::Bus}};
    const auto diagnostics = ValidateActiveFieldGroup(FieldGroupId::Send,
        *roundTrip.send.shared.localHistory, targets);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics.front().code, FieldGroupDiagnosticCode::DuplicateSendTarget);
}

TEST(FieldGroupJson, PropertyModelMarkerRejectsOldEmptyDocumentsAndInvalidHeaders)
{
    for (const auto* text : {"{}", "[]", R"({"content":[]})",
                             R"({"harkenDocument":{},"content":[]})",
                             R"({"harkenDocument":{"schemaVersion":6},"content":[]})",
                             R"({"harkenDocument":null})", R"({"harkenDocument":[]})",
                             R"({"harkenDocument":{"propertyModel":null}})",
                             R"({"harkenDocument":{"propertyModel":1}})",
                             R"({"harkenDocument":{"propertyModel":""}})",
                             R"({"harkenDocument":{"propertyModel":"FieldGroups"}})",
                             R"({"harkenDocument":{"propertyModel":"fieldGroups"}})",
                             R"({"harkenDocument":{"propertyModel":"playableProperties"}})",
                             R"({"propertyModel":"fieldGroups","harkenDocument":{}})",
                             R"({"harkenDocument":{"propertyModel":"fieldGroups","propertyModel":"old"}})"}) {
        SCOPED_TRACE(text);
        std::string error;
        EXPECT_FALSE(ValidateFieldGroupsPropertyModelMarkerJson(text, error));
        EXPECT_FALSE(error.empty());
    }
}

TEST(FieldGroupJson, PropertyModelMarkerOnlyValidatesTheAuthoringModelIdentifier)
{
    std::string error = "old error";
    EXPECT_TRUE(ValidateFieldGroupsPropertyModelMarkerJson(
        R"({"harkenDocument":{"propertyModel":"fieldGroupsV1"},"content":[]})", error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(ValidateFieldGroupsPropertyModelMarkerJson(
        R"({"harkenDocument":{"propertyModel":"fieldGroupsV1","schemaVersion":"unchecked"},"content":false})", error));
}

TEST(FieldGroupJson, OccurrenceRejectsGroupsAndTheirCanonicalAndLegacyFieldKeys)
{
    for (const auto* key : {"fieldGroups", "mainOutput", "send", "spatialRating", "spatial",
                            "mainOutputBusRef", "outputBusVolume", "sends", "spatialPanning",
                            "spatialIs3DEnabled", "spatialAttenuationRef",
                            "spatialAttenuationDistanceScaling", "spatial.panning",
                            "spatial.is3DEnabled", "spatial.attenuationRef", "spatial.attenuationDistanceScaling"}) {
        const std::string text = "{\"" + std::string(key) + "\":null}";
        SCOPED_TRACE(text);
        std::string error;
        EXPECT_FALSE(ValidateOccurrenceFieldGroupExclusionJson(text, error));
        EXPECT_FALSE(error.empty());
    }
}

TEST(FieldGroupJson, OccurrenceRejectsGroupPresenceEvenWithoutFields)
{
    for (const auto* mask : {"0000000000000020", "0000000000000040", "0000000000000080",
                             "0000000000000100", "0000000000000400",
                             "0000000000000800", "0000000000002000", "0000000000002fe0"}) {
        const std::string text = "{\"playableProperties\":{\"presence\":\"" + std::string(mask) + "\"}}";
        SCOPED_TRACE(text);
        std::string error;
        EXPECT_FALSE(ValidateOccurrenceFieldGroupExclusionJson(text, error));
        EXPECT_FALSE(error.empty());
    }
    for (const auto* presence : {"null", "8192", "\"2000\"", "\"000000000000zzzz\""}) {
        std::string error;
        EXPECT_FALSE(ValidateOccurrenceFieldGroupExclusionJson(
            "{\"presence\":" + std::string(presence) + "}", error));
    }
}

TEST(FieldGroupJson, OccurrenceChecksOnlyKnownPropertyEnvelopes)
{
    for (const auto* text : {
             R"({"playableProperties":{"mainOutputBusRef":"42"}})",
             R"({"properties":{"send":{}}})",
             R"({"components":{"mixnode":{"outputBusVolume":0}}})",
             R"({"components":{"mixnode":{"outputBusId":0}}})",
             R"({"components":{"audio":{"outputBusOverride":false}}})",
             R"({"components":{"audio":{"overrideBits":0}}})",
             R"({"components":{"spatial":{"panning":0}}})",
             R"({"platformOverrides":[{"platform":"Windows","sends":[]}]})",
             R"({"playableProperties":{"platformOverrides":[{"platform":"iOS","presence":"0000000000002000"}]}})",
             R"({"properties":null})", R"({"platformOverrides":{}})"}) {
        SCOPED_TRACE(text);
        std::string error;
        EXPECT_FALSE(ValidateOccurrenceFieldGroupExclusionJson(text, error));
        EXPECT_FALSE(error.empty());
    }
    for (const auto* text : {
             R"({"playableProperties":{"presence":"0000000000000003","volume":0,"pitch":12},"volumeBinding":[],"pitchBinding":[]})",
             R"({"platformOverrides":[{"platform":"Windows","presence":"0000000000000003","volume":-6,"pitch":0}]})",
             R"({"components":{"mixnode":{"volume":-6},"bindings":{"sends":0,"spatialPanning":0}}})",
             R"({"components":{"spatial":{"spatializationMix":0.5}}})",
             R"({"userData":{"fieldGroups":{},"presence":"0000000000002fe0"},"bindings":[{"field":"spatialPanning","properties":{"send":{}}}]})",
             R"({"target":{"fieldGroups":{}},"name":"mainOutputBusRef"})"}) {
        SCOPED_TRACE(text);
        std::string error = "old error";
        EXPECT_TRUE(ValidateOccurrenceFieldGroupExclusionJson(text, error)) << error;
        EXPECT_TRUE(error.empty());
    }
}

}
}
