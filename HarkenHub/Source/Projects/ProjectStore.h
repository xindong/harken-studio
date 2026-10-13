#pragma once

#include <QString>
#include <vector>

namespace VNH {

struct ProjectRecord
{
    QString projectId;
    QString canonicalPath;
    QString displayName;
    int detectedSchema = 0;
    QString preferredStudioBuildId;
    QString lastKnownGoodStudioBuildId;
    QString lastOpenedAt;
    QString lastPreflightAt;

    QString linkedStudioBuildId() const
    {
        return preferredStudioBuildId;
    }
};

class ProjectStore
{
public:
    ProjectStore() = default;
    ~ProjectStore();

    ProjectStore(const ProjectStore&) = delete;
    ProjectStore& operator=(const ProjectStore&) = delete;

    bool open(const QString& databasePath);
    void close();
    bool isOpen() const;

    bool addProject(const ProjectRecord& record);
    bool removeProject(const QString& canonicalPath);
    bool contains(const QString& canonicalPath) const;
    bool setPreferredBuild(const QString& canonicalPath, const QString& buildId);
    bool setLastKnownGood(const QString& canonicalPath, const QString& buildId,
                          const QString& at);
    bool recordSuccessfulOpen(const QString& canonicalPath,
                              const QString& projectId,
                              const QString& displayName,
                              int detectedSchema,
                              const QString& buildId,
                              const QString& at);
    bool setLastOpened(const QString& canonicalPath, const QString& at);
    bool setLastPreflight(const QString& canonicalPath, const QString& at);
    std::vector<ProjectRecord> listProjects() const;

    QString lastError() const { return lastError_; }

private:
    QString connectionName_;
    mutable QString lastError_;
};

}
