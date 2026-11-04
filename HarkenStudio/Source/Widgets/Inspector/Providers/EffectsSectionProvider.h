#pragma once

#include "Widgets/Inspector/InspectorCustomizationRegistry.h"

namespace VNX {

class EffectsSectionProvider final : public ISectionProvider
{
public:
    QWidget* build(QWidget* parent) override;
    void bind(QWidget* widget, ProjectItem* item) override;
    bool refresh(QWidget* widget, ProjectItem* item, const QString& field) override;
};

void registerEffectsSectionProvider();

}
