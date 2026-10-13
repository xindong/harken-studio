
#pragma once

#include <QString>
#include <QVariant>
#include <memory>

namespace VNX {

class ISettingsStorage {
public:
    virtual ~ISettingsStorage() = default;
    
    virtual void beginGroup(const QString& group) = 0;
    virtual void endGroup() = 0;
    
    virtual QVariant value(const QString& key, const QVariant& defaultValue = QVariant()) const = 0;
    virtual void setValue(const QString& key, const QVariant& value) = 0;
    
    virtual void sync() = 0;
    virtual bool isWritable() const = 0;
    
    virtual QStringList allKeys() const = 0;
    virtual QStringList childGroups() const = 0;
    virtual QStringList childKeys() const = 0;
    virtual bool contains(const QString& key) const = 0;
    virtual void remove(const QString& key) = 0;
    virtual void clear() = 0;
    
    virtual QString fileName() const = 0;
    virtual QString storageName() const = 0;
};

class SettingsStorageFactory {
public:
    enum class StorageType {
        QSettings,
        Json,
        Memory
    };
    
    static std::unique_ptr<ISettingsStorage> create(
        StorageType type,
        const QString& organization,
        const QString& application);
    
    static std::unique_ptr<ISettingsStorage> createFromFile(
        StorageType type,
        const QString& filePath);
};

}
