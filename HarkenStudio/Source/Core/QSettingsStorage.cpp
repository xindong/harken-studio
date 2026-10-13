
#include "QSettingsStorage.h"
#include "JsonSettingsStorage.h"
#include "MemorySettingsStorage.h"

namespace VNX {

QSettingsStorage::QSettingsStorage(const QString& organization, const QString& application)
    : settings_(std::make_unique<QSettings>(QSettings::defaultFormat(),
                                            QSettings::UserScope,
                                            organization,
                                            application))
{
}

QSettingsStorage::QSettingsStorage(const QString& filePath)
    : settings_(std::make_unique<QSettings>(filePath, QSettings::IniFormat))
{
}

void QSettingsStorage::beginGroup(const QString& group)
{
    settings_->beginGroup(group);
}

void QSettingsStorage::endGroup()
{
    settings_->endGroup();
}

QVariant QSettingsStorage::value(const QString& key, const QVariant& defaultValue) const
{
    return settings_->value(key, defaultValue);
}

void QSettingsStorage::setValue(const QString& key, const QVariant& value)
{
    settings_->setValue(key, value);
}

void QSettingsStorage::sync()
{
    settings_->sync();
}

bool QSettingsStorage::isWritable() const
{
    return settings_->isWritable();
}

QStringList QSettingsStorage::allKeys() const
{
    return settings_->allKeys();
}

QStringList QSettingsStorage::childGroups() const
{
    return settings_->childGroups();
}

QStringList QSettingsStorage::childKeys() const
{
    return settings_->childKeys();
}

bool QSettingsStorage::contains(const QString& key) const
{
    return settings_->contains(key);
}

void QSettingsStorage::remove(const QString& key)
{
    settings_->remove(key);
}

void QSettingsStorage::clear()
{
    settings_->clear();
}

QString QSettingsStorage::fileName() const
{
    return settings_->fileName();
}

std::unique_ptr<ISettingsStorage> SettingsStorageFactory::create(
    StorageType type,
    const QString& organization,
    const QString& application)
{
    switch (type) {
        case StorageType::QSettings:
            return std::make_unique<QSettingsStorage>(organization, application);
        case StorageType::Json:
            return std::make_unique<JsonSettingsStorage>(organization, application);
        case StorageType::Memory:
            return std::make_unique<MemorySettingsStorage>();
        default:
            return std::make_unique<QSettingsStorage>(organization, application);
    }
}

std::unique_ptr<ISettingsStorage> SettingsStorageFactory::createFromFile(
    StorageType type,
    const QString& filePath)
{
    switch (type) {
        case StorageType::QSettings:
            return std::make_unique<QSettingsStorage>(filePath);
        case StorageType::Json:
            return std::make_unique<JsonSettingsStorage>(filePath);
        case StorageType::Memory:
            return std::make_unique<MemorySettingsStorage>();
        default:
            return std::make_unique<QSettingsStorage>(filePath);
    }
}

}
