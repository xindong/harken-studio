#pragma once

#include <QLockFile>
#include <QString>

#include <memory>

namespace VNH {

class DownloadLease
{
public:
    static std::shared_ptr<DownloadLease> acquire(const QString& path,
                                                QString* error = nullptr);
    static bool isCachePath(const QString& path);
    bool covers(const QString& path) const;

private:
    explicit DownloadLease(const QString& lockPath);
    static QString lockPathFor(const QString& path);

    QString lockPath_;
    QLockFile lock_;
};

}
