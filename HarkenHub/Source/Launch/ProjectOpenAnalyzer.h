#pragma once

#include "Versions/VersionScanner.h"

#include <QByteArray>
#include <QString>

namespace VNH {

struct ProjectOpenAnalysis
{
    bool reportValid = false;
    bool ok = false;
    QString errorCode;
    QString technicalDetails;
    QString studioVersion;
    QString buildId;
    QString requestId;
    QString projectPath;
    QString projectId;
    int currentSchema = 0;
    int targetSchema = 0;
    bool upgradeRequired = false;
    int migrationStepCount = 0;
    int affectedFileCount = 0;
    qint64 requiredProjectBytes = 0;
    qint64 requiredRecoveryBytes = 0;
    QString recoveryRoot;
    QString protection;
    QString approvalDigest;
};

class ProjectOpenAnalyzer
{
public:
    static constexpr int kProtocolVersion = 1;

    static ProjectOpenAnalysis parseOutput(
        const QByteArray& output, const QString& expectedRequestId,
        const QString& expectedBuildId, const QString& expectedProjectPath,
        int exitCode = 0);
    static ProjectOpenAnalysis run(const InstalledVersion& version,
                                   const QString& projectPath,
                                   const QString& requestId,
                                   int timeoutMs = 120000);
};

}
