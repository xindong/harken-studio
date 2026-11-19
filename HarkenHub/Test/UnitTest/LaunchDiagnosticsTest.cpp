#include "Support/StudioProtocolTestUtils.h"
#include <QJsonDocument>
#include "Versions/LaunchDiagnostics.h"
#include "Versions/SelfTestRunner.h"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QTemporaryDir>

using VNH::LaunchDiagnostics;
using VNH::SelfTestRunner;

namespace {

const QDateTime kNow =
    QDateTime::fromString(QStringLiteral("2026-07-24T12:00:00Z"), Qt::ISODate);

class LaunchDiagnosticsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        qputenv("HARKEN_HUB_USER_ROOT", dir.path().toUtf8());
    }
    void TearDown() override { qunsetenv("HARKEN_HUB_USER_ROOT"); }

    QTemporaryDir dir;
};

TEST_F(LaunchDiagnosticsTest, RecordsAndReadsBackStates)
{
    ASSERT_TRUE(LaunchDiagnostics::record(QStringLiteral("run1.aaaa1111"),
                                          QStringLiteral("SelfTesting"),
                                          QString(), kNow));
    ASSERT_TRUE(LaunchDiagnostics::record(QStringLiteral("run1.aaaa1111"),
                                          QStringLiteral("Ready"), QString(),
                                          kNow));
    ASSERT_TRUE(LaunchDiagnostics::record(
        QStringLiteral("run2.bbbb2222"), QStringLiteral("LaunchIssue"),
        QStringLiteral("Self-test crashed"), kNow));

    const auto snapshot = LaunchDiagnostics::snapshot();
    ASSERT_EQ(snapshot.size(), 2);
    EXPECT_EQ(snapshot[QStringLiteral("run1.aaaa1111")].userLaunchState,
              QStringLiteral("Ready"));
    EXPECT_FALSE(snapshot[QStringLiteral("run1.aaaa1111")]
                     .lastSelfTestAt.isEmpty());
    EXPECT_TRUE(snapshot[QStringLiteral("run1.aaaa1111")]
                    .lastLaunchError.isEmpty());
    EXPECT_EQ(snapshot[QStringLiteral("run2.bbbb2222")].userLaunchState,
              QStringLiteral("LaunchIssue"));
    EXPECT_EQ(snapshot[QStringLiteral("run2.bbbb2222")].lastLaunchError,
              QStringLiteral("Self-test crashed"));
}

TEST_F(LaunchDiagnosticsTest, RejectsEmptyBuildId)
{
    EXPECT_FALSE(LaunchDiagnostics::record(QString(), QStringLiteral("Ready"),
                                           QString(), kNow));
}

TEST(SelfTestParse, AcceptsMatchingReport)
{
    const auto output = QJsonDocument(HarkenHubTest::studioSelfTest()).toJson();
    const auto result = SelfTestRunner::parseOutput(output, QStringLiteral("run1.aaaa1111"));
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(result.reportedVersion, QStringLiteral("1.4.0"));
}

TEST(SelfTestParse, RejectsInvalidIdentityProtocolAndDiagnosticFields)
{
    for (const QJsonObject& overrides : {
            QJsonObject{{QStringLiteral("product"), QStringLiteral("OtherProduct")}},
            QJsonObject{{QStringLiteral("buildId"), QStringLiteral("other-build")}},
            QJsonObject{{QStringLiteral("version"), 4}},
            QJsonObject{{QStringLiteral("channel"), 4}},
            QJsonObject{{QStringLiteral("ok"), false}},
            QJsonObject{{QStringLiteral("ok"), QStringLiteral("true")}}}) {
        auto report = HarkenHubTest::studioSelfTest();
        for (auto it = overrides.begin(); it != overrides.end(); ++it)
            report.insert(it.key(), it.value());
        EXPECT_FALSE(SelfTestRunner::parseOutput(QJsonDocument(report).toJson(),
            QStringLiteral("run1.aaaa1111")).ok) << QJsonDocument(overrides).toJson().constData();
    }
}

TEST(SelfTestParse, RejectsIncompleteAndGarbageReports)
{
    EXPECT_FALSE(SelfTestRunner::parseOutput("{\"ok\":true,\"buildId\":\"old\"}", {}).ok);
    EXPECT_FALSE(SelfTestRunner::parseOutput("not json", {}).ok);
}

}
