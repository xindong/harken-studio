
#pragma once

#include "ISettingsStorage.h"
#include <QSettings>
#include <memory>

namespace VNX {

class QSettingsStorage : public ISettingsStorage {
public:
    QSettingsStorage(const QString& organization, const QString& application);
    
    explicit QSettingsStorage(const QString& filePath);
    
    ~QSettingsStorage() override = default;
    
    void beginGroup(const QString& group) override;
    void endGroup() override;
    
    QVariant value(const QString& key, const QVariant& defaultValue = QVariant()) const override;
    void setValue(const QString& key, const QVariant& value) override;
    
    void sync() override;
    bool isWritable() const override;
    
    QStringList allKeys() const override;
    QStringList childGroups() const override;
    QStringList childKeys() const override;
    bool contains(const QString& key) const override;
    void remove(const QString& key) override;
    void clear() override;
    
    QString fileName() const override;
    QString storageName() const override { return "QSettings"; }

private:
    std::unique_ptr<QSettings> settings_;
};

}
