
#pragma once

#include "ISettingsStorage.h"
#include <QMap>
#include <QStringList>

namespace VNX {

class MemorySettingsStorage : public ISettingsStorage {
public:
    MemorySettingsStorage() = default;
    ~MemorySettingsStorage() override = default;

    void beginGroup(const QString& group) override;
    void endGroup() override;

    QVariant value(const QString& key, const QVariant& defaultValue = QVariant()) const override;
    void setValue(const QString& key, const QVariant& value) override;

    void sync() override {}
    bool isWritable() const override { return true; }

    QStringList allKeys() const override;
    QStringList childGroups() const override;
    QStringList childKeys() const override;
    bool contains(const QString& key) const override;
    void remove(const QString& key) override;
    void clear() override;

    QString fileName() const override { return QString(); }
    QString storageName() const override { return QStringLiteral("Memory"); }

private:
    QString resolvedKey(const QString& key) const;

    QMap<QString, QVariant> data_;
    QStringList groupStack_;
};

}
