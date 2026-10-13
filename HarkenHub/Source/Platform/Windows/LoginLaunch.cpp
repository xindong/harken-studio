#include "Platform/LoginLaunch.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

namespace VNH {

namespace {

const QString kRunKey = QStringLiteral(
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
const QString kValueName = QStringLiteral("HarkenHub");

}

bool registerBackgroundOnLogin()
{
    QSettings run(kRunKey, QSettings::NativeFormat);
    const QString command =
        QStringLiteral("\"%1\" --background")
            .arg(QDir::toNativeSeparators(
                QCoreApplication::applicationFilePath()));
    run.setValue(kValueName, command);
    run.sync();
    return run.status() == QSettings::NoError;
}

bool unregisterBackgroundOnLogin()
{
    QSettings run(kRunKey, QSettings::NativeFormat);
    run.remove(kValueName);
    run.sync();
    return run.status() == QSettings::NoError;
}

bool isBackgroundOnLoginRegistered()
{
    QSettings run(kRunKey, QSettings::NativeFormat);
    return run.contains(kValueName);
}

}
