
#include "ObjectReferencePicker.h"

#include "Core/ActionManager.h"
#include "Core/ItemTypeCatalog.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <QHBoxLayout>
#include <QJsonObject>

namespace VNX {

ObjectReferencePicker::ObjectReferencePicker(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("objectReferencePicker"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    refWidget_ = new ObjectRefWidget(this);
    refWidget_->setFilterType(ProjectItem::Type::Sound);
    layout->addWidget(refWidget_, 1);

    connect(refWidget_, &ObjectRefWidget::objectChanged,
            this, &ObjectReferencePicker::onObjectPicked);
}

ObjectReferencePicker::~ObjectReferencePicker() = default;


void ObjectReferencePicker::setFilterType(ProjectItem::Type t)
{
    refWidget_->setFilterType(t);
}

ProjectItem::Type ObjectReferencePicker::filterType() const noexcept
{
    return refWidget_->filterType();
}

void ObjectReferencePicker::setCurrentObjectID(uint32_t objectID)
{
    refWidget_->setCurrentObjectID(objectID);
}

uint32_t ObjectReferencePicker::currentObjectID() const noexcept
{
    return refWidget_->currentObjectID();
}


void ObjectReferencePicker::bindProjectItem(ProjectItem* item)
{
    boundObjectID_ = (item ? item->objectID : 0u);
}


void ObjectReferencePicker::onObjectPicked(uint32_t id)
{
    emit objectChanged(id);

    if (actionFieldName_.isEmpty() || actionId_.isEmpty()) return;
    if (boundObjectID_ == 0) return;

    auto camelToSnake = [](const QString& camel) -> QString {
        QString out;
        out.reserve(camel.size() + 4);
        for (int i = 0; i < camel.size(); ++i) {
            const QChar c = camel[i];
            if (c.isUpper() && i > 0 && camel[i - 1].isLower())
                out.append(QChar('_'));
            out.append(c.toLower());
        }
        return out;
    };
    const QString argKey = camelToSnake(actionFieldName_);

    QJsonObject args;
    args.insert(QStringLiteral("object_id"), static_cast<qint64>(boundObjectID_));
    args.insert(argKey,                       static_cast<qint64>(id));

    ActionManager::instance().execute(
        actionId_, args, ActionSource::Internal);
}


ProjectItem::Type ObjectReferencePicker::parseTypeString(const QString& s) noexcept
{
    return ItemTypeCatalog::parseTypeName(s, Qt::CaseSensitive, false)
        .value_or(ProjectItem::Type::Sound);
}
}
