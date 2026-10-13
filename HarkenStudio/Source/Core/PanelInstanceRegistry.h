
#pragma once

#include "WorkspaceWindowTypes.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QPair>
#include <QMap>
#include <QVariantMap>
#include <QPointer>
#include <QWidget>
#include <functional>

namespace VNX {

class PanelInstanceRegistry : public QObject
{
    Q_OBJECT

public:
    struct PanelTypeInfo
    {
        QString label;
        QString dockObjectPrefix;
        std::function<void()> create;
        std::function<void(const DockInstanceSpec&)> recreate;
    };

    static constexpr int kNoFocus = -1;

    explicit PanelInstanceRegistry(QObject* parent = nullptr);

    void registerPanelType(const QString& panelType, PanelTypeInfo info);
    bool hasPanelType(const QString& panelType) const;
    QStringList registeredPanelTypes() const;
    const PanelTypeInfo* panelTypeInfo(const QString& panelType) const;

    bool recreate(const DockInstanceSpec& spec) const;

    bool isMultiInstanceDock(const QString& dockObjectName) const;

    int allocatePanelId(const QString& panelType);

    void registerPanel(const QString& panelType, int panelId, QWidget* widget);
    void unregisterPanel(const QString& panelType, int panelId);
    QWidget* findPanel(const QString& panelType, int panelId) const;
    QList<QWidget*> allPanels(const QString& panelType) const;

    QList<QPair<int, QWidget*>> instances(const QString& panelType) const;

    int lastFocusedPanelId(const QString& panelType) const;
    void setLastFocusedPanelId(const QString& panelType, int panelId);

signals:
    void panelRegistered(const QString& panelType, int panelId, QWidget* widget);
    void panelUnregistered(const QString& panelType, int panelId);
    void lastFocusedChanged(const QString& panelType, int panelId);

private:
    struct InstanceEntry
    {
        QString panelType;
        int panelId = 0;
        QPointer<QWidget> widget;
    };

    QStringList typeOrder_;
    QMap<QString, PanelTypeInfo> types_;
    QList<InstanceEntry> instances_;
    QMap<QString, int> lastFocused_;
};

}
