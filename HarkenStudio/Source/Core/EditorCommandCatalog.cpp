#include "EditorCommandCatalog.h"
#include "ActionManager.h"
#include "AI/AICommandCatalog.h"
#include "IPC/IPCServer.h"

#include <QJsonArray>

namespace VNX {
namespace {

QJsonObject inputSchema(const QJsonObject& parameters)
{
    if (parameters.value(QStringLiteral("type")) == QStringLiteral("object"))
        return parameters;
    QJsonObject properties;
    for (auto it = parameters.begin(); it != parameters.end(); ++it) {
        QJsonObject field;
        if (it.value().isObject()) {
            field = it.value().toObject();
        } else {
            const QString type = it.value().toString();
            if (type == QStringLiteral("string") || type == QStringLiteral("object")
                || type == QStringLiteral("array") || type == QStringLiteral("number")
                || type == QStringLiteral("boolean") || type == QStringLiteral("integer"))
                field.insert(QStringLiteral("type"), type);
            else if (type == QStringLiteral("bool"))
                field.insert(QStringLiteral("type"), QStringLiteral("boolean"));
            else if (type == QStringLiteral("float") || type == QStringLiteral("double"))
                field.insert(QStringLiteral("type"), QStringLiteral("number"));
            else if (type.startsWith(QStringLiteral("int")) || type.startsWith(QStringLiteral("uint"))) {
                field.insert(QStringLiteral("type"), QStringLiteral("integer"));
                if (type.startsWith(QStringLiteral("uint")))
                    field.insert(QStringLiteral("minimum"), 0);
            }
            field.insert(QStringLiteral("description"), type);
        }
        properties.insert(it.key(), field);
    }
    return {{QStringLiteral("type"), QStringLiteral("object")},
            {QStringLiteral("properties"), properties}};
}

}

QJsonObject describeEditorCommand(const QString& name, const IPCServer* dispatcher)
{
    const auto* action = ActionManager::instance().findAction(name);
    const auto* spec = AICommandCatalog::find(name);
    const bool handler = dispatcher && dispatcher->hasCommandHandler(name);
    QJsonObject result{
        {QStringLiteral("command"), name},
        {QStringLiteral("registered"), action != nullptr || handler},
        {QStringLiteral("readOnly"), dispatcher && dispatcher->isReadOnlyCommand(name)},
        {QStringLiteral("requiresDesktop"), dispatcher && dispatcher->commandRequiresDesktop(name)}
    };
    QJsonObject parameters;
    if (spec && (!spec->argumentSchema.isEmpty() || !action || action->paramSchema.isEmpty())) {
        parameters = spec->argumentSchema;
        result.insert(QStringLiteral("source"), QStringLiteral("catalog"));
        result.insert(QStringLiteral("description"), spec->description);
        result.insert(QStringLiteral("contractVersion"), spec->contractVersion);
    } else if (action) {
        parameters = action->paramSchema;
        result.insert(QStringLiteral("source"), QStringLiteral("action"));
        result.insert(QStringLiteral("description"), action->displayName);
    } else {
        result.insert(QStringLiteral("source"), handler ? QStringLiteral("handler") : QStringLiteral("unknown"));
    }
    if (action) {
        result.insert(QStringLiteral("displayName"), action->displayName);
        result.insert(QStringLiteral("category"), action->category);
        result.insert(QStringLiteral("undoable"), action->undoable);
    }
    result.insert(QStringLiteral("parameters"), parameters);
    result.insert(QStringLiteral("inputSchema"), inputSchema(parameters));
    const bool declared = parameters.value(QStringLiteral("type")) == QStringLiteral("object");
    result.insert(QStringLiteral("schemaSource"), declared ? QStringLiteral("declared")
        : parameters.isEmpty() ? QStringLiteral("undocumented") : QStringLiteral("legacy"));
    if (!declared)
        result.insert(QStringLiteral("note"), QStringLiteral(
            "Legacy parameter descriptions may omit requirements and constraints. "
            "An undocumented schema does not mean the command takes no arguments. "
            "The editor validates execution and returns errors for invalid input."));
    return result;
}

}
