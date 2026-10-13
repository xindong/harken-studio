#include "Platform/Windows/UserSessionReady.h"

#include <QElapsedTimer>
#include <QUuid>

#include <sddl.h>

namespace VNH {

namespace {

const QString kEventPrefix = QStringLiteral("Global\\HarkenHub.Ready.");

}

UserSessionReady::~UserSessionReady()
{
    if (event_)
        CloseHandle(event_);
}

bool UserSessionReady::create(const QString& userId, QString* error)
{
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(userId.toStdWString().c_str(), &sid)) {
        *error = QStringLiteral("Invalid Hub restart user (error %1)")
                     .arg(GetLastError());
        return false;
    }
    LocalFree(sid);
    const QString sddl =
        QStringLiteral("D:P(A;;GA;;;SY)(A;;0x0002;;;%1)S:(ML;;NW;;;ME)").arg(userId);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.toStdWString().c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        *error = QStringLiteral("Cannot secure the Hub ready event (error %1)")
                     .arg(GetLastError());
        return false;
    }
    SECURITY_ATTRIBUTES attributes = {sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    name_ = kEventPrefix + QUuid::createUuid().toString(QUuid::WithoutBraces);
    event_ = CreateEventW(&attributes, TRUE, FALSE, name_.toStdWString().c_str());
    const DWORD result = GetLastError();
    LocalFree(descriptor);
    if (!event_ || result == ERROR_ALREADY_EXISTS) {
        *error = QStringLiteral("Cannot create the Hub ready event (error %1)")
                     .arg(result);
        return false;
    }
    return true;
}

bool UserSessionReady::wait(HANDLE process, int timeoutMs, QString* error) const
{
    QElapsedTimer timer;
    timer.start();
    const HANDLE handles[] = {event_, process};
    DWORD result = WaitForMultipleObjects(2, handles, FALSE,
                                          static_cast<DWORD>(timeoutMs));
    if (result == WAIT_OBJECT_0)
        return true;
    if (result == WAIT_OBJECT_0 + 1) {
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(process, &exitCode)) {
            *error = QStringLiteral("Cannot read the Hub exit code (error %1)")
                         .arg(GetLastError());
            return false;
        }
        if (exitCode != 0) {
            *error = QStringLiteral("Hub exited before startup confirmation (exit %1)")
                         .arg(exitCode);
            return false;
        }
        result = WaitForSingleObject(
            event_, static_cast<DWORD>(qMax<qint64>(0, timeoutMs - timer.elapsed())));
        if (result == WAIT_OBJECT_0)
            return true;
    }
    *error = result == WAIT_TIMEOUT
                 ? QStringLiteral("Timed out waiting for Hub startup confirmation")
                 : QStringLiteral("Cannot wait for Hub startup confirmation (error %1)")
                       .arg(GetLastError());
    return false;
}

bool UserSessionReady::signal(const QString& name, QString* error)
{
    if (!name.startsWith(kEventPrefix) ||
        QUuid(name.mid(kEventPrefix.size())).isNull()) {
        *error = QStringLiteral("Invalid Hub ready event name");
        return false;
    }
    const HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE,
                                    name.toStdWString().c_str());
    if (!event) {
        *error = QStringLiteral("Cannot open the Hub ready event (error %1)")
                     .arg(GetLastError());
        return false;
    }
    const BOOL signaled = SetEvent(event);
    const DWORD result = GetLastError();
    CloseHandle(event);
    if (!signaled)
        *error = QStringLiteral("Cannot signal Hub startup readiness (error %1)")
                     .arg(result);
    return signaled != FALSE;
}

}
