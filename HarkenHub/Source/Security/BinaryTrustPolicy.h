#pragma once

#include <QString>

namespace VNH {

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
