#include "Versions/LocalBuildRegistry.h"

#include "Packaging/PackageReader.h"
#include "Platform/AtomicFile.h"
#include "Platform/PlatformPaths.h"
#include "Versions/SelfTestRunner.h"
#include "Versions/StudioCapabilities.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>

namespace VNH {

namespace {

QMutex registryMutex;

QString normalizedPath(const QString& path)
{
    const QFileInfo info(path);
    QString normalized = info.canonicalFilePath();
    if (normalized.isEmpty())
        normalized = info.absoluteFilePath();
    normalized = QDir::cleanPath(QDir::fromNativeSeparators(normalized));
#ifdef Q_OS_WIN
    normalized = normalized.toLower();
#endif
    return normalized;
}

QJsonObject toJson(const LocalBuildRecord& record)
{
    QJsonObject object;
    object[QStringLiteral("executablePath")] = record.executablePath;
    object[QStringLiteral("reportedBuildId")] = record.reportedBuildId;
    object[QStringLiteral("version")] = record.semanticVersion;
    object[QStringLiteral("channel")] = record.channel;
    object[QStringLiteral("buildType")] = record.buildType;
    object[QStringLiteral("lastProbeError")] = record.lastProbeError;
    object[QStringLiteral("executableFingerprint")] = record.executableFingerprint;
    object[QStringLiteral("executableSize")] = record.executableSize;
    object[QStringLiteral("executableModifiedMs")] =
        record.executableModifiedMs;
    object[QStringLiteral("capabilitiesVersion")] = record.capabilitiesVersion;
    object[QStringLiteral("releaseSequence")] = record.releaseSequence;
    return object;
}

LocalBuildRecord fromJson(const QJsonObject& object)
{
    LocalBuildRecord record;
    record.executablePath =
        object[QStringLiteral("executablePath")].toString();
    record.reportedBuildId =
        object[QStringLiteral("reportedBuildId")].toString();
    record.semanticVersion = object[QStringLiteral("version")].toString();
    record.channel = object[QStringLiteral("channel")].toString();
    record.buildType = object[QStringLiteral("buildType")].toString();
    record.lastProbeError = object[QStringLiteral("lastProbeError")].toString();
    record.executableFingerprint = object[QStringLiteral("executableFingerprint")].toString();
    record.executableSize =
        object[QStringLiteral("executableSize")].toInteger(-1);
    record.executableModifiedMs =
        object[QStringLiteral("executableModifiedMs")].toInteger(0);
    record.capabilitiesVersion =
        object[QStringLiteral("capabilitiesVersion")].toInt(0);
    record.releaseSequence = object[QStringLiteral("releaseSequence")].toInteger(0);
    return record;
}

}

QString LocalBuildRegistry::filePath()
{
    return QDir(userDataDir()).filePath(QStringLiteral("local-builds.json"));
}

QString LocalBuildRegistry::stableIdForPath(const QString& executablePath)
{
    const QByteArray digest = QCryptographicHash::hash(
        normalizedPath(executablePath).toUtf8(), QCryptographicHash::Sha256);
    return QStringLiteral("local-user-%1")
        .arg(QString::fromLatin1(digest.toHex().left(16)));
}

QString LocalBuildRegistry::fileFingerprint(const QString& executablePath)
{
    return PackageReader::fileSha256Hex(executablePath);
}

bool LocalBuildRegistry::metadataMatchesFile(const LocalBuildRecord& record)
{
    const QFileInfo executable(record.executablePath);
    return (record.capabilitiesVersion == 1 || record.capabilitiesVersion == kStudioCapabilitiesVersion)
        && !record.executableFingerprint.isEmpty()
        && record.lastProbeError.isEmpty()
        && !record.reportedBuildId.isEmpty() && !record.semanticVersion.isEmpty()
        && executable.exists() && executable.isFile() &&
           record.executableSize == executable.size() &&
           record.executableModifiedMs ==
               executable.lastModified().toMSecsSinceEpoch();
}

bool LocalBuildRegistry::contains(const QString& executablePath,
                                  QString* error)
{
    const std::vector<LocalBuildRecord> records = load(error);
    const QString identity = normalizedPath(executablePath);
    return std::any_of(
        records.begin(), records.end(), [&identity](const LocalBuildRecord& item) {
            return normalizedPath(item.executablePath) == identity;
        });
}

static std::vector<LocalBuildRecord> loadRecords(QString* error)
{
    if (error)
        error->clear();
    QFile file(LocalBuildRegistry::filePath());
    if (!file.exists())
        return {};
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("Local build registry is invalid: %1")
                         .arg(parseError.errorString());
        return {};
    }

    std::vector<LocalBuildRecord> records;
    const QJsonArray entries =
        document.object()[QStringLiteral("localBuilds")].toArray();
    records.reserve(static_cast<size_t>(entries.size()));
    for (const QJsonValue& value : entries) {
        if (!value.isObject())
            continue;
        LocalBuildRecord record = fromJson(value.toObject());
        if (!record.executablePath.isEmpty())
            records.push_back(std::move(record));
    }
    return records;
}

std::vector<LocalBuildRecord> LocalBuildRegistry::load(QString* error)
{
    const QMutexLocker lock(&registryMutex);
    return loadRecords(error);
}

static bool saveRecords(const std::vector<LocalBuildRecord>& records,
                        QString* error)
{
    QJsonArray entries;
    for (const auto& record : records)
        entries.append(toJson(record));
    QJsonObject root;
    root[QStringLiteral("schemaVersion")] = 1;
    root[QStringLiteral("localBuilds")] = entries;

    return writeAtomicFile(LocalBuildRegistry::filePath(),
                           QJsonDocument(root).toJson(QJsonDocument::Indented), error);
}

bool LocalBuildRegistry::probe(const QString& executablePath,
                               LocalBuildRecord* record, QString* error)
{
    const QFileInfo executable(executablePath);
    if (!executable.exists() || !executable.isFile()) {
        if (error)
            *error = QStringLiteral("Select an existing HarkenStudio executable");
        return false;
    }

    const QString fingerprint = fileFingerprint(executablePath);
    const SelfTestResult result = SelfTestRunner::run(
        executable.absoluteFilePath(),
        {QStringLiteral("--self-test"), QStringLiteral("--json")},
        QString(), 10000);
    if (!result.ok) {
        if (error)
            *error = result.error;
        return false;
    }
    if (fingerprint.isEmpty() || fileFingerprint(executablePath) != fingerprint) {
        if (error)
            *error = QStringLiteral("The executable changed during its self-test. Try again.");
        return false;
    }
    if (record) {
        const QFileInfo refreshed(executable.absoluteFilePath());
        record->executablePath = refreshed.absoluteFilePath();
        record->executableFingerprint = fingerprint;
        record->reportedBuildId = result.reportedBuildId;
        record->semanticVersion = result.reportedVersion;
        record->channel = result.channel;
        record->buildType = result.buildType;
        record->executableSize = refreshed.size();
        record->executableModifiedMs =
            refreshed.lastModified().toMSecsSinceEpoch();
        record->capabilitiesVersion = result.capabilitiesVersion;
        record->lastProbeError.clear();
    }
    return true;
}

bool LocalBuildRegistry::upsert(const LocalBuildRecord& record, QString* error)
{
    const QMutexLocker lock(&registryMutex);
    QString loadError;
    std::vector<LocalBuildRecord> records = loadRecords(&loadError);
    if (!loadError.isEmpty()) {
        if (error)
            *error = loadError;
        return false;
    }
    const QString identity = normalizedPath(record.executablePath);
    const auto match = std::find_if(
        records.begin(), records.end(), [&identity](const LocalBuildRecord& item) {
            return normalizedPath(item.executablePath) == identity;
        });
    if (match == records.end())
        records.push_back(record);
    else
        *match = record;
    return saveRecords(records, error);
}

bool LocalBuildRegistry::updateExisting(const LocalBuildRecord& record,
                                        QString* error)
{
    const QMutexLocker lock(&registryMutex);
    QString loadError;
    std::vector<LocalBuildRecord> records = loadRecords(&loadError);
    if (!loadError.isEmpty()) {
        if (error)
            *error = loadError;
        return false;
    }
    const QString identity = normalizedPath(record.executablePath);
    const auto match = std::find_if(
        records.begin(), records.end(), [&identity](const LocalBuildRecord& item) {
            return normalizedPath(item.executablePath) == identity;
        });
    if (match == records.end()) {
        if (error)
            *error = QStringLiteral("The local build is no longer registered");
        return false;
    }
    *match = record;
    return saveRecords(records, error);
}

bool LocalBuildRegistry::requestRecheck(const QString& executablePath,
                                       const QString& expectedFingerprint,
                                       QString* error)
{
    const QMutexLocker lock(&registryMutex);
    QString loadError;
    auto records = loadRecords(&loadError);
    if (!loadError.isEmpty()) {
        if (error)
            *error = loadError;
        return false;
    }
    const auto identity = normalizedPath(executablePath);
    const auto match = std::find_if(records.begin(), records.end(), [&identity](const auto& record) {
        return normalizedPath(record.executablePath) == identity;
    });
    if (match == records.end()) {
        if (error)
            *error = QStringLiteral("The local build is no longer registered");
        return false;
    }
    if (match->executableFingerprint != expectedFingerprint || !match->lastProbeError.isEmpty())
        return true;
    match->lastProbeError = QStringLiteral("This local build changed and is being checked again");
    return saveRecords(records, error);
}

bool LocalBuildRegistry::remove(const QString& executablePath, QString* error)
{
    const QMutexLocker lock(&registryMutex);
    QString loadError;
    std::vector<LocalBuildRecord> records = loadRecords(&loadError);
    if (!loadError.isEmpty()) {
        if (error)
            *error = loadError;
        return false;
    }
    const QString identity = normalizedPath(executablePath);
    const auto end = std::remove_if(
        records.begin(), records.end(), [&identity](const LocalBuildRecord& item) {
            return normalizedPath(item.executablePath) == identity;
        });
    if (end == records.end())
        return true;
    records.erase(end, records.end());
    return saveRecords(records, error);
}

}
