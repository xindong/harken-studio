#include "Core/ProjectOpenProtocol.h"

#include <gtest/gtest.h>

#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>

namespace {

VNX::ProjectUpgradeAnalysis analysisFor(const QString& projectPath)
{
    VNX::ProjectUpgradeAnalysis analysis;
    analysis.ok = true;
    analysis.upgradeRequired = true;
    analysis.projectFilePath = projectPath;
    analysis.sourceSchema = 1;
    analysis.targetSchema = 2;
    analysis.sourceFingerprint = QByteArray(32, 'a');
    analysis.sourceTreeFingerprint = QByteArray(32, 'b');
    analysis.probe.projectId = QStringLiteral("project-id");
    analysis.protection = VNX::ProjectUpgradeProtection::AtomicInPlace;
    analysis.requiredProjectBytes = 4096;
    analysis.migrationPlan.ok = true;
    analysis.migrationPlan.sourceSchema = 1;
    analysis.migrationPlan.targetSchema = 2;
    analysis.migrationPlan.stepCount = 1;
    analysis.migrationPlan.fileCount = 2;
    analysis.migrationPlan.writeSet = {
        QStringLiteral("Test.hsp"), QStringLiteral("Layers/Main.vp")};
    VNX::ProjectMigrationStep step;
    step.id = QStringLiteral("project-v1-v2");
    step.fromSchema = 1;
    step.toSchema = 2;
    analysis.migrationPlan.steps.push_back(std::move(step));
    return analysis;
}

}

TEST(ProjectOpenProtocol, ReportContainsUpgradeSafetyFacts)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const VNX::ProjectUpgradeAnalysis analysis = analysisFor(
        directory.filePath(QStringLiteral("Test.hsp")));

    const QJsonObject report = VNX::projectOpenAnalysisReport(
        analysis, QStringLiteral("request"));
    EXPECT_TRUE(report.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(report.value(QStringLiteral("upgradeRequired")).toBool());
    EXPECT_EQ(report.value(QStringLiteral("currentSchema")).toInt(), 1);
    EXPECT_EQ(report.value(QStringLiteral("targetSchema")).toInt(), 2);
    EXPECT_EQ(report.value(QStringLiteral("migrationStepCount")).toInt(), 1);
    EXPECT_EQ(report.value(QStringLiteral("affectedFileCount")).toInt(), 2);
    EXPECT_EQ(report.value(QStringLiteral("approvalDigest")).toString().size(),
              64);
}

TEST(ProjectOpenProtocol, ApprovalChangesWithTheProjectFingerprint)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectUpgradeAnalysis analysis = analysisFor(
        directory.filePath(QStringLiteral("Test.hsp")));
    const QString before = VNX::projectOpenApprovalDigest(
        analysis, QStringLiteral("request"));
    analysis.sourceTreeFingerprint = QByteArray(32, 'c');
    const QString after = VNX::projectOpenApprovalDigest(
        analysis, QStringLiteral("request"));
    EXPECT_NE(before, after);
}

TEST(ProjectOpenProtocol, ApprovalPreservesMigrationExecutionOrder)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectUpgradeAnalysis analysis = analysisFor(
        directory.filePath(QStringLiteral("Test.hsp")));
    VNX::ProjectMigrationStep second;
    second.id = QStringLiteral("project-v2-v3");
    second.fromSchema = 2;
    second.toSchema = 3;
    analysis.migrationPlan.steps.push_back(std::move(second));

    const QString before = VNX::projectOpenApprovalDigest(
        analysis, QStringLiteral("request"));
    std::reverse(analysis.migrationPlan.steps.begin(),
                 analysis.migrationPlan.steps.end());
    const QString after = VNX::projectOpenApprovalDigest(
        analysis, QStringLiteral("request"));
    EXPECT_NE(before, after);
}
