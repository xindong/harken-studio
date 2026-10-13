
#include "ObjectRefFieldDrawer.h"

#include "Selector/ObjectRefWidget.h"
#include "Core/Components/VBus.h"
#include "Core/ProjectTypes.h"

#include <QMetaType>

namespace VNX {

QWidget* ObjectRefFieldDrawer::build(const PropertyDescriptor& d, QWidget* parent)
{
    auto* w = new ObjectRefWidget(parent);
    w->setFilterType(static_cast<ProjectItem::Type>(d.refFilter));
    w->setSelectorTitle(QObject::tr("Select %1").arg(d.label.isEmpty() ? d.field : d.label));
    w->setObjectName(QStringLiteral("prop_") + d.field);
    w->setRequired(d.refFilter != static_cast<int>(ProjectItem::Type::Attenuation)
        && d.field != QLatin1String("outputBusId")
        && d.field != QLatin1String("mainOutputBusRef"));
    w->setClearSupport(d.refFilter == static_cast<int>(ProjectItem::Type::Attenuation)
        || d.field == QLatin1String("mainOutputBusRef"));
    if (d.flags.readOnly)
        w->setReadOnly(true);
    if (d.refSelectable)
        w->setIsSelectable([w, pred = d.refSelectable](uint32_t candidateID) {
            const uint32_t host = w->property("hostObjectID").toUInt();
            return host != 0 && pred(host, candidateID);
        });
    return w;
}

void ObjectRefFieldDrawer::bind(QWidget* widget, const QVariant& value, ProjectItem* item)
{
    auto* w = qobject_cast<ObjectRefWidget*>(widget);
    if (!w) return;

    w->setProperty("hostObjectID", item ? item->objectID : 0);

    const bool busHost = item && item->comp<VBus>();
    const bool busRef = w->filterType() == ProjectItem::Type::Bus;
    w->setInheritSupport(false);
    w->setInherited(false);
    w->setSelectorEnabled(!busHost || !busRef);
    if (!value.isValid())
        w->setMixedValue();
    else if (value.metaType().id() == QMetaType::QString
        && value.toString() != QLatin1String("0"))
        w->setUnresolvedReference(value.toString());
    else
        w->setCurrentObjectID(value.toUInt());
}

void ObjectRefFieldDrawer::connectChanged(QWidget* widget, std::function<void(const QVariant&)> onChange)
{
    if (auto* w = qobject_cast<ObjectRefWidget*>(widget))
    {
        QObject::connect(w, &ObjectRefWidget::objectChanged, w,
            [onChange = std::move(onChange)](uint32_t newID) {
                onChange(QVariant(static_cast<uint>(newID)));
            });
    }
}

}
