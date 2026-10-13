
#include "JsonSettingsStorage.h"
#include <QFile>
#include <QJsonDocument>
#include <QDir>
#include <QStandardPaths>
#include <QJsonArray>

namespace VNX {

JsonSettingsStorage::JsonSettingsStorage(const QString& organization, const QString& application)
{
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir dir(configDir);
    if (!dir.exists())
        dir.mkpath(QStringLiteral("."));
    filePath_ = dir.filePath(organization + QLatin1Char('_') + application + QStringLiteral(".json"));
    load();
}

JsonSettingsStorage::JsonSettingsStorage(const QString& filePath)
    : filePath_(filePath)
{
    load();
}

void JsonSettingsStorage::load()
{
    QFile file(filePath_);
    if (!file.exists())
        return;
    if (!file.open(QIODevice::ReadOnly))
        return;

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error == QJsonParseError::NoError && doc.isObject())
        root_ = doc.object();
}

QString JsonSettingsStorage::resolvedKey(const QString& key) const
{
    if (groupStack_.isEmpty())
        return key;
    return groupStack_.join(QLatin1Char('/')) + QLatin1Char('/') + key;
}

void JsonSettingsStorage::beginGroup(const QString& group)
{
    groupStack_.push_back(group);
}

void JsonSettingsStorage::endGroup()
{
    if (!groupStack_.isEmpty())
        groupStack_.removeLast();
}

QVariant JsonSettingsStorage::value(const QString& key, const QVariant& defaultValue) const
{
    const QString fullKey = resolvedKey(key);
    if (!root_.contains(fullKey))
        return defaultValue;

    const QJsonValue v = root_[fullKey];
    if (v.isNull() || v.isUndefined())
        return defaultValue;
    return v.toVariant();
}

void JsonSettingsStorage::setValue(const QString& key, const QVariant& val)
{
    const QString fullKey = resolvedKey(key);
    root_[fullKey] = QJsonValue::fromVariant(val);
    dirty_ = true;
}

void JsonSettingsStorage::sync()
{
    if (!dirty_)
        return;

    QFileInfo fi(filePath_);
    QDir dir = fi.absoluteDir();
    if (!dir.exists())
        dir.mkpath(QStringLiteral("."));

    QFile file(filePath_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;

    QJsonDocument doc(root_);
    file.write(doc.toJson(QJsonDocument::Indented));
    dirty_ = false;
}

bool JsonSettingsStorage::isWritable() const
{
    if (filePath_.isEmpty())
        return false;
    QFileInfo fi(filePath_);
    if (fi.exists())
        return fi.isWritable();
    return QFileInfo(fi.absolutePath()).isWritable();
}

QStringList JsonSettingsStorage::allKeys() const
{
    const QString prefix = groupStack_.isEmpty()
        ? QString()
        : groupStack_.join(QLatin1Char('/')) + QLatin1Char('/');

    QStringList result;
    for (auto it = root_.constBegin(); it != root_.constEnd(); ++it) {
        if (prefix.isEmpty() || it.key().startsWith(prefix))
            result.append(prefix.isEmpty() ? it.key() : it.key().mid(prefix.size()));
    }
    return result;
}

QStringList JsonSettingsStorage::childGroups() const
{
    const QString prefix = groupStack_.isEmpty()
        ? QString()
        : groupStack_.join(QLatin1Char('/')) + QLatin1Char('/');

    QSet<QString> groups;
    for (auto it = root_.constBegin(); it != root_.constEnd(); ++it) {
        const QString& k = it.key();
        if (!prefix.isEmpty() && !k.startsWith(prefix))
            continue;
        const QString remainder = prefix.isEmpty() ? k : k.mid(prefix.size());
        const int slash = remainder.indexOf(QLatin1Char('/'));
        if (slash > 0)
            groups.insert(remainder.left(slash));
    }
    return groups.values();
}

QStringList JsonSettingsStorage::childKeys() const
{
    const QString prefix = groupStack_.isEmpty()
        ? QString()
        : groupStack_.join(QLatin1Char('/')) + QLatin1Char('/');

    QStringList result;
    for (auto it = root_.constBegin(); it != root_.constEnd(); ++it) {
        const QString& k = it.key();
        if (!prefix.isEmpty() && !k.startsWith(prefix))
            continue;
        const QString remainder = prefix.isEmpty() ? k : k.mid(prefix.size());
        if (!remainder.contains(QLatin1Char('/')))
            result.append(remainder);
    }
    return result;
}

bool JsonSettingsStorage::contains(const QString& key) const
{
    return root_.contains(resolvedKey(key));
}

void JsonSettingsStorage::remove(const QString& key)
{
    const QString resolved = resolvedKey(key);
    const QString groupPrefix = resolved + QLatin1Char('/');
    QStringList toRemove;
    for (auto it = root_.constBegin(); it != root_.constEnd(); ++it) {
        if (it.key() == resolved || it.key().startsWith(groupPrefix))
            toRemove.append(it.key());
    }
    for (const QString& k : toRemove)
        root_.remove(k);
    if (!toRemove.isEmpty())
        dirty_ = true;
}

void JsonSettingsStorage::clear()
{
    root_ = QJsonObject();
    dirty_ = true;
}

}
