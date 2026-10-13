#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

namespace VNH {

struct InstallPreparation
{
    bool ok = false;
    QString error;
    QString transactionId;
    QString version;
    QString buildId;
};

class InstallRequest
{
public:
    static QString manifestPathForPackage(const QString& packagePath);
    static InstallPreparation prepare(const QString& packagePath,
                                      const QString& manifestEnvelopePath,
                                      const QByteArray& manifestPublicKey,
                                      const QDateTime& now,
                                      bool allowExpired = false);
};

}
