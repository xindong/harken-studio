#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

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
    QString integrationSourcePath;
    QString integrationInstallPath;
    bool checkOnly = false;
    bool inspectOnly = false;
    bool removeIntegration = false;
    bool restorePrevious = false;
};

struct UnityIntegrationResult
{
    bool cancelled = false;
    bool success = false;
    bool hasWarnings = false;
    bool targetInstalled = false;
    bool targetCurrent = false;
    bool integrationFound = false;
    bool integrationRemoved = false;
    bool previousRestored = false;
    bool previousVersionAvailable = false;
    QVector<UnityIntegrationStep> steps;
    QString unityProjectPath;
    QString integrationSourcePath;
    QString integrationInstallPath;
    QString installedVersion;
    QString sourceVersion;
    QString backupPath;
    QString jsonReportPath;
    QString textReportPath;
    int integrationFilesCopied = 0;
};

class HubUnityIntegrationService
{
public:
    static UnityIntegrationResult run(const UnityIntegrationRequest& request);
    static QString localizeText(const QString& text);
};

}
