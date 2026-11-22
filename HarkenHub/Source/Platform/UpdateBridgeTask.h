#pragma once

#include <QString>

namespace harken::hub {

QString updateBridgeTaskName();
bool updateBridgeTaskAvailable();
bool registerUpdateBridgeTask(const QString& hubExePath, QString* error);
bool unregisterUpdateBridgeTask(QString* error);
bool runUpdateBridgeTask(QString* error);

}
