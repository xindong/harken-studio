#pragma once

#include <QString>

namespace harken::hub {

class BinaryTrustPolicy
{
public:
    static bool installedTreeRequired();
    static bool installerRequired();
    static bool verifyInstalledTree(const QString& installRoot, QString* error);
    static bool verifyInstallerBinary(const QString& installerPath,
                                      QString* error);
};

}
