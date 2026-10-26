#include "PlayablePropertyAuthoringV1.h"
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

}
