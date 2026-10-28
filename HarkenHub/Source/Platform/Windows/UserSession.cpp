#include "Platform/UserSession.h"
#include "Platform/Windows/UserSessionReady.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QVector>

#include <windows.h>
#include <sddl.h>
#include <userenv.h>
#include <wtsapi32.h>

namespace VNH {

namespace {

QString quoteArgument(const QString& argument)
{
    if (!argument.contains(QLatin1Char(' ')))
        return argument;
    return QStringLiteral("\"%1\"").arg(argument);
}

QString tokenUserSid(HANDLE token)
{
    QByteArray buffer(256, 0);
    DWORD needed = 0;
    if (!GetTokenInformation(token, TokenUser, buffer.data(),
                             static_cast<DWORD>(buffer.size()), &needed)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            return QString();
        buffer.resize(static_cast<int>(needed));
        if (!GetTokenInformation(token, TokenUser, buffer.data(),
                                 static_cast<DWORD>(buffer.size()), &needed))
            return QString();
    }
    LPWSTR sidString = nullptr;
    const PSID sid =
        reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;
    if (!ConvertSidToStringSidW(sid, &sidString))
        return QString();
    const QString result = QString::fromWCharArray(sidString);
    LocalFree(sidString);
    return result;
}

QVector<DWORD> candidateSessionIds()
{
    QVector<DWORD> sessionIds;
    const DWORD consoleId = WTSGetActiveConsoleSessionId();
    if (consoleId != 0xFFFFFFFF)
        sessionIds.append(consoleId);

    PWTS_SESSION_INFOW sessions = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions,
                               &count))
        return sessionIds;
    const WTS_CONNECTSTATE_CLASS preference[] = {WTSActive, WTSDisconnected};
    for (const WTS_CONNECTSTATE_CLASS wanted : preference) {
        for (DWORD i = 0; i < count; ++i) {
            if (sessions[i].State != wanted)
                continue;
            if (sessions[i].SessionId == consoleId)
                continue;
            sessionIds.append(sessions[i].SessionId);
        }
    }
    WTSFreeMemory(sessions);
    return sessionIds;
}

HANDLE acquireLoggedOnUserToken(const QString& preferredUserId,
                                QString* error)
{
    const QVector<DWORD> sessionIds = candidateSessionIds();
    HANDLE token = nullptr;
    if (!preferredUserId.isEmpty()) {
        for (const DWORD sessionId : sessionIds) {
            if (!WTSQueryUserToken(sessionId, &token))
                continue;
            if (tokenUserSid(token).compare(preferredUserId,
                                            Qt::CaseInsensitive) == 0)
                return token;
            CloseHandle(token);
            token = nullptr;
        }
        *error = QStringLiteral("The user who requested the Hub update has no available session");
        return nullptr;
    }
    for (const DWORD sessionId : sessionIds) {
        if (WTSQueryUserToken(sessionId, &token))
            return token;
    }
    *error = QStringLiteral(
                 "No session with a logged-on user was found (error %1)")
                 .arg(GetLastError());
    return nullptr;
}

}

QString currentUserId()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return QString();
    const QString sid = tokenUserSid(token);
    CloseHandle(token);
    return sid;
}

bool launchInActiveUserSession(const QString& program,
                               const QStringList& arguments,
                               const QString& preferredUserId, QString* error)
{
    HANDLE userToken = acquireLoggedOnUserToken(preferredUserId, error);
    if (!userToken)
        return false;

    UserSessionReady ready;
    if (!ready.create(tokenUserSid(userToken), error)) {
        CloseHandle(userToken);
        return false;
    }

    HANDLE primaryToken = nullptr;
    const BOOL duplicated =
        DuplicateTokenEx(userToken, TOKEN_ALL_ACCESS, nullptr,
                         SecurityImpersonation, TokenPrimary, &primaryToken);
    CloseHandle(userToken);
    if (!duplicated) {
        *error = QStringLiteral("Cannot duplicate the user token (error %1)")
                     .arg(GetLastError());
        return false;
    }

    void* environment = nullptr;
    if (!CreateEnvironmentBlock(&environment, primaryToken, FALSE)) {
        *error = QStringLiteral(
                     "Cannot build the user environment (error %1)")
                     .arg(GetLastError());
        CloseHandle(primaryToken);
        return false;
    }

    QString commandLine = quoteArgument(QDir::toNativeSeparators(program));
    for (const QString& argument : arguments)
        commandLine += QLatin1Char(' ') + quoteArgument(argument);
    commandLine += QStringLiteral(" --update-ready-event ") + ready.name();
    std::wstring commandLineW = commandLine.toStdWString();
    std::wstring desktopW = L"winsta0\\default";
    const std::wstring workingDirectory =
        QDir::toNativeSeparators(QFileInfo(program).absolutePath()).toStdWString();

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.lpDesktop = desktopW.data();
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = arguments.contains(QStringLiteral("--background"))
                             ? SW_HIDE : SW_SHOWNORMAL;
    PROCESS_INFORMATION process = {};

    const BOOL started = CreateProcessAsUserW(
        primaryToken, nullptr, commandLineW.data(), nullptr, nullptr, FALSE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP, environment,
        workingDirectory.c_str(), &startup, &process);
    const DWORD startError = GetLastError();

    DestroyEnvironmentBlock(environment);
    CloseHandle(primaryToken);

    if (!started) {
        *error =
            QStringLiteral("Cannot start the process in the user session "
                           "(error %1)")
                .arg(startError);
        return false;
    }
    CloseHandle(process.hThread);
    qInfo() << "Hub restart process created:" << process.dwProcessId;
    const bool confirmed = ready.wait(process.hProcess, 30000, error);
    CloseHandle(process.hProcess);
    return confirmed;
}

bool systemAnimationsEnabled()
{
    BOOL enabled = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0))
        return enabled == TRUE;
    return true;
}

bool launchInDesktopUserSession(const QString& program)
{
    const std::wstring path = QDir::toNativeSeparators(program).toStdWString();
    std::wstring arguments = L"\"" + path + L"\"";
    const std::wstring directory =
        QDir::toNativeSeparators(QFileInfo(program).absolutePath()).toStdWString();
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL queried = GetTokenInformation(token, TokenElevation, &elevation,
                                             sizeof(elevation), &returned);
    CloseHandle(token);
    if (!queried)
        return false;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!elevation.TokenIsElevated) {
        if (!CreateProcessW(path.c_str(), arguments.data(), nullptr, nullptr,
                            FALSE, 0, nullptr, directory.c_str(), &startup, &process))
            return false;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }

    const HWND shellWindow = GetShellWindow();
    if (!shellWindow)
        return false;
    DWORD shellPid = 0;
    GetWindowThreadProcessId(shellWindow, &shellPid);
    const HANDLE shellProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, shellPid);
    if (!shellProcess)
        return false;
    HANDLE shellToken = nullptr;
    const BOOL opened = OpenProcessToken(shellProcess, TOKEN_DUPLICATE | TOKEN_QUERY, &shellToken);
    CloseHandle(shellProcess);
    if (!opened)
        return false;
    const BOOL shellQueried = GetTokenInformation(shellToken, TokenElevation, &elevation,
                                                  sizeof(elevation), &returned);
    HANDLE launchToken = nullptr;
    const BOOL duplicated = shellQueried && !elevation.TokenIsElevated && DuplicateTokenEx(
        shellToken, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY |
                        TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
        nullptr, SecurityImpersonation, TokenPrimary, &launchToken);
    CloseHandle(shellToken);
    if (!duplicated)
        return false;

    void* environment = nullptr;
    if (!CreateEnvironmentBlock(&environment, launchToken, FALSE)) {
        CloseHandle(launchToken);
        return false;
    }
    const BOOL launched = CreateProcessWithTokenW(
        launchToken, 0, path.c_str(), arguments.data(), CREATE_UNICODE_ENVIRONMENT,
        environment, directory.c_str(), &startup, &process);
    DestroyEnvironmentBlock(environment);
    CloseHandle(launchToken);
    if (!launched)
        return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

}
