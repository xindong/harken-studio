#pragma once

#include <QString>
#include <QSet>
#include <memory>

namespace VNX {

struct ProjectData;
struct PhysicalFolder;

class FolderSaveState
{
public:
    static std::unique_ptr<FolderSaveState> capture(const ProjectData& project,
                                                   QString* error);
    ~FolderSaveState();

    bool stage(const ProjectData& project, const QString& directory, QString* error);
    void markSaved(const ProjectData& project);
    bool isFolderDirty(const ProjectData& project, const PhysicalFolder* folder) const;
    QSet<QString> bucketsWithPendingFileChanges(const ProjectData& project) const;

private:
    struct Data;
    explicit FolderSaveState(std::unique_ptr<Data> data);
    std::unique_ptr<Data> data_;
};

}
