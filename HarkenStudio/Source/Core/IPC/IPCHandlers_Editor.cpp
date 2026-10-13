#include "IPCServerInternal.h"
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

QJsonObject deprecateCommand(QJsonObject response, const char* replacement);

void IPCServer::registerEditorHandlers()
{
    commandHandlers_["get_blueprint"] = [this](const QJsonObject& args) { return handleGetNodeGraph(args); };
    commandHandlers_["get_node_graph"] = [this](const QJsonObject& args) {
        return deprecateCommand(handleGetNodeGraph(args), "get_blueprint");
    };
    commandHandlers_["get_signal_chain"] = [this](const QJsonObject& args) { return handleGetSignalChain(args); };
    commandHandlers_["set_blueprint_layer"] = [this](const QJsonObject& args) { return handleSetBlueprintLayer(args); };
    commandHandlers_["set_nodegraph_layer"] = [this](const QJsonObject& args) {
        return deprecateCommand(handleSetBlueprintLayer(args), "set_blueprint_layer");
    };
    commandHandlers_["get_blueprint_layers"] = [this](const QJsonObject& args) { return handleGetBlueprintLayers(args); };
    commandHandlers_["get_nodegraph_layers"] = [this](const QJsonObject& args) {
        return deprecateCommand(handleGetBlueprintLayers(args), "get_blueprint_layers");
    };
    commandHandlers_["get_referencing_actions"] = [this](const QJsonObject& args) { return handleGetReferencingActions(args); };


    commandHandlers_["get_action_definition"] = [this](const QJsonObject& args) { return handleGetActionDefinition(args); };
    commandHandlers_["list_actions"] = [this](const QJsonObject& args) { return handleListActions(args); };

    commandHandlers_["undo"] = [this](const QJsonObject& args) { return handleUndo(args); };
    commandHandlers_["redo"] = [this](const QJsonObject& args) { return handleRedo(args); };
    commandHandlers_["can_undo"] = [this](const QJsonObject& args) { return handleCanUndo(args); };
    commandHandlers_["can_redo"] = [this](const QJsonObject& args) { return handleCanRedo(args); };
    commandHandlers_["preview_actions"] = [](const QJsonObject& args) { return ActionManager::instance().preview(args); };
    commandHandlers_["confirm_actions"] = [](const QJsonObject& args) { return ActionManager::instance().confirm(args); };
    commandHandlers_["apply_actions"] = [](const QJsonObject& args) { return ActionManager::instance().apply(args); };
    commandHandlers_["rollback_actions"] = [](const QJsonObject& args) { return ActionManager::instance().rollback(args); };
    commandHandlers_["get_action_tx"] = [](const QJsonObject& args) { return ActionManager::instance().getTransaction(args); };

    commandHandlers_["query"] = [this](const QJsonObject& args) { return handleQuery(args); };
    commandHandlers_["batch"] = [this](const QJsonObject& args) { return handleBatch(args); };

    commandHandlers_["observe"] = [this](const QJsonObject& args) -> QJsonObject {
        if (!vqlEngine_)
            return errorResponse("NO_ENGINE", "VQL engine not available");
        QString vql = args["vql"].toString();
        if (vql.isEmpty())
            return errorResponse("MISSING_PARAM", "Missing 'vql' parameter");
        uint32_t id = vqlEngine_->observe(vql, [](const VQLResultSet&) {});
        if (id == 0)
            return errorResponse("COMPILE_ERROR", "Failed to compile VQL query");
        QJsonObject result;
        result["observer_id"] = static_cast<int>(id);
        return successResponse(result);
    };
    commandHandlers_["unobserve"] = [this](const QJsonObject& args) -> QJsonObject {
        if (!vqlEngine_)
            return errorResponse("NO_ENGINE", "VQL engine not available");
        uint32_t id = static_cast<uint32_t>(args["observer_id"].toInt());
        vqlEngine_->unobserve(id);
        return successResponse();
    };

    commandHandlers_["search_fulltext"] = [this](const QJsonObject& args) -> QJsonObject {
        QString query = args["query"].toString();
        if (query.isEmpty())
            return errorResponse("INVALID_ARGS", "query is required");
        int maxResults = args["max_results"].toInt(100);

        auto* project = ProjectManager::instance().currentProject();
        if (!project)
            return errorResponse("NO_PROJECT", "No project open");

        if (!vqlEngine_)
            return errorResponse("NO_ENGINE", "VQL engine not available");

        vqlEngine_->setProject(project);

#ifdef HARKEN_HAS_LMDB
        auto* svc = vqlEngine_->lmdbService();
        if (!svc || !svc->isOpen())
            return successResponse(QJsonObject{{"items", QJsonArray()}, {"count", 0}, {"query", query}});

        LMDBQueryPipeline pipeline(svc);
        pipeline.setQuery(query);
        pipeline.setMaxResults(maxResults);
        while (!pipeline.isFinished()) {
            auto status = pipeline.step(256);
            if (status == PipelineStatus::Error)
                return errorResponse("SEARCH_ERROR", "Pipeline error at stage: " + pipeline.stageName());
        }

        QJsonArray items;
        for (const auto& r : pipeline.results()) {
            QJsonObject obj;
            obj["objectID"] = static_cast<qint64>(r.objectID);
            obj["name"] = r.name;
            obj["type"] = r.type;
            obj["score"] = static_cast<double>(r.score);
            items.append(obj);
        }

        return successResponse(QJsonObject{
            {"items", items},
            {"count", items.size()},
            {"query", query}
        });
#else
        return successResponse(QJsonObject{{"items", QJsonArray()}, {"count", 0}, {"query", query}});
#endif
    };

    commandHandlers_["search_status"] = [this](const QJsonObject&) -> QJsonObject {
        if (!vqlEngine_)
            return errorResponse("NO_ENGINE", "VQL engine not available");
        bool searching = vqlEngine_->isSearching();
        return successResponse(QJsonObject{
            {"searching", searching}
        });
    };

    commandHandlers_["run_search_benchmark"] = [this](const QJsonObject& args) -> QJsonObject {
        Q_UNUSED(this);
        int count = args["object_count"].toInt(1000);
        if (count < 1) count = 1;
        if (count > 500000) count = 500000;

        auto benchResult = LMDBBenchmark::runSynthetic(count);
        return successResponse(benchResult.toJson());
    };

    commandHandlers_["list_open_dialogs"] = [](const QJsonObject&) -> QJsonObject {
        auto& dm = DialogManager::instance();
        QJsonArray arr;
        for (const QString& id : dm.openDialogIds()) {
            QDialog* dlg = dm.getDialog(id);
            if (!dlg) continue;
            QJsonObject entry;
            entry["id"] = id;
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
    };

    commandHandlers_["dismiss_dialog"] = [](const QJsonObject& args) -> QJsonObject {
        QString id = args["id"].toString();
        if (id.isEmpty()) {
            QJsonObject err;
            err["status"] = "error";
            err["code"] = "MISSING_PARAM";
            err["message"] = "id is required";
            return err;
        }
        int resultCode = args["result"].toInt(QDialog::Rejected);
        bool dismissed = DialogManager::instance().dismissDialog(id, resultCode);
        if (!dismissed) {
            QJsonObject err;
            err["status"] = "error";
            err["code"] = "DIALOG_NOT_FOUND";
            err["message"] = QString("No open dialog with id '%1'").arg(id);
            return err;
        }
        QJsonObject result;
        result["status"] = "ok";
        result["result"] = QJsonObject{{"id", id}, {"dismissed", true}};
        return result;
    };
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
            Q_UNUSED(bp);
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
                        obj["x"]     = c->canvasPos.x();
                        obj["y"]     = c->canvasPos.y();
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





QJsonObject IPCServer::handleGetActionDefinition(const QJsonObject& args)
{

    if (!projectManager_ || !projectManager_->isProjectOpen())
    {
        return errorResponse("NO_PROJECT_OPEN",
                             "No project is currently open");
    }

    QString name = args["name"].toString();
    if (name.isEmpty())
    {
        return errorResponse("INVALID_ARGS",
                             "Action name is required",
                             "Provide a 'name' parameter with the action path or name.");
    }

    ProjectData* project = projectManager_->currentProject();
    if (!project) return errorResponse("NO_PROJECT_OPEN", "Project closed during command");

    ProjectItem* actionItem = findItemByPathInProject(project, name);
    if (!actionItem ||
        actionItem->type != ProjectItem::Type::Action)
    {
        return successResponse(QJsonObject{
            {"name", name},
            {"found", false},
            {"actions", QJsonArray{}}
        });
    }

    QJsonArray actionsArray;
    auto* actionComponent = actionItem->comp<VAction>();
    if (actionComponent)
    {
        for (const auto& action : actionComponent->steps)
        {
            QJsonObject actionObj;
            actionObj["type"] = action.typeString();
            actionObj["target"] = action.targetName;
            actionObj["target_object_id"] = static_cast<qint64>(action.targetObjectID);
            actionObj["delay"] = static_cast<double>(action.delay);
            actionObj["fade_time"] = static_cast<double>(action.fadeTime);
            actionsArray.append(actionObj);
        }
    }

    return successResponse(QJsonObject{
        {"name", actionItem->name},
        {"found", true},
        {"actions", actionsArray},
        {"object_id", static_cast<qint64>(itemShortID(actionItem))},
        {"authoring_handle", static_cast<qint64>(actionItem->objectID)}
    });
}


QJsonObject IPCServer::handleListActions(const QJsonObject& args)
{
    Q_UNUSED(args);
    
    if (!projectManager_ || !projectManager_->isProjectOpen())
    {
        return errorResponse("NO_PROJECT_OPEN", 
                             "No project is currently open");
    }
    
    ProjectData* project = projectManager_->currentProject();
    if (!project) return errorResponse("NO_PROJECT_OPEN", "Project closed during command");
    
    QJsonArray actions;
    std::function<void(const ProjectItem*)> collect = [&](const ProjectItem* item) {
        if (!item) return;
        if (item->type == ProjectItem::Type::Action)
            actions.append(item->name);
        for (const auto& child : item->children)
            collect(child.get());
    };
    for (const auto* binding : project->allFileBindings()) {
        if (binding && binding->rootItem) collect(binding->rootItem.get());
    }

    return successResponse(QJsonObject{{"actions", actions}});
}





QJsonObject IPCServer::handleUndo(const QJsonObject& args)
{
    Q_UNUSED(args);
    
    UndoRedoManager& undoManager = UndoRedoManager::instance();
    
    if (!undoManager.undo())
    {
        return errorResponse("CANNOT_UNDO", 
                             undoManager.undoStack()->canUndo()
                                 ? "Undo is blocked by active work or a changed project"
                                 : "Nothing to undo");
    }
    
    
    return successResponse(QJsonObject{
        {"success", true}
    });
}


QJsonObject IPCServer::handleRedo(const QJsonObject& args)
{
    Q_UNUSED(args);
    
    UndoRedoManager& undoManager = UndoRedoManager::instance();
    
    if (!undoManager.redo())
    {
        return errorResponse("CANNOT_REDO", 
                             undoManager.undoStack()->canRedo()
                                 ? "Redo is blocked by active work or a changed project"
                                 : "Nothing to redo");
    }
    
    
    return successResponse(QJsonObject{
        {"success", true}
    });
}


QJsonObject IPCServer::handleCanUndo(const QJsonObject& args)
{
    Q_UNUSED(args);
    
    UndoRedoManager& undoManager = UndoRedoManager::instance();
    
    QJsonObject result;
    result["can_undo"] = undoManager.canUndo();
    result["text"] = undoManager.undoStack()->undoText();
    
    return successResponse(result);
}


QJsonObject IPCServer::handleCanRedo(const QJsonObject& args)
{
    Q_UNUSED(args);
    
    UndoRedoManager& undoManager = UndoRedoManager::instance();
    
    QJsonObject result;
    result["can_redo"] = undoManager.canRedo();
    result["text"] = undoManager.undoStack()->redoText();
    
    return successResponse(result);
}



QJsonObject IPCServer::handleQuery(const QJsonObject& args)
{
    QString vql = args["vql"].toString();
    if (vql.isEmpty())
        return errorResponse("INVALID_QUERY", "Missing 'vql' parameter",
                             "Provide a VQL query string, e.g. $ type:Sound where volume < -6");

    auto* project = ProjectManager::instance().currentProject();
    if (!project)
        return errorResponse("NO_PROJECT", "No project is loaded");

    vqlEngine_->setProject(project);

    std::vector<QString> returnFields;
    QJsonArray retArr = args["return"].toArray();
    for (const auto& v : retArr)
        returnFields.push_back(v.toString());

    VQLError error;
    QJsonArray results = vqlEngine_->queryJson(vql, returnFields, &error);

    if (error.isError())
        return errorResponse("QUERY_ERROR", error.toString());

    QJsonObject result;
    result["items"]  = results;
    result["count"]  = results.size();
    result["vql"]    = vql;
    return successResponse(result);
}



QJsonObject IPCServer::handleBatch(const QJsonObject& args)
{
    QJsonArray commands = args["commands"].toArray();
    if (commands.isEmpty())
        return errorResponse("INVALID_BATCH", "Missing or empty 'commands' array");

    bool atomic = args["atomic"].toBool(false);
    QJsonArray results;
    int succeeded = 0, failed = 0;

    for (const auto& cmdVal : commands) {
        QJsonObject cmdObj = cmdVal.toObject();
        QString cmd = cmdObj["cmd"].toString();
        QJsonObject cmdArgs = cmdObj["args"].toObject();

        if (cmd.isEmpty()) {
            results.append(QJsonObject{{"status", "error"}, {"message", "Missing 'cmd' in batch entry"}});
            ++failed;
            continue;
        }

        QJsonObject response = handleCommand(cmd, cmdArgs);
        results.append(response);

        if (response["status"].toString() == "ok")
            ++succeeded;
        else {
            ++failed;
            if (atomic) break;
        }
    }

    QJsonObject result;
    result["results"]   = results;
    result["succeeded"] = succeeded;
    result["failed"]    = failed;
    result["total"]     = commands.size();
    return successResponse(result);
}



QJsonObject serializeWidget(QWidget* w, int depth, int maxDepth)
{
    QJsonObject obj;
    obj["name"] = w->objectName();
    obj["type"] = QString::fromLatin1(w->metaObject()->className());
    obj["visible"] = w->isVisible();
    obj["enabled"] = w->isEnabled();

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
            if (!child->objectName().isEmpty() || depth < 2)
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


QJsonObject IPCServer::handleGetSignalChain(const QJsonObject& args)
{
    QJsonObject result;
    auto* project = ProjectManager::instance().currentProject();
    if (!project) {
        result["status"] = "error";
        result["code"] = "HARKEN_ERR_NO_PROJECT";
        result["message"] = "No project is open";
        return result;
    }

    QString path = args["path"].toString();
    uint32_t objectID = static_cast<uint32_t>(args["object_id"].toInteger());
    ProjectItem* item = resolveItemByPathOrObjectID(project, path, objectID);
    if (!item) {
        result["status"] = "error";
        result["code"] = "HARKEN_ERR_NOT_FOUND";
        result["message"] = "Item not found";
        return result;
    }

    auto view = ProjectManager::buildSignalChainView(project, item);

    result["status"] = "ok";
    result["result"] = view.toJson();
    return result;
}

QJsonObject deprecateCommand(QJsonObject response, const char* replacement)
{
    if (response["status"].toString() == QLatin1String("ok")) {
        response["deprecated"] = true;
        response["use_instead"] = QLatin1String(replacement);
    }
    return response;
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

    QMetaObject::invokeMethod(chain, [chain, layer, visible]() {
        chain->setLayerVisible(layer, visible);
    }, Qt::QueuedConnection);

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

QJsonObject IPCServer::handleGetReferencingActions(const QJsonObject& args)
{
    auto* project = ProjectManager::instance().currentProject();
    if (!project) {
        return errorResponse("HARKEN_ERR_NO_PROJECT", "No project is open");
    }

    uint32_t objectID = static_cast<uint32_t>(args["object_id"].toInteger());
    if (objectID == 0) {
        QString path = args["path"].toString();
        if (!path.isEmpty()) {
            ProjectItem* item = resolveItemByPathOrObjectID(project, path, 0);
            if (item) objectID = item->objectID;
        }
    }

    if (objectID == 0) {
        return errorResponse("HARKEN_ERR_MISSING_PARAM",
                             "object_id or path required",
                             "Provide object_id (uint32) or path (string)");
    }

    auto refs = ProjectManager::findReferencingActions(project, objectID);

    QJsonArray actions;
    for (const auto& ref : refs) {
        QJsonObject action;
        action["actionName"] = ref.actionName;
        action["actionObjectID"] = static_cast<qint64>(ref.actionObjectID);
        action["actionType"] = ref.actionType;
        actions.append(action);
    }

    QJsonObject result;
    result["status"] = "ok";
    result["result"] = QJsonObject{
        {"objectID", static_cast<qint64>(objectID)},
        {"count", static_cast<int>(refs.size())},
        {"actions", actions}
    };
    return result;
}


}
