#pragma once

#include <QString>

namespace harken::studio {

class HubIntake
{
public:
    static QString intakeDir();
    static bool recordProjectOpened(const QString& projectFilePath);
};

}
