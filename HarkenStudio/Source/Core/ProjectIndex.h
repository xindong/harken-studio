
#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QUuid>
#include <cstdint>
#include <memory>
#include <vector>

#include "ProjectTypes.h"
class QLockFile;

namespace VNX {

class ProjectIndex
{
public:
    static constexpr int kIndexFormatVersion = 3;

    struct Entry
    {
        QUuid             guid;
        QString           name;
        ProjectItem::Type type = ProjectItem::Type::None;
        QUuid             parentGuid;
        QString           filePath;
        std::vector<int>  embedPath;
        qint64            mtime = 0;
        qint64            size  = 0;
        QByteArray        hash;
    };

    enum class ScanResult : uint8_t
    {
        Ok = 0,
        VersionMismatch,
        LockBusy,
        IoError
    };

    ProjectIndex();
    ~ProjectIndex();

    ProjectIndex(const ProjectIndex&)            = delete;
    ProjectIndex& operator=(const ProjectIndex&) = delete;

    bool loadFromCache(const QString& projectRoot);

    ScanResult scanProjectRoot(const QString& projectRoot);

    bool saveToCache(const QString& projectRoot) const;

    const Entry* findByGuid(const QUuid& guid) const;
    const Entry* findByName(ProjectItem::Type type, const QString& name) const;
    std::vector<const Entry*> findAllByName(ProjectItem::Type type, const QString& name) const;

    std::vector<QUuid> guidsWithMissingParent() const;

    QByteArray contentHash() const;

    int  entryCount() const;
    void clear();

private:
    QHash<QUuid, Entry> m_byGuid;

    QHash<int, QMultiHash<QString, QUuid>> m_byName;

    mutable std::unique_ptr<QLockFile> m_lock;
};

}
