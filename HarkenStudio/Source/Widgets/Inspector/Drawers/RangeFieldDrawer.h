
#pragma once

#include "Widgets/Inspector/InspectorCustomizationRegistry.h"

namespace VNX {

class RangeFieldDrawer : public IFieldDrawer
{
public:
    QWidget* build(const PropertyDescriptor& d, QWidget* parent) override;
    void bind(QWidget* widget, const QVariant& value, ProjectItem* item) override;
    void connectChanged(QWidget* widget, std::function<void(const QVariant&)> onChange) override;
    void applyDomain(QWidget* widget, const FieldDomain& domain) override;
};

}
