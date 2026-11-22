#pragma once

#include "Widgets/Inspector/InspectorCustomizationRegistry.h"

namespace harken::studio {

class SourcesSectionProvider final : public ISectionProvider
{
public:
    QWidget* build(QWidget* parent) override;
    QWidget* buildHeader(QWidget* widget, QWidget* parent) override;
    void bind(QWidget* widget, ProjectItem* item) override;
};

void registerSourcesSectionProvider();

}
