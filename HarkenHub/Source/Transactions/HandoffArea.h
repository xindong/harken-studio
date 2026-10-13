#pragma once

#include <QByteArray>
#include <QString>

namespace VNH {

struct HandoffPlacement
{
    bool ok = false;
    QString error;
    QString transactionId;
    QString packagePath;
    QString manifestPath;
};

class HandoffArea
{
public:
    static QString rootDir();
    static QString packagePath(const QString& transactionId);
    static QString manifestPath(const QString& transactionId);
    static QString queuePath(const QString& transactionId);
    static QString newTransactionId();
    static bool isValidTransactionId(const QString& transactionId);
    static HandoffPlacement place(const QString& packageFilePath,
                                  const QByteArray& releaseManifestJson);
    static HandoffPlacement placeWithId(const QString& transactionId,
                                        const QString& packageFilePath,
                                        const QByteArray& releaseManifestJson);
    static bool discard(const QString& transactionId);
};

}
