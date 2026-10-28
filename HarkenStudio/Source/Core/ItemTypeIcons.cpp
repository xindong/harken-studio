
#include "ItemTypeIcons.h"

#include "ItemTypeCatalog.h"
#include "Components/VSelection.h"
#include "Components/VVariable.h"
#include "ThemeAssetCatalog.h"
#include "ThemeManager.h"

namespace VNX {

QIcon iconForItemType(ProjectItem::Type type)
{
    return ThemeManager::instance()
        .resolveIcon(QString::fromLatin1(
            ItemTypeCatalog::iconSpecOf(type).logicalId))
        .icon;
}

QIcon iconForItemType(ProjectItem::Type type, const QColor& tint)
{
    return ThemeManager::instance()
        .resolveIcon(QString::fromLatin1(
            ItemTypeCatalog::iconSpecOf(type).logicalId),
            ThemeAssetVariant{QSize(), 1.0, 0, tint})
        .icon;
}

namespace {

const char* instanceIconLogicalId(const ProjectItem& item)
{
    if (item.type == ProjectItem::Type::Container) {
        const auto* selection = item.comp<VSelection>();
        if (!selection) return nullptr;
        switch (selection->mode) {
        case SelectionMode::Random:
            return selection->isQueue() ? "projectItem.container.randomQueue" : "projectItem.container.random";
        case SelectionMode::Sequence:
            return selection->isQueue() ? "projectItem.container.sequenceQueue" : "projectItem.container.sequence";
        case SelectionMode::Match: return "projectItem.container.match";
        case SelectionMode::Blend: return "projectItem.container.blend";
        }
    }
    if (item.type == ProjectItem::Type::Variable) {
        const auto* variable = item.comp<VVariable>();
        if (!variable) return nullptr;
        return variable->kind == VariableKind::Step
            ? "projectItem.variable.step"
            : "projectItem.variable.smooth";
    }
    return nullptr;
}

QIcon resolveItemIcon(const char* logicalId, const QColor& tint = {})
{
    ThemeAssetVariant variant;
    variant.tint = tint;
    return ThemeManager::instance()
        .resolveIcon(QString::fromLatin1(logicalId), variant)
        .icon;
}

}

const char* iconLogicalIdForItem(const ProjectItem& item)
{
    const char* logicalId = instanceIconLogicalId(item);
    return logicalId ? logicalId : ItemTypeCatalog::iconSpecOf(item.type).logicalId;
}

QIcon iconForItem(const ProjectItem& item)
{
    return resolveItemIcon(iconLogicalIdForItem(item));
}

QIcon iconForItem(const ProjectItem& item, const QColor& tint)
{
    return resolveItemIcon(iconLogicalIdForItem(item), tint);
}

}
