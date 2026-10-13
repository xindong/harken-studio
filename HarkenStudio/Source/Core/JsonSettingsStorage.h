
#pragma once

#include "ISettingsStorage.h"
#include <QJsonObject>
#include <QStringList>

namespace VNX {

class JsonSettingsStorage : public ISettingsStorage {
public:
    JsonSettingsStorage(const QString& organization, const QString& application);

    explicit JsonSettingsStorage(const QString& filePath);

    ~JsonSettingsStorage() override { sync(); }

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

    QString fileName() const override { return filePath_; }
    QString storageName() const override { return QStringLiteral("JSON"); }

private:
    void load();
    QString resolvedKey(const QString& key) const;

    QString filePath_;
    QJsonObject root_;
    QStringList groupStack_;
    bool dirty_ = false;
};

}
