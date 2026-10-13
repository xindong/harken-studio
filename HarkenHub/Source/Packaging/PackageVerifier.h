#pragma once

#include <QHash>
#include <QString>

namespace VNH {

struct ExpectedPackage
{
    QString buildId;
    QString semanticVersion;
    QString sha256Hex;
    qint64 sizeBytes = -1;
};

struct PackageVerifyResult
{
    bool ok = false;
    QString error;
    QString channel;
    int capabilitiesVersion = 0;
    QHash<QString, QString> fileSha256ByPath;
};

class PackageVerifier
{
public:
    static bool checkHostPlatform(const QString& archivePath, QString* error);
    static PackageVerifyResult verify(const QString& archivePath,
                                      const ExpectedPackage& expected);
};

}
