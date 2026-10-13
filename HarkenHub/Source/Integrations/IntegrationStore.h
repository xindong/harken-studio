#pragma once

#include <QString>
#include <vector>

namespace VNH {

struct IntegrationRecord
{
    QString integrationId;
    QString harkenProjectPath;
    QString engineTargetId;
    QString sdkSourcePath;
    QString createdAt;
    QString updatedAt;
    bool enabled = true;
};

class IntegrationStore
{
public:
    IntegrationStore() = default;
    ~IntegrationStore();

    IntegrationStore(const IntegrationStore&) = delete;
    IntegrationStore& operator=(const IntegrationStore&) = delete;

    bool open(const QString& databasePath);
    void close();
    bool add(const IntegrationRecord& record, bool replaceTarget);
    bool updateSdkSourcePath(const QString& integrationId,
                             const QString& sdkSourcePath);
    bool remove(const QString& integrationId);
    std::vector<IntegrationRecord> list() const;
    QString lastError() const { return lastError_; }

private:
    QString connectionName_;
    mutable QString lastError_;
};

}
