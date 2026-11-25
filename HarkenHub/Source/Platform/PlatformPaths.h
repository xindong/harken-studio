#pragma once

#include <QString>

namespace harken::hub {

QString studioVersionsRoot();

QString studioBinaryRelativePath();

QString hubInstallDir();

#ifdef Q_OS_WIN
QString registeredHubInstallDir();
#endif

QString userDataDir();

QString machineDataDir();

}
