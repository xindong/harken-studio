#include "UnitTest/StudioObjectIdTestSupport.h"
#include "BindingSemantics.h"
#include "CommandArgumentValidator.h"
#include "CommandSchemas.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>

namespace {

QUuid guid(int value)
{
    return QUuid(QStringLiteral("{%1-0000-0000-0000-000000000000}")
        .arg(value, 8, 16, QLatin1Char('0')));
}

}

TEST(BindingSemantics, TypedLedgerRoundtripsCurveAndExtensions)
{
    harken::studio::ParameterBindingDef binding = harken::studio::BindingSemantics::defaultBinding(
        harken::studio::BindingSemantics::volumeAddress(), guid(3));
    binding.guid = harken::studio::authoringUuid(harkentest::StudioObjectId(3));
    binding.extensions[QStringLiteral("future")] = 42;
    binding.curveExtensions[QStringLiteral("interpolation")] = QStringLiteral("future");
    harken::studio::BindingLedger ledger;
    ASSERT_TRUE(ledger.upsert(binding));

    const QJsonArray saved = ledger.toJson();
    harken::studio::BindingLedger loaded = harken::studio::BindingLedger::fromJson(saved);
    ASSERT_NE(loaded.findFirst(harken::studio::BindingSemantics::volumeAddress()), nullptr);
    EXPECT_EQ(loaded.toJson(), saved);
    EXPECT_EQ(loaded.toJson().first().toObject()[QStringLiteral("future")].toInt(), 42);
    EXPECT_EQ(loaded.toJson().first().toObject()[QStringLiteral("curve")]
                  .toObject()[QStringLiteral("interpolation")].toString(),
              QStringLiteral("future"));
    QString error;
    EXPECT_TRUE(harken::studio::validateCommandArguments({{"path", "Sound"}, {"binding", saved.first()}},
        harken::studio::commandInputSchema("set_binding"), &error)) << error.toStdString();
    EXPECT_TRUE(harken::studio::validateCommandArguments({{"path", "Sound"}, {"bindings", saved}},
        harken::studio::commandInputSchema("set_modulator_binding"), &error)) << error.toStdString();
}

TEST(BindingSemantics, AddressFamiliesRoundtripWithoutAliasing)
{
    const auto playable = harken::studio::BindingSemantics::volumeAddress();
    const auto filter = harken::studio::BindingContractSemantics::filterAddress(
        playable.slotId, harken::studio::ProjectItemType::Sound);
    ASSERT_NE(playable, filter);
    EXPECT_NE(harken::studio::BindingSemantics::addressKey(playable),
              harken::studio::BindingSemantics::addressKey(filter));
    harken::studio::BindingLedger ledger;
    auto volume = harken::studio::BindingSemantics::defaultBinding(playable, guid(3));
    volume.guid = guid(4);
    auto frequency = harken::studio::BindingSemantics::defaultBinding(filter, guid(3));
    frequency.guid = guid(5);
    ASSERT_TRUE(ledger.upsert(volume));
    ASSERT_TRUE(ledger.upsert(frequency));

    const auto loaded = harken::studio::BindingLedger::fromJson(ledger.toJson());
    ASSERT_EQ(loaded.size(), 2);
    ASSERT_NE(loaded.findFirst(playable), nullptr);
    ASSERT_NE(loaded.findFirst(filter), nullptr);
    EXPECT_EQ(loaded.findFirst(playable)->guid, volume.guid);
    EXPECT_EQ(loaded.findFirst(filter)->guid, frequency.guid);
    EXPECT_EQ(loaded.toJson(), ledger.toJson());
    for (float input : {0.0f, 0.5f, 1.0f})
        EXPECT_FLOAT_EQ(harken::studio::BindingSemantics::contribution(
            *loaded.findFirst(filter), harken::studio::VariableKind::Smooth, input, 0.0f, 1.0f), 0.0f);
    EXPECT_EQ(harken::studio::BindingContractSemantics::engineTargetId(filter), std::nullopt);
    EXPECT_NE(harken::studio::BindingContractSemantics::engineTargetId(playable), std::nullopt);
}

TEST(BindingSemantics, FixedAuthoringRangePreservesAbsoluteCurveValues)
{
    const auto address = harken::studio::BindingSemantics::pitchAddress();
    auto original = harken::studio::BindingSemantics::defaultBinding(address, guid(3));
    original.guid = guid(4);
    original.outputMin = -7200.0f;
    original.outputMax = 7200.0f;
    original.curve = {{0.0f, -6000.0f}, {1.0f, 6500.0f}};
    original.handles = {{0.25f, -7000.0f, 0.75f, 7000.0f}};
    const auto contract = harken::studio::BindingContractSemantics::derive(address);

    auto loaded = harken::studio::ParameterBindingDef::fromJson(original.toJson());
    ASSERT_TRUE(loaded.typed);
    EXPECT_FLOAT_EQ(loaded.outputMin, contract.legalOutputRange.min);
    EXPECT_FLOAT_EQ(loaded.outputMax, contract.legalOutputRange.max);
    EXPECT_EQ(loaded.toJson().toObject()[QStringLiteral("curve")],
              original.toJson().toObject()[QStringLiteral("curve")]);
    for (float input : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
        EXPECT_FLOAT_EQ(harken::studio::BindingSemantics::contribution(
            loaded, harken::studio::VariableKind::Smooth, input, 0.0f, 1.0f),
            harken::studio::BindingSemantics::contribution(
                original, harken::studio::VariableKind::Smooth, input, 0.0f, 1.0f));

    loaded.curve.front().value = contract.legalOutputRange.min;
    loaded.curve.back().value = contract.legalOutputRange.max;
    const auto edited = harken::studio::ParameterBindingDef::fromJson(loaded.toJson());
    EXPECT_EQ(edited.toJson().toObject()[QStringLiteral("curve")],
              loaded.toJson().toObject()[QStringLiteral("curve")]);
}
