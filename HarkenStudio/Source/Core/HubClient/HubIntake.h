#pragma once

#include <QString>

namespace VNX {

class HubIntake
{
public:
    static QString intakeDir();
    static bool recordProjectOpened(const QString& projectFilePath);
};

}
