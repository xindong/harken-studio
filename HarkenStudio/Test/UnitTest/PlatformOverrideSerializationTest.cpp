#include "PlayablePropertyAuthoringV1.h"
#include "FilterSemantics.h"
#include "Support/ProjectPlatformTestUtils.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>

namespace {

using VNX::PlayablePropertyAuthoringV1;

QJsonObject overlayRowJson(const QString& platform, double volume)
{
    return QJsonObject{
        {QStringLiteral("platformId"), platform},
        {QStringLiteral("presence"), QStringLiteral("0000000000000001")},
        {QStringLiteral("volume"), volume},
    };
}

void expectOverlayLoadRejected(const QJsonValue& value)
{
    VNX::PlatformOverrideLedgerV1 ledger;
    QString error;
    EXPECT_FALSE(PlayablePropertyAuthoringV1::platformOverridesFromJson(
        value, &ledger, &error));
}

TEST(PlatformOverrideSerialization, DuplicatePlatformRowsAreRejected)
{
    expectOverlayLoadRejected(QJsonArray{
        overlayRowJson(HarkenTest::platformIdString(QStringLiteral("Android")), -12.0),
        overlayRowJson(HarkenTest::platformIdString(QStringLiteral("Android")), -3.0),
    });
}

TEST(PlatformOverrideSerialization, MissingOrNonStringPlatformIsRejected)
{
    QJsonObject missing = overlayRowJson(HarkenTest::platformIdString(QStringLiteral("Android")), -12.0);
    missing.remove(QStringLiteral("platformId"));
    expectOverlayLoadRejected(QJsonArray{missing});

    QJsonObject numeric = overlayRowJson(HarkenTest::platformIdString(QStringLiteral("Android")), -12.0);
    numeric[QStringLiteral("platformId")] = 1;
    expectOverlayLoadRejected(QJsonArray{numeric});
}

TEST(PlatformOverrideSerialization, IndependentAuthoringGroupsSurviveRoundTripAndPruning)
{
    const auto platform = HarkenTest::platformId(QStringLiteral("Android"));
    VNX::PlatformOverrideLedgerV1 authored;
    auto* row = authored.ensure(platform);
    row->properties.presence.Set(harkenplayableproperties::PlayablePropertyIdV1::Volume);
    ASSERT_TRUE(row->properties.setValue(harkenplayableproperties::PlayablePropertyIdV1::Volume, -6.0f));
    row->filter = QJsonObject{{QStringLiteral("lowPass"), QJsonObject{{QStringLiteral("frequency"), 1000.0}}}};
    const auto json = PlayablePropertyAuthoringV1::platformOverridesToJson(authored);
    VNX::PlatformOverrideLedgerV1 restored;
    QString error;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::platformOverridesFromJson(json, &restored, &error, false, true))
        << error.toStdString();
    EXPECT_EQ(restored, authored);
    restored.find(platform)->properties.presence.bits = 0;
    EXPECT_FALSE(restored.eraseEmpty(platform));
    EXPECT_EQ(restored.find(platform)->filter, authored.find(platform)->filter);
    restored.find(platform)->filter = {};
    EXPECT_TRUE(restored.eraseEmpty(platform));
    EXPECT_TRUE(restored.rows.empty());
}

TEST(PlatformOverrideSerialization, InvalidAuthoringExtensionDoesNotReplaceExistingLedger)
{
    VNX::PlatformOverrideLedgerV1 ledger;
    ledger.ensure(HarkenTest::platformId(QStringLiteral("Windows")));
    const auto before = ledger;
    auto row = overlayRowJson(HarkenTest::platformIdString(QStringLiteral("Android")), -12.0);
    auto filter = VNX::FilterSemantics::toJson(VNX::FilterState{});
    auto section = filter.value(QStringLiteral("lowPass")).toObject();
    section.insert(QStringLiteral("unexpected"), true);
    filter.insert(QStringLiteral("lowPass"), section);
    row.insert(QStringLiteral("filter"), filter);
    QString error;
    EXPECT_FALSE(PlayablePropertyAuthoringV1::platformOverridesFromJson(
        QJsonArray{row}, &ledger, &error, false, true));
    EXPECT_EQ(ledger, before);
}

}
