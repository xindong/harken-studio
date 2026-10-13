
#pragma once

#include "Component.h"
#include "ProjectTypes.h"

#include <QPoint>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <cstdint>
#include <optional>

class QWidget;

namespace VNX {

enum class ContextTopicKind : uint8_t
{
    None,
    Folder,
    Object,
    Field,
    Panel
};

struct ContextTopic
{
    ContextTopicKind kind = ContextTopicKind::None;

    QUuid objectGuid;
    ProjectItem::Type objectType = ProjectItem::Type::None;
    QString objectName;
    QString folderPath;

    std::optional<ComponentType> ownerComponent;
    QString field;
    QString fieldLabel;

    QString panelKey;
    QStringList helpArguments;

    bool isNone() const { return kind == ContextTopicKind::None; }

    static ContextTopic forObject(const QUuid& guid, ProjectItem::Type type, const QString& name);
    static ContextTopic forFolder(const QString& name, const QString& path);
    static ContextTopic forField(std::optional<ComponentType> ownerComponent,
                                 const QString& field,
                                 const QString& fieldLabel,
                                 const QUuid& ownerGuid,
                                 ProjectItem::Type ownerType,
                                 const QString& ownerName);
    static ContextTopic forPanel(const QString& key, const QStringList& helpArguments = {});

    bool operator==(const ContextTopic&) const = default;
};

class IContextInfoSource
{
public:
    virtual ~IContextInfoSource() = default;
    virtual ContextTopic contextTopicAt(const QPoint& pos) const = 0;
};

namespace ContextInfo {

void tag(QWidget* widget, const ContextTopic& topic);
void clearTag(QWidget* widget);
std::optional<ContextTopic> tagOf(const QWidget* widget);

}

}

Q_DECLARE_METATYPE(VNX::ContextTopic)
