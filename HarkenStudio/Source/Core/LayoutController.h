
#pragma once

#include "../MainWindow.h"
#include "WorkspaceWindowTypes.h"

#include <DockSplitter.h>

#include <QObject>
#include <QHash>
#include <QString>
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QPointer>
#include <functional>

class QWidget;
class QMenu;
class QActionGroup;

namespace VNX {

struct WorkspaceDockCreateSpec;

QByteArray migrateDockNames(const QByteArray& state);

class LayoutController : public QObject
{
    Q_OBJECT

public:
    using PreferredOpenArea = MainWindow::PreferredOpenArea;
    using WindowDockResolver = std::function<ads::CDockWidget*(const QString&)>;

    explicit LayoutController(MainWindow* host);

    struct Docks
    {
        ads::CDockManager* dockManager = nullptr;
        ads::CDockWidget* blueprintDock = nullptr;
        ads::CDockWidget* projectExplorerDock = nullptr;
        ads::CDockWidget* assistantDock = nullptr;
        ads::CDockWidget* controlDock = nullptr;
    };
    void setDocks(const Docks& d);
    void setWindowDockResolver(WindowDockResolver resolver);

    void setInstanceSnapshotProvider(std::function<QList<VNX::DockInstanceSpec>()> provider);
    void setInstanceRestoreFactory(std::function<bool(const VNX::DockInstanceSpec&)> factory);
    void setSingletonSnapshotProvider(std::function<QVariantMap()> provider);
    void setSingletonStateRestorer(std::function<void(const QVariantMap&)> restorer);
    void setVisibleSingletonMaterializer(std::function<void()> materializer);
    void setInstanceCloser(std::function<void()> closer);

    void setLayoutsMenu(QMenu* menu);

    void setPlacementHint(const QString& objectName, PreferredOpenArea area);

    void applyDefaultLayout();
    bool selectBuiltinLayout(const QString& builtinId);
    void revealDock(ads::CDockWidget* dock);
    bool revealPanel(const QString& objectName, bool visible);
    bool isDockLocked(ads::CDockWidget* dock) const;
    ads::CDockWidget* createEditorDock(WorkspaceDockCreateSpec spec, ads::DockWidgetArea edge);
    void relayoutDockSplitters(bool seedSizes);
    void reanchorFloatingZoneDock(ads::CDockWidget* dock);
    void setZoneCollapsed(ads::CDockWidget* representative, bool collapsed);
    void enforceDisabledDocks();
    void loadProjectLayout();
    void saveProjectLayout();
    void selectDefaultLayout();
    QString currentNamedLayout() const { return currentLayoutName_; }
    bool hasNamedLayout() const { return !currentLayoutName_.isEmpty(); }
    void onResetCurrentLayout();
    bool resetCurrentLayout();
    bool resetAllLayouts();
    bool saveNamedLayout(const QString& name, QString* pathOut = nullptr);
    QString saveCurrentLayout(QWidget* dialogParent);
    void onLoadNamedLayout(const QString& name);
    bool loadNamedLayout(const QString& name);
    void onDeleteNamedLayout(const QString& name);
    void onNamedLayoutRenamed(const QString& oldName, const QString& newName);
    void onNamedLayoutRemoved(const QString& name);
    void rebuildLayoutsMenu();

    bool firstShowDone() const { return firstShowDone_; }
    void setFirstShowDone(bool v) { firstShowDone_ = v; }
    bool currentLayoutFromCache() const { return currentLayoutFromCache_; }

signals:
    void defaultLayoutSeeded();
    void statusMessage(const QString& text);
    void dockStateRestored();
    void manageLayoutsRequested();
    void currentLayoutChanged(const QString& name);

private:
    struct ZoneSpec { ads::CDockWidget* dock; ads::DockWidgetArea area; bool open; };

    struct ZoneCollapse
    {
        QList<QPointer<ads::CDockWidget>> hidden;
        QList<QPair<QPointer<ads::CDockSplitter>, QList<int>>> splitterSizes;
        bool restored = false;
    };

    PreferredOpenArea openPlacementFor(const QString& objectName) const;
    ads::CDockAreaWidget* centralTabArea() const;
    void dockIntoCentralTab(ads::CDockWidget* dock);
    bool isFixedSubtree(QWidget* w) const;
    int fixedZoneExtent(QWidget* child, bool horizontal, int total) const;
    ads::CDockAreaWidget* placeDock(ads::CDockWidget* dock, ads::DockWidgetArea zone,
                                    ads::CDockAreaWidget* anchor, bool open);

    QList<ZoneSpec> defaultZoneSpecs() const;
    void seedZoneHomeAreas();
    QWidget* zoneSubtree(ads::CDockWidget* representative) const;
    void applyZoneSizes(const ZoneCollapse& state);
    static QList<ads::CDockAreaWidget*> zoneAreas(QWidget* node);

    QJsonArray collapsedZonesToJson() const;
    void collapsedZonesFromJson(const QJsonArray& zones);

    void ensureDocksForSpecs(const QList<VNX::DockInstanceSpec>& specs);
    QJsonObject captureLayoutState() const;
    void restoreLayoutState(const QJsonObject& json);
    void activateLayout(const QString& name, const QJsonObject& json);
    bool switchLayout(const QString& name);
    bool resetLayouts(bool all);
    void setCurrentLayoutName(const QString& name);

    void restoreDockState(const QByteArray& state);
    ads::CDockWidget* dockForWindow(const QString& typeId) const;
    void hideMonitorDocks();

    MainWindow* host_ = nullptr;
    QMenu* layoutsMenu_ = nullptr;
    QActionGroup* layoutActionGroup_ = nullptr;
    std::function<QList<VNX::DockInstanceSpec>()> instanceSnapshotProvider_;
    std::function<bool(const VNX::DockInstanceSpec&)> instanceRestoreFactory_;
    std::function<QVariantMap()> singletonSnapshotProvider_;
    std::function<void(const QVariantMap&)> singletonStateRestorer_;
    std::function<void()> visibleSingletonMaterializer_;
    std::function<void()> instanceCloser_;

    ads::CDockManager* dockManager_ = nullptr;

    ads::CDockWidget* blueprintDock_ = nullptr;
    ads::CDockWidget* projectExplorerDock_ = nullptr;
    ads::CDockWidget* assistantDock_ = nullptr;
    ads::CDockWidget* controlDock_ = nullptr;
    WindowDockResolver windowDockResolver_;

    QHash<QString, PreferredOpenArea> openPlacementHints_;
    QHash<QString, ads::DockWidgetArea> zoneHomeArea_;
    QHash<QString, ZoneCollapse> collapsedZones_;
    QString currentLayoutName_;
    QJsonObject workingLayouts_;
    bool firstShowDone_ = false;
    bool currentLayoutFromCache_ = false;
};

}
