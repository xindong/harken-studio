#pragma once

#include <QString>
#include <QStringList>

namespace VNH {

struct IntegrityReport
{
    bool ok = false;
    QString error;
    QStringList missingFiles;
    QStringList mismatchedFiles;
};

class IntegrityChecker
{
public:
    static IntegrityReport check(
        const QString& installPath,
        const QString& expectedPackageManifestHash = QString(),
        const QString& expectedCapabilitiesHash = QString());
};

}
