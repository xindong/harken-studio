#include "ContextInfo.h"

#include <QMetaType>
#include <QVariant>
#include <QWidget>

namespace VNX {

constexpr char kTopicProperty[] = "harkenContextTopic";

ContextTopic ContextTopic::forObject(const QUuid& guid, ProjectItem::Type type, const QString& name)
{
    ContextTopic topic;
    topic.kind = ContextTopicKind::Object;
    topic.objectGuid = guid;
    topic.objectType = type;
    topic.objectName = name;
    return topic;
}

ContextTopic ContextTopic::forFolder(const QString& name, const QString& path)
{
    ContextTopic topic;
    topic.kind = ContextTopicKind::Folder;
    topic.objectName = name;
    topic.folderPath = path;
    return topic;
}

ContextTopic ContextTopic::forField(std::optional<ComponentType> ownerComponent,
                                    const QString& field,
                                    const QString& fieldLabel,
                                    const QUuid& ownerGuid,
                                    ProjectItem::Type ownerType,
                                    const QString& ownerName)
{
    ContextTopic topic;
    topic.kind = ContextTopicKind::Field;
    topic.objectGuid = ownerGuid;
    topic.objectType = ownerType;
    topic.objectName = ownerName;
    topic.ownerComponent = ownerComponent;
    topic.field = field;
    topic.fieldLabel = fieldLabel;
    return topic;
}

ContextTopic ContextTopic::forPanel(const QString& key, const QStringList& helpArguments)
{
    ContextTopic topic;
    topic.kind = ContextTopicKind::Panel;
    topic.panelKey = key;
    topic.helpArguments = helpArguments;
    return topic;
}

namespace ContextInfo {

void tag(QWidget* widget, const ContextTopic& topic)
{
    if (!widget)
        return;

    widget->setProperty(kTopicProperty, QVariant::fromValue(topic));
}

void clearTag(QWidget* widget)
{
    if (!widget)
        return;

    widget->setProperty(kTopicProperty, QVariant());
}

std::optional<ContextTopic> tagOf(const QWidget* widget)
{
    if (!widget)
        return std::nullopt;

    const QVariant variant = widget->property(kTopicProperty);
    if (!variant.isValid() || variant.metaType() != QMetaType::fromType<ContextTopic>())
        return std::nullopt;

    return variant.value<ContextTopic>();
}

}

}
