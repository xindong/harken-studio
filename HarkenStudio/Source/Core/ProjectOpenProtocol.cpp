#include "ProjectOpenProtocol.h"

#include "harken_buildinfo.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

namespace VNX {

namespace {

QString canonicalPath(const QString& path)
{
    if (path.trimmed().isEmpty())
        return {};
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath()
                                               : canonical);
}

QJsonArray sortedArray(QStringList values)
{
    values.sort();
    QJsonArray result;
    for (const QString& value : values)
        result.append(value);
    return result;
}

QJsonArray orderedArray(const QStringList& values)
{
    QJsonArray result;
    for (const QString& value : values)
        result.append(value);
    return result;
}

QJsonObject approvalPayload(const ProjectUpgradeAnalysis& analysis,
                            const QString& requestId)
{
    QStringList migrationSteps;
    migrationSteps.reserve(static_cast<qsizetype>(
        analysis.migrationPlan.steps.size()));
    for (const ProjectMigrationStep& step : analysis.migrationPlan.steps)
        migrationSteps.append(step.id);

    QJsonObject payload;
    payload[QStringLiteral("protocolVersion")] =
        kProjectOpenAnalysisProtocolVersion;
    payload[QStringLiteral("requestId")] = requestId;
    payload[QStringLiteral("buildId")] = currentStudioBuildId();
    payload[QStringLiteral("executablePath")] = canonicalPath(
        QCoreApplication::applicationFilePath());
    payload[QStringLiteral("projectPath")] = canonicalPath(
        analysis.projectFilePath);
    payload[QStringLiteral("projectId")] = analysis.probe.projectId;
    payload[QStringLiteral("sourceSchema")] = analysis.sourceSchema;
    payload[QStringLiteral("targetSchema")] = analysis.targetSchema;
    payload[QStringLiteral("sourceFingerprint")] =
        QString::fromLatin1(analysis.sourceFingerprint.toHex());
    payload[QStringLiteral("sourceTreeFingerprint")] =
        QString::fromLatin1(analysis.sourceTreeFingerprint.toHex());
    payload[QStringLiteral("migrationSteps")] = orderedArray(migrationSteps);
    payload[QStringLiteral("writeSet")] = sortedArray(
        analysis.migrationPlan.writeSet);
    payload[QStringLiteral("protection")] =
        analysis.protection == ProjectUpgradeProtection::AtomicInPlace
            ? QStringLiteral("atomicInPlace")
            : QStringLiteral("recoverableInPlace");
    payload[QStringLiteral("requiredProjectBytes")] =
        analysis.requiredProjectBytes;
    payload[QStringLiteral("requiredRecoveryBytes")] =
        analysis.requiredRecoveryBytes;
    payload[QStringLiteral("recoveryRoot")] = canonicalPath(
        analysis.recoveryRoot);
    return payload;
}

}

QString currentStudioBuildId()
{
    return QString::fromUtf8(
        Harken::BuildInfo::BuildId.data(),
        static_cast<int>(Harken::BuildInfo::BuildId.size()));
}

QString currentStudioVersion()
{
    return QString::fromUtf8(
        Harken::BuildInfo::Version.data(),
        static_cast<int>(Harken::BuildInfo::Version.size()));
}

QString projectOpenApprovalDigest(const ProjectUpgradeAnalysis& analysis,
                                  const QString& requestId)
{
    const QByteArray payload = QJsonDocument(
        approvalPayload(analysis, requestId)).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(
        payload, QCryptographicHash::Sha256).toHex());
}

QJsonObject projectOpenAnalysisReport(const ProjectUpgradeAnalysis& analysis,
                                      const QString& requestId)
{
    QJsonObject report;
    report[QStringLiteral("protocolVersion")] =
        kProjectOpenAnalysisProtocolVersion;
    report[QStringLiteral("ok")] = analysis.ok;
    report[QStringLiteral("product")] = QStringLiteral("HarkenStudio");
    report[QStringLiteral("version")] = currentStudioVersion();
    report[QStringLiteral("buildId")] = currentStudioBuildId();
    report[QStringLiteral("requestId")] = requestId;
    report[QStringLiteral("projectPath")] = canonicalPath(
        analysis.projectFilePath);
    report[QStringLiteral("projectId")] = analysis.probe.projectId;
    report[QStringLiteral("currentSchema")] = analysis.sourceSchema;
    report[QStringLiteral("targetSchema")] = analysis.targetSchema;
    report[QStringLiteral("upgradeRequired")] = analysis.upgradeRequired;
    report[QStringLiteral("migrationStepCount")] =
        analysis.migrationPlan.stepCount;
    report[QStringLiteral("affectedFileCount")] =
        analysis.migrationPlan.fileCount;
    report[QStringLiteral("requiredProjectBytes")] =
        analysis.requiredProjectBytes;
    report[QStringLiteral("requiredRecoveryBytes")] =
        analysis.requiredRecoveryBytes;
    report[QStringLiteral("recoveryRoot")] = analysis.recoveryRoot;
    report[QStringLiteral("protection")] =
        analysis.protection == ProjectUpgradeProtection::AtomicInPlace
            ? QStringLiteral("atomicInPlace")
            : QStringLiteral("recoverableInPlace");
    report[QStringLiteral("approvalDigest")] =
        analysis.ok ? projectOpenApprovalDigest(analysis, requestId) : QString();
    report[QStringLiteral("errorCode")] =
        analysis.ok ? QString() : projectIssueCode(
            analysis.issue == ProjectIssue::None
                ? ProjectIssue::InternalFailure : analysis.issue);
    report[QStringLiteral("technicalDetails")] =
        analysis.blockingErrors.join(QLatin1Char('\n'));
    return report;
}

}
