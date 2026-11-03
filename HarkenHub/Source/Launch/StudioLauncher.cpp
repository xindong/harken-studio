#include "Launch/StudioLauncher.h"

#include "Platform/PlatformPaths.h"

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
    return {QStringLiteral("--project"), spec.projectPath};
}

QStringList StudioLauncher::argumentsForCreate(const StudioCreateSpec& spec)
{
    QStringList arguments = {
        QStringLiteral("--create-project"),
        QStringLiteral("--project-name"), spec.projectName,
        QStringLiteral("--project-location"), spec.projectLocation,
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
                                 spec.installPath)) {
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
                                 spec.launch.installPath)) {
        *error = hubText("HarkenStudio did not start");
        return false;
    }
    return true;
}

}
