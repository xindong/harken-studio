#pragma once

#include "ProjectContract.h"
#include "ProjectDataPolicy.h"
#include "ProjectIssue.h"
#include "ProjectMigration.h"
#include "ProjectVolume.h"

#include <QString>
#include <QStringList>

#include <QByteArray>
#include <QJsonObject>

#include <functional>
#include <utility>

namespace VNX {

class MigrationManager;

enum class ProjectUpgradeProtection
{
    AtomicInPlace,
    RecoverableInPlace,
};

enum class ProjectUpgradePhase
{
    Analyzing,
    CopyingStaging,
    Migrating,
    ValidatingStaging,
    BackingUp,
    Replacing,
    ValidatingProject,
    Committing,
    RollingBack,
    Completed,
};

struct ProjectUpgradeProgress
{
    ProjectUpgradePhase phase = ProjectUpgradePhase::Analyzing;
    int completedItems = 0;
    int totalItems = 0;
    qint64 completedBytes = 0;
    qint64 totalBytes = 0;
    QString detail;
};

using ProjectUpgradeProgressCallback = std::function<void(
    const ProjectUpgradeProgress&)>;

struct ProjectUpgradeStorageRequirements
{
    qint64 projectBytes = 0;
    qint64 recoveryBytes = 0;
    qint64 safetyBytes = 0;
};

struct ProjectUpgradeAnalysis
{
    bool ok = false;
    bool upgradeRequired = false;
    ProjectIssue issue = ProjectIssue::None;
    QString detail;
    QString projectFilePath;
    QString projectDirectory;
    int sourceSchema = 0;
    int targetSchema = 0;
    QByteArray sourceFingerprint;
    QByteArray sourceTreeFingerprint;
    ProjectDataPolicy dataPolicy;
    ProjectContractProbe probe;
    ProjectMigrationPlan migrationPlan;
    ProjectUpgradeProtection protection = ProjectUpgradeProtection::AtomicInPlace;
    ProjectVolumeInfo projectVolume;
    ProjectVolumeInfo recoveryVolume;
    qint64 requiredProjectBytes = 0;
    qint64 requiredRecoveryBytes = 0;
    qint64 stagingBytes = 0;
    qint64 transactionBytes = 0;
    int stagingFileCount = 0;
    int transactionFileCount = 0;
    QString recoveryRoot;
    QStringList warnings;
    QStringList blockingErrors;
};

using ProjectUpgradeApprovalCallback = std::function<bool(
    const ProjectUpgradeAnalysis&)>;

struct ProjectUpgradeResult
{
    bool ok = false;
    bool changed = false;
    ProjectIssue issue = ProjectIssue::None;
    QString detail;
    QString projectFilePath;
    QString backupPath;
    QString transactionPath;
    QByteArray backupFingerprint;
    QByteArray committedFingerprint;
    QStringList warnings;
    QString error;
};

using StagedProjectValidator = std::function<bool(const QString&, QString*)>;

class ProjectUpgradeManager
{
public:
    explicit ProjectUpgradeManager(MigrationManager* migrationManager = nullptr);

    using ProjectTreeValidatorForTesting = std::function<QStringList(
        const QString& projectDirectory, const QJsonObject& projectRoot,
        bool requireCurrentSchema)>;
    void setTargetSchemaForTesting(int schema) { targetSchemaForTesting_ = schema; }
    void setProjectTreeValidatorForTesting(ProjectTreeValidatorForTesting validator)
    {
        treeValidatorForTesting_ = std::move(validator);
    }

    ProjectUpgradeAnalysis analyze(
        const QString& projectFilePath,
        ProjectUpgradeProgressCallback progress = {}) const;

    ProjectUpgradeAnalysis analyzeForProjectOpen(
        const QString& projectFilePath,
        ProjectUpgradeProgressCallback progress = {}) const;

    static ProjectUpgradeStorageRequirements storageRequirements(
        qint64 stagingBytes,
        qint64 transactionSourceBytes,
        qint64 transactionTargetBytes,
        int fileCount,
        ProjectUpgradeProtection protection);

    ProjectUpgradeResult execute(const ProjectUpgradeAnalysis& analysis,
                                 StagedProjectValidator validator = {},
                                 ProjectUpgradeProgressCallback progress = {}) const;

    bool finalize(ProjectUpgradeResult& result, QString* error = nullptr) const;

    ProjectUpgradeResult readReceipt(const QString& projectFilePath,
                                     QString* error = nullptr) const;

    bool discardReceipt(const QString& projectFilePath,
                        QString* error = nullptr) const;

    bool rollbackLastUpgrade(const QString& projectFilePath,
                             QString* error = nullptr) const;

    bool recover(const QString& projectFilePath, QString* error = nullptr) const;

    bool rollback(const ProjectUpgradeResult& result,
                  QString* error = nullptr) const;

private:
    MigrationManager* migrationManager_ = nullptr;
    int targetSchemaForTesting_ = 0;
    ProjectTreeValidatorForTesting treeValidatorForTesting_;
};

}
