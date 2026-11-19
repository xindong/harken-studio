#include "Versions/SelfTestRunner.h"

#include "Platform/PlatformPaths.h"

#include <QJsonDocument>
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
    if (report.value(QStringLiteral("ok")) != QJsonValue(true)) {
        result.error = QStringLiteral("Studio self-test failed.");
        return result;
    }
    for (const QString& key : {QStringLiteral("version"), QStringLiteral("buildId"), QStringLiteral("channel")}) {
        if (!report.value(key).isString() || report.value(key).toString().trimmed().isEmpty()) {
            result.error = QStringLiteral("Self-test is missing its build identity: %1").arg(key);
            return result;
        }
    }
    result.product = QStringLiteral("HarkenStudio");
    result.reportedBuildId = report.value(QStringLiteral("buildId")).toString();
    result.reportedVersion = report.value(QStringLiteral("version")).toString();
    result.channel = report.value(QStringLiteral("channel")).toString();
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
