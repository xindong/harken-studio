#include "WorkspaceWindowManager.h"
#include "WorkspaceDockFactory.h"

#include <QAction>
#include <QWidget>

#include <utility>

namespace VNX {

WorkspaceWindowManager::WorkspaceWindowManager(WorkspaceWindowCatalog catalog,
                                               QtAdsWindowAdapter adapter)
    : catalog_(std::move(catalog))
    , adapter_(std::move(adapter))
{
}

void WorkspaceWindowManager::registerWindowDescriptor(WorkspaceWindowDescriptor descriptor)
{
    catalog_.registerWindow(std::move(descriptor));
}

void WorkspaceWindowManager::registerSingletonFactory(const QString& typeId, SingletonFactory factory)
{
    if (typeId.isEmpty())
        return;
    singletonFactories_.insert(typeId, std::move(factory));
}

bool WorkspaceWindowManager::registerSingleton(const QString& typeId, ads::CDockWidget* dock)
{
    const WorkspaceWindowDescriptor* descriptor = singletonDescriptor(typeId);
    if (!descriptor || !dock)
        return false;
    if (!descriptor->dockObjectName.isEmpty() && dock->objectName() != descriptor->dockObjectName)
        return false;
    return registry_.registerSingleton(typeId, dock);
}

int WorkspaceWindowManager::registerSingletons(const QList<QPair<QString, ads::CDockWidget*>>& docks)
{
    int count = 0;
    for (const auto& entry : docks) {
        if (registerSingleton(entry.first, entry.second))
            ++count;
    }
    return count;
}

ads::CDockWidget* WorkspaceWindowManager::ensureSingleton(const QString& typeId)
{
    if (ads::CDockWidget* dock = registry_.singletonDock(typeId))
        return dock;

    const WorkspaceWindowDescriptor* descriptor = singletonDescriptor(typeId);
    if (!descriptor || descriptor->availability == AvailabilityPolicy::FeatureDisabled)
        return nullptr;

    auto* dock = new ads::CDockWidget(descriptor->title);
    dock->setObjectName(descriptor->dockObjectName);

    const auto factoryIt = singletonFactories_.constFind(typeId);
    if (factoryIt != singletonFactories_.constEnd() && factoryIt.value().configureShell)
        factoryIt.value().configureShell(dock);

    if (descriptor->closePolicy == ClosePolicy::Disable)
        dock->setFeatures(ads::CDockWidget::NoDockWidgetFeatures);

    if (!adapter_.addDock(dock, *descriptor))
    {
        delete dock;
        return nullptr;
    }

    if (!registerSingleton(typeId, dock))
        return nullptr;

    if (descriptor->materialization == MaterializationPolicy::Eager)
        materializeSingletonContent(typeId);

    return dock;
}

QList<ads::CDockWidget*> WorkspaceWindowManager::ensureSingletons(const QList<QString>& typeIds)
{
    QList<ads::CDockWidget*> docks;
    docks.reserve(typeIds.size());
    for (const QString& typeId : typeIds) {
        if (ads::CDockWidget* dock = ensureSingleton(typeId))
            docks.append(dock);
    }
    return docks;
}

QWidget* WorkspaceWindowManager::materializeSingletonContent(const QString& typeId)
{
    ads::CDockWidget* dock = ensureSingleton(typeId);
    if (!dock)
        return nullptr;
    const WorkspaceWindowDescriptor* descriptor = singletonDescriptor(typeId);
    if (!descriptor || descriptor->availability == AvailabilityPolicy::FeatureDisabled)
        return nullptr;
    if (QWidget* existing = dock->widget())
        return existing;

    const auto factoryIt = singletonFactories_.constFind(typeId);
    if (factoryIt == singletonFactories_.constEnd() || !factoryIt.value().createContent)
        return nullptr;

    QWidget* content = factoryIt.value().createContent();
    if (!content)
        return nullptr;

    WorkspaceDockFactory::setContent(dock, content);
    if (factoryIt.value().contentCreated)
        factoryIt.value().contentCreated(content);
    return content;
}

bool WorkspaceWindowManager::materializeVisibleSingleton(const QString& typeId)
{
    const WorkspaceWindowDescriptor* descriptor = singletonDescriptor(typeId);
    if (!descriptor || descriptor->materialization != MaterializationPolicy::ShellEagerContentLazy)
        return false;

    ads::CDockWidget* dock = registry_.singletonDock(typeId);
    if (!isSingletonAvailable(typeId)) {
        adapter_.hideDock(dock);
        return false;
    }
    if (!adapter_.isDockVisible(dock))
        return false;

    return materializeSingletonContent(typeId) != nullptr;
}

int WorkspaceWindowManager::materializeVisibleSingletons()
{
    int count = 0;
    const QList<QString> typeIds = registeredSingletonTypeIds();
    for (const QString& typeId : typeIds) {
        if (materializeVisibleSingleton(typeId))
            ++count;
    }
    return count;
}

QWidget* WorkspaceWindowManager::singletonContent(const QString& typeId) const
{
    ads::CDockWidget* dock = registry_.singletonDock(typeId);
    return dock ? dock->widget() : nullptr;
}

QVariantMap WorkspaceWindowManager::singletonStates() const
{
    QVariantMap states;
    for (const QString& typeId : registeredSingletonTypeIds()) {
        const auto factory = singletonFactories_.constFind(typeId);
        if (factory == singletonFactories_.constEnd() || !factory->snapshotState)
            continue;
        if (QWidget* content = singletonContent(typeId))
            states.insert(typeId, factory->snapshotState(content));
    }
    return states;
}

void WorkspaceWindowManager::restoreSingletonStates(const QVariantMap& states)
{
    for (const QString& typeId : registeredSingletonTypeIds()) {
        const auto factory = singletonFactories_.constFind(typeId);
        if (factory == singletonFactories_.constEnd() || !factory->restoreState)
            continue;
        const QString stateTypeId = typeId == QLatin1String("Control") && !states.contains(typeId)
            ? QStringLiteral("Preview") : typeId;
        QWidget* content = singletonContent(typeId);
        if (!content && states.contains(stateTypeId))
            content = materializeSingletonContent(typeId);
        if (content)
            factory->restoreState(content, states.value(stateTypeId).toMap());
    }
}

bool WorkspaceWindowManager::showSingleton(const QString& typeId) const
{
    auto* self = const_cast<WorkspaceWindowManager*>(this);
    if (!self->ensureSingleton(typeId))
        return false;
    if (!isSingletonAvailable(typeId))
        return false;
    self->materializeSingletonContent(typeId);
    return adapter_.showDock(registry_.singletonDock(typeId));
}

bool WorkspaceWindowManager::hideSingleton(const QString& typeId) const
{
    return adapter_.hideDock(registry_.singletonDock(typeId));
}

bool WorkspaceWindowManager::focusSingleton(const QString& typeId) const
{
    if (!isSingletonAvailable(typeId))
        return false;
    const_cast<WorkspaceWindowManager*>(this)->materializeSingletonContent(typeId);
    return adapter_.focusDock(registry_.singletonDock(typeId));
}

bool WorkspaceWindowManager::setSingletonVisible(const QString& typeId, bool visible) const
{
    return visible ? showSingleton(typeId) : hideSingleton(typeId);
}

bool WorkspaceWindowManager::isSingletonVisible(const QString& typeId) const
{
    return adapter_.isDockVisible(registry_.singletonDock(typeId));
}

bool WorkspaceWindowManager::isSingletonAvailable(const QString& typeId) const
{
    const WorkspaceWindowDescriptor* descriptor = singletonDescriptor(typeId);
    if (!descriptor || descriptor->availability == AvailabilityPolicy::FeatureDisabled)
        return false;
    return adapter_.isDockAvailable(registry_.singletonDock(typeId));
}

QAction* WorkspaceWindowManager::singletonToggleAction(const QString& typeId) const
{
    return adapter_.toggleAction(registry_.singletonDock(typeId));
}

ads::CDockWidget* WorkspaceWindowManager::singletonDock(const QString& typeId) const
{
    return registry_.singletonDock(typeId);
}

QString WorkspaceWindowManager::singletonTypeForDockObjectName(const QString& dockObjectName) const
{
    return registry_.singletonTypeForDockObjectName(dockObjectName);
}

QList<QString> WorkspaceWindowManager::registeredSingletonTypeIds() const
{
    return registry_.singletonTypeIds();
}

QList<ads::CDockWidget*> WorkspaceWindowManager::registeredSingletonDocks() const
{
    return registry_.singletonDocks();
}

void WorkspaceWindowManager::registerMultiInstanceFactory(const QString& typeId,
                                                          MultiInstanceFactory factory)
{
    if (typeId.isEmpty())
        return;
    multiInstanceFactories_.insert(typeId, std::move(factory));
}

bool WorkspaceWindowManager::createInstance(const QString& typeId) const
{
    const WorkspaceWindowDescriptor* descriptor = multiInstanceDescriptor(typeId);
    if (!descriptor || descriptor->availability == AvailabilityPolicy::FeatureDisabled)
        return false;
    const auto factoryIt = multiInstanceFactories_.constFind(typeId);
    if (factoryIt == multiInstanceFactories_.constEnd() || !factoryIt.value().create)
        return false;
    return factoryIt.value().create();
}

bool WorkspaceWindowManager::restoreInstance(const WorkspaceWindowInstanceSpec& spec)
{
    const WorkspaceWindowDescriptor* descriptor = multiInstanceDescriptor(spec.typeId);
    if (!descriptor || descriptor->availability == AvailabilityPolicy::FeatureDisabled)
        return false;
    const auto factoryIt = multiInstanceFactories_.constFind(spec.typeId);
    if (factoryIt == multiInstanceFactories_.constEnd() || !factoryIt.value().recreate)
        return false;

    if (registry_.containsInstance(spec.typeId, spec.instanceId)) {
        registry_.setInstanceRestoreSpec(spec);
        return factoryIt.value().applyRestore
            ? factoryIt.value().applyRestore(spec)
            : true;
    }

    const bool restored = factoryIt.value().recreate(spec);
    if (restored)
        registry_.setInstanceRestoreSpec(spec);
    return restored;
}

bool WorkspaceWindowManager::registerInstance(const QString& typeId, int instanceId,
                                              ads::CDockWidget* dock, QWidget* content)
{
    const WorkspaceWindowDescriptor* descriptor = multiInstanceDescriptor(typeId);
    if (!descriptor || !dock || !content || instanceId <= 0)
        return false;
    if (!descriptor->dockObjectName.isEmpty()
        && !dock->objectName().startsWith(descriptor->dockObjectName))
        return false;
    return registry_.registerInstance(typeId, instanceId, dock, content);
}

void WorkspaceWindowManager::unregisterInstance(const QString& typeId, int instanceId)
{
    registry_.unregisterInstance(typeId, instanceId);
}

QWidget* WorkspaceWindowManager::instanceContent(const QString& typeId, int instanceId) const
{
    return registry_.instanceContent(typeId, instanceId);
}

ads::CDockWidget* WorkspaceWindowManager::instanceDock(const QString& typeId, int instanceId) const
{
    return registry_.instanceDock(typeId, instanceId);
}

bool WorkspaceWindowManager::focusInstance(const QString& typeId, int instanceId) const
{
    return adapter_.focusDock(registry_.instanceDock(typeId, instanceId));
}

bool WorkspaceWindowManager::setInstanceVisible(const QString& typeId, int instanceId, bool visible) const
{
    return adapter_.setDockVisible(registry_.instanceDock(typeId, instanceId), visible);
}

bool WorkspaceWindowManager::isInstanceVisible(const QString& typeId, int instanceId) const
{
    return adapter_.isDockVisible(registry_.instanceDock(typeId, instanceId));
}

bool WorkspaceWindowManager::closeInstance(const QString& typeId, int instanceId) const
{
    ads::CDockWidget* dock = registry_.instanceDock(typeId, instanceId);
    if (!dock)
        return false;
    const WorkspaceWindowDescriptor* descriptor = multiInstanceDescriptor(typeId);
    if (descriptor && descriptor->closePolicy == ClosePolicy::Hide)
        return adapter_.hideDock(dock);
    dock->closeDockWidget();
    return true;
}

QList<WorkspaceWindowInstanceSpec> WorkspaceWindowManager::instanceSpecs() const
{
    QList<WorkspaceWindowInstanceSpec> specs;
    const QList<WorkspaceWindowInstanceEntry> entries = registry_.instanceEntries();
    specs.reserve(entries.size());
    for (const WorkspaceWindowInstanceEntry& entry : entries) {
        WorkspaceWindowInstanceSpec spec = entry.restoreSpec;
        const auto factoryIt = multiInstanceFactories_.constFind(entry.typeId);
        if (factoryIt != multiInstanceFactories_.constEnd() && factoryIt.value().snapshot)
            spec = factoryIt.value().snapshot(entry.instanceId, entry.content.data(), entry.dock.data());
        if (spec.typeId.isEmpty())
            spec.typeId = entry.typeId;
        if (spec.instanceId <= 0)
            spec.instanceId = entry.instanceId;
        specs.append(spec);
    }
    return specs;
}

const WorkspaceWindowDescriptor* WorkspaceWindowManager::singletonDescriptor(const QString& typeId) const
{
    const WorkspaceWindowDescriptor* descriptor = catalog_.find(typeId);
    if (!descriptor || descriptor->kind != WindowKind::Singleton)
        return nullptr;
    return descriptor;
}

const WorkspaceWindowDescriptor* WorkspaceWindowManager::multiInstanceDescriptor(const QString& typeId) const
{
    const WorkspaceWindowDescriptor* descriptor = catalog_.find(typeId);
    if (!descriptor || descriptor->kind != WindowKind::MultiInstance)
        return nullptr;
    return descriptor;
}

}
