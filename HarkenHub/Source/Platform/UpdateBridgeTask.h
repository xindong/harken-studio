#pragma once

#include <QString>

namespace VNH {

QString updateBridgeTaskName();
bool updateBridgeTaskAvailable();
bool registerUpdateBridgeTask(const QString& hubExePath, QString* error);
bool unregisterUpdateBridgeTask(QString* error);
bool runUpdateBridgeTask(QString* error);

}
