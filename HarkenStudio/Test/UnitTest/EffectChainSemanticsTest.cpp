#include "UnitTest/StudioObjectIdTestSupport.h"
#include "gtest/gtest.h"

#include "Core/EffectChainSemantics.h"
#include "Core/Components/VMixNode.h"
#include "Core/ProjectSerializer.h"

using namespace VNX;

TEST(EffectChainSemanticsTest, TypedFiveEffectsRoundtrip)
{
    std::vector<EffectEntry> entries;
    uint64_t id = 1;
    for (EffectType type : {EffectType::FeedbackDelay, EffectType::ThreeBandTone,
                            EffectType::Gain, EffectType::Limiter,
                            EffectType::Compressor}) {
        EffectEntry entry;
        entry.guid = VNX::authoringUuid(harkentest::StudioObjectId(id++));
        entry.type = type;
        entry.parameters = EffectChainSemantics::defaultParameters(type);
        entries.push_back(entry);
    }
    EffectChainLedger ledger;
    ASSERT_TRUE(ledger.replace(entries));
    const EffectChainLedger parsed = EffectChainLedger::fromJson(ledger.toJson());
    EXPECT_EQ(parsed.toJson(), ledger.toJson());
}

TEST(EffectChainSemanticsTest, RejectsUnknownAndInvalidParameters)
{
    const EffectChainLedger unknown = EffectChainLedger::fromJson(
        QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("plugin")},
                               {QStringLiteral("parameters"), QJsonArray{0.0}}}});
    ASSERT_EQ(unknown.size(), 1u);
    EXPECT_FALSE(unknown.entries().front().typed);

    EffectEntry invalid;
    invalid.guid = VNX::authoringUuid(harkentest::StudioObjectId(1));
    invalid.type = EffectType::Gain;
    invalid.parameters = {100.0f};
    EXPECT_FALSE(EffectChainSemantics::validate(invalid));
    EffectChainLedger ledger;
    EXPECT_FALSE(ledger.replace({invalid}));
}

TEST(EffectChainSemanticsTest, MixNodePersistsTypedChain)
{
    ProjectItem item(QStringLiteral("Sound"), ProjectItem::Type::Sound);
    item.guid = QUuid::createUuid();
    item.attachDefaultComponents();
    auto* mix = item.comp<VMixNode>();
    EffectEntry gain;
    gain.guid = VNX::authoringUuid(harkentest::StudioObjectId(10));
    gain.type = EffectType::Gain;
    gain.parameters = {3.0f};
    ASSERT_TRUE(mix->effects.replace({gain}));

    ProjectSerializer serializer;
    const QJsonObject json = serializer.projectItemToJson(item);
    ProjectItem loaded;
    ASSERT_TRUE(serializer.projectItemFromJson(json, loaded));
    ASSERT_NE(loaded.comp<VMixNode>(), nullptr);
    EXPECT_EQ(loaded.comp<VMixNode>()->effects.toJson(), mix->effects.toJson());
}
