#include "UnitTest/StudioObjectIdTestSupport.h"
#include "BindingSemantics.h"

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
    VNX::ParameterBindingDef binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), guid(3));
    binding.guid = VNX::authoringUuid(harkentest::StudioObjectId(3));
    binding.extensions[QStringLiteral("future")] = 42;
    binding.curveExtensions[QStringLiteral("interpolation")] = QStringLiteral("future");
    VNX::BindingLedger ledger;
    ASSERT_TRUE(ledger.upsert(binding));

    const QJsonArray saved = ledger.toJson();
    VNX::BindingLedger loaded = VNX::BindingLedger::fromJson(saved);
    ASSERT_NE(loaded.findFirst(VNX::BindingSemantics::volumeAddress()), nullptr);
    EXPECT_EQ(loaded.toJson(), saved);
    EXPECT_EQ(loaded.toJson().first().toObject()[QStringLiteral("future")].toInt(), 42);
    EXPECT_EQ(loaded.toJson().first().toObject()[QStringLiteral("curve")]
                  .toObject()[QStringLiteral("interpolation")].toString(),
              QStringLiteral("future"));
}

TEST(BindingSemantics, AddressFamiliesRoundtripWithoutAliasing)
{
    const auto playable = VNX::BindingSemantics::volumeAddress();
    const auto filter = VNX::BindingContractSemantics::filterAddress(
        playable.slotId, VNX::ProjectItemType::Sound);
    ASSERT_NE(playable, filter);
    EXPECT_NE(VNX::BindingSemantics::addressKey(playable),
              VNX::BindingSemantics::addressKey(filter));
    VNX::BindingLedger ledger;
    auto volume = VNX::BindingSemantics::defaultBinding(playable, guid(3));
    volume.guid = guid(4);
    auto frequency = VNX::BindingSemantics::defaultBinding(filter, guid(3));
    frequency.guid = guid(5);
    ASSERT_TRUE(ledger.upsert(volume));
    ASSERT_TRUE(ledger.upsert(frequency));

    const auto loaded = VNX::BindingLedger::fromJson(ledger.toJson());
    ASSERT_EQ(loaded.size(), 2);
    ASSERT_NE(loaded.findFirst(playable), nullptr);
    ASSERT_NE(loaded.findFirst(filter), nullptr);
    EXPECT_EQ(loaded.findFirst(playable)->guid, volume.guid);
    EXPECT_EQ(loaded.findFirst(filter)->guid, frequency.guid);
    EXPECT_EQ(loaded.toJson(), ledger.toJson());
    for (float input : {0.0f, 0.5f, 1.0f})
        EXPECT_FLOAT_EQ(VNX::BindingSemantics::contribution(
            *loaded.findFirst(filter), VNX::VariableKind::Smooth, input, 0.0f, 1.0f), 0.0f);
    EXPECT_EQ(VNX::BindingContractSemantics::engineTargetId(filter), std::nullopt);
    EXPECT_NE(VNX::BindingContractSemantics::engineTargetId(playable), std::nullopt);
}

TEST(BindingSemantics, FixedAuthoringRangePreservesAbsoluteCurveValues)
{
    const auto address = VNX::BindingSemantics::pitchAddress();
    auto original = VNX::BindingSemantics::defaultBinding(address, guid(3));
    original.guid = guid(4);
    original.outputMin = -7200.0f;
    original.outputMax = 7200.0f;
    original.curve = {{0.0f, -6000.0f}, {1.0f, 6500.0f}};
    original.handles = {{0.25f, -7000.0f, 0.75f, 7000.0f}};
    const auto contract = VNX::BindingContractSemantics::derive(address);

    auto loaded = VNX::ParameterBindingDef::fromJson(original.toJson());
    ASSERT_TRUE(loaded.typed);
    EXPECT_FLOAT_EQ(loaded.outputMin, contract.legalOutputRange.min);
    EXPECT_FLOAT_EQ(loaded.outputMax, contract.legalOutputRange.max);
    EXPECT_EQ(loaded.toJson().toObject()[QStringLiteral("curve")],
              original.toJson().toObject()[QStringLiteral("curve")]);
    for (float input : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
        EXPECT_FLOAT_EQ(VNX::BindingSemantics::contribution(
            loaded, VNX::VariableKind::Smooth, input, 0.0f, 1.0f),
            VNX::BindingSemantics::contribution(
                original, VNX::VariableKind::Smooth, input, 0.0f, 1.0f));

    loaded.curve.front().value = contract.legalOutputRange.min;
    loaded.curve.back().value = contract.legalOutputRange.max;
    const auto edited = VNX::ParameterBindingDef::fromJson(loaded.toJson());
    EXPECT_EQ(edited.toJson().toObject()[QStringLiteral("curve")],
              loaded.toJson().toObject()[QStringLiteral("curve")]);
}
