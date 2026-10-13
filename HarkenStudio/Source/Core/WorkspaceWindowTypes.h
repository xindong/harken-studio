#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>

class QWidget;

namespace VNX {

enum class WindowKind
{
    Singleton,
    MultiInstance
};

enum class MaterializationPolicy
{
    Eager,
    ProjectReady,
    OnDemand,
    ShellEagerContentLazy
};

enum class ClosePolicy
{
    Hide,
    Destroy,
    Disable
};

enum class AvailabilityPolicy
{
    Always,
    ProjectOnly,
    FeatureDisabled
};

enum class DefaultDockArea
{
    Top,
    Left,
    Right,
    Bottom,
    Center,
    Floating
};

struct DefaultPlacement
{
    DefaultDockArea area = DefaultDockArea::Floating;
    QString anchorDockObjectName;
    bool initiallyVisible = true;
};

struct WorkspaceWindowDescriptor
{
    QString typeId;
    QString title;
    QString dockObjectName;
    WindowKind kind = WindowKind::Singleton;
    DefaultPlacement placement;
    MaterializationPolicy materialization = MaterializationPolicy::Eager;
    ClosePolicy closePolicy = ClosePolicy::Hide;
    AvailabilityPolicy availability = AvailabilityPolicy::Always;
};

struct WorkspaceWindowInstanceSpec
{
    QString typeId;
    int instanceId = 0;
    bool deleteOnClose = true;
    QVariantMap params;
    QString pinnedObjectGuid;
};

using DockInstanceSpec = WorkspaceWindowInstanceSpec;

class WorkspaceWindowCatalog
{
public:
    void registerWindow(WorkspaceWindowDescriptor descriptor);
    bool contains(const QString& typeId) const;
    const WorkspaceWindowDescriptor* find(const QString& typeId) const;
    QVector<WorkspaceWindowDescriptor> descriptors() const;

private:
    QVector<WorkspaceWindowDescriptor> descriptors_;
};

QVector<WorkspaceWindowDescriptor> defaultWorkspaceWindowDescriptors();
WorkspaceWindowCatalog defaultWorkspaceWindowCatalog();
QStringList ipcKnownPanelObjectNames();
QString ipcKnownPanelObjectNameFromWidget(const QWidget* widget);
QString ipcFocusedWidgetKnownPanelName(const QWidget* focused);
QString ipcLastInteractedWidgetKnownPanelName(const QWidget* widget);
bool workspaceWindowSelectorMatches(const QString& selector, const QString& typeId,
                                   const QString& objectName = QString(),
                                   const QString& title = QString(),
                                   int instanceId = 0);

QJsonObject workspaceWindowInstanceSpecToJson(const WorkspaceWindowInstanceSpec& spec);
WorkspaceWindowInstanceSpec workspaceWindowInstanceSpecFromJson(const QJsonObject& obj);
QJsonArray workspaceWindowInstanceSpecsToJson(const QList<WorkspaceWindowInstanceSpec>& specs);
QList<WorkspaceWindowInstanceSpec> workspaceWindowInstanceSpecsFromJson(const QJsonArray& arr);

}
