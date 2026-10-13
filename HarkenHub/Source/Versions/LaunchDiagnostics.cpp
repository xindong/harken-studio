#include "Versions/LaunchDiagnostics.h"

#include "Platform/PlatformPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace VNH {

namespace {

QJsonObject loadRoot()
{
    QFile file(LaunchDiagnostics::filePath());
    if (!file.open(QIODevice::ReadOnly))
        return QJsonObject();
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() ? doc.object() : QJsonObject();
}

}

QString LaunchDiagnostics::filePath()
{
    return userDataDir() + QStringLiteral("/launch-diagnostics.json");
}

bool LaunchDiagnostics::record(const QString& buildId,
                               const QString& userLaunchState,
                               const QString& lastLaunchError,
                               const QDateTime& at)
{
    if (buildId.isEmpty())
        return false;

    QJsonObject root = loadRoot();
    QJsonObject entry = root[buildId].toObject();
    entry[QStringLiteral("userLaunchState")] = userLaunchState;
    if (userLaunchState == QStringLiteral("Ready") ||
        userLaunchState == QStringLiteral("LaunchIssue"))
        entry[QStringLiteral("lastSelfTestAt")] =
            at.toUTC().toString(Qt::ISODateWithMs);
    if (lastLaunchError.isEmpty())
        entry.remove(QStringLiteral("lastLaunchError"));
    else
        entry[QStringLiteral("lastLaunchError")] = lastLaunchError;
    root[buildId] = entry;

    const QString path = filePath();
    if (!QDir().mkpath(QFileInfo(path).dir().path()))
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    return file.write(json) == json.size() && file.commit();
}

QHash<QString, LaunchDiagnosticRecord> LaunchDiagnostics::snapshot()
{
    QHash<QString, LaunchDiagnosticRecord> records;
    const QJsonObject root = loadRoot();
    for (auto it = root.begin(); it != root.end(); ++it) {
        const QJsonObject entry = it.value().toObject();
        LaunchDiagnosticRecord record;
        record.buildId = it.key();
        record.userLaunchState =
            entry[QStringLiteral("userLaunchState")].toString();
        record.lastSelfTestAt =
            entry[QStringLiteral("lastSelfTestAt")].toString();
        record.lastLaunchError =
            entry[QStringLiteral("lastLaunchError")].toString();
        records.insert(it.key(), record);
    }
    return records;
}

}
