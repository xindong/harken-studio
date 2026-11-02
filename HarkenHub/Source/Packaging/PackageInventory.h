#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>

namespace VNH {

struct PackageFileRecord
{
    qint64 size = -1;
    QString sha256;
};

struct PackageInventory
{
    bool ok = false;
    QString error;
    QString channel;
    QString unityRoot;
    QString unityVersion;
    QHash<QString, PackageFileRecord> files;

    static constexpr qint64 maximumBytes = 16 * 1024 * 1024;
    static PackageInventory parse(const QByteArray& data, const QString& version,
                                  const QString& buildId, const QString& channel = {});
};

}
