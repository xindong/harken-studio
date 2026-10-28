#pragma once

#include <QString>
#include <QStringList>

namespace VNH {

QString currentUserId();
#ifdef Q_OS_WIN
bool launchInDesktopUserSession(const QString& program);
#endif
bool launchInActiveUserSession(const QString& program,
                               const QStringList& arguments,
                               const QString& preferredUserId, QString* error);
bool systemAnimationsEnabled();

}
