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

TEST(BindingSemantics, FixedAuthoringRangePreservesAbsoluteCurveValues)
{
    const auto address = VNX::BindingSemantics::pitchAddress();
    auto original = VNX::BindingSemantics::defaultBinding(address, guid(3));
    original.guid = guid(4);
    original.outputMin = -7200.0f;
    original.outputMax = 7200.0f;
    original.curve = {{0.0f, -6000.0f}, {1.0f, 6500.0f}};
    original.handles = {{0.25f, -7000.0f, 0.75f, 7000.0f}};
    const auto contract = VNX::BindingContractSemantics::derive(address.slotId, VNX::ProjectItemType::Sound);

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
