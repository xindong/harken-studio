#pragma once

#include <QString>

#include <vector>

namespace VNH {

struct InstalledVersionRecord
{
    QString buildId;
    QString semanticVersion;
    QString channel;
    QString installPath;
    QString packageHash;
    QString packageManifestHash;
    QString capabilitiesHash;
    QString installedAt;
    QString healthState;
};

struct InstallIndexSnapshot
{
    bool ok = false;
    QString error;
    qint64 lastReleaseSequence = 0;
    std::vector<InstalledVersionRecord> records;
};

class InstallIndex
{
public:
    static QString filePath();
    static InstallIndexSnapshot snapshot();
    static bool commitInstallation(const InstalledVersionRecord& record,
                                   qint64 releaseSequence, QString* error);
    static bool setHealthState(const QString& buildId, const QString& healthState,
                               QString* error);
    static bool removeRecord(const QString& buildId, QString* error);
};

}
