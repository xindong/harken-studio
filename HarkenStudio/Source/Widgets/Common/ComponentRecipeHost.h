#pragma once

#include "Core/ComponentRecipeRegistry.h"

#include <QMap>
#include <QStringList>
#include <QWidget>

namespace VNX {

class ThemeManager;
struct ComponentRecipe;

class ComponentRecipeHost : public QWidget
{
    Q_OBJECT

public:
    explicit ComponentRecipeHost(
        ComponentRecipeHostId host,
        QWidget* parent = nullptr,
        ThemeManager* themeManager = nullptr);
    ~ComponentRecipeHost() override;

    ComponentRecipeHostId hostId() const { return host_; }
    ComponentRecipeBreakpoint breakpoint() const { return breakpoint_; }
    ComponentRecipeVariant appliedVariant() const { return appliedVariant_; }
    QStringList orderedSlotNames() const;

    void setBreakpoint(ComponentRecipeBreakpoint breakpoint);
    bool registerSlot(ComponentRecipeSlotId slot, QWidget* widget);
    QWidget* slotWidget(ComponentRecipeSlotId slot) const;
    QString slotAsset(ComponentRecipeSlotId slot) const;
    void applyRecipe();

signals:
    void recipeApplied(VNX::ComponentRecipeVariant variant);

private:
    class SlotContainer;

    const ComponentRecipe* selectedRecipe() const;

    ComponentRecipeHostId host_;
    ComponentRecipeBreakpoint breakpoint_ =
        ComponentRecipeBreakpoint::Default;
    ComponentRecipeVariant appliedVariant_;
    ThemeManager* themeManager_;
    QMap<ComponentRecipeSlotId, SlotContainer*> slots_;
    QStringList orderedSlotNames_;
};

}
