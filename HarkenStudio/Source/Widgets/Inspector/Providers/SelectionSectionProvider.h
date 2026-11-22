
#pragma once

#include "Widgets/Inspector/InspectorCustomizationRegistry.h"

namespace harken::studio {

struct ProjectItem;

class SelectionSectionProvider final : public ISectionProvider
{
public:
    QWidget* build(QWidget* parent) override;
    void bind(QWidget* widget, ProjectItem* item) override;
};

void registerSelectionSectionProvider();

}
