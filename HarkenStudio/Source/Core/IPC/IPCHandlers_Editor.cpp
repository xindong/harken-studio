#include "IPCServerInternal.h"
#include "ActionManager.h"
#include "../Components/VAudio.h"
#include "../Components/VMixNode.h"
#include "../../Query/LMDBService.h"
#include "../../Query/LMDBQueryPipeline.h"
#include "../../Query/LMDBBenchmark.h"
#include <QElapsedTimer>
#include <QDateTime>
#include <algorithm>

namespace VNX {

QJsonObject deprecateCommand(QJsonObject response, const char* replacement);

void IPCServer::registerEditorHandlers()
{
    registerDesktopHandler("get_blueprint");
    registerDesktopHandler("get_node_graph");
    commandHandlers_["get_signal_chain"] = [this](const QJsonObject& args) { return handleGetSignalChain(args); };
    registerDesktopHandler("set_blueprint_layer");
    registerDesktopHandler("set_nodegraph_layer");
    registerDesktopHandler("get_blueprint_layers");
    registerDesktopHandler("get_nodegraph_layers");
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
            return errorResponse("SEARCH_UNAVAILABLE", "The project search index could not be initialized or rebuilt");

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
        return errorResponse("SEARCH_UNAVAILABLE", "This build does not include the full-text search backend");
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
        if (!benchResult.complete) {
            auto response = errorResponse("SEARCH_BENCHMARK_FAILED", "The search benchmark could not complete");
            response.insert("result", benchResult.toJson());
            return response;
        }
        return successResponse(benchResult.toJson());
    };

    registerDesktopHandler("list_open_dialogs");

    registerDesktopHandler("dismiss_dialog");
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
