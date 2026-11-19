#pragma once

#include <QString>
#include <QJsonObject>
#include <QStringList>

namespace VNH {

struct IntegrityReport
{
    bool ok = false;
    QString error;
    QStringList missingFiles;
    QStringList mismatchedFiles;
};

struct InstallationSnapshot
{
    QJsonObject files;
    QString hash;
    QString error;
};

class IntegrityChecker
{
public:
    static InstallationSnapshot snapshot(const QString& installPath);
    static IntegrityReport check(
        const QString& installPath,
        const QString& expectedPackageManifestHash = QString());
};

}
