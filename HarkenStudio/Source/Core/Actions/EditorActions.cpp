
#include "../ActionManager.h"
#include "../ProjectManager.h"
#include "../NameValidator.h"
#include "ActionSelectors.h"
#include "../IPC/IPCServer.h"
#include "../AudioEngine.h"
#include "../AudioEngine.h"
#include "../UndoCommands.h"
#include "../ActionStepUtils.h"
#include "ActionSelectors.h"
#include "../Components/VAction.h"
#include "../Components/VBus.h"
#include "../../Widgets/Blueprint/NodeItem.h"
#include "../../Widgets/Blueprint/BlueprintCanvasWidget.h"
#include "../../Widgets/Blueprint/SignalChainSource.h"
#include "../../MainWindow.h"
#include "../ProjectTypes.h"

#include <QApplication>
#include <QJsonArray>
#include <QMetaObject>
#include <QWidget>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <cstring>

namespace VNX {

static BlueprintCanvasWidget* findBlueprintCanvas()
{
    for (QWidget* tlw : QApplication::topLevelWidgets()) {
        if (auto* mw = qobject_cast<MainWindow*>(tlw)) {
            return mw->blueprintCanvas();
        }
    }
    return nullptr;
}

ProjectItem* findItemByPathInProject(ProjectData* project, const QString& path);
ProjectItem* findItemByObjectIDInProject(ProjectData* project, uint32_t objectID);
uint32_t itemShortID(const ProjectItem* item);
ProjectItem* resolveItemByPathOrObjectID(ProjectData* project,
                                         const QString& path,
                                         uint32_t objectID);
ProjectFileBinding* findBindingByFolder(ProjectData* project, const QString& folder);


void registerEditorActions(ActionManager& mgr,
                           ProjectManager* projectManager,
                           AudioEngine* engine,
                           BlueprintCanvasWidget* blueprintCanvas,
                           IPCServer* ipcServer)
{

    {
        ActionDef def;
        def.id = "connect_nodes";
        def.displayName = "Connect Nodes";
        def.category = "editor";
        def.paramSchema = QJsonObject{{"output_node", "int"}, {"input_node", "int"}, {"output_node_kind", "string"}, {"input_node_kind", "string"}, {"edge_type", "string"}};
        def.undoable = false;
        def.handler = [blueprintCanvas](const QJsonObject& args) -> ActionResult {
            int outputNode = args["output_node"].toInt(-1);
            int inputNode = args["input_node"].toInt(-1);
            if (outputNode < 0 || inputNode < 0)
                return {false, {}, "INVALID_ARGS", "output_node and input_node are required"};

            if (auto* bp = findBlueprintCanvas()) {
                auto* project = ProjectManager::instance().currentProject();
                if (project) {
                    QString outputKind = args.value("output_node_kind").toString();
                    QString inputKind  = args.value("input_node_kind").toString();
                    bool fromIsBlueprint = false, toIsBlueprint = false;
                    auto resolves = [&](uint32_t id, const QString& kind) -> bool {
                        if (id == 0) return false;
                        const QString k = kind.toLower();
                        auto walkItems = [&]() -> bool {
                            std::function<bool(ProjectItem*)> walk = [&](ProjectItem* n) -> bool {
                                if (!n) return false;
                                if (n->objectID == id) return true;
                                for (auto const& c : n->children) if (walk(c.get())) return true;
                                return false;
                            };
                            for (const auto* wu : project->allFileBindings())
                                if (wu && wu->rootItem && walk(wu->rootItem.get())) return true;
                            return false;
                        };
                        auto walkBuses = [&]() -> bool {
                            return ProjectManager::instance().findBusItem(id) != nullptr;
                        };
                        if (k == QStringLiteral("bus"))  return walkBuses();
                        if (k == QStringLiteral("item")) return walkItems();
                        return walkItems() || walkBuses();
                    };
                    fromIsBlueprint = resolves(static_cast<uint32_t>(outputNode), outputKind);
                    toIsBlueprint   = resolves(static_cast<uint32_t>(inputNode),  inputKind);

                    if (fromIsBlueprint && toIsBlueprint) {
                        QString edgeType = args.value("edge_type").toString();
                        auto r = bp->connectNodes(static_cast<uint32_t>(outputNode),
                                                  static_cast<uint32_t>(inputNode),
                                                  edgeType, outputKind, inputKind);
                        if (!r.success) {
                            return {false, {}, "BLUEPRINT_CONNECT_FAILED", r.error};
                        }
                        return {true, QJsonObject{
                            {"success", true},
                            {"output_node", outputNode},
                            {"input_node", inputNode},
                            {"effective_type", r.effectiveType}
                        }};
                    }
                }
            }

            if (!blueprintCanvas)
                return {false, {}, "BLUEPRINT_CANVAS_NOT_AVAILABLE", "Blueprint is not available"};
            return {false, {}, "BLUEPRINT_CONNECT_FAILED",
                    QString("No Blueprint object resolved for %1 -> %2")
                        .arg(outputNode).arg(inputNode)};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "disconnect_nodes";
        def.displayName = "Disconnect Nodes";
        def.category = "editor";
        def.paramSchema = QJsonObject{{"output_node", "int"}, {"input_node", "int"}, {"output_node_kind", "string"}, {"input_node_kind", "string"}, {"edge_type", "string"}};
        def.undoable = false;
        def.handler = [blueprintCanvas](const QJsonObject& args) -> ActionResult {
            int outputNode = args["output_node"].toInt(-1);
            int inputNode = args["input_node"].toInt(-1);
            if (outputNode < 0 || inputNode < 0)
                return {false, {}, "INVALID_ARGS", "output_node and input_node are required"};

            if (auto* bp = findBlueprintCanvas()) {
                auto* project = ProjectManager::instance().currentProject();
                if (project) {
                    QString outputKind = args.value("output_node_kind").toString();
                    QString inputKind  = args.value("input_node_kind").toString();
                    auto resolves = [&](uint32_t id, const QString& kind) -> bool {
                        if (id == 0) return false;
                        const QString k = kind.toLower();
                        auto walkItems = [&]() -> bool {
                            std::function<bool(ProjectItem*)> walk = [&](ProjectItem* n) -> bool {
                                if (!n) return false;
                                if (n->objectID == id) return true;
                                for (auto const& c : n->children) if (walk(c.get())) return true;
                                return false;
                            };
                            for (const auto* wu : project->allFileBindings())
                                if (wu && wu->rootItem && walk(wu->rootItem.get())) return true;
                            return false;
                        };
                        auto walkBuses = [&]() -> bool {
                            return ProjectManager::instance().findBusItem(id) != nullptr;
                        };
                        if (k == QStringLiteral("bus"))  return walkBuses();
                        if (k == QStringLiteral("item")) return walkItems();
                        return walkItems() || walkBuses();
                    };
                    if (resolves(static_cast<uint32_t>(outputNode), outputKind) &&
                        resolves(static_cast<uint32_t>(inputNode),  inputKind)) {
                        QString edgeType = args.value("edge_type").toString();
                        if (!bp->disconnectNodes(static_cast<uint32_t>(outputNode),
                                                  static_cast<uint32_t>(inputNode),
                                                  edgeType, outputKind, inputKind)) {
                            return {false, {}, "BLUEPRINT_DISCONNECT_FAILED",
                                    QString("No Blueprint edge for %1 -> %2").arg(outputNode).arg(inputNode)};
                        }
                        return {true, QJsonObject{
                            {"success", true},
                            {"output_node", outputNode},
                            {"input_node", inputNode}
                        }};
                    }
                }
            }

            if (!blueprintCanvas)
                return {false, {}, "BLUEPRINT_CANVAS_NOT_AVAILABLE", "Blueprint is not available"};
            return {false, {}, "BLUEPRINT_DISCONNECT_FAILED",
                    QString("No Blueprint object resolved for %1 -> %2")
                        .arg(outputNode).arg(inputNode)};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_node_override";
        def.displayName = "Set Node Override";
        def.category = "editor";
        def.paramSchema = QJsonObject{{"node_id", "int"}, {"override", "bool"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            int nodeId = args["node_id"].toInt(-1);
            if (nodeId < 0)
                return {false, {}, "INVALID_ARGS", "node_id is required"};
            if (!args.contains("override"))
                return {false, {}, "INVALID_ARGS", "override (bool) is required"};
            const bool ov = args["override"].toBool();

            auto* bp = findBlueprintCanvas();
            if (!bp)
                return {false, {}, "BLUEPRINT_NOT_AVAILABLE", "Blueprint not available"};
            if (!bp->setNodeOverride(static_cast<uint32_t>(nodeId), ov))
                return {false, {}, "BLUEPRINT_SET_OVERRIDE_FAILED",
                        QString("setNodeOverride failed for node %1").arg(nodeId)};
            return {true, QJsonObject{
                {"success", true}, {"node_id", nodeId}, {"override", ov}
            }};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_canvas_flow";
        def.displayName = "Set Blueprint Flow Direction";
        def.category = "editor";
        def.paramSchema = QJsonObject{{"direction", "string"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            const QString d = args.value(QStringLiteral("direction")).toString().toUpper();
            CanvasFlowDirection dir;
            if      (d == QLatin1String("LR")) dir = CanvasFlowDirection::LR;
            else if (d == QLatin1String("RL")) dir = CanvasFlowDirection::RL;
            else if (d == QLatin1String("TB")) dir = CanvasFlowDirection::TB;
            else if (d == QLatin1String("BT")) dir = CanvasFlowDirection::BT;
            else {
                return {false, {}, "INVALID_ARGS",
                        "direction must be one of LR/RL/TB/BT"};
            }
            auto* bp = findBlueprintCanvas();
            if (!bp)
                return {false, {}, "BLUEPRINT_NOT_AVAILABLE",
                        "Blueprint canvas is not available"};
            bp->setFlowDirection(dir);
            return {true, QJsonObject{
                {"success", true},
                {"direction", d},
            }};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "get_canvas_flow";
        def.displayName = "Get Blueprint Flow Direction";
        def.category = "editor";
        def.undoable = false;
        def.handler = [](const QJsonObject& ) -> ActionResult {
            auto* bp = findBlueprintCanvas();
            if (!bp)
                return {false, {}, "BLUEPRINT_NOT_AVAILABLE",
                        "Blueprint canvas is not available"};
            const char* dStr = "LR";
            switch (bp->flowDirection()) {
            case CanvasFlowDirection::LR: dStr = "LR"; break;
            case CanvasFlowDirection::RL: dStr = "RL"; break;
            case CanvasFlowDirection::TB: dStr = "TB"; break;
            case CanvasFlowDirection::BT: dStr = "BT"; break;
            }
            return {true, QJsonObject{
                {"success",   true},
                {"direction", QString::fromLatin1(dStr)},
            }};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "create_action";
        def.displayName = "Create Action";
        def.category = "editor";
        def.paramSchema = QJsonObject{{"name", "string"}, {"parent", "string"}};
        def.undoable = false;
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            if (!projectManager || !projectManager->isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};

            QString name = args["name"].toString();
            if (name.isEmpty())
                return {false, {}, "INVALID_ARGS", "Action name is required"};

            ProjectData* project = projectManager->currentProject();
            if (!project) return {false, {}, "NO_PROJECT_OPEN", "Project closed during command"};

            // Surface the specific validation reason (e.g. ASCII-only) so an AI
            // caller can correct the name; createItem otherwise reports it only
            // via errorOccurred, which never reaches the caller.
            NameValidator::Result vr;
            if (!NameValidator::validateItemName(name, ProjectItem::Type::Action, &vr))
                return {false, {}, "INVALID_NAME", NameValidator::resultMessage(vr)};

            ProjectItem* parent = nullptr;
            if (!args["parent"].toString().isEmpty()) {
                const auto selected = resolveProjectSelector(project, args["parent"].toString());
                if (!selected.ok)
                    return {false, {{"candidates", selected.candidates}}, selected.errorCode, selected.errorMessage};
                parent = selected.item;
            }
            ProjectItem* actionItem = projectManager->createItem(parent, name, ProjectItem::Type::Action);
            if (!actionItem)
                return {false, {}, "CREATE_FAILED", "Failed to create Action item"};

            return {true, QJsonObject{
                {"success", true},
                {"name", actionItem->name},
                {"guid", actionItem->guid.toString(QUuid::WithoutBraces)},
                {"object_id", static_cast<qint64>(itemShortID(actionItem))},
                {"authoring_handle", static_cast<qint64>(actionItem->objectID)}
            }};
        };
        mgr.registerAction(def);
    }
}

}
