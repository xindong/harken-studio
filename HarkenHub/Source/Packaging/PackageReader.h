#pragma once

#include <QHash>
#include <QString>
#include <vector>

namespace VNH {

struct PackageEntry
{
    QString path;
    qint64 uncompressedSize = 0;
    bool isDirectory = false;
};

struct PackageListResult
{
    bool ok = false;
    QString error;
    std::vector<PackageEntry> entries;
};

class PackageReader
{
public:
    static QString fileSha256Hex(const QString& filePath);
    static bool isSafeEntryPath(const QString& rawName, QString* errorOut = nullptr);
    static PackageListResult listEntries(const QString& archivePath);
    static bool readEntry(const QString& archivePath, const QString& entryPath,
                          QByteArray* out, QString* errorOut);
    static bool extractAll(const QString& archivePath, const QString& targetDir,
                           QString* errorOut);
    static bool extractAllVerified(const QString& archivePath, const QString& targetDir,
                                   const QHash<QString, QString>& expectedSha256ByPath,
                                   QString* errorOut);
};

}
