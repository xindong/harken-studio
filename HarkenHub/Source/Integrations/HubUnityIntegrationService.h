#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QMap>
#include <QJsonObject>

namespace VNH {

enum class UnityIntegrationStepStatus
{
    Pending,
    Running,
    Succeeded,
    Warning,
    Failed,
    Skipped,
};

struct UnityIntegrationStep
{
    QString id;
    QString title;
    UnityIntegrationStepStatus status = UnityIntegrationStepStatus::Pending;
    QString summary;
    QString cause;
    QString remediation;
    QStringList details;
    QStringList artifacts;
    qint64 elapsedMs = 0;
};

struct UnityIntegrationRequest
{
    QString unityProjectPath;
    QString sdkSourcePath;
    QString sdkInstallPath;
    bool installToAssets = false;
    bool checkOnly = false;
    bool verifyInstalledOnly = false;
    bool confirmTargetChanges = false;
    QByteArray approvedPlanHash;
    bool preserveUnownedTarget = false;
    bool restoreIntegration = false;
    bool confirmUnownedConflicts = false;
    bool recoverRestore = false;
    QJsonObject officialRelease;
    QMap<QString, QByteArray> officialFileHashes;
    QString restorationBackupPath;
    bool removeIntegration = false;
    bool confirmRemoval = false;
    QString removalBackupPath;
    bool migrateIntegration = false;
    bool recoverMigration = false;
    bool confirmMigration = false;
    QString migrationTargetPath;
    QString migrationBackupPath;
    QString migrationPlanHash;
};

struct UnityIntegrationFileChange
{
    QString action;
    QString path;
};

struct UnityIntegrationResult
{
    bool cancelled = false;
    bool success = false;
    bool hasWarnings = false;
    bool requiresConfirmation = false;
    bool targetAlreadyCurrent = false;
    bool targetCurrent = false;
    bool targetInstalled = false;
    bool integrationFound = false;
    bool integrationRemoved = false;
    bool removalBackupCreated = false;
    bool changesDetected = false;
    QByteArray planHash;
    QStringList modifiedFiles;
    QStringList conflictingFiles;
    QStringList lineEndingFiles;
    QStringList encodingFiles;
    QStringList missingFiles;
    QStringList missingOptionalFiles;
    QStringList unreadableFiles;
    bool integrationMigrated = false;
    bool migrationRecovered = false;
    QString migrationBackupPath;
    QString migrationTargetPath;
    QString migrationPlanHash;
    QVector<UnityIntegrationStep> steps;
    QVector<UnityIntegrationFileChange> fileChanges;
    QString unityProjectPath;
    QString sdkSourcePath;
    QString sdkInstallPath;
    QString removalBackupPath;
    QString restorationBackupPath;
    QString jsonReportPath;
    QString textReportPath;
    int sdkFilesCopied = 0;
    int filesAdded = 0;
    int filesChanged = 0;
    int staleFilesToRemove = 0;
    int targetOnlyFilesPreserved = 0;
    int filesRemoved = 0;
};

class HubUnityIntegrationService
{
public:
    static UnityIntegrationResult run(const UnityIntegrationRequest& request);
    static QString localizeText(const QString& text);
};

}
