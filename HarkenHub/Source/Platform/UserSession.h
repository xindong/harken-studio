#pragma once

#include <QString>
#include <QStringList>

namespace VNH {

QString currentUserId();
bool launchInActiveUserSession(const QString& program,
                               const QStringList& arguments,
                               const QString& preferredUserId, QString* error);
bool systemAnimationsEnabled();

}
