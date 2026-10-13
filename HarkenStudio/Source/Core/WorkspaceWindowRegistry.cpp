#include "WorkspaceWindowRegistry.h"

namespace VNX {

bool WorkspaceWindowRegistry::registerSingleton(const QString& typeId, ads::CDockWidget* dock)
{
    if (typeId.isEmpty() || !dock)
        return false;

    unregisterSingleton(typeId);
    singletonDocks_.insert(typeId, dock);
    if (!dock->objectName().isEmpty())
        dockObjectNameToTypeId_.insert(dock->objectName(), typeId);
    return true;
}

void WorkspaceWindowRegistry::unregisterSingleton(const QString& typeId)
{
    const QPointer<ads::CDockWidget> dock = singletonDocks_.take(typeId);
    if (dock && !dock->objectName().isEmpty())
        dockObjectNameToTypeId_.remove(dock->objectName());
}

bool WorkspaceWindowRegistry::containsSingleton(const QString& typeId) const
{
    return singletonDock(typeId) != nullptr;
}

ads::CDockWidget* WorkspaceWindowRegistry::singletonDock(const QString& typeId) const
{
    const auto it = singletonDocks_.constFind(typeId);
    if (it == singletonDocks_.constEnd())
        return nullptr;
    return it.value().data();
}

QString WorkspaceWindowRegistry::singletonTypeForDockObjectName(const QString& dockObjectName) const
{
    return dockObjectNameToTypeId_.value(dockObjectName);
}

QList<QString> WorkspaceWindowRegistry::singletonTypeIds() const
{
    QList<QString> ids;
    ids.reserve(singletonDocks_.size());
    for (auto it = singletonDocks_.constBegin(); it != singletonDocks_.constEnd(); ++it) {
        if (it.value())
            ids.append(it.key());
    }
    return ids;
}

QList<ads::CDockWidget*> WorkspaceWindowRegistry::singletonDocks() const
{
    QList<ads::CDockWidget*> docks;
    docks.reserve(singletonDocks_.size());
    for (const QPointer<ads::CDockWidget>& dock : singletonDocks_) {
        if (dock)
            docks.append(dock.data());
    }
    return docks;
}

QString WorkspaceWindowRegistry::instanceKey(const QString& typeId, int instanceId)
{
    return QStringLiteral("%1:%2").arg(typeId).arg(instanceId);
}

bool WorkspaceWindowRegistry::registerInstance(const QString& typeId, int instanceId,
                                               ads::CDockWidget* dock, QWidget* content)
{
    if (typeId.isEmpty() || instanceId <= 0 || !dock || !content)
        return false;

    unregisterInstance(typeId, instanceId);

    WorkspaceWindowInstanceEntry entry;
    entry.typeId = typeId;
    entry.instanceId = instanceId;
    entry.dock = dock;
    entry.content = content;
    entry.restoreSpec.typeId = typeId;
    entry.restoreSpec.instanceId = instanceId;
    entry.restoreSpec.deleteOnClose =
        dock->features().testFlag(ads::CDockWidget::DockWidgetDeleteOnClose);

    const QString key = instanceKey(typeId, instanceId);
    instanceEntries_.insert(key, entry);
    if (!dock->objectName().isEmpty())
        instanceDockObjectNameToKey_.insert(dock->objectName(), key);
    return true;
}

void WorkspaceWindowRegistry::unregisterInstance(const QString& typeId, int instanceId)
{
    const QString key = instanceKey(typeId, instanceId);
    const WorkspaceWindowInstanceEntry entry = instanceEntries_.take(key);
    if (entry.dock && !entry.dock->objectName().isEmpty())
        instanceDockObjectNameToKey_.remove(entry.dock->objectName());
}

bool WorkspaceWindowRegistry::containsInstance(const QString& typeId, int instanceId) const
{
    return instanceContent(typeId, instanceId) != nullptr;
}

ads::CDockWidget* WorkspaceWindowRegistry::instanceDock(const QString& typeId, int instanceId) const
{
    const auto it = instanceEntries_.constFind(instanceKey(typeId, instanceId));
    if (it == instanceEntries_.constEnd())
        return nullptr;
    return it.value().dock.data();
}

QWidget* WorkspaceWindowRegistry::instanceContent(const QString& typeId, int instanceId) const
{
    const auto it = instanceEntries_.constFind(instanceKey(typeId, instanceId));
    if (it == instanceEntries_.constEnd())
        return nullptr;
    return it.value().content.data();
}

QString WorkspaceWindowRegistry::instanceTypeForDockObjectName(const QString& dockObjectName) const
{
    const QString key = instanceDockObjectNameToKey_.value(dockObjectName);
    const auto it = instanceEntries_.constFind(key);
    if (it == instanceEntries_.constEnd())
        return QString();
    return it.value().typeId;
}

QList<WorkspaceWindowInstanceEntry> WorkspaceWindowRegistry::instanceEntries(const QString& typeId) const
{
    QList<WorkspaceWindowInstanceEntry> entries;
    entries.reserve(instanceEntries_.size());
    for (const WorkspaceWindowInstanceEntry& entry : instanceEntries_) {
        if (!entry.dock || !entry.content)
            continue;
        if (!typeId.isEmpty() && entry.typeId != typeId)
            continue;
        entries.append(entry);
    }
    return entries;
}

void WorkspaceWindowRegistry::setInstanceRestoreSpec(const WorkspaceWindowInstanceSpec& spec)
{
    auto it = instanceEntries_.find(instanceKey(spec.typeId, spec.instanceId));
    if (it == instanceEntries_.end())
        return;
    it->restoreSpec = spec;
}

WorkspaceWindowInstanceSpec WorkspaceWindowRegistry::instanceRestoreSpec(const QString& typeId,
                                                                         int instanceId) const
{
    const auto it = instanceEntries_.constFind(instanceKey(typeId, instanceId));
    if (it == instanceEntries_.constEnd())
        return {};
    return it.value().restoreSpec;
}

}
