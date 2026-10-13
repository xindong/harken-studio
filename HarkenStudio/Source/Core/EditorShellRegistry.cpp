
#include "EditorShellRegistry.h"

#include <algorithm>

namespace VNX {

void EditorShellRegistry::registerEditorType(const QString& editorType, EditorTypeInfo info)
{
    if (editorType.isEmpty())
        return;
    if (!types_.contains(editorType))
        typeOrder_.append(editorType);
    types_.insert(editorType, std::move(info));
}

bool EditorShellRegistry::hasEditorType(const QString& editorType) const
{
    return types_.contains(editorType);
}

QStringList EditorShellRegistry::registeredEditorTypes() const
{
    return typeOrder_;
}

const EditorShellRegistry::EditorTypeInfo* EditorShellRegistry::editorTypeInfo(const QString& editorType) const
{
    auto it = types_.constFind(editorType);
    return it == types_.constEnd() ? nullptr : &it.value();
}

QString EditorShellRegistry::editorTypeForDock(const QString& dockObjectName) const
{
    if (dockObjectName.isEmpty())
        return QString();

    for (const QString& editorType : typeOrder_) {
        const auto it = types_.constFind(editorType);
        if (it == types_.constEnd())
            continue;
        const QString prefix = it.value().dockObjectPrefix;
        if (!prefix.isEmpty() && dockObjectName.startsWith(prefix, Qt::CaseSensitive))
            return editorType;
    }
    return QString();
}

bool EditorShellRegistry::isEditorDock(const QString& dockObjectName) const
{
    return !editorTypeForDock(dockObjectName).isEmpty();
}

bool EditorShellRegistry::recreate(const EditorShellSpec& spec) const
{
    auto it = types_.constFind(spec.editorType);
    if (it == types_.constEnd() || !it.value().recreate)
        return false;
    it.value().recreate(spec);
    return true;
}

int EditorShellRegistry::allocateShellId(const QString& editorType) const
{
    QList<int> used;
    for (const Entry& entry : entries_) {
        if (entry.editorType == editorType && entry.widget)
            used.append(entry.instanceId);
    }

    std::sort(used.begin(), used.end());
    int candidate = 1;
    for (int id : used) {
        if (id < candidate)
            continue;
        if (id == candidate)
            ++candidate;
        else
            break;
    }
    return candidate;
}

void EditorShellRegistry::registerShell(const QString& editorType, int instanceId, QWidget* widget)
{
    if (editorType.isEmpty() || instanceId <= 0 || !widget)
        return;
    if (findEntry(editorType, instanceId))
        return;

    Entry entry;
    entry.editorType = editorType;
    entry.instanceId = instanceId;
    entry.widget = widget;
    entries_.append(entry);
}

void EditorShellRegistry::unregisterShell(const QString& editorType, int instanceId)
{
    for (int i = 0; i < entries_.size(); ++i) {
        if (entries_[i].editorType == editorType && entries_[i].instanceId == instanceId) {
            entries_.removeAt(i);
            if (lastFocused_.value(editorType, kNoFocus) == instanceId)
                lastFocused_.insert(editorType, kNoFocus);
            return;
        }
    }
}

QWidget* EditorShellRegistry::findShell(const QString& editorType, int instanceId) const
{
    const Entry* entry = findEntry(editorType, instanceId);
    return entry ? entry->widget.data() : nullptr;
}

void EditorShellRegistry::setObjectKey(const QString& editorType, int instanceId, const QString& objectKey)
{
    if (Entry* entry = findEntry(editorType, instanceId))
        entry->objectKey = objectKey;
}

QString EditorShellRegistry::objectKey(const QString& editorType, int instanceId) const
{
    const Entry* entry = findEntry(editorType, instanceId);
    return entry ? entry->objectKey : QString();
}

void EditorShellRegistry::setPinned(const QString& editorType, int instanceId, bool pinned)
{
    if (Entry* entry = findEntry(editorType, instanceId))
        entry->pinned = pinned;
}

bool EditorShellRegistry::isPinned(const QString& editorType, int instanceId) const
{
    const Entry* entry = findEntry(editorType, instanceId);
    return entry ? entry->pinned : false;
}

void EditorShellRegistry::setVisible(const QString& editorType, int instanceId, bool visible)
{
    if (Entry* entry = findEntry(editorType, instanceId))
        entry->visible = visible;
}

bool EditorShellRegistry::isVisible(const QString& editorType, int instanceId) const
{
    const Entry* entry = findEntry(editorType, instanceId);
    return entry ? entry->visible : false;
}

int EditorShellRegistry::lastFocusedShellId(const QString& editorType) const
{
    return lastFocused_.value(editorType, kNoFocus);
}

void EditorShellRegistry::setLastFocusedShellId(const QString& editorType, int instanceId)
{
    if (editorType.isEmpty())
        return;
    lastFocused_.insert(editorType, instanceId > 0 ? instanceId : kNoFocus);
}

EditorShellRouteTarget EditorShellRegistry::routeTarget(const QString& editorType,
                                                        const QString& objectKey) const
{
    const QString key = objectKey.trimmed();
    if (!key.isEmpty()) {
        for (const Entry& entry : entries_) {
            if (entry.editorType == editorType && entry.widget && entry.objectKey == key) {
                return {entry.instanceId, entry.widget, true};
            }
        }
    }

    const int lastFocused = lastFocusedShellId(editorType);
    EditorShellRouteTarget firstVisible;
    for (const Entry& entry : entries_) {
        if (entry.editorType != editorType || !entry.widget || !entry.visible || entry.pinned)
            continue;
        if (!firstVisible.isValid())
            firstVisible = {entry.instanceId, entry.widget, false};
        if (entry.instanceId == lastFocused)
            return {entry.instanceId, entry.widget, false};
    }
    return firstVisible;
}

QList<EditorShellSpec> EditorShellRegistry::snapshot(const QString& editorType) const
{
    QList<EditorShellSpec> specs;
    for (const Entry& entry : entries_) {
        if (!editorType.isEmpty() && entry.editorType != editorType)
            continue;
        if (!entry.widget)
            continue;
        EditorShellSpec spec;
        spec.editorType = entry.editorType;
        spec.instanceId = entry.instanceId;
        spec.objectKey = entry.objectKey;
        spec.pinned = entry.pinned;
        spec.visible = entry.visible;
        specs.append(spec);
    }
    return specs;
}

EditorShellRegistry::Entry* EditorShellRegistry::findEntry(const QString& editorType, int instanceId)
{
    for (Entry& entry : entries_) {
        if (entry.editorType == editorType && entry.instanceId == instanceId)
            return &entry;
    }
    return nullptr;
}

const EditorShellRegistry::Entry* EditorShellRegistry::findEntry(const QString& editorType, int instanceId) const
{
    for (const Entry& entry : entries_) {
        if (entry.editorType == editorType && entry.instanceId == instanceId)
            return &entry;
    }
    return nullptr;
}

}
