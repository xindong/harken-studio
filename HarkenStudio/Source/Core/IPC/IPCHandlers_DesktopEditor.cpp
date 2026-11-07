#include "IPCDesktopInternal.h"
#include "DesktopWidgetTargets.h"
#include "ActionManager.h"
#include "DialogManager.h"
#include "../Components/VAudio.h"
#include "../Components/VMixNode.h"
#include "../../Query/LMDBService.h"
#include "../../Query/LMDBQueryPipeline.h"
#include "../../Query/LMDBBenchmark.h"
#include "../../Widgets/Blueprint/BlueprintEdgeKind.h"
#include "../../MainWindow.h"
#include "../../Widgets/Blueprint/BlueprintCanvasWidget.h"
#include "../../Widgets/Blueprint/SignalChainSource.h"
#include "../../Widgets/Blueprint/NodeGraphLayer.h"
#include <QElapsedTimer>
#include <QDateTime>
#include <QPlainTextEdit>
#include <algorithm>

namespace VNX {

namespace Bridge = HarkenEditor::Bridge;

void IPCServer::registerDesktopEditorHandlers()
{
    registerDesktopHandler("get_blueprint", [this](const QJsonObject& args) { return handleGetNodeGraph(args); });
    registerDesktopHandler("set_blueprint_layer", [this](const QJsonObject& args) { return handleSetBlueprintLayer(args); });
    registerDesktopHandler("get_blueprint_layers", [this](const QJsonObject& args) { return handleGetBlueprintLayers(args); });
    registerDesktopHandler("list_open_dialogs", [](const QJsonObject&) -> QJsonObject {
        auto& dm = DialogManager::instance();
        QJsonArray arr;
        for (const QString& id : dm.openDialogIds()) {
            QDialog* dlg = dm.getDialog(id);
            if (!dlg) continue;
            QJsonObject entry;
            entry["id"] = id;
            entry["widget_id"] = desktopWidgetId(dlg);
            entry["title"] = dlg->windowTitle();
            entry["modal"] = dlg->isModal();
            entry["visible"] = dlg->isVisible();
            arr.append(entry);
        }
        QJsonObject result;
        result["status"] = "ok";
        result["result"] = QJsonObject{
            {"dialogs", arr},
            {"count", arr.size()}
        };
        return result;
    });
    registerDesktopHandler("dismiss_dialog", [](const QJsonObject& args) -> QJsonObject {
        QString id = args["id"].toString();
        if (id.isEmpty()) {
            QJsonObject err;
            err["status"] = "error";
            err["code"] = "MISSING_PARAM";
            err["message"] = "id is required";
            return err;
        }
        int resultCode = args["result"].toInt(QDialog::Rejected);
        const QPointer<QDialog> dialog = DialogManager::instance().getDialog(id);
        bool dismissed = DialogManager::instance().dismissDialog(id, resultCode);
        if (!dismissed) {
            QJsonObject err;
            err["status"] = "error";
            err["code"] = "DIALOG_NOT_FOUND";
            err["message"] = QString("No open dialog with id '%1'").arg(id);
            return err;
        }
        if (dialog && dialog->isVisible())
            return QJsonObject{{"status", "error"}, {"code", "DIALOG_NOT_CLOSED"},
                {"message", "The dialog did not accept the requested result."}};
        QJsonObject result;
        result["status"] = "ok";
        result["result"] = QJsonObject{{"id", id}, {"dismissed", true}};
        return result;
    });
}

QJsonObject IPCServer::handleGetNodeGraph(const QJsonObject& args)
{
    Q_UNUSED(args);

    if (projectManager_ && !projectManager_->isProjectOpen())
    {
        return errorResponse("NO_PROJECT_OPEN",
                             "No project is currently open");
    }

    if (mainWindow_)
    {
        if (auto* bp = mainWindow_->blueprintCanvas())
        {
            auto* project = projectManager_ ? projectManager_->currentProject() : nullptr;
            if (project)
            {
                QJsonArray bpNodes;
                std::function<void(ProjectItem*)> walkNode = [&](ProjectItem* node) {
                    if (!node) return;
                    for (auto const& child : node->children) {
                        ProjectItem* c = child.get();
                        if (!c) continue;
                        QJsonObject obj;
                        obj["id"]    = static_cast<qint64>(c->objectID);
                        obj["title"] = c->name;
                        obj["kind"]  = QStringLiteral("project_item");
                        if (const auto position = bp->nodePosition(c->objectID)) {
                            obj["x"] = position->x();
                            obj["y"] = position->y();
                        }
                        const auto* mc = c->comp<VMixNode>();
                        obj["output_bus_id"]    = static_cast<qint64>(mc ? mc->outputBusId : 0);
                        obj["output_bus_override"] = c->isOutputBusOverride();
                        bpNodes.append(obj);
                        walkNode(c);
                    }
                };
                for (const auto* binding : project->allFileBindings()) {
                    if (!binding || !binding->rootItem) continue;
                    walkNode(binding->rootItem.get());
                }
                for (auto* busItem : ProjectManager::instance().collectBusItems()) {
                    if (!busItem) continue;
                    const auto* bc = busItem->comp<VBus>();
                    if (!bc) continue;
                    QJsonObject obj;
                    obj["id"]    = static_cast<qint64>(busItem->objectID);
                    obj["title"] = busItem->name;
                    obj["kind"]  = ProjectManager::instance().busHasIncomingSends(busItem->objectID)
                                       ? QStringLiteral("receive_bus") : QStringLiteral("bus");
                    obj["parent_bus_id"] = static_cast<qint64>(busItem->parent ? busItem->parent->objectID : busItem->objectID);
                    bpNodes.append(obj);
                }

                QJsonArray bpConns;
                auto edges = deriveAllEdges(*project);
                int connId = 0;
                for (auto const& e : edges) {
                    QJsonObject obj;
                    obj["id"]          = connId++;
                    obj["source_node"] = static_cast<qint64>(e.fromItemId);
                    obj["target_node"] = static_cast<qint64>(e.toItemId);
                    QString type;
                    switch (e.kind) {
                        case BlueprintEdgeKind::Containment: type = QStringLiteral("containment"); break;
                        case BlueprintEdgeKind::BusRoute:    type = QStringLiteral("bus_route");   break;
                        case BlueprintEdgeKind::Override:    type = QStringLiteral("override");    break;
                        case BlueprintEdgeKind::Send:        type = QStringLiteral("send");        break;
                        case BlueprintEdgeKind::Placement:   type = QStringLiteral("placement");   break;
                        case BlueprintEdgeKind::Organization:type = QStringLiteral("organization");break;
                        case BlueprintEdgeKind::Through:     type = QStringLiteral("through");     break;
                    }
                    obj["type"] = type;
                    if (e.kind == BlueprintEdgeKind::Send) {
                        obj["send_index"] = static_cast<int>(e.sendIndex);
                        obj["mode"] = static_cast<int>(e.sendMode);
                    }
                    bpConns.append(obj);
                }

                return successResponse(QJsonObject{
                    {"mode", "blueprint"},
                    {"nodes", bpNodes},
                    {"connections", bpConns}
                });
            }
        }
    }

    return errorResponse("NO_ACTIVE_GRAPH",
                         "Blueprint canvas is not available; no node graph to expose.");
}

QJsonObject serializeWidget(QWidget* w, int depth, int maxDepth)
{
    QJsonObject obj;
    obj["widget_id"] = desktopWidgetId(w);
    obj["name"] = w->objectName();
    obj["type"] = QString::fromLatin1(w->metaObject()->className());
    obj["visible"] = w->isVisible();
    obj["enabled"] = w->isEnabled();
    if (w->isWindow()) {
        obj["title"] = w->windowTitle();
        obj["modal"] = w->isModal();
    }

    if (auto* label = qobject_cast<QLabel*>(w))
        obj["text"] = label->text();
    if (auto* btn = qobject_cast<QAbstractButton*>(w)) {
        obj["text"] = btn->text();
        obj["checked"] = btn->isChecked();
    }
    if (auto* tab = qobject_cast<QTabWidget*>(w)) {
        obj["currentIndex"] = tab->currentIndex();
        if (tab->count() > 0)
            obj["currentTab"] = tab->tabText(tab->currentIndex());
        obj["tabCount"] = tab->count();
    }
    if (auto* stack = qobject_cast<QStackedWidget*>(w)) {
        obj["currentIndex"] = stack->currentIndex();
        obj["pageCount"] = stack->count();
    }
    if (auto* edit = qobject_cast<QLineEdit*>(w))
        obj["text"] = edit->text();
    if (auto* plainEdit = qobject_cast<QPlainTextEdit*>(w))
        obj["text"] = plainEdit->toPlainText();
    if (auto* combo = qobject_cast<QComboBox*>(w)) {
        obj["currentText"] = combo->currentText();
        obj["currentIndex"] = combo->currentIndex();
        QJsonArray items;
        for (int i = 0; i < combo->count(); ++i)
            items.append(combo->itemText(i));
        obj["items"] = items;
        obj["count"] = combo->count();
    }
    if (auto* spin = qobject_cast<QSpinBox*>(w)) {
        obj["value"] = spin->value();
        obj["minimum"] = spin->minimum();
        obj["maximum"] = spin->maximum();
    }
    if (auto* dspin = qobject_cast<QDoubleSpinBox*>(w)) {
        obj["value"] = dspin->value();
        obj["minimum"] = dspin->minimum();
        obj["maximum"] = dspin->maximum();
        obj["decimals"] = dspin->decimals();
        if (!dspin->suffix().isEmpty())
            obj["suffix"] = dspin->suffix();
    }
    if (auto* slider = qobject_cast<QSlider*>(w)) {
        obj["value"] = slider->value();
        obj["minimum"] = slider->minimum();
        obj["maximum"] = slider->maximum();
    }
    if (auto* paramSlider = qobject_cast<ParameterSlider*>(w)) {
        obj["value"] = paramSlider->value();
        obj["minimum"] = paramSlider->minimum();
        obj["maximum"] = paramSlider->maximum();
        if (!paramSlider->suffix().isEmpty())
            obj["suffix"] = paramSlider->suffix();
    }
    if (auto* toggle = qobject_cast<ToggleSwitch*>(w)) {
        obj["checked"] = toggle->isChecked();
        if (auto* lbl = toggle->findChild<QLabel*>())
            obj["text"] = lbl->text();
    }
    if (auto* table = qobject_cast<QTableWidget*>(w)) {
        obj["rowCount"] = table->rowCount();
        obj["columnCount"] = table->columnCount();
        obj["currentRow"] = table->currentRow();
        obj["currentColumn"] = table->currentColumn();
        QJsonArray headers;
        for (int c = 0; c < table->columnCount(); ++c) {
            auto* item = table->horizontalHeaderItem(c);
            headers.append(item ? item->text() : QString::number(c));
        }
        obj["headers"] = headers;
        int previewRows = qMin(table->rowCount(), 20);
        QJsonArray rows;
        for (int r = 0; r < previewRows; ++r) {
            QJsonArray row;
            for (int c = 0; c < table->columnCount(); ++c) {
                auto* item = table->item(r, c);
                row.append(item ? item->text() : QString());
            }
            rows.append(row);
        }
        obj["rows"] = rows;
        if (table->rowCount() > 20)
            obj["truncated"] = true;
    }
    if (auto* list = qobject_cast<QListWidget*>(w)) {
        obj["count"] = list->count();
        obj["currentRow"] = list->currentRow();
        int previewCount = qMin(list->count(), 20);
        QJsonArray items;
        for (int i = 0; i < previewCount; ++i) {
            auto* item = list->item(i);
            items.append(item ? item->text() : QString());
        }
        obj["items"] = items;
        if (list->count() > 20)
            obj["truncated"] = true;
    }
    if (auto* tree = qobject_cast<QTreeWidget*>(w)) {
        obj["topLevelItemCount"] = tree->topLevelItemCount();
        obj["columnCount"] = tree->columnCount();
        int previewCount = qMin(tree->topLevelItemCount(), 20);
        QJsonArray topItems;
        for (int i = 0; i < previewCount; ++i) {
            auto* item = tree->topLevelItem(i);
            QJsonObject entry;
            entry["text"] = item->text(0);
            entry["childCount"] = item->childCount();
            entry["expanded"] = item->isExpanded();
            topItems.append(entry);
        }
        obj["topItems"] = topItems;
        if (tree->topLevelItemCount() > 20)
            obj["truncated"] = true;
    }
    if (auto* progress = qobject_cast<QProgressBar*>(w))
        obj["value"] = progress->value();
    if (auto* adsDock = qobject_cast<ads::CDockWidget*>(w))
        obj["floating"] = adsDock->isFloating();

    obj["x"] = w->x();
    obj["y"] = w->y();
    obj["width"] = w->width();
    obj["height"] = w->height();

    if (depth < maxDepth) {
        QJsonArray children;
        for (auto* child : w->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
            children.append(serializeWidget(child, depth + 1, maxDepth));
        }
        if (!children.isEmpty())
            obj["children"] = children;
    }
    return obj;
}

QString widgetPath(QWidget* w)
{
    QStringList parts;
    for (QWidget* p = w; p != nullptr; p = p->parentWidget()) {
        QString name = p->objectName();
        if (name.isEmpty())
            name = QString::fromLatin1(p->metaObject()->className());
        parts.prepend(name);
    }
    return parts.join('/');
}

QString widgetText(QWidget* w)
{
    if (auto* label = qobject_cast<QLabel*>(w))
        return label->text();
    if (auto* btn = qobject_cast<QAbstractButton*>(w))
        return btn->text();
    if (auto* edit = qobject_cast<QLineEdit*>(w))
        return edit->text();
    if (auto* combo = qobject_cast<QComboBox*>(w))
        return combo->currentText();
    return {};
}

QJsonObject IPCServer::handleSetBlueprintLayer(const QJsonObject& args)
{
    auto* bp = mainWindow_ ? mainWindow_->blueprintCanvas() : nullptr;
    auto* chain = bp ? bp->activeSignalChainSource() : nullptr;
    if (!chain) {
        return errorResponse("HARKEN_ERR_NO_CHAIN",
                             "No active signal-chain to toggle layer on.");
    }

    QString layerName = args["layer"].toString().toLower();
    bool visible = args["visible"].toBool(true);

    NodeGraphLayer layer;
    if (layerName == "children") {
        layer = NodeGraphLayer::Children;
    } else if (layerName == "parents") {
        layer = NodeGraphLayer::Parents;
    } else if (layerName == "bus") {
        layer = NodeGraphLayer::Bus;
    } else {
        return errorResponse("HARKEN_ERR_INVALID_ARG",
                             "Invalid layer name. Use: children, parents, bus",
                             "Valid layers: 'children', 'parents', 'bus'");
    }

    chain->setLayerVisible(layer, visible);

    return successResponse(QJsonObject{
        {"layer", layerName},
        {"visible", visible}
    });
}

QJsonObject IPCServer::handleGetBlueprintLayers(const QJsonObject& args)
{
    Q_UNUSED(args);
    auto* bp = mainWindow_ ? mainWindow_->blueprintCanvas() : nullptr;
    auto* chain = bp ? bp->activeSignalChainSource() : nullptr;
    if (!chain) {
        return errorResponse("HARKEN_ERR_NO_CHAIN",
                             "No active signal-chain to query layers on.");
    }

    return successResponse(QJsonObject{
        {"children", chain->isLayerVisible(NodeGraphLayer::Children)},
        {"parents",  chain->isLayerVisible(NodeGraphLayer::Parents)},
        {"bus",      chain->isLayerVisible(NodeGraphLayer::Bus)}
    });
}

}
