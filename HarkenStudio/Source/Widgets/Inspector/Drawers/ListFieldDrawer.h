
#pragma once

#include "Widgets/Inspector/InspectorCustomizationRegistry.h"

namespace VNX {

class ListFieldDrawer : public IFieldDrawer
{
public:
    QWidget* build(const PropertyDescriptor& d, QWidget* parent) override;
    void bind(QWidget* widget, const QVariant& value, ProjectItem* item) override;
    void connectChanged(QWidget* widget, std::function<void(const QVariant&)> onChange) override;
    void applyDomain(QWidget* widget, const FieldDomain& domain) override;
    static void setWidgetReadOnly(QWidget* widget, bool readOnly);
    static void setWidgetEditGuard(QWidget* widget, std::function<bool()> guard);
    static void setWidgetPreviewCallbacks(QWidget* widget,
        std::function<void(const QVariant&)> preview, std::function<void()> finished);
    static void cancelWidgetEdits(QWidget* widget, bool preserveRows = false);
    static void setWidgetMixed(QWidget* widget, bool mixed);
};

}
