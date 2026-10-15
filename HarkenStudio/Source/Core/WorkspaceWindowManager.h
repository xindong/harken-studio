#pragma once

#include "QtAdsWindowAdapter.h"
#include "WorkspaceWindowRegistry.h"
#include "WorkspaceWindowTypes.h"

#include <QList>
#include <QHash>
#include <QPair>
#include <QString>

#include <functional>

class QAction;
class QWidget;

namespace VNX {

class WorkspaceWindowManager
{
public:
    struct SingletonFactory
    {
        std::function<QWidget*()> createContent;
        std::function<void(ads::CDockWidget*)> configureShell;
        std::function<void(QWidget*)> contentCreated;
        std::function<QVariantMap(QWidget*)> snapshotState = {};
        std::function<void(QWidget*, const QVariantMap&)> restoreState = {};
    };

    struct MultiInstanceFactory
    {
        std::function<bool()> create;
        std::function<bool(const WorkspaceWindowInstanceSpec&)> recreate;
        std::function<bool(const WorkspaceWindowInstanceSpec&)> applyRestore;
        std::function<WorkspaceWindowInstanceSpec(int, QWidget*, ads::CDockWidget*)> snapshot;
    };

    WorkspaceWindowManager(WorkspaceWindowCatalog catalog, QtAdsWindowAdapter adapter);

    const WorkspaceWindowCatalog& catalog() const { return catalog_; }
    const WorkspaceWindowRegistry& registry() const { return registry_; }

    void registerWindowDescriptor(WorkspaceWindowDescriptor descriptor);

    void registerSingletonFactory(const QString& typeId, SingletonFactory factory);
    bool registerSingleton(const QString& typeId, ads::CDockWidget* dock);
    int registerSingletons(const QList<QPair<QString, ads::CDockWidget*>>& docks);
    ads::CDockWidget* ensureSingleton(const QString& typeId);
    QList<ads::CDockWidget*> ensureSingletons(const QList<QString>& typeIds);
    QWidget* materializeSingletonContent(const QString& typeId);
    bool materializeVisibleSingleton(const QString& typeId);
    int materializeVisibleSingletons();
    QWidget* singletonContent(const QString& typeId) const;

    bool showSingleton(const QString& typeId) const;
    bool hideSingleton(const QString& typeId) const;
    bool focusSingleton(const QString& typeId) const;
    bool setSingletonVisible(const QString& typeId, bool visible) const;

    bool isSingletonVisible(const QString& typeId) const;
    bool isSingletonAvailable(const QString& typeId) const;
    QAction* singletonToggleAction(const QString& typeId) const;
    ads::CDockWidget* singletonDock(const QString& typeId) const;
    QString singletonTypeForDockObjectName(const QString& dockObjectName) const;

    QList<QString> registeredSingletonTypeIds() const;
    QList<ads::CDockWidget*> registeredSingletonDocks() const;
    QVariantMap singletonStates() const;
    void restoreSingletonStates(const QVariantMap& states);

    void registerMultiInstanceFactory(const QString& typeId, MultiInstanceFactory factory);
    bool createInstance(const QString& typeId) const;
    bool restoreInstance(const WorkspaceWindowInstanceSpec& spec);
    bool registerInstance(const QString& typeId, int instanceId,
                          ads::CDockWidget* dock, QWidget* content);
    void unregisterInstance(const QString& typeId, int instanceId);
    QWidget* instanceContent(const QString& typeId, int instanceId) const;
    ads::CDockWidget* instanceDock(const QString& typeId, int instanceId) const;
    bool focusInstance(const QString& typeId, int instanceId) const;
    bool setInstanceVisible(const QString& typeId, int instanceId, bool visible) const;
    bool isInstanceVisible(const QString& typeId, int instanceId) const;
    bool closeInstance(const QString& typeId, int instanceId) const;
    QList<WorkspaceWindowInstanceSpec> instanceSpecs() const;

private:
    const WorkspaceWindowDescriptor* singletonDescriptor(const QString& typeId) const;
    const WorkspaceWindowDescriptor* multiInstanceDescriptor(const QString& typeId) const;

    WorkspaceWindowCatalog catalog_;
    QtAdsWindowAdapter adapter_;
    WorkspaceWindowRegistry registry_;
    QHash<QString, SingletonFactory> singletonFactories_;
    QHash<QString, MultiInstanceFactory> multiInstanceFactories_;
};

}
