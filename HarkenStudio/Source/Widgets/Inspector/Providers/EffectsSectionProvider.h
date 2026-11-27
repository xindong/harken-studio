#pragma once

#include "Widgets/Inspector/InspectorCustomizationRegistry.h"

namespace harken::studio {

class EffectsSectionProvider final : public ISectionProvider
{
public:
    QWidget* build(QWidget* parent) override;
    void bind(QWidget* widget, ProjectItem* item) override;
    void bindGroups(QWidget* widget, ProjectItem* item, const InspectorGroupLookup& groups) override;
    void refreshGroups(QWidget* widget, ProjectItem* item, const InspectorGroupLookup& groups) override;
    bool refresh(QWidget* widget, ProjectItem* item, const QString& field) override;
};

void registerEffectsSectionProvider();

}
