#include "Platform/Elevation.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QUuid>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace VNH {

namespace {

struct ElevatedProcess
{
    pid_t pid = -1;
    bool reaped = false;
    int osascriptExit = 1;
    QString statusPath;
    QString scratchDir;
};

QString shellQuoted(const QString& text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

QString appleScriptQuoted(const QString& text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

int readExitStatus(const QString& statusPath)
{
    QFile file(statusPath);
    if (!file.open(QIODevice::ReadOnly))
        return 1;
    bool valid = false;
    const int value =
        QString::fromUtf8(file.readAll()).trimmed().toInt(&valid);
    return valid ? value : 1;
}

int exitFromWaitStatus(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 1;
}

}

ElevationLaunchResult launchElevatedCommand(const QStringList& arguments)
{
    ElevationLaunchResult result;

    const QString scratchDir =
        QDir::tempPath() + QStringLiteral("/harken-elevate-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().mkpath(scratchDir)) {
        result.error =
            QStringLiteral("Cannot create the elevation scratch directory");
        return result;
    }
    const QString startedPath = scratchDir + QStringLiteral("/started");
    const QString statusPath = scratchDir + QStringLiteral("/status");

    QString command = QStringLiteral("/usr/bin/touch ") +
                      shellQuoted(startedPath) + QStringLiteral("; ") +
                      shellQuoted(QCoreApplication::applicationFilePath());
    for (const QString& argument : arguments)
        command += QLatin1Char(' ') + shellQuoted(argument);
    command += QStringLiteral("; /bin/echo $? > ") + shellQuoted(statusPath);

    const QString script = QStringLiteral("do shell script ") +
                           appleScriptQuoted(command) +
                           QStringLiteral(" with administrator privileges");

    const QByteArray scriptUtf8 = script.toUtf8();
    const char* argv[] = {"/usr/bin/osascript", "-e", scriptUtf8.constData(),
                          nullptr};
    pid_t pid = -1;
    if (posix_spawn(&pid, "/usr/bin/osascript", nullptr, nullptr,
                    const_cast<char* const*>(argv), environ) != 0) {
        QDir(scratchDir).removeRecursively();
        result.error = QStringLiteral("Cannot start the elevation prompt");
        return result;
    }

    bool reaped = false;
    int waitStatus = 0;
    while (!QFile::exists(startedPath)) {
        int status = 0;
        const pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited != 0) {
            reaped = true;
            waitStatus = waited == pid ? status : 0;
            break;
        }
        usleep(50 * 1000);
    }

    if (reaped && !QFile::exists(startedPath)) {
        QDir(scratchDir).removeRecursively();
        result.canceled = true;
        result.error = QStringLiteral("Elevation was declined");
        return result;
    }

    auto* handle = new ElevatedProcess;
    handle->pid = pid;
    handle->reaped = reaped;
    if (reaped)
        handle->osascriptExit = exitFromWaitStatus(waitStatus);
    handle->statusPath = statusPath;
    handle->scratchDir = scratchDir;

    result.ok = true;
    result.processHandle = handle;
    return result;
}

bool startInstallerDetached(const QString& program,
                            const QStringList& arguments, QString* error)
{
    if (!QProcess::startDetached(program, arguments)) {
        *error = QStringLiteral("Cannot start the hub installer");
        return false;
    }
    return true;
}

bool pollElevatedInstall(void* processHandle, int* exitCode)
{
    if (!processHandle)
        return true;
    auto* handle = static_cast<ElevatedProcess*>(processHandle);
    if (!handle->reaped) {
        int status = 0;
        const pid_t waited = waitpid(handle->pid, &status, WNOHANG);
        if (waited == 0)
            return false;
        handle->reaped = true;
        if (waited == handle->pid)
            handle->osascriptExit = exitFromWaitStatus(status);
        else
            handle->osascriptExit =
                QFile::exists(handle->statusPath) ? 0 : 1;
    }
    *exitCode = handle->osascriptExit == 0 ? readExitStatus(handle->statusPath)
                                           : 1;
    return true;
}

void releaseElevatedInstall(void* processHandle)
{
    if (!processHandle)
        return;
    auto* handle = static_cast<ElevatedProcess*>(processHandle);
    if (handle->reaped && !handle->scratchDir.isEmpty())
        QDir(handle->scratchDir).removeRecursively();
    delete handle;
}

}
