#include "Maintenance/BuildPinRegistry.h"

#include "Platform/PlatformPaths.h"
#include "Projects/ProjectStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

namespace VNH {

namespace {

constexpr qint64 kPinFileMaxBytes = 64 * 1024;

bool isValidBuildIdToken(const QString& buildId)
{
    if (buildId.isEmpty() || buildId.size() > 128)
        return false;
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]*$"));
    return pattern.match(buildId).hasMatch();
}

QString sanitizedUserToken()
{
    const QString overrideUser =
        qEnvironmentVariable("HARKEN_HUB_PIN_USER");
    QString user = overrideUser;
    if (user.isEmpty())
        user = qEnvironmentVariable("USERNAME");
    if (user.isEmpty())
        user = qEnvironmentVariable("USER");
    if (user.isEmpty())
        user = QStringLiteral("user");

    QString token;
    for (const QChar character : user) {
        if (character.isLetterOrNumber() || character == QLatin1Char('-') ||
            character == QLatin1Char('_') || character == QLatin1Char('.'))
            token.append(character);
        else
            token.append(QLatin1Char('_'));
    }
    return token.left(64);
}

}

QString BuildPinRegistry::pinsDir()
{
    return machineDataDir() + QStringLiteral("/pins");
}

QString BuildPinRegistry::currentUserPinFile()
{
    return pinsDir() + QStringLiteral("/") + sanitizedUserToken() +
           QStringLiteral(".json");
}

bool BuildPinRegistry::writeCurrentUserPins(const QStringList& buildIds,
                                            QString* error)
{
    if (!QDir().mkpath(pinsDir())) {
        *error = QStringLiteral("Cannot create the pin directory: %1")
                     .arg(pinsDir());
        return false;
    }

    QJsonArray pinned;
    QSet<QString> seen;
    for (const QString& buildId : buildIds) {
        if (!isValidBuildIdToken(buildId) || seen.contains(buildId))
            continue;
        seen.insert(buildId);
        pinned.append(buildId);
    }

    QJsonObject root;
    root[QStringLiteral("updatedAt")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    root[QStringLiteral("pinned")] = pinned;

    QSaveFile file(currentUserPinFile());
    if (!file.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("Cannot write the pin file: %1")
                     .arg(currentUserPinFile());
        return false;
    }
    const QByteArray json =
        QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(json) != json.size() || !file.commit()) {
        *error = QStringLiteral("Cannot commit the pin file: %1")
                     .arg(currentUserPinFile());
        return false;
    }
    return true;
}

bool BuildPinRegistry::refreshFromProjects(const ProjectStore& store,
                                           QString* error)
{
    QStringList buildIds;
    for (const auto& project : store.listProjects()) {
        if (!project.preferredStudioBuildId.isEmpty())
            buildIds.append(project.preferredStudioBuildId);
    }
    return writeCurrentUserPins(buildIds, error);
}

QSet<QString> BuildPinRegistry::machinePinnedBuildIds()
{
    QSet<QString> pinned;
    const QDir dir(pinsDir());
    if (!dir.exists())
        return pinned;

    const auto files = dir.entryInfoList({QStringLiteral("*.json")},
                                         QDir::Files | QDir::NoSymLinks);
    for (const QFileInfo& info : files) {
        if (info.size() <= 0 || info.size() > kPinFileMaxBytes)
            continue;
        QFile file(info.filePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (!doc.isObject())
            continue;
        const QJsonArray entries =
            doc.object()[QStringLiteral("pinned")].toArray();
        for (const auto& value : entries) {
            const QString buildId = value.toString();
            if (isValidBuildIdToken(buildId))
                pinned.insert(buildId);
        }
    }
    return pinned;
}

}
