#include "Versions/SelfTestRunner.h"

#include "Platform/PlatformPaths.h"
#include "Versions/StudioCapabilities.h"

#include <QJsonDocument>
#include <QRegularExpression>
#include <limits>
#include <QJsonObject>
#include <QProcess>

namespace VNH {

SelfTestResult SelfTestRunner::parseOutput(const QByteArray& output,
                                           const QString& expectedBuildId)
{
    SelfTestResult result;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(output.trimmed(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        result.error = QStringLiteral("Self-test report is not valid JSON: %1")
                           .arg(parseError.errorString());
        return result;
    }
    const QJsonObject report = document.object();
    if (report.value(QStringLiteral("product")).toString() != QStringLiteral("HarkenStudio")) {
        result.error = QStringLiteral("The selected executable is not HarkenStudio");
        return result;
    }
    const QJsonValue protocol = report.value(QStringLiteral("capabilitiesVersion"));
    if (!protocol.isDouble() || (protocol.toDouble() != 1
        && protocol.toDouble() != kStudioCapabilitiesVersion)) {
        result.error = QStringLiteral("Studio reported an unsupported self-test protocol.");
        return result;
    }
    if (!report.value(QStringLiteral("ok")).isBool()
        || !report.value(QStringLiteral("ok")).toBool()
        || !report.value(QStringLiteral("schemaArchiveOk")).isBool()
        || !report.value(QStringLiteral("schemaArchiveOk")).toBool()) {
        result.error = QStringLiteral("Studio self-test failed: %1")
                           .arg(report.value(QStringLiteral("schemaError")).toString());
        return result;
    }
    for (const QString& key : {QStringLiteral("version"), QStringLiteral("buildId"),
                              QStringLiteral("channel"), QStringLiteral("buildType")}) {
        if (!report.value(key).isString() || report.value(key).toString().trimmed().isEmpty()) {
            result.error = QStringLiteral("Self-test is missing its build identity: %1").arg(key);
            return result;
        }
    }
    if (protocol.toInt() == 1) {
        if (report.value(QStringLiteral("projectSchema")).toObject()
                .value(QStringLiteral("current")).toInt() <= 0) {
            result.error = QStringLiteral("Studio self-test has invalid legacy schema information");
            return result;
        }
    } else {
        for (const QString& key : {QStringLiteral("editorSchemaVersion"), QStringLiteral("packSchemaVersion")}) {
            const QJsonValue value = report.value(key);
            if (!value.isDouble() || value.toInteger(-1) <= 0
                || value.toInteger(-1) > std::numeric_limits<int>::max()) {
                result.error = QStringLiteral("Self-test has an invalid diagnostic field: %1").arg(key);
                return result;
            }
        }
        const QString commit = report.value(QStringLiteral("gitCommit")).toString();
        const QString snapshot = report.value(QStringLiteral("schemaSnapshotHash")).toString();
        if (!QRegularExpression(QStringLiteral("^[0-9a-f]{40}$")).match(commit).hasMatch()
            || !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(snapshot).hasMatch()
            || !report.value(QStringLiteral("gitDirty")).isBool()) {
            result.error = QStringLiteral("Self-test has an invalid build or schema snapshot identity");
            return result;
        }
    }
    result.product = QStringLiteral("HarkenStudio");
    result.reportedBuildId = report.value(QStringLiteral("buildId")).toString();
    result.reportedVersion = report.value(QStringLiteral("version")).toString();
    result.channel = report.value(QStringLiteral("channel")).toString();
    result.buildType = report.value(QStringLiteral("buildType")).toString();
    result.capabilitiesVersion = protocol.toInt();
    result.gitCommit = report.value(QStringLiteral("gitCommit")).toString();
    result.gitDirty = report.value(QStringLiteral("gitDirty")).toBool();
    result.editorSchemaVersion = report.value(QStringLiteral("editorSchemaVersion")).toInt();
    result.packSchemaVersion = report.value(QStringLiteral("packSchemaVersion")).toInt();
    result.schemaSnapshotHash = report.value(QStringLiteral("schemaSnapshotHash")).toString();
    if (!expectedBuildId.isEmpty() && result.reportedBuildId != expectedBuildId) {
        result.error = QStringLiteral("Self-test reported build id %1, expected %2")
                           .arg(result.reportedBuildId, expectedBuildId);
        return result;
    }
    result.ok = true;
    return result;
}

SelfTestResult SelfTestRunner::run(const QString& program,
                                   const QStringList& args,
                                   const QString& expectedBuildId,
                                   int timeoutMs)
{
    SelfTestResult result;

    QProcess process;
    process.setProgram(program);
    process.setArguments(args);
    process.start(QIODevice::ReadOnly);
    if (!process.waitForStarted(timeoutMs)) {
        result.error =
            QStringLiteral("Cannot start the self-test: %1").arg(program);
        return result;
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        result.error = QStringLiteral("Self-test timed out");
        return result;
    }
    if (process.exitStatus() != QProcess::NormalExit) {
        result.error = QStringLiteral("Self-test crashed");
        return result;
    }
    if (process.exitCode() != 0) {
        result.error = QStringLiteral("Self-test exited with code %1")
                           .arg(process.exitCode());
        return result;
    }
    return parseOutput(process.readAllStandardOutput(), expectedBuildId);
}

SelfTestResult SelfTestRunner::runForInstall(const QString& installPath,
                                             const QString& expectedBuildId)
{
    const QString program =
        installPath + QStringLiteral("/") + studioBinaryRelativePath();
    return run(program,
               {QStringLiteral("--self-test"), QStringLiteral("--json")},
               expectedBuildId, 30000);
}

}
