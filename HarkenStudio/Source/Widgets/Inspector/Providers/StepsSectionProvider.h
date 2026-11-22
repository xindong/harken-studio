
#pragma once

#include "Widgets/Inspector/InspectorCustomizationRegistry.h"

namespace harken::studio {

struct ProjectItem;

class StepsSectionProvider final : public ISectionProvider
{
public:
    QWidget* build(QWidget* parent) override;
    void bind(QWidget* widget, ProjectItem* item) override;
};

void registerStepsSectionProvider();

}
