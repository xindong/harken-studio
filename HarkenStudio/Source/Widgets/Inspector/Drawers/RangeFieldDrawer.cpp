#include "RangeFieldDrawer.h"

#include "Widgets/Common/RangeField.h"

#include <QVariantList>

namespace VNX {

namespace {

RangeField* fieldOf(QWidget* widget)
{
    return qobject_cast<RangeField*>(widget);
}

}

QWidget* RangeFieldDrawer::build(const PropertyDescriptor& d, QWidget* parent)
{
    auto* field = new RangeField(parent);
    field->setObjectName(QStringLiteral("prop_") + d.field);
    return field;
}

void RangeFieldDrawer::bind(QWidget* widget, const QVariant& value, ProjectItem*)
{
    auto* field = fieldOf(widget);
    if (!field) return;
    const QVariantList bounds = value.toList();
    if (bounds.size() != 2) return;
    field->setValues(bounds.at(0).toDouble(), bounds.at(1).toDouble());
}

void RangeFieldDrawer::connectChanged(QWidget* widget,
                                      std::function<void(const QVariant&)> onChange)
{
    auto* field = fieldOf(widget);
    if (!field) return;
    QObject::connect(field, &RangeField::valuesCommitted, field,
        [onChange = std::move(onChange)](double low, double high) {
            onChange(QVariant(QVariantList{low, high}));
        });
}

void RangeFieldDrawer::applyDomain(QWidget* widget, const FieldDomain& domain)
{
    auto* field = fieldOf(widget);
    if (!field) return;
    field->setDomain(domain.min, domain.max, domain.decimals,
                     domain.quantum > 0.0 ? domain.quantum : 1.0);
}

}
