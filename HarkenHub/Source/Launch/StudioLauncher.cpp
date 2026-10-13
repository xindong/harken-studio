#include "Launch/StudioLauncher.h"

#include "Platform/PlatformPaths.h"
#include "Platform/ProcessScan.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>

namespace VNH {

namespace {

QString hubText(const char* source)
{
    return QCoreApplication::translate("HarkenHub", source);
}

}

QStringList StudioLauncher::argumentsFor(const StudioLaunchSpec& spec)
{
    if (spec.capabilitiesVersion != 1 && spec.capabilitiesVersion != kStudioCapabilitiesVersion)
        return {QStringLiteral("--project"), spec.projectPath};
    QStringList arguments{
        QStringLiteral("--project"),           spec.projectPath,
        QStringLiteral("--hub-session"),  spec.serverName,
        QStringLiteral("--hub-token"),    spec.sessionToken,
        QStringLiteral("--expected-build-id"), spec.expectedBuildId,
        QStringLiteral("--launch-intent"), launchIntentName(spec.intent),
    };
    if (spec.capabilitiesVersion == kStudioCapabilitiesVersion
        && spec.intent == LaunchIntent::UpgradeApprovedByHub
        && !spec.projectOpenRequestId.isEmpty() && !spec.projectOpenApproval.isEmpty()) {
        arguments << QStringLiteral("--project-open-request-id")
                  << spec.projectOpenRequestId;
        arguments << QStringLiteral("--project-open-approval")
                  << spec.projectOpenApproval;
    }
    return arguments;
}

QStringList StudioLauncher::argumentsForCreate(const StudioCreateSpec& spec)
{
    QStringList arguments = {
        QStringLiteral("--create-project"),
        QStringLiteral("--project-name"), spec.projectName,
        QStringLiteral("--project-location"), spec.projectLocation,
        QStringLiteral("--hub-session"), spec.launch.serverName,
        QStringLiteral("--hub-token"), spec.launch.sessionToken,
        QStringLiteral("--expected-build-id"), spec.launch.expectedBuildId,
        QStringLiteral("--launch-intent"), launchIntentName(LaunchIntent::DirectOpenOnly),
    };
    if (!spec.templatePath.isEmpty()) {
        arguments << QStringLiteral("--project-template-id") << spec.templateId
                  << QStringLiteral("--project-template-file") << spec.templatePath;
    }
    return arguments;
}

bool StudioLauncher::start(const StudioLaunchSpec& spec, QString* error)
{
    const QString program = spec.executablePath.isEmpty()
                                ? spec.installPath + QStringLiteral("/") +
                                      studioBinaryRelativePath()
                                : spec.executablePath;
    if (!QFileInfo::exists(program)) {
        *error = hubText("HarkenStudio executable is missing: %1")
                     .arg(program);
        return false;
    }
    if (!QProcess::startDetached(program, argumentsFor(spec),
                                 spec.installPath, &pid_)) {
        *error = hubText("HarkenStudio did not start");
        return false;
    }
    return true;
}

bool StudioLauncher::startCreate(const StudioCreateSpec& spec, QString* error)
{
    const QString program = spec.launch.executablePath.isEmpty()
                                ? spec.launch.installPath + QStringLiteral("/") +
                                      studioBinaryRelativePath()
                                : spec.launch.executablePath;
    if (!QFileInfo::exists(program)) {
        *error = hubText("HarkenStudio executable is missing: %1")
                     .arg(program);
        return false;
    }
    if (!QProcess::startDetached(program, argumentsForCreate(spec),
                                 spec.launch.installPath, &pid_)) {
        *error = hubText("HarkenStudio did not start");
        return false;
    }
    return true;
}

bool StudioLauncher::stillRunning() const
{
    return pid_ > 0 && !executablePathForPid(pid_).isEmpty();
}

bool StudioLauncher::terminate()
{
    return terminateProcessById(pid_);
}

}
