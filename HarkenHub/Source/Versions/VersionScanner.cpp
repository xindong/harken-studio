#include "Versions/VersionScanner.h"

#include "Platform/PlatformPaths.h"
#include "Transactions/InstallIndex.h"
#include "Versions/LocalBuildRegistry.h"
#include "Versions/StudioCapabilities.h"

#include <QDir>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace VNH {

namespace {

InstallationIssue fail(InstallationIssue issue, const QString& message, QString* error)
{
    if (error)
        *error = message;
    return issue;
}

QString text(const char* source)
{
    return QCoreApplication::translate("HarkenHub", source);
}

InstallationIssue readJsonObject(const QString& path, QJsonObject& out, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (!QFileInfo::exists(path) && QFileInfo(QFileInfo(path).absolutePath()).isReadable())
            return fail(InstallationIssue::Missing,
                        text("Installation metadata is missing: %1").arg(path), error);
        return fail(InstallationIssue::Unreadable,
                    text("Cannot read %1: %2").arg(path, file.errorString()), error);
    }
    const QByteArray data = file.readAll();
    if (file.error() != QFileDevice::NoError)
        return fail(InstallationIssue::Unreadable,
                    text("Cannot read %1: %2").arg(path, file.errorString()), error);

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return fail(InstallationIssue::InvalidMetadata,
                    text("Invalid installation metadata: %1").arg(path), error);
    out = doc.object();
    return InstallationIssue::None;
}

InstallationIssue readVersion(const QString& path, InstalledVersion* version, QString* error)
{
    if (!QFileInfo(path).isDir())
        return fail(InstallationIssue::Missing,
                    text("Installation directory is missing: %1").arg(path), error);
    QJsonObject manifest;
    auto issue = readJsonObject(QDir(path).filePath(QStringLiteral("package-manifest.json")),
                                manifest, error);
    if (issue != InstallationIssue::None)
        return issue;
    const QString buildId = manifest.value(QStringLiteral("buildId")).toString();
    const QString semanticVersion = manifest.value(QStringLiteral("version")).toString();
    if (manifest.value(QStringLiteral("product")).toString() != QStringLiteral("HarkenStudio")
        || buildId.isEmpty() || semanticVersion.isEmpty())
        return fail(InstallationIssue::InvalidMetadata,
                    text("The package manifest has an invalid Studio identity."), error);
    version->buildId = buildId;
    version->semanticVersion = semanticVersion;
    QJsonObject capabilities;
    issue = readJsonObject(QDir(path).filePath(QStringLiteral("capabilities.json")),
                           capabilities, error);
    if (issue != InstallationIssue::None)
        return issue;
    const QString capabilitiesBuildId = capabilities.value(QStringLiteral("buildId")).toString();
    const QString capabilitiesStudioVersion = capabilities.value(QStringLiteral("studioVersion")).toString();
    if ((!capabilitiesBuildId.isEmpty() && capabilitiesBuildId != buildId)
        || (!capabilitiesStudioVersion.isEmpty() && capabilitiesStudioVersion != semanticVersion))
        return fail(InstallationIssue::IdentityMismatch,
                    text("The installed version identity does not match its installation record."), error);
    int protocol = 0;
    if (!validateStudioCapabilities(capabilities, semanticVersion, buildId, error, &protocol))
        return InstallationIssue::InvalidMetadata;
    const QString executable = QDir(path).filePath(studioBinaryRelativePath());
    if (!QFileInfo(executable).isFile())
        return fail(InstallationIssue::Missing,
                    text("Studio executable is missing: %1").arg(executable), error);
    version->directoryName = QFileInfo(path).fileName();
    version->installPath = path;
    version->executablePath = executable;
    version->channel = manifest.value(QStringLiteral("channel")).toString();
    version->releaseSequence = manifest.value(QStringLiteral("releaseSequence")).toInteger();
    version->capabilitiesVersion = protocol;
    return InstallationIssue::None;
}

}

std::vector<InstalledVersion> VersionScanner::scan(const QString& versionsRoot)
{
    std::vector<InstalledVersion> versions;

    const QDir root(versionsRoot);
    if (!root.exists())
        return versions;

    const QStringList entries =
        root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& entry : entries) {
        if (entry.startsWith(QLatin1Char('.')))
            continue;
        InstalledVersion version;
        if (readVersion(root.filePath(entry), &version, nullptr) == InstallationIssue::None)
            versions.push_back(std::move(version));
    }

    return versions;
}

InstallationIssue VersionScanner::inspect(const InstalledVersionRecord& record,
                                          InstalledVersion* version, QString* error)
{
    const QString root = QDir::cleanPath(QFileInfo(studioVersionsRoot()).absoluteFilePath());
    const QFileInfo path(record.installPath);
    if (record.installPath.isEmpty() || QDir::cleanPath(path.absolutePath()) != root
        || path.fileName().startsWith(QLatin1Char('.')))
        return fail(InstallationIssue::IdentityMismatch,
                    text("The registered installation path is outside the versions directory."), error);
    InstalledVersion found;
    const auto issue = readVersion(record.installPath, &found, error);
    if (!found.buildId.isEmpty()
        && (found.buildId != record.buildId || found.semanticVersion != record.semanticVersion))
        return fail(InstallationIssue::IdentityMismatch,
                    text("The installed version identity does not match its installation record."), error);
    if (issue != InstallationIssue::None)
        return issue;
    if (version)
        *version = std::move(found);
    return InstallationIssue::None;
}

std::vector<InstalledVersion> VersionScanner::scanInstalled()
{
    const InstallIndexSnapshot index = InstallIndex::snapshot();
    if (!index.ok)
        return {};

    std::vector<InstalledVersion> versions;
    for (const auto& record : index.records) {
        if (record.healthState != QStringLiteral("Installed"))
            continue;
        InstalledVersion version;
        if (inspect(record, &version) == InstallationIssue::None)
            versions.push_back(std::move(version));
    }
    return versions;
}

std::vector<InstalledVersion> VersionScanner::scanLocalBuilds()
{
    std::vector<LocalBuildRecord> records = LocalBuildRegistry::load();
    std::vector<InstalledVersion> versions;
    versions.reserve(records.size());
    for (const auto& record : records) {
        const QFileInfo executable(record.executablePath);
        InstalledVersion version;
        version.directoryName =
            LocalBuildRegistry::stableIdForPath(record.executablePath);
        version.installPath = executable.absolutePath();
        version.executablePath = executable.absoluteFilePath();
        version.buildId = version.directoryName;
        version.runtimeBuildId = record.reportedBuildId;
        version.executableFingerprint = record.executableFingerprint;
        version.semanticVersion = record.semanticVersion;
        version.channel = record.channel;
        version.capabilitiesVersion = record.capabilitiesVersion;
        version.releaseSequence = record.releaseSequence;
        version.source = VersionSource::LocalBuild;
        const bool exists = executable.exists() && executable.isFile();
        version.available =
            exists && LocalBuildRegistry::metadataMatchesFile(record);
        if (!exists)
            version.availabilityError =
                QStringLiteral("The registered executable no longer exists: %1")
                    .arg(executable.absoluteFilePath());
        else if (!version.available)
            version.availabilityError = record.lastProbeError.isEmpty()
                ? QStringLiteral("This local build changed and is being checked again")
                : record.lastProbeError;
        versions.push_back(std::move(version));
    }
    return versions;
}

std::vector<InstalledVersion> VersionScanner::scanManaged()
{
    std::vector<InstalledVersion> versions = scanInstalled();
    const std::vector<InstalledVersion> local = scanLocalBuilds();
    for (const auto& candidate : local) {
        const QString identity = LocalBuildRegistry::stableIdForPath(candidate.executablePath);
        const bool duplicate = std::any_of(
            versions.begin(), versions.end(), [&identity](const auto& installed) {
                return identity == LocalBuildRegistry::stableIdForPath(installed.executablePath);
            });
        if (!duplicate)
            versions.push_back(candidate);
    }
    return versions;
}

std::vector<InstalledVersion> VersionScanner::scanAvailable()
{
    std::vector<InstalledVersion> versions = scanManaged();
    versions.erase(
        std::remove_if(versions.begin(), versions.end(),
                       [](const InstalledVersion& version) {
                           return !version.available;
                       }),
        versions.end());
    return versions;
}

}
