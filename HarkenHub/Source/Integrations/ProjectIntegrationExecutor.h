#pragma once

#include "Integrations/EngineTargetStore.h"

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <vector>

namespace VNH {

enum class IntegrationOperation
{
    Install,
    Update,
    Repair,
    Remove
};

struct IntegrationFileChange
{
    QString path;
    QString action;
};

struct IntegrationPlan
{
    bool cancelled = false;
    bool valid = false;
    bool confirmationRequired = false;
    QString error;
    QString sourceRoot;
    QString targetRoot;
    QString version;
    QByteArray fingerprint;
    QStringList modifiedManagedFiles;
    QStringList preservedTargetFiles;
    std::vector<IntegrationFileChange> changes;
};

struct IntegrationOperationResult
{
    bool success = false;
    QString error;
    QString backupPath;
    QString installedPath;
};

struct SdkMigrationPlan
{
    bool valid = false;
    bool recovering = false;
    QString error;
    QString sourceRoot;
    QString targetRoot;
    QString backupRoot;
    QString transactionId;
    QJsonObject inventory;
    QByteArray fingerprint;
};

class ProjectIntegrationExecutor
{
public:
    static QString locateUnrealSource(const QString& path);
    static bool resolveUnrealLocation(const EngineTargetRecord& target, const QString& input,
                                      QString* path, QString* error);
    static bool locateUnrealInstallation(const EngineTargetRecord& target, QString* path, QString* error);
    static QString unrealMigrationJournalPath(const EngineTargetRecord& target);
    static bool hasPendingUnrealMigration(const EngineTargetRecord& target);
    static SdkMigrationPlan planUnrealMigration(const EngineTargetRecord& target,
                                               const QString& destination, bool recover = false);
    static IntegrationOperationResult executeUnrealMigration(const EngineTargetRecord& target,
                                                             const SdkMigrationPlan& plan, bool confirmed);
    static IntegrationPlan inspectUnreal(const EngineTargetRecord& target, bool verifyContent = true);
    static bool checkUnrealClosed(const EngineTargetRecord& target, QString* error);
    static IntegrationPlan planUnreal(
        const EngineTargetRecord& target,
        const QString& sourceRoot,
        IntegrationOperation operation);
    static IntegrationOperationResult executeUnreal(
        const EngineTargetRecord& target,
        const IntegrationPlan& plan,
        IntegrationOperation operation,
        bool confirmed);
};

}
