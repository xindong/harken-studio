
#include "NamedLayoutStore.h"

#include "BuiltinLayouts.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>

namespace VNX {
namespace NamedLayoutStore {

namespace {

QString safeFileName(const QString& name)
{
    QString safe = name;
    safe.replace(QRegularExpression(QStringLiteral("[^a-zA-Z0-9_\\- ]")),
                 QStringLiteral("_"));
    return safe + QStringLiteral(".json");
}

QString findFileForName(const QString& name, QJsonObject* jsonOut = nullptr)
{
    QDir dir(dirPath());
    for (const QFileInfo& entry : dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files)) {
        QFile file(entry.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        if (json.value(QStringLiteral("name")).toString() != name)
            continue;
        if (jsonOut)
            *jsonOut = json;
        return entry.absoluteFilePath();
    }
    return QString();
}

bool occupiedByOther(const QString& filePath, const QString& name)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    return json.value(QStringLiteral("name")).toString() != name;
}

bool writeFile(const QString& filePath, const QJsonObject& json)
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(json).toJson(QJsonDocument::Compact));
    file.close();
    return true;
}

}

QString& dirPathOverride()
{
    static QString path;
    return path;
}

QString dirPath()
{
    if (!dirPathOverride().isEmpty()) {
        QDir().mkpath(dirPathOverride());
        return dirPathOverride();
    }
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                        + QStringLiteral("/layouts");
    QDir().mkpath(dir);
    return dir;
}

void setDirPathOverrideForTesting(const QString& path)
{
    dirPathOverride() = path;
}

QList<NamedLayoutInfo> list()
{
    QList<NamedLayoutInfo> layouts;
    QDir dir(dirPath());
    for (const QFileInfo& entry : dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files)) {
        QFile file(entry.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        NamedLayoutInfo info;
        info.name = json.value(QStringLiteral("name")).toString();
        if (info.name.isEmpty())
            info.name = entry.baseName();
        info.savedAt = json.value(QStringLiteral("savedAt")).toString();
        info.filePath = entry.absoluteFilePath();
        layouts.append(info);
    }
    std::sort(layouts.begin(), layouts.end(),
              [](const NamedLayoutInfo& a, const NamedLayoutInfo& b) {
                  return QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
              });
    return layouts;
}

bool isReservedName(const QString& name)
{
    return BuiltinLayouts::isReservedToken(name);
}

bool read(const QString& name, QJsonObject* jsonOut)
{
    return !findFileForName(name, jsonOut).isEmpty();
}

bool write(const QString& name, const QJsonObject& json, QString* pathOut)
{
    if (name.isEmpty() || isReservedName(name))
        return false;
    const QString filePath = dirPath() + QLatin1Char('/') + safeFileName(name);
    if (occupiedByOther(filePath, name))
        return false;
    if (!writeFile(filePath, json))
        return false;
    if (pathOut)
        *pathOut = filePath;
    return true;
}

bool remove(const QString& name)
{
    const QString path = findFileForName(name);
    return !path.isEmpty() && QFile::remove(path);
}

bool rename(const QString& oldName, const QString& newName)
{
    if (newName.isEmpty() || isReservedName(newName))
        return false;

    QJsonObject json;
    const QString oldPath = findFileForName(oldName, &json);
    if (oldPath.isEmpty())
        return false;
    if (newName != oldName && !findFileForName(newName).isEmpty())
        return false;

    json[QStringLiteral("name")] = newName;
    const QString newPath = dirPath() + QLatin1Char('/') + safeFileName(newName);
    if (occupiedByOther(newPath, oldName))
        return false;
    if (!writeFile(newPath, json))
        return false;

    if (QFileInfo(newPath) != QFileInfo(oldPath))
        QFile::remove(oldPath);
    return true;
}

}
}
