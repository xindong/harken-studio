#include "DesktopWidgetTargets.h"
#include "PackGenerationRecord.h"

#include <QApplication>
#include <QHash>
#include <QJsonArray>
#include <QPointer>
#include <QUuid>
#include <QWidget>
#include <algorithm>

namespace VNX {
namespace {

QJsonObject failure(const QString& code, const QString& message)
{
    return {{"status", "error"}, {"code", code}, {"message", message}};
}

struct DesktopTargets {
    QString session = QUuid::createUuid().toString(QUuid::WithoutBraces);
    quint64 nextWidget = 0;
    quint64 nextOperation = 0;
    QHash<QWidget*, QString> widgetIds;
    QHash<QString, QPointer<QWidget>> widgets;
    QHash<QString, QJsonObject> operations;
    QList<QString> completed;
};

DesktopTargets& targets()
{
    static DesktopTargets value;
    return value;
}

void finishOperation(const QString& id, const QJsonObject& error)
{
    auto& state = targets();
    auto& result = state.operations[id];
    result["state"] = error.isEmpty() ? "succeeded" : "failed";
    if (!error.isEmpty()) result["error"] = error;
    state.completed.append(id);
    while (state.completed.size() > 256)
        state.operations.remove(state.completed.takeFirst());
}

}

QString desktopWidgetId(QWidget* widget)
{
    if (!widget) return {};
    auto& state = targets();
    const auto existing = state.widgetIds.constFind(widget);
    if (existing != state.widgetIds.cend()) return *existing;
    const QString id = state.session + ":w:" + QString::number(++state.nextWidget);
    state.widgetIds.insert(widget, id);
    state.widgets.insert(id, widget);
    QObject::connect(widget, &QObject::destroyed, qApp, [widget, id] {
        targets().widgetIds.remove(widget);
        targets().widgets.remove(id);
    });
    return id;
}

QList<QWidget*> desktopWidgets()
{
    auto widgets = QApplication::allWidgets();
    for (auto* widget : widgets) desktopWidgetId(widget);
    std::sort(widgets.begin(), widgets.end(), [](QWidget* a, QWidget* b) {
        return desktopWidgetId(a) < desktopWidgetId(b);
    });
    return widgets;
}

QJsonObject desktopWidgetInteractionError(QWidget* widget)
{
    if (!widget) return failure("STALE_WIDGET", "The widget no longer exists. Discover the target again.");
    if (!widget->isVisible()) return failure("WIDGET_NOT_VISIBLE", "The target widget is hidden.");
    if (!widget->isEnabled()) return failure("WIDGET_DISABLED", "The target widget is disabled.");
    if (auto* modal = QApplication::activeModalWidget()) {
        if (modal != widget && !modal->isAncestorOf(widget))
            return failure("WIDGET_BLOCKED_BY_DIALOG", "Close or answer the active modal dialog before interacting with this widget.");
    }
    return {};
}

QWidget* resolveDesktopWidget(const QJsonObject& args, QJsonObject* error,
                              bool interaction, QWidget* defaultTarget)
{
    *error = {};
    const auto fail = [&](const QString& code, const QString& message) -> QWidget* {
        *error = failure(code, message);
        return nullptr;
    };
    const QString id = args.value("widget_id").toString();
    const QString name = args.value("name").toString();
    if (args.contains("widget_id") && args.contains("name"))
        return fail("INVALID_ARGS", "Use exactly one of widget_id or name to identify a widget.");
    QWidget* scope = nullptr;
    if (args.contains("scope_widget_id")) {
        scope = targets().widgets.value(args.value("scope_widget_id").toString());
        if (!scope) return fail("STALE_WIDGET", "scope_widget_id is unknown or its widget no longer exists.");
    }
    QWidget* selected = nullptr;
    if (args.contains("widget_id")) {
        selected = targets().widgets.value(id);
        if (!selected) return fail("STALE_WIDGET", "widget_id is unknown or its widget no longer exists. Discover the target again.");
        if (scope && scope != selected && !scope->isAncestorOf(selected))
            return fail("WIDGET_OUTSIDE_SCOPE", "The target widget is outside scope_widget_id.");
    } else if (!name.isEmpty()) {
        QJsonArray candidates;
        for (auto* widget : desktopWidgets()) {
            if (widget->objectName() != name) continue;
            if (scope && scope != widget && !scope->isAncestorOf(widget)) continue;
            selected = widget;
            candidates.append(QJsonObject{{"widget_id", desktopWidgetId(widget)},
                {"name", widget->objectName()}, {"type", widget->metaObject()->className()},
                {"visible", widget->isVisible()}, {"enabled", widget->isEnabled()}});
        }
        if (candidates.isEmpty()) return fail("WIDGET_NOT_FOUND", QString("No widget is named '%1'. Use find_widget.").arg(name));
        if (candidates.size() > 1) {
            *error = failure("AMBIGUOUS_WIDGET", "Several widgets match name. Use a widget_id from candidates.");
            error->insert("result", QJsonObject{{"candidates", candidates}});
            return nullptr;
        }
    } else {
        selected = defaultTarget;
        if (!selected) return fail("INVALID_ARGS", "Provide widget_id or a unique widget name.");
        if (scope && scope != selected && !scope->isAncestorOf(selected))
            return fail("WIDGET_OUTSIDE_SCOPE", "The default target widget is outside scope_widget_id.");
    }
    if (interaction) {
        *error = desktopWidgetInteractionError(selected);
        if (!error->isEmpty()) return nullptr;
    }
    return selected;
}

QJsonObject queueDesktopWidgetOperation(QWidget* widget,
    std::function<QJsonObject(QWidget*)> operation, QJsonObject details,
    std::function<QJsonObject()> canExecute)
{
    const auto error = desktopWidgetInteractionError(widget);
    if (!error.isEmpty()) return error;
    auto& state = targets();
    if (state.operations.size() - state.completed.size() >= 256)
        return failure("UI_QUEUE_FULL", "Wait for pending UI operations to finish before queuing more.");
    const QString id = state.session + ":ui:" + QString::number(++state.nextOperation);
    state.operations.insert(id, QJsonObject{{"operation_id", id}, {"state", "queued"},
        {"widget_id", desktopWidgetId(widget)}, {"completion", "ui_operation_returned"}});
    const QPointer<QWidget> safe = widget;
    QMetaObject::invokeMethod(qApp, [safe, id, operation = std::move(operation), canExecute = std::move(canExecute)] {
        if (canExecute) {
            const auto guardError = canExecute();
            if (!guardError.isEmpty()) {
                finishOperation(id, guardError);
                return;
            }
        }
        if (PackGenerationRecordStore::instance().isExporting()) {
            finishOperation(id, failure("BUSY", "An export started before the queued UI operation could run."));
            return;
        }
        const auto error = desktopWidgetInteractionError(safe);
        if (!error.isEmpty()) {
            finishOperation(id, error);
            return;
        }
        targets().operations[id].insert("state", "running");
        finishOperation(id, operation(safe));
    }, Qt::QueuedConnection);
    details["queued"] = true;
    details["widget_id"] = desktopWidgetId(widget);
    details["operation_id"] = id;
    details["operation"] = QJsonObject{{"status_command", "get_ui_operation"},
        {"status_args", QJsonObject{{"operation_id", id}}}};
    return {{"status", "ok"}, {"result", details}};
}

QJsonObject desktopUiOperation(const QString& operationId)
{
    const auto found = targets().operations.constFind(operationId);
    if (found == targets().operations.cend())
        return failure("UI_OPERATION_NOT_FOUND", "Unknown or expired operation_id. The latest 256 completed UI operations are retained for this session.");
    return {{"status", "ok"}, {"result", *found}};
}

}
