#pragma once

#include <QString>
#include <vector>

namespace VNH {

enum class EngineKind
{
    Unity,
    Unreal
};

struct EngineTargetRecord
{
    QString targetId;
    EngineKind engineKind = EngineKind::Unity;
    QString canonicalRoot;
    QString descriptorPath;
    QString displayName;
    QString addedAt;
    QString sdkInstallPath;
};

class EngineTargetStore
{
public:
    EngineTargetStore() = default;
    ~EngineTargetStore();

    EngineTargetStore(const EngineTargetStore&) = delete;
    EngineTargetStore& operator=(const EngineTargetStore&) = delete;

    bool open(const QString& databasePath);
    void close();
    bool add(const EngineTargetRecord& record);
    bool remove(const QString& targetId);
    bool updateSdkInstallPath(const QString& targetId, const QString& relativePath);
    std::vector<EngineTargetRecord> list() const;
    QString lastError() const { return lastError_; }

private:
    QString connectionName_;
    mutable QString lastError_;
};

}
