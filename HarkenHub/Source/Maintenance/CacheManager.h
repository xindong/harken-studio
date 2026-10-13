#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

namespace VNH {

struct CacheCleanOptions
{
    int partialRetentionDays = 7;
    int downloadRetentionDays = 14;
    int quarantineRetentionDays = 14;
    int logRetentionDays = 30;
    QStringList protectedFiles;
};

struct CacheCleanReport
{
    QStringList removedFiles;
    QStringList failedFiles;
};

class CacheManager
{
public:
    static CacheCleanReport clean(const QString& userDataRoot,
                                  const CacheCleanOptions& options,
                                  const QDateTime& now);
};

}
