#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace VNX {

class UnityIntegrationService final
{
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

    struct Request
    {
        QString unityProjectPath;
        QString integrationSourcePath;
        QString integrationInstallPath;
        bool checkOnly = false;
        bool removeIntegration = false;
        bool restorePrevious = false;
    };

    struct Result
    {
        bool success = false;
        bool hasWarnings = false;
        bool targetInstalled = false;
        bool targetCurrent = false;
        bool integrationRemoved = false;
        bool previousRestored = false;
        bool previousVersionAvailable = false;
        QVector<StepResult> steps;
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

    Result run(const Request& request);

    static QString locateIntegrationSource(const QString& preferredPath = QString());
    static QString previousVersionBackup(const QString& unityProjectPath);
    static QString statusName(StepStatus status);

private:
    Result install(const Request& request);
    Result remove(const Request& request);
    Result restorePrevious(const Request& request);
};

} // namespace VNX
