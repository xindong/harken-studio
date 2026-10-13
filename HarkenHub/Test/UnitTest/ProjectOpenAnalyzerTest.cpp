#include "Launch/ProjectOpenAnalyzer.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

QByteArray reportFor(const QString& path)
{
    return QJsonDocument(QJsonObject{
        {QStringLiteral("protocolVersion"), 1},
        {QStringLiteral("ok"), true},
        {QStringLiteral("product"), QStringLiteral("HarkenStudio")},
        {QStringLiteral("version"), QStringLiteral("0.1.20-dev")},
        {QStringLiteral("buildId"), QStringLiteral("run2.bbbb2222")},
        {QStringLiteral("requestId"), QStringLiteral("request")},
        {QStringLiteral("projectPath"), path},
        {QStringLiteral("projectId"), QStringLiteral("11111111-2222-3333-4444-555555555555")},
        {QStringLiteral("currentSchema"), 1},
        {QStringLiteral("targetSchema"), 2},
        {QStringLiteral("upgradeRequired"), true},
        {QStringLiteral("migrationStepCount"), 1},
        {QStringLiteral("affectedFileCount"), 3},
        {QStringLiteral("requiredProjectBytes"), 4096},
        {QStringLiteral("requiredRecoveryBytes"), 0},
        {QStringLiteral("recoveryRoot"), QString()},
        {QStringLiteral("protection"), QStringLiteral("atomicInPlace")},
        {QStringLiteral("approvalDigest"), QString(64, QLatin1Char('a'))},
        {QStringLiteral("errorCode"), QString()},
        {QStringLiteral("technicalDetails"), QString()},
    }).toJson(QJsonDocument::Compact);
}

}

TEST(ProjectOpenAnalyzer, ParsesBoundUpgradeReport)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = directory.filePath(QStringLiteral("Test.hsp"));
    QFile file(projectPath);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("{}");
    file.close();

    const VNH::ProjectOpenAnalysis result =
        VNH::ProjectOpenAnalyzer::parseOutput(
            reportFor(projectPath), QStringLiteral("request"),
            QStringLiteral("run2.bbbb2222"), projectPath);
    ASSERT_TRUE(result.reportValid);
    ASSERT_TRUE(result.ok);
    EXPECT_TRUE(result.upgradeRequired);
    EXPECT_EQ(result.currentSchema, 1);
    EXPECT_EQ(result.targetSchema, 2);
    EXPECT_EQ(result.affectedFileCount, 3);
}

TEST(ProjectOpenAnalyzer, RejectsAReportForAnotherRequest)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = directory.filePath(QStringLiteral("Test.hsp"));
    QFile file(projectPath);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("{}");
    file.close();

    const VNH::ProjectOpenAnalysis result =
        VNH::ProjectOpenAnalyzer::parseOutput(
            reportFor(projectPath), QStringLiteral("another-request"),
            QStringLiteral("run2.bbbb2222"), projectPath);
    EXPECT_FALSE(result.reportValid);
    EXPECT_EQ(result.errorCode, QStringLiteral("ANALYSIS_REQUEST_MISMATCH"));
}

TEST(ProjectOpenAnalyzer, PreservesStructuredBlockingError)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = directory.filePath(QStringLiteral("Test.hsp"));
    QFile file(projectPath);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("{}");
    file.close();
    QJsonObject report = QJsonDocument::fromJson(reportFor(projectPath)).object();
    report[QStringLiteral("ok")] = false;
    report[QStringLiteral("upgradeRequired")] = false;
    report[QStringLiteral("currentSchema")] = 0;
    report[QStringLiteral("approvalDigest")] = QString();
    report[QStringLiteral("errorCode")] =
        QStringLiteral("PROJECT_FILE_DAMAGED");

    const VNH::ProjectOpenAnalysis result =
        VNH::ProjectOpenAnalyzer::parseOutput(
            QJsonDocument(report).toJson(QJsonDocument::Compact),
            QStringLiteral("request"), QStringLiteral("run2.bbbb2222"),
            projectPath);
    ASSERT_TRUE(result.reportValid);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, QStringLiteral("PROJECT_FILE_DAMAGED"));
}

TEST(ProjectOpenAnalyzer, RejectsSuccessfulReportWithFailureExitCode)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = directory.filePath(QStringLiteral("Test.hsp"));
    QFile file(projectPath);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("{}");
    file.close();

    const VNH::ProjectOpenAnalysis result =
        VNH::ProjectOpenAnalyzer::parseOutput(
            reportFor(projectPath), QStringLiteral("request"),
            QStringLiteral("run2.bbbb2222"), projectPath, 4);
    EXPECT_FALSE(result.reportValid);
    EXPECT_EQ(result.errorCode, QStringLiteral("ANALYZER_EXIT_MISMATCH"));
}

TEST(ProjectOpenAnalyzer, AcceptsBlockingReportWithFailureExitCode)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = directory.filePath(QStringLiteral("Test.hsp"));
    QFile file(projectPath);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("{}");
    file.close();
    QJsonObject report = QJsonDocument::fromJson(reportFor(projectPath)).object();
    report[QStringLiteral("ok")] = false;
    report[QStringLiteral("upgradeRequired")] = false;
    report[QStringLiteral("currentSchema")] = 0;
    report[QStringLiteral("approvalDigest")] = QString();
    report[QStringLiteral("errorCode")] =
        QStringLiteral("PROJECT_UPGRADE_RECOVERY_REQUIRED");

    const VNH::ProjectOpenAnalysis result =
        VNH::ProjectOpenAnalyzer::parseOutput(
            QJsonDocument(report).toJson(QJsonDocument::Compact),
            QStringLiteral("request"), QStringLiteral("run2.bbbb2222"),
            projectPath, 4);
    ASSERT_TRUE(result.reportValid);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode,
              QStringLiteral("PROJECT_UPGRADE_RECOVERY_REQUIRED"));
}

TEST(ProjectOpenAnalyzer, RejectsStorageMetricsThatAreNotJsonIntegers)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = directory.filePath(QStringLiteral("Test.hsp"));
    QFile file(projectPath);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("{}");
    file.close();
    QJsonObject report = QJsonDocument::fromJson(reportFor(projectPath)).object();
    report[QStringLiteral("requiredProjectBytes")] = QStringLiteral("4096");

    const VNH::ProjectOpenAnalysis result =
        VNH::ProjectOpenAnalyzer::parseOutput(
            QJsonDocument(report).toJson(QJsonDocument::Compact),
            QStringLiteral("request"), QStringLiteral("run2.bbbb2222"),
            projectPath);
    EXPECT_FALSE(result.reportValid);
    EXPECT_EQ(result.errorCode, QStringLiteral("ANALYSIS_REPORT_INVALID"));
}

TEST(ProjectOpenAnalyzer, UsesTargetStudioReportWithoutAnInstalledSchemaTable)
{
    auto report = QJsonDocument::fromJson(reportFor(QStringLiteral("test.hsp"))).object();
    report[QStringLiteral("targetSchema")] = 37;
    const auto result = VNH::ProjectOpenAnalyzer::parseOutput(QJsonDocument(report).toJson(),
        QStringLiteral("request"), QStringLiteral("run2.bbbb2222"), QStringLiteral("test.hsp"));
    EXPECT_TRUE(result.reportValid);
    EXPECT_EQ(result.targetSchema, 37);
}

TEST(ProjectOpenAnalyzer, RejectsInvalidSuccessfulUpgradeRelations)
{
    for (const QJsonObject& overrides : {
            QJsonObject{{QStringLiteral("currentSchema"), 3}},
            QJsonObject{{QStringLiteral("targetSchema"), 2.5}},
            QJsonObject{{QStringLiteral("upgradeRequired"), false}},
            QJsonObject{{QStringLiteral("migrationStepCount"), 0}},
            QJsonObject{{QStringLiteral("projectId"), QStringLiteral("invalid")}},
            QJsonObject{{QStringLiteral("approvalDigest"), QStringLiteral("invalid")}}}) {
        auto report = QJsonDocument::fromJson(reportFor(QStringLiteral("test.hsp"))).object();
        for (auto it = overrides.begin(); it != overrides.end(); ++it)
            report.insert(it.key(), it.value());
        EXPECT_FALSE(VNH::ProjectOpenAnalyzer::parseOutput(QJsonDocument(report).toJson(),
            QStringLiteral("request"), QStringLiteral("run2.bbbb2222"), QStringLiteral("test.hsp")).reportValid);
    }
}
