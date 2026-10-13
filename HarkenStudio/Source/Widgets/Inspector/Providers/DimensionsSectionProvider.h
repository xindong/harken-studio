#pragma once

#include "Widgets/Inspector/InspectorCustomizationRegistry.h"

namespace VNX {

struct ProjectItem;

class DimensionsSectionProvider final : public ISectionProvider
{
public:
    QWidget* build(QWidget* parent) override;
    void bind(QWidget* widget, ProjectItem* item) override;
};

void registerDimensionsSectionProvider();

}
