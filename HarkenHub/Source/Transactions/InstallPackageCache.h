#pragma once

#include <QString>

namespace VNH {

class InstallPackageCache
{
public:
    static QString packagePath(const QString& buildId);
    static QString manifestPath(const QString& buildId);
    static bool store(const QString& buildId, const QString& packageSource,
                      const QString& manifestSource, QString* error);
    static bool remove(const QString& buildId);
};

}
