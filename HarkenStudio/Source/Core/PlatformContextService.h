#pragma once

#include "ISettingsStorage.h"
#include "PlatformContext.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>
#include <optional>

namespace VNX {

class PlatformContextService : public QObject
{
    Q_OBJECT

public:
    static PlatformContextService& instance();

    explicit PlatformContextService(std::unique_ptr<ISettingsStorage> storage,
                                    QObject* parent = nullptr);

    const PlatformContext& context() const { return context_; }

    void setAll(bool all);
    void setSelected(const QVector<ProjectPlatformId>& selected, ProjectPlatformId primary);
    void setSelected(const QVector<ProjectPlatformId>& selected, ProjectPlatformId primary, bool all);
    void setStorageForTesting(std::unique_ptr<ISettingsStorage> storage);

signals:
    void contextChanged(const VNX::PlatformContext& context);

private:
    PlatformContextService();

    QString storageGroup() const;
    void persistContext(const PlatformContext& context);
    QStringList persistedSelectedIds() const;
    QString persistedPrimaryId() const;
    std::optional<bool> persistedAll() const;
    void recompute();
    void applyContext(const PlatformContext& next);
    void setStorage(std::unique_ptr<ISettingsStorage> storage);

    std::unique_ptr<ISettingsStorage> storage_;
    PlatformContext context_;

    friend class PlatformFieldMergeInspector;
    friend class PlatformOverrideInspector;
    friend class PropertyEditorEnginePlatformContextSessionTest;
    friend class CurrentWorldPrimaryScope;
};

}
