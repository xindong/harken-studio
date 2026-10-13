#include "Platform/UserSession.h"

#include <QProcess>
#include <QString>

#include <CoreFoundation/CoreFoundation.h>
#include <sys/stat.h>
#include <unistd.h>

namespace VNH {

QString currentUserId()
{
    return QString::number(getuid());
}

bool launchInActiveUserSession(const QString& program,
                               const QStringList& arguments,
                               const QString& preferredUserId, QString* error)
{
    Q_UNUSED(preferredUserId);
    if (geteuid() != 0) {
        if (QProcess::startDetached(program, arguments))
            return true;
        *error = QStringLiteral("Cannot start %1").arg(program);
        return false;
    }

    struct stat consoleInfo = {};
    if (stat("/dev/console", &consoleInfo) != 0 || consoleInfo.st_uid == 0) {
        *error = QStringLiteral("No active console user session");
        return false;
    }

    QStringList launchArguments;
    launchArguments << QStringLiteral("asuser")
                    << QString::number(consoleInfo.st_uid)
                    << QStringLiteral("/usr/bin/sudo") << QStringLiteral("-u")
                    << QStringLiteral("#%1").arg(consoleInfo.st_uid)
                    << program;
    launchArguments << arguments;
    if (!QProcess::startDetached(QStringLiteral("/bin/launchctl"),
                                 launchArguments)) {
        *error = QStringLiteral(
            "Cannot start the process in the user session");
        return false;
    }
    return true;
}

bool systemAnimationsEnabled()
{
    Boolean keyExists = false;
    const Boolean reduceMotion = CFPreferencesGetAppBooleanValue(
        CFSTR("reduceMotion"), CFSTR("com.apple.universalaccess"), &keyExists);
    if (keyExists && reduceMotion)
        return false;
    return true;
}

}
