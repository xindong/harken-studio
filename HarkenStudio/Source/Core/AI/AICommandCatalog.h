#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include "AIPermissionModel.h"

namespace VNX {

struct AICommandSpec
{
    enum class Effect { Query, EditorState, ProjectState, RuntimeState, External };
    enum class Availability { Enabled, Disabled, InternalOnly, TestOnly, Retired };
    using Confirmation = AIConfirmationPolicy;
    enum class ChangeSupport { None, Preview, ChangeSet };
    enum class Source { ActionManager, CommandHandler };

    QString cmd;
    int contractVersion = 1;
    Effect effect = Effect::Query;
    Availability availability = Availability::Disabled;
    Confirmation confirmation = Confirmation::None;
    ChangeSupport changeSupport = ChangeSupport::None;
    Source source = Source::CommandHandler;
    int toolPriority = 0;
    QJsonObject argumentSchema;
    QString description;
};

class AICommandCatalog
{
public:
    static const QList<AICommandSpec>& all();
    static const AICommandSpec* find(const QString& cmd);
    static QList<AICommandSpec> enabled();
    static QList<AICommandSpec> selectTools(const QString& permission = QStringLiteral("ask"));
    static QList<AICommandSpec> selectTools(AIPermissionMode permission);
};

}
