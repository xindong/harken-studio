#pragma once

#include <QString>

namespace harken::hub::unity {

bool checkUnityProjectClosed(const QString& project, QString* error, bool* isOpen = nullptr);

}
