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
