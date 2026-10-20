#include "HubLauncher.h"

#include "harken_buildinfo.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStringList>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace VNX {

namespace {

void appendInstallCandidates(QStringList& candidates, const QString& root)
{
    if (root.isEmpty())
        return;
    const QFileInfo info(root);
    if (info.isFile())
        candidates.append(info.absoluteFilePath());
#ifdef Q_OS_MACOS
    if (root.endsWith(QStringLiteral(".app"), Qt::CaseInsensitive))
        candidates.append(QDir(root).filePath(
            QStringLiteral("Contents/MacOS/HarkenHub")));
    candidates.append(QDir(root).filePath(
        QStringLiteral("HarkenHub.app/Contents/MacOS/HarkenHub")));
    candidates.append(QDir(root).filePath(QStringLiteral("HarkenHub")));
#else
    candidates.append(QDir(root).filePath(QStringLiteral("HarkenStudioHub.exe")));
    candidates.append(QDir(root).filePath(QStringLiteral("HarkenHub.exe")));
#endif
}

}

QString HubLauncher::executablePath()
{
    return resolveExecutablePath(
        Harken::BuildInfo::BuildType == std::string_view("local"),
        QCoreApplication::instance()
            ? QCoreApplication::applicationDirPath()
            : QString());
}

QString HubLauncher::resolveExecutablePath(
    bool localBuild, const QString& applicationDirPath)
{
    QStringList candidates;
    appendInstallCandidates(
        candidates, qEnvironmentVariable("HARKEN_HUB_INSTALL_DIR"));

    if (localBuild)
        appendInstallCandidates(candidates, applicationDirPath);

#ifdef Q_OS_WIN
    const QSettings machine(
        QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\HarkenSound\\HarkenHub"),
        QSettings::NativeFormat);
    appendInstallCandidates(
        candidates,
        QDir::fromNativeSeparators(
            machine.value(QStringLiteral("InstallDir")).toString()));
#else
    candidates.append(QStringLiteral(
        "/Applications/HarkenHub.app/Contents/MacOS/HarkenHub"));
#endif

    if (!localBuild)
        appendInstallCandidates(candidates, applicationDirPath);
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate))
            return QFileInfo(candidate).absoluteFilePath();
    }
    return {};
}

QStringList HubLauncher::launchArguments(
    HubPage page, const QString& studioExecutablePath,
    bool revealLaunchingVersion, const QString& harkenProjectPath)
{
    QString destination;
    switch (page) {
        case HubPage::NewProject:
            destination = QStringLiteral("--show-new-project");
            break;
        case HubPage::EditIntegration:
            destination = QStringLiteral("--edit-integration");
            break;
        case HubPage::Projects:
            destination = QStringLiteral("--show-projects");
            break;
    }
    QStringList arguments{
        destination,
        QStringLiteral("--register-local-build=%1")
            .arg(QFileInfo(studioExecutablePath).absoluteFilePath())};
    if (revealLaunchingVersion)
        arguments.append(QStringLiteral("--reveal-local-build"));
    if (!harkenProjectPath.trimmed().isEmpty())
        arguments.append(QStringLiteral("--harken-project=%1")
                             .arg(QFileInfo(harkenProjectPath).absoluteFilePath()));
    return arguments;
}

bool HubLauncher::open(HubPage page, QString* error,
                       bool revealLaunchingVersion,
                       const QString& harkenProjectPath)
{
    const QString program = executablePath();
    if (program.isEmpty()) {
        if (error)
            *error = QStringLiteral("Harken Studio Hub is not installed.");
        return false;
    }
    qint64 processId = 0;
    if (!QProcess::startDetached(
            program,
            launchArguments(page, QCoreApplication::applicationFilePath(),
                            revealLaunchingVersion, harkenProjectPath),
                                 QFileInfo(program).absolutePath(),
                                 &processId)) {
        if (error)
            *error = QStringLiteral("Harken Studio Hub did not start.");
        return false;
    }
#ifdef Q_OS_WIN
    if (processId > 0 && processId <= MAXDWORD)
        AllowSetForegroundWindow(static_cast<DWORD>(processId));
#endif
    return true;
}

}
