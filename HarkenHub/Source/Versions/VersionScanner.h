#pragma once

#include <QString>
#include <vector>

namespace VNH {

struct InstalledVersionRecord;

enum class InstallationIssue
{
    None,
    Missing,
    Unreadable,
    InvalidMetadata,
    IdentityMismatch
};

enum class VersionSource
{
    InstalledPackage,
    LocalBuild
};

struct InstalledVersion
{
    QString directoryName;
    QString installPath;
    QString executablePath;
    QString buildId;
    QString semanticVersion;
    QString channel;
    QString runtimeBuildId;
    QString executableFingerprint;
    int capabilitiesVersion = 0;
    qint64 releaseSequence = 0;
    VersionSource source = VersionSource::InstalledPackage;
    bool available = true;
    QString availabilityError;

    bool isLocalBuild() const { return source == VersionSource::LocalBuild; }
    QString launchBuildId() const
    {
        return runtimeBuildId.isEmpty() ? buildId : runtimeBuildId;
    }
    QString expectedBuildId() const
    {
        return launchBuildId();
    }
};

class VersionScanner
{
public:
    static InstallationIssue inspect(const InstalledVersionRecord& record,
                                     InstalledVersion* version = nullptr,
                                     QString* error = nullptr);
    static std::vector<InstalledVersion> scan(const QString& versionsRoot);
    static std::vector<InstalledVersion> scanInstalled();
    static std::vector<InstalledVersion> scanLocalBuilds();
    static std::vector<InstalledVersion> scanManaged();
    static std::vector<InstalledVersion> scanAvailable();
};

}
