
#include "PanelInstanceRegistry.h"

namespace VNX {

PanelInstanceRegistry::PanelInstanceRegistry(QObject* parent)
    : QObject(parent)
{
}

void PanelInstanceRegistry::registerPanelType(const QString& panelType, PanelTypeInfo info)
{
    if (!types_.contains(panelType))
        typeOrder_.append(panelType);
    types_.insert(panelType, std::move(info));
}

bool PanelInstanceRegistry::hasPanelType(const QString& panelType) const
{
    return types_.contains(panelType);
}

QStringList PanelInstanceRegistry::registeredPanelTypes() const
{
    return typeOrder_;
}

const PanelInstanceRegistry::PanelTypeInfo* PanelInstanceRegistry::panelTypeInfo(const QString& panelType) const
{
    auto it = types_.constFind(panelType);
    return it == types_.constEnd() ? nullptr : &it.value();
}

bool PanelInstanceRegistry::recreate(const DockInstanceSpec& spec) const
{
    auto it = types_.constFind(spec.typeId);
    if (it == types_.constEnd() || !it.value().recreate)
        return false;
    it.value().recreate(spec);
    return true;
}

bool PanelInstanceRegistry::isMultiInstanceDock(const QString& dockObjectName) const
{
    for (const QString& t : typeOrder_)
    {
        const auto& info = types_[t];
        if (!info.dockObjectPrefix.isEmpty() && dockObjectName.startsWith(info.dockObjectPrefix))
            return true;
    }
    return false;
}

int PanelInstanceRegistry::allocatePanelId(const QString& panelType)
{
    QList<int> used;
    for (const auto& e : instances_)
    {
        if (e.panelType == panelType && e.widget)
            used.append(e.panelId);
    }
    std::sort(used.begin(), used.end());
    int candidate = 1;
    for (int id : used)
    {
        if (id < candidate) continue;
        if (id == candidate) ++candidate;
        else break;
    }
    return candidate;
}

void PanelInstanceRegistry::registerPanel(const QString& panelType, int panelId, QWidget* widget)
{
    if (!widget || panelId <= 0) return;

    for (const auto& e : instances_)
    {
        if (e.panelType == panelType && e.panelId == panelId)
            return;
    }

    InstanceEntry entry;
    entry.panelType = panelType;
    entry.panelId = panelId;
    entry.widget = widget;
    instances_.append(entry);

    emit panelRegistered(panelType, panelId, widget);
}

void PanelInstanceRegistry::unregisterPanel(const QString& panelType, int panelId)
{
    for (int i = 0; i < instances_.size(); ++i)
    {
        if (instances_[i].panelType == panelType && instances_[i].panelId == panelId)
        {
            instances_.removeAt(i);
            if (lastFocused_.value(panelType, kNoFocus) == panelId)
            {
                int fallback = kNoFocus;
                for (const auto& e : instances_)
                {
                    if (e.panelType == panelType && e.widget)
                    {
                        fallback = e.panelId;
                        break;
                    }
                }
                lastFocused_.insert(panelType, fallback);
                emit lastFocusedChanged(panelType, fallback);
            }
            emit panelUnregistered(panelType, panelId);
            return;
        }
    }
}

QWidget* PanelInstanceRegistry::findPanel(const QString& panelType, int panelId) const
{
    for (const auto& e : instances_)
    {
        if (e.panelType == panelType && e.panelId == panelId)
            return e.widget.data();
    }
    return nullptr;
}

QList<QWidget*> PanelInstanceRegistry::allPanels(const QString& panelType) const
{
    QList<QWidget*> out;
    for (const auto& e : instances_)
    {
        if (e.panelType == panelType && e.widget)
            out.append(e.widget.data());
    }
    return out;
}

QList<QPair<int, QWidget*>> PanelInstanceRegistry::instances(const QString& panelType) const
{
    QList<QPair<int, QWidget*>> out;
    for (const auto& e : instances_)
    {
        if (e.panelType == panelType && e.widget)
            out.append(qMakePair(e.panelId, e.widget.data()));
    }
    return out;
}

int PanelInstanceRegistry::lastFocusedPanelId(const QString& panelType) const
{
    return lastFocused_.value(panelType, kNoFocus);
}

void PanelInstanceRegistry::setLastFocusedPanelId(const QString& panelType, int panelId)
{
    if (lastFocused_.value(panelType, kNoFocus) == panelId) return;
    lastFocused_.insert(panelType, panelId);
    emit lastFocusedChanged(panelType, panelId);
}

}
