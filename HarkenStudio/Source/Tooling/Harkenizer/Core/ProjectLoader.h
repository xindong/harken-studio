#pragma once

#include "Core/AssetPipeline/AssetTypes.h"
#include "Core/IntegrityScanner.h"
#include "Core/ProjectSerializer.h"

#include <QList>
#include <QStringList>
#include <functional>
#include <memory>

namespace VNX {

class ProjectInstance;
class FolderSaveState;
class ProjectIndex;

struct PreparedProject
{
    PreparedProject();
    PreparedProject(PreparedProject&& other) noexcept;
    PreparedProject& operator=(PreparedProject&& other) noexcept;
    PreparedProject(const PreparedProject&) = delete;
    PreparedProject& operator=(const PreparedProject&) = delete;
    ~PreparedProject();

    void releaseUpgradeRecovery() { upgradePending = false; }
    void rollbackUpgrade();

    std::shared_ptr<ProjectInstance> instance;
    std::unique_ptr<ProjectData> project;
    std::unique_ptr<FolderSaveState> folderSaveState;
    std::unique_ptr<ProjectIndex> index;
    std::unique_ptr<IntegrityScanner> integrityScanner;
    IntegrityScanner::Report integrityReport;
    QList<AssetInfo> assets;
    QHash<QString, QByteArray> loadedDocuments;
    QStringList diagnostics;
    QString error;
    bool rebuildAssetRegistry = false;
    int audioLoadFailures = 0;
    ProjectUpgradeResult upgrade;
    bool upgradePending = false;
};

namespace ProjectLoader {

PreparedProject prepareProject(const QString& filePath,
                               bool allowUpgrade = false,
                               ProjectUpgradeProgressCallback upgradeProgress = {},
                               ProjectUpgradeApprovalCallback upgradeApproval = {},
                               std::function<bool()> cancelled = {},
                               ProjectContractReadContext* context = nullptr,
                               ProjectSerializer::LoadMode mode = ProjectSerializer::LoadMode::RecoverTransactions);

}

}
