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
constexpr char kOutput[] = R"({"outputBusRef":"12345678-9abc-def0-0123-456789abcdef","outputBusVolume":0})";
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

std::string Block(std::string_view output = kInherit,
                  std::string_view send = kInherit,
                  std::string_view rating = kInherit,
                  std::string_view platforms = {},
                  std::string_view effects = kInherit)
{
    std::string result = "{\"shared\":{\"output\":" + std::string(output)
        + ",\"send\":" + std::string(send) + ",\"spatialRating\":" + std::string(rating)
        + ",\"effect\":" + std::string(effects) + "}";
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
    groups.output.shared = {FieldGroupMode::Local, OutputFieldGroup{kBus, -12.0f}};
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
    ASSERT_TRUE(ReadFieldGroupsJson(Block(State("Inherit", kOutput),
                                           State("Inherit", kSend),
                                           State("Inherit", rating)), groups, error)) << error;
    ASSERT_TRUE(groups.send.shared.localHistory);
    ASSERT_EQ(groups.send.shared.localHistory->sends.size(), 1u);
    EXPECT_EQ(groups.send.shared.localHistory->sends[0].targetBusID, kSendBus);
    ASSERT_TRUE(groups.spatialRating.shared.localHistory);
    EXPECT_EQ(groups.spatialRating.shared.localHistory->spatialAttenuationRef,
              (AuthoringGuid{UINT64_MAX, UINT64_MAX}));
    EffectEntry effect;
    effect.guid = QUuid(QStringLiteral("12345678-9abc-def0-0123-456789abcdef"));
    effect.effectGuid = QUuid(QStringLiteral("ffffffff-ffff-ffff-ffff-ffffffffffff"));
    EffectFieldGroup effects;
    ASSERT_TRUE(effects.effects.replace({effect}));
    groups.effects.shared.localHistory = effects;
    std::string text;
    ASSERT_TRUE(WriteFieldGroupsJson(groups, text, error)) << error;
    DefinitionFieldGroups roundTrip;
    ASSERT_TRUE(ReadFieldGroupsJson(text, roundTrip, error)) << error;
    EXPECT_EQ(roundTrip, groups);
}

TEST(FieldGroupJson, UuidCaseNormalizesWithoutChangingIdentity)
{
    const auto output = Replace(kOutput, "12345678-9abc-def0-0123-456789abcdef",
                                    "12345678-9ABC-DEF0-0123-456789ABCDEF");
    DefinitionFieldGroups groups;
    std::string error;
    ASSERT_TRUE(ReadFieldGroupsJson(Block(State("Local", output)), groups, error)) << error;
    EXPECT_EQ(groups.output.shared.localHistory->outputBusRef, kBus);
}

TEST(FieldGroupJson, FiniteFloatBitsAndTableRowOrderRoundTripDeterministically)
{
    DefinitionFieldGroups groups;
    groups.output.shared = {FieldGroupMode::Local, OutputFieldGroup{kBus, -0.0f}};
    groups.send.shared = {FieldGroupMode::Local,
                         SendFieldGroup{{{kSendBus, 0.1f, SendMode::Sidechain,
                                          {{SendMode::Mix, -12.0f}, {SendMode::Ducking, 3.0f}}},
                                         {kBus, -0.0f, SendMode::Ducking,
                                          {{SendMode::Sidechain, -96.0f}}}}}};
    SpatialRatingFieldGroup rating;
    rating.spatialPanning = std::numeric_limits<float>::denorm_min();
    rating.spatialAttenuationDistanceScaling = 3.333333f;
    groups.spatialRating.shared = {FieldGroupMode::Local, rating};
    EffectEntry custom;
    custom.guid = QUuid(QStringLiteral("a0b1c2d3-e4f5-4638-9876-123456789abc"));
    custom.mode = EffectMode::Custom;
    custom.custom.parameters = {-0.1f, 3.333333f};
    custom.wetDry = 0.1234567f;
    custom.extensions.insert(QStringLiteral("extension"), QJsonObject{
        {QStringLiteral("values"), QJsonArray{QJsonValue::Null, true,
            QJsonValue(qint64{9007199254740993}), QString::fromLatin1("a\0b", 3)}}});
    EffectEntry preset;
    preset.guid = QUuid(QStringLiteral("a0b1c2d3-e4f5-4638-9876-123456789abd"));
    preset.effectGuid = QUuid(QStringLiteral("fedcba98-7654-3210-ffff-ffffffffffff"));
    preset.bypass = true;
    EffectFieldGroup effects;
    ASSERT_TRUE(effects.effects.replace({custom, preset}));
    groups.effects.shared = {FieldGroupMode::Local, effects};
    preset.wetDry = 0.5f;
    ASSERT_TRUE(effects.effects.replace({preset, custom}));
    groups.effects.platforms[{0, 101}] = {FieldGroupMode::UseShared, effects};
    std::string text;
    std::string error;
    ASSERT_TRUE(WriteFieldGroupsJson(groups, text, error)) << error;
    DefinitionFieldGroups roundTrip;
    ASSERT_TRUE(ReadFieldGroupsJson(text, roundTrip, error)) << error;
    EXPECT_EQ(roundTrip, groups);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(roundTrip.output.shared.localHistory->outputBusVolume),
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
    for (const auto* value : {"null", "true", "\"0\"", "1e400", "1e-400", "1e39"}) {
        ExpectRejected(Block(State("Inherit", Replace(kOutput, "\"outputBusVolume\":0",
            "\"outputBusVolume\":" + std::string(value)))));
        ExpectRejected(Block(kInherit, State("Inherit", Replace(kSend, "\"sendLevel\":-6",
            "\"sendLevel\":-6,\"modeVolumes\":{\"1\":" + std::string(value) + "}"))));
    }
    for (const auto* volumes : {R"({"0":-12})", R"({"01":-12})", R"({"3":-12})",
                                R"({"1":-97})", R"({"1":25})", "[]"})
        ExpectRejected(Block(kInherit, State("Inherit", Replace(kSend, "\"sendLevel\":-6",
            "\"sendLevel\":-6,\"modeVolumes\":" + std::string(volumes)))));
}

TEST(FieldGroupJson, RejectsDuplicateMembersAndMalformedJson)
{
    ExpectRejected(Block() + "{}");
    ExpectRejected(Replace(Block(), "\"inherit\":true", "\"inherit\":true,\"inherit\":false"));
    ExpectRejected(Block(kInherit, kInherit, kInherit, R"({"7e69e0e2-8d15-52df-a609-ab96fff38750":{},"7e69e0e2-8d15-52df-a609-ab96fff38750":{}})"));
    ExpectRejected(Replace(Block(), "\"shared\":", "\"shared\":{},\"shared\":"));
    ExpectRejected(Replace(Block(), "\"shared\":", "/*comment*/\"shared\":"));
    ExpectRejected(Replace(Block(), "\"output\":", "\"output\":,"));
    ExpectRejected("[]");
}

TEST(FieldGroupJson, WriterRejectsInvalidStatesWithoutChangingOutput)
{
    DefinitionFieldGroups groups;
    groups.output.shared.localHistory = OutputFieldGroup{kBus, std::numeric_limits<float>::quiet_NaN()};
    ExpectWriteRejected(groups);
    groups.output.shared.localHistory = OutputFieldGroup{kBus, std::numeric_limits<float>::infinity()};
    ExpectWriteRejected(groups);
}

}
}
