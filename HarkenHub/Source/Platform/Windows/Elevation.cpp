#include "Platform/Elevation.h"

#include <QCoreApplication>
#include <QDir>

#include <windows.h>
#include <shellapi.h>

namespace VNH {

ElevationLaunchResult launchElevatedCommand(const QStringList& arguments)
{
    ElevationLaunchResult result;

    const QString exePath =
        QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    QStringList quoted;
    for (const QString& argument : arguments) {
        if (argument.contains(QLatin1Char(' ')))
            quoted.append(QStringLiteral("\"%1\"").arg(argument));
        else
            quoted.append(argument);
    }
    const QString parameters = quoted.join(QLatin1Char(' '));
    const std::wstring exeW = exePath.toStdWString();
    const std::wstring parametersW = parameters.toStdWString();

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = exeW.c_str();
    info.lpParameters = parametersW.c_str();
    info.nShow = SW_HIDE;

    if (!ShellExecuteExW(&info)) {
        const DWORD lastError = GetLastError();
        if (lastError == ERROR_CANCELLED) {
            result.canceled = true;
            result.error = QStringLiteral("Elevation was declined");
        } else {
            result.error = QStringLiteral(
                               "Cannot start the elevated hub (error %1)")
                               .arg(lastError);
        }
        return result;
    }
    if (!info.hProcess) {
        result.error =
            QStringLiteral("The elevated hub did not return a process");
        return result;
    }

    result.ok = true;
    result.processHandle = info.hProcess;
    return result;
}

bool startInstallerDetached(const QString& program,
                            const QStringList& arguments, QString* error)
{
    QStringList quoted;
    for (const QString& argument : arguments) {
        if (argument.contains(QLatin1Char(' ')))
            quoted.append(QStringLiteral("\"%1\"").arg(argument));
        else
            quoted.append(argument);
    }
    const std::wstring programW =
        QDir::toNativeSeparators(program).toStdWString();
    const std::wstring parametersW =
        quoted.join(QLatin1Char(' ')).toStdWString();

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOASYNC;
    info.lpFile = programW.c_str();
    info.lpParameters = parametersW.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        const DWORD lastError = GetLastError();
        *error = lastError == ERROR_CANCELLED
                     ? QStringLiteral("Elevation was declined")
                     : QStringLiteral(
                           "Cannot start the hub installer (error %1)")
                           .arg(lastError);
        return false;
    }
    return true;
}

bool pollElevatedInstall(void* processHandle, int* exitCode)
{
    if (!processHandle)
        return true;
    if (WaitForSingleObject(processHandle, 0) != WAIT_OBJECT_0)
        return false;
    DWORD code = 1;
    GetExitCodeProcess(processHandle, &code);
    *exitCode = static_cast<int>(code);
    return true;
}

void releaseElevatedInstall(void* processHandle)
{
    if (processHandle)
        CloseHandle(processHandle);
}

}
