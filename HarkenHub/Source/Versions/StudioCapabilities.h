#pragma once

#include <QString>

class QJsonObject;

namespace VNH {

inline constexpr int kStudioCapabilitiesVersion = 2;

bool validateStudioCapabilities(const QJsonObject& object,
                                const QString& expectedVersion,
                                const QString& expectedBuildId,
                                QString* error = nullptr,
                                int* capabilitiesVersion = nullptr);

}
