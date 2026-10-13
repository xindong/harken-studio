#include "ProjectOpenAnalyzer.h"

#include "Platform/PlatformPaths.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QUuid>
#include <limits>

namespace VNH {

namespace {

ProjectOpenAnalysis invalid(const QString& code,
                            const QString& technicalDetails = {})
{
    ProjectOpenAnalysis result;
    result.errorCode = code;
    result.technicalDetails = technicalDetails;
    return result;
}

QString canonicalPath(const QString& path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath()
                                               : canonical);
}

bool samePath(const QString& left, const QString& right)
{
#ifdef Q_OS_WIN
    return canonicalPath(left).compare(canonicalPath(right),
                                       Qt::CaseInsensitive) == 0;
#else
    return canonicalPath(left) == canonicalPath(right);
#endif
}

bool isNonNegativeInteger(const QJsonValue& value)
{
    return value.isDouble() && value.toInteger(-1) >= 0;
}

}

ProjectOpenAnalysis ProjectOpenAnalyzer::parseOutput(
    const QByteArray& output, const QString& expectedRequestId,
    const QString& expectedBuildId, const QString& expectedProjectPath,
    int exitCode)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        output.trimmed(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return invalid(QStringLiteral("ANALYSIS_REPORT_INVALID"),
                       parseError.errorString());

    const QJsonObject report = document.object();
    if (report.value(QStringLiteral("protocolVersion")).toInt(0)
        != kProtocolVersion)
        return invalid(QStringLiteral("ANALYSIS_PROTOCOL_MISMATCH"));
    if (report.value(QStringLiteral("product")).toString()
        != QStringLiteral("HarkenStudio"))
        return invalid(QStringLiteral("ANALYSIS_PRODUCT_MISMATCH"));
    if (expectedRequestId.isEmpty() || report.value(QStringLiteral("requestId")).toString()
        != expectedRequestId)
        return invalid(QStringLiteral("ANALYSIS_REQUEST_MISMATCH"));
    const QString buildId = report.value(QStringLiteral("buildId")).toString();
    if (expectedBuildId.isEmpty() || buildId != expectedBuildId)
        return invalid(QStringLiteral("ANALYSIS_BUILD_MISMATCH"));
    const QString projectPath =
        report.value(QStringLiteral("projectPath")).toString();
    if (projectPath.isEmpty() || !samePath(projectPath, expectedProjectPath))
        return invalid(QStringLiteral("ANALYSIS_PROJECT_MISMATCH"));
    if (!report.value(QStringLiteral("ok")).isBool()
        || !report.value(QStringLiteral("upgradeRequired")).isBool())
        return invalid(QStringLiteral("ANALYSIS_REPORT_INVALID"));

    ProjectOpenAnalysis result;
    result.reportValid = true;
    result.ok = report.value(QStringLiteral("ok")).toBool();
    result.errorCode = report.value(QStringLiteral("errorCode")).toString();
    result.technicalDetails =
        report.value(QStringLiteral("technicalDetails")).toString();
    result.studioVersion = report.value(QStringLiteral("version")).toString();
    result.buildId = buildId;
    result.requestId = expectedRequestId;
    result.projectPath = projectPath;
    result.projectId = report.value(QStringLiteral("projectId")).toString();
    result.currentSchema =
        report.value(QStringLiteral("currentSchema")).toInt(0);
    result.targetSchema =
        report.value(QStringLiteral("targetSchema")).toInt(0);
    result.upgradeRequired =
        report.value(QStringLiteral("upgradeRequired")).toBool();
    result.migrationStepCount =
        report.value(QStringLiteral("migrationStepCount")).toInt(-1);
    result.affectedFileCount =
        report.value(QStringLiteral("affectedFileCount")).toInt(-1);
    result.requiredProjectBytes =
        report.value(QStringLiteral("requiredProjectBytes")).toInteger(-1);
    result.requiredRecoveryBytes =
        report.value(QStringLiteral("requiredRecoveryBytes")).toInteger(-1);
    result.recoveryRoot =
        report.value(QStringLiteral("recoveryRoot")).toString();
    result.protection =
        report.value(QStringLiteral("protection")).toString();
    result.approvalDigest =
        report.value(QStringLiteral("approvalDigest")).toString();

    if (result.studioVersion.isEmpty()
        || !report.value(QStringLiteral("technicalDetails")).isString()
        || !report.value(QStringLiteral("errorCode")).isString())
        return invalid(QStringLiteral("ANALYSIS_REPORT_INVALID"));
    if (!result.ok) {
        if (result.errorCode.isEmpty())
            return invalid(QStringLiteral("ANALYSIS_REPORT_INVALID"));
        return result;
    }
    if (exitCode != 0)
        return invalid(QStringLiteral("ANALYZER_EXIT_MISMATCH"),
                       QStringLiteral("Exit code %1").arg(exitCode));
    const bool metricsValid =
        isNonNegativeInteger(report.value(QStringLiteral("migrationStepCount")))
        && isNonNegativeInteger(report.value(QStringLiteral("affectedFileCount")))
        && isNonNegativeInteger(report.value(
            QStringLiteral("requiredProjectBytes")))
        && isNonNegativeInteger(report.value(
            QStringLiteral("requiredRecoveryBytes")));
    const bool digestValid = QRegularExpression(
        QStringLiteral("^[0-9a-f]{64}$"))
                                 .match(result.approvalDigest).hasMatch();
    if (result.currentSchema <= 0 || result.targetSchema <= 0
        || !isNonNegativeInteger(report.value(QStringLiteral("currentSchema")))
        || !isNonNegativeInteger(report.value(QStringLiteral("targetSchema")))
        || result.currentSchema > result.targetSchema || !metricsValid
        || result.migrationStepCount < 0 || result.affectedFileCount < 0
        || QUuid(result.projectId).isNull() || !result.errorCode.isEmpty()
        || result.requiredProjectBytes > std::numeric_limits<qint64>::max() - result.requiredRecoveryBytes
        || !digestValid
        || (result.protection != QStringLiteral("atomicInPlace")
            && result.protection != QStringLiteral("recoverableInPlace")))
        return invalid(QStringLiteral("ANALYSIS_REPORT_INVALID"));
    if (result.upgradeRequired
        != (result.currentSchema < result.targetSchema))
        return invalid(QStringLiteral("ANALYSIS_REPORT_INVALID"));
    if (result.upgradeRequired && (result.migrationStepCount == 0 || result.affectedFileCount == 0))
        return invalid(QStringLiteral("ANALYSIS_REPORT_INVALID"));
    if (!result.upgradeRequired && result.migrationStepCount != 0)
        return invalid(QStringLiteral("ANALYSIS_REPORT_INVALID"));
    return result;
}

ProjectOpenAnalysis ProjectOpenAnalyzer::run(
    const InstalledVersion& version, const QString& projectPath,
    const QString& requestId, int timeoutMs)
{
    const QString program = version.executablePath.isEmpty()
        ? QDir(version.installPath).filePath(studioBinaryRelativePath())
        : version.executablePath;
    QProcess process;
    process.setProgram(program);
    process.setWorkingDirectory(version.installPath);
    process.setArguments({
        QStringLiteral("--analyze-project-open"), projectPath,
        QStringLiteral("--project-open-request-id"), requestId,
    });
    process.start(QIODevice::ReadOnly);
    if (!process.waitForStarted(qMin(timeoutMs, 15000)))
        return invalid(QStringLiteral("ANALYZER_START_FAILED"),
                       process.errorString());
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        return invalid(QStringLiteral("ANALYZER_TIMEOUT"));
    }
    if (process.exitStatus() != QProcess::NormalExit)
        return invalid(QStringLiteral("ANALYZER_CRASHED"),
                       QString::fromUtf8(process.readAllStandardError()));

    ProjectOpenAnalysis result = parseOutput(
        process.readAllStandardOutput(), requestId, version.expectedBuildId(),
        projectPath, process.exitCode());
    if (result.reportValid)
        return result;
    if (process.exitCode() != 0 && result.technicalDetails.isEmpty())
        result.technicalDetails = QStringLiteral("Exit code %1")
                                      .arg(process.exitCode());
    return result;
}

}
