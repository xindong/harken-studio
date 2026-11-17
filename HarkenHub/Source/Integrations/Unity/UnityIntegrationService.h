#pragma once

#include <QObject>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace VNX {

class UnityIntegrationService final : public QObject
{
    Q_OBJECT

public:
    enum class StepStatus
    {
        Pending,
        Running,
        Succeeded,
        Warning,
        Failed,
        Skipped,
    };

    struct StepResult
    {
        QString id;
        QString title;
        StepStatus status = StepStatus::Pending;
        QString summary;
        QString cause;
        QString remediation;
        QStringList details;
        QStringList artifacts;
        qint64 elapsedMs = 0;
    };

    struct FileChange
    {
        QString action;
        QString relative;
    };

    struct Request
    {
        QString unityProjectPath;
        QString integrationSourcePath;
        QString integrationInstallPath;
        bool installToAssets = false;
        bool checkOnly = false;
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

    struct Result
    {
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
        QVector<StepResult> steps;
        QVector<FileChange> fileChanges;
        QString unityProjectPath;
        QString integrationSourcePath;
        QString integrationInstallPath;
        QString removalBackupPath;
        QString restorationBackupPath;
        QString jsonReportPath;
        QString textReportPath;
        int integrationFilesCopied = 0;
        int filesAdded = 0;
        int filesChanged = 0;
        int staleFilesToRemove = 0;
        int targetOnlyFilesPreserved = 0;
        int filesRemoved = 0;
    };

    explicit UnityIntegrationService(QObject* parent = nullptr);

    Result run(const Request& request);

    static QString locateIntegrationSource(const QString& preferredPath = QString());
    static QString statusName(StepStatus status);

signals:
    void stepUpdated(int index, const VNX::UnityIntegrationService::StepResult& step);

private:
    Result restoreIntegration(const Request& request);
    Result migrateIntegration(const Request& request);
    Result removeIntegration(const Request& request);
    static QJsonObject stepToJson(const StepResult& step);
};

} // namespace VNX
