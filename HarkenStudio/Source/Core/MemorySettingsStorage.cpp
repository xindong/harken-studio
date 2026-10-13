
#include "MemorySettingsStorage.h"
#include <QSet>

namespace VNX {

QString MemorySettingsStorage::resolvedKey(const QString& key) const
{
    if (groupStack_.isEmpty())
        return key;
    return groupStack_.join(QLatin1Char('/')) + QLatin1Char('/') + key;
}

void MemorySettingsStorage::beginGroup(const QString& group)
{
    groupStack_.push_back(group);
}

void MemorySettingsStorage::endGroup()
{
    if (!groupStack_.isEmpty())
        groupStack_.removeLast();
}

QVariant MemorySettingsStorage::value(const QString& key, const QVariant& defaultValue) const
{
    return data_.value(resolvedKey(key), defaultValue);
}

void MemorySettingsStorage::setValue(const QString& key, const QVariant& value)
{
    data_[resolvedKey(key)] = value;
}

QStringList MemorySettingsStorage::allKeys() const
{
    const QString prefix = groupStack_.isEmpty()
        ? QString()
        : groupStack_.join(QLatin1Char('/')) + QLatin1Char('/');

    QStringList result;
    for (auto it = data_.constBegin(); it != data_.constEnd(); ++it) {
        if (prefix.isEmpty() || it.key().startsWith(prefix))
            result.append(prefix.isEmpty() ? it.key() : it.key().mid(prefix.size()));
    }
    return result;
}

QStringList MemorySettingsStorage::childGroups() const
{
    const QString prefix = groupStack_.isEmpty()
        ? QString()
        : groupStack_.join(QLatin1Char('/')) + QLatin1Char('/');

    QSet<QString> groups;
    for (auto it = data_.constBegin(); it != data_.constEnd(); ++it) {
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

QStringList MemorySettingsStorage::childKeys() const
{
    const QString prefix = groupStack_.isEmpty()
        ? QString()
        : groupStack_.join(QLatin1Char('/')) + QLatin1Char('/');

    QStringList result;
    for (auto it = data_.constBegin(); it != data_.constEnd(); ++it) {
        const QString& k = it.key();
        if (!prefix.isEmpty() && !k.startsWith(prefix))
            continue;
        const QString remainder = prefix.isEmpty() ? k : k.mid(prefix.size());
        if (!remainder.contains(QLatin1Char('/')))
            result.append(remainder);
    }
    return result;
}

bool MemorySettingsStorage::contains(const QString& key) const
{
    return data_.contains(resolvedKey(key));
}

void MemorySettingsStorage::remove(const QString& key)
{
    const QString resolved = resolvedKey(key);
    const QString groupPrefix = resolved + QLatin1Char('/');
    QStringList toRemove;
    for (auto it = data_.constBegin(); it != data_.constEnd(); ++it) {
        if (it.key() == resolved || it.key().startsWith(groupPrefix))
            toRemove.append(it.key());
    }
    for (const QString& k : toRemove)
        data_.remove(k);
}

void MemorySettingsStorage::clear()
{
    data_.clear();
}

}
