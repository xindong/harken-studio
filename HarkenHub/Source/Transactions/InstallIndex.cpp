#include "Transactions/InstallIndex.h"

#include "Platform/AtomicFile.h"
#include "Platform/PlatformPaths.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>

namespace VNH {

namespace {

InstallIndexSnapshot parseIndex(const QJsonObject& root)
{
    const auto invalid = [](const QString& field, const QString& reason) {
        InstallIndexSnapshot result;
        result.error = QStringLiteral("Install index is corrupt: %1 (field %2: %3)")
                           .arg(InstallIndex::filePath(), field, reason);
        return result;
    };
    InstallIndexSnapshot data;
    const auto sequence = root.value(QStringLiteral("lastReleaseSequence"));
    if (!sequence.isDouble() || sequence.toInteger(-1) < 0)
        return invalid(QStringLiteral("lastReleaseSequence"), QStringLiteral("Expected a non-negative integer"));
    data.lastReleaseSequence = sequence.toInteger();
    const auto installed = root.value(QStringLiteral("installed"));
    if (!installed.isArray())
        return invalid(QStringLiteral("installed"), QStringLiteral("Expected an array"));
    QSet<QString> buildIds;
    const auto records = installed.toArray();
    for (qsizetype i = 0; i < records.size(); ++i) {
        const QString prefix = QStringLiteral("installed[%1]").arg(i);
        if (!records[i].isObject())
            return invalid(prefix, QStringLiteral("Expected an object"));
        const auto object = records[i].toObject();
        InstalledVersionRecord record;
        static const struct {
            const char* name;
            QString InstalledVersionRecord::*member;
            bool optional;
        } fields[] = {
            {"buildId", &InstalledVersionRecord::buildId, false},
            {"semanticVersion", &InstalledVersionRecord::semanticVersion, false},
            {"channel", &InstalledVersionRecord::channel, false},
            {"installPath", &InstalledVersionRecord::installPath, false},
            {"packageHash", &InstalledVersionRecord::packageHash, false},
            {"packageManifestHash", &InstalledVersionRecord::packageManifestHash, true},
            {"capabilitiesHash", &InstalledVersionRecord::capabilitiesHash, true},
            {"installedAt", &InstalledVersionRecord::installedAt, false},
            {"healthState", &InstalledVersionRecord::healthState, false},
        };
        for (const auto& field : fields) {
            const auto value = object.value(QLatin1String(field.name));
            if (field.optional && value.isUndefined())
                continue;
            if (!value.isString())
                return invalid(prefix + QLatin1Char('.') + QLatin1String(field.name),
                               QStringLiteral("Expected a string"));
            record.*field.member = value.toString();
        }
        for (const char* field : {"buildId", "semanticVersion", "installPath"}) {
            if (object.value(QLatin1String(field)).toString().trimmed().isEmpty())
                return invalid(prefix + QLatin1Char('.') + QLatin1String(field),
                               QStringLiteral("Expected a non-empty string"));
        }
        if (buildIds.contains(record.buildId))
            return invalid(prefix + QStringLiteral(".buildId"), QStringLiteral("Duplicate build identity"));
        if (record.healthState != QStringLiteral("Installing")
            && record.healthState != QStringLiteral("Installed")
            && record.healthState != QStringLiteral("NeedsRepair"))
            return invalid(prefix + QStringLiteral(".healthState"), QStringLiteral("Unknown installation state"));
        buildIds.insert(record.buildId);
        data.records.push_back(std::move(record));
    }
    data.ok = true;
    return data;
}

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
    return parseIndex(doc.object());
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

    const auto validated = parseIndex(root);
    if (!validated.ok) {
        *error = validated.error;
        return false;
    }
    return writeAtomicFile(InstallIndex::filePath(),
                           QJsonDocument(root).toJson(QJsonDocument::Indented), error);
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
    if (releaseSequence < 0) {
        *error = QStringLiteral("Install releaseSequence must be non-negative");
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
