#include "Transactions/InstallIndex.h"

#include "Platform/PlatformPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

namespace VNH {

namespace {

InstallIndexSnapshot loadIndex()
{
    InstallIndexSnapshot data;
    const QString path = InstallIndex::filePath();
    if (!QFileInfo::exists(path)) {
        data.ok = true;
        return data;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        data.error = QStringLiteral("Cannot open the install index for reading: %1 (Qt error %2: %3)")
                         .arg(path).arg(file.error()).arg(file.errorString());
        return data;
    }
    const QByteArray json = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        data.error = QStringLiteral("Cannot read the install index: %1 (Qt error %2: %3)")
                         .arg(path).arg(file.error()).arg(file.errorString());
        return data;
    }
    QJsonParseError parseError;
    const QJsonDocument doc =
        QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        data.error = QStringLiteral("Install index is corrupt: %1 (offset %2: %3)")
                         .arg(path).arg(parseError.offset)
                         .arg(parseError.error != QJsonParseError::NoError
                                  ? parseError.errorString()
                                  : QStringLiteral("Expected a JSON object"));
        return data;
    }
    const QJsonObject root = doc.object();
    data.lastReleaseSequence =
        root[QStringLiteral("lastReleaseSequence")].toInteger(0);
    const QJsonArray installed = root[QStringLiteral("installed")].toArray();
    for (const auto& value : installed) {
        const QJsonObject object = value.toObject();
        InstalledVersionRecord record;
        record.buildId = object[QStringLiteral("buildId")].toString();
        record.semanticVersion =
            object[QStringLiteral("semanticVersion")].toString();
        record.channel = object[QStringLiteral("channel")].toString();
        record.installPath = object[QStringLiteral("installPath")].toString();
        record.packageHash = object[QStringLiteral("packageHash")].toString();
        record.packageManifestHash =
            object[QStringLiteral("packageManifestHash")].toString();
        record.capabilitiesHash =
            object[QStringLiteral("capabilitiesHash")].toString();
        record.installedAt = object[QStringLiteral("installedAt")].toString();
        record.healthState = object[QStringLiteral("healthState")].toString();
        if (!record.buildId.isEmpty())
            data.records.push_back(record);
    }
    data.ok = true;
    return data;
}

bool saveIndex(const InstallIndexSnapshot& data, QString* error)
{
    QJsonArray installed;
    for (const auto& record : data.records) {
        QJsonObject object;
        object[QStringLiteral("buildId")] = record.buildId;
        object[QStringLiteral("semanticVersion")] = record.semanticVersion;
        object[QStringLiteral("channel")] = record.channel;
        object[QStringLiteral("installPath")] = record.installPath;
        object[QStringLiteral("packageHash")] = record.packageHash;
        object[QStringLiteral("packageManifestHash")] =
            record.packageManifestHash;
        object[QStringLiteral("capabilitiesHash")] = record.capabilitiesHash;
        object[QStringLiteral("installedAt")] = record.installedAt;
        object[QStringLiteral("healthState")] = record.healthState;
        installed.append(object);
    }
    QJsonObject root;
    root[QStringLiteral("lastReleaseSequence")] = data.lastReleaseSequence;
    root[QStringLiteral("installed")] = installed;

    const QString path = InstallIndex::filePath();
    if (!QDir().mkpath(QFileInfo(path).dir().path())) {
        *error = QStringLiteral("Cannot create the install index directory: %1")
                     .arg(QFileInfo(path).dir().path());
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("Cannot open the install index for writing: %1 (Qt error %2: %3)")
                     .arg(path).arg(file.error()).arg(file.errorString());
        return false;
    }
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(json) != json.size()) {
        *error = QStringLiteral("Cannot write the install index: %1 (Qt error %2: %3)")
                     .arg(path).arg(file.error()).arg(file.errorString());
        return false;
    }
    if (!file.commit()) {
        *error = QStringLiteral("Cannot commit the install index: %1 (Qt error %2: %3)")
                     .arg(path).arg(file.error()).arg(file.errorString());
        return false;
    }
    return true;
}

}

QString InstallIndex::filePath()
{
    return machineDataDir() + QStringLiteral("/install-index.json");
}

InstallIndexSnapshot InstallIndex::snapshot()
{
    return loadIndex();
}

bool InstallIndex::commitInstallation(const InstalledVersionRecord& record,
                                      qint64 releaseSequence, QString* error)
{
    if (record.buildId.isEmpty()) {
        *error = QStringLiteral("Install record has no buildId");
        return false;
    }
    InstallIndexSnapshot data = loadIndex();
    if (!data.ok) {
        *error = data.error;
        return false;
    }
    data.lastReleaseSequence = std::max(data.lastReleaseSequence, releaseSequence);
    const auto existing =
        std::find_if(data.records.begin(), data.records.end(),
                     [&record](const InstalledVersionRecord& candidate) {
                         return candidate.buildId == record.buildId;
                     });
    if (existing != data.records.end())
        *existing = record;
    else
        data.records.push_back(record);
    return saveIndex(data, error);
}

bool InstallIndex::setHealthState(const QString& buildId,
                                  const QString& healthState, QString* error)
{
    InstallIndexSnapshot data = loadIndex();
    if (!data.ok) {
        *error = data.error;
        return false;
    }
    const auto existing =
        std::find_if(data.records.begin(), data.records.end(),
                     [&buildId](const InstalledVersionRecord& candidate) {
                         return candidate.buildId == buildId;
                     });
    if (existing == data.records.end()) {
        *error = QStringLiteral("Unknown buildId: %1").arg(buildId);
        return false;
    }
    existing->healthState = healthState;
    return saveIndex(data, error);
}

bool InstallIndex::removeRecord(const QString& buildId, QString* error)
{
    InstallIndexSnapshot data = loadIndex();
    if (!data.ok) {
        *error = data.error;
        return false;
    }
    const auto existing =
        std::find_if(data.records.begin(), data.records.end(),
                     [&buildId](const InstalledVersionRecord& candidate) {
                         return candidate.buildId == buildId;
                     });
    if (existing == data.records.end()) {
        *error = QStringLiteral("Unknown buildId: %1").arg(buildId);
        return false;
    }
    data.records.erase(existing);
    return saveIndex(data, error);
}

}
