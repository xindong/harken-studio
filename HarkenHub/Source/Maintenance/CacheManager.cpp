#include "Maintenance/CacheManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace VNH {

namespace {

void cleanDirectory(const QString& directory, const QStringList& nameFilters,
                    int retentionDays, const QDateTime& now,
                    const QList<QFileInfo>& protectedFiles,
                    CacheCleanReport& report)
{
    if (retentionDays < 0)
        return;
    QDir dir(directory);
    if (!dir.exists())
        return;
    const QDateTime cutoff = now.addDays(-retentionDays);
    const auto entries =
        dir.entryInfoList(nameFilters, QDir::Files | QDir::NoSymLinks);
    for (const QFileInfo& info : entries) {
        if (info.lastModified() >= cutoff || protectedFiles.contains(info))
            continue;
        if (QFile::remove(info.filePath()))
            report.removedFiles.append(info.filePath());
        else
            report.failedFiles.append(info.filePath());
    }
}

}

CacheCleanReport CacheManager::clean(const QString& userDataRoot,
                                     const CacheCleanOptions& options,
                                     const QDateTime& now)
{
    CacheCleanReport report;
    QList<QFileInfo> protectedFiles;
    for (const QString& path : options.protectedFiles) {
        if (!path.isEmpty())
            protectedFiles.append(QFileInfo(path));
    }
    const QString downloads = userDataRoot + QStringLiteral("/downloads");
    const QString hubUpdates =
        userDataRoot + QStringLiteral("/hub-updates");
    const QString quarantine = userDataRoot + QStringLiteral("/quarantine");
    const QString logs = userDataRoot + QStringLiteral("/logs");

    cleanDirectory(downloads,
                   {QStringLiteral("*.part"), QStringLiteral("*.part.meta")},
                   options.partialRetentionDays, now, protectedFiles, report);
    cleanDirectory(downloads,
                   {QStringLiteral("*.zip"), QStringLiteral("*.manifest")},
                   options.downloadRetentionDays, now, protectedFiles, report);
    cleanDirectory(hubUpdates,
                   {QStringLiteral("*.part"), QStringLiteral("*.part.meta")},
                   options.partialRetentionDays, now, protectedFiles, report);
    cleanDirectory(hubUpdates,
                   {QStringLiteral("*.exe"), QStringLiteral("*.msi"),
                    QStringLiteral("*.manifest")},
                   options.downloadRetentionDays, now, protectedFiles, report);
    cleanDirectory(quarantine, {}, options.quarantineRetentionDays, now,
                   protectedFiles, report);
    cleanDirectory(logs, {QStringLiteral("*.log")}, options.logRetentionDays,
                   now, protectedFiles, report);
    return report;
}

}
