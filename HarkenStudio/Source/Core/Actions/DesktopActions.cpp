#include "../DesktopCommandRegistration.h"
#include "../IPC/IPCServerInternal.h"
#include "../BuiltinLayouts.h"
#include "../CurveSelection.h"
#include "../AttenuationStore.h"
#include "ActionSelectors.h"
#include "../../MainWindow.h"
#include "../../Widgets/Blueprint/BlueprintCanvasWidget.h"
#include "../../Widgets/ProjectExplorer/ProjectExplorer.h"

#include <QApplication>
#include <QJsonArray>
#include <QMetaObject>
#include <QThread>

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



static ActionResult mainWindowNotAvailable()
{
    return {false, {}, "MAIN_WINDOW_NOT_AVAILABLE", "MainWindow is not available"};
}

void registerDesktopEditorCommands(const DesktopEditorCommandContext& context)
{
    EditorCommandContext sharedContext{
        context.engine, context.project, context.dispatcher, context.audition};
    if (context.explorer)
        sharedContext.selectFolder = [explorer = QPointer<ProjectExplorer>(context.explorer)](PhysicalFolder* folder) {
            return explorer && explorer->selectFolder(folder);
        };
    registerEditorCommands(sharedContext);
    auto& manager = ActionManager::instance();
    manager.setDesktopAvailable(true);
    const QPointer<MainWindow> mainWindow(context.window);
    auto* projectManager = context.project;
    const QPointer<ProjectExplorer> projectExplorer(context.explorer);
    const QPointer<BlueprintCanvasWidget> blueprintCanvas(context.blueprint);


    auto selectHandler = [projectManager, projectExplorer](const QJsonObject& args) -> ActionResult {
        if (!projectManager)
            return {false, {}, "PROJECT_MANAGER_NOT_AVAILABLE", "ProjectManager is not available"};
        if (!projectManager->isProjectOpen())
            return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};
        if (!projectExplorer)
            return {false, {}, "PROJECT_EXPLORER_NOT_AVAILABLE", "Explorer is not available"};

        QString path = args["path"].toString().trimmed();
        if (path.isEmpty())
            return {false, {}, "INVALID_ARGS", "path is required"};

        ProjectData* project = projectManager->currentProject();
        if (!project)
            return {false, {}, "NO_PROJECT_OPEN", "Project closed during command"};

        ProjectItem* item = findItemByPathInProject(project, path);
        if (!item)
            return {false, {}, "ITEM_NOT_FOUND",
                    QString("Item '%1' not found").arg(path)};

        Q_ASSERT(QThread::currentThread() == projectExplorer->thread());

        if (!projectExplorer->selectProjectItem(item))
        {
            return {false, {}, "ITEM_NOT_VISIBLE",
                    QString("Item '%1' exists but is not visible in Explorer").arg(path)};
        }

        ActionResult r;
        r.success = true;
        r.data = QJsonObject{
            {"path", path},
            {"name", item->name},
            {"type", itemTypeToString(item->type)},
            {"selected", true}
        };
        return r;
    };

    manager.setActionHandler("connect_nodes", [blueprintCanvas](const QJsonObject& args) -> ActionResult {
            qint64 outputNode = args["output_node"].toInteger(-1);
            qint64 inputNode = args["input_node"].toInteger(-1);
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
        });

    manager.setActionHandler("disconnect_nodes", [blueprintCanvas](const QJsonObject& args) -> ActionResult {
            qint64 outputNode = args["output_node"].toInteger(-1);
            qint64 inputNode = args["input_node"].toInteger(-1);
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
        });

    manager.setActionHandler("set_node_override", [](const QJsonObject& args) -> ActionResult {
            qint64 nodeId = args["node_id"].toInteger(-1);
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
        });

    manager.setActionHandler("set_canvas_flow", [](const QJsonObject& args) -> ActionResult {
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
        });

    manager.setActionHandler("get_canvas_flow", [](const QJsonObject& ) -> ActionResult {
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
        });

    manager.setActionHandler("select_bus", [mainWindow](const QJsonObject& args) -> ActionResult {
            if (!mainWindow) return mainWindowNotAvailable();
            auto& pm = ProjectManager::instance();
            if (!pm.isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};
            const auto idValue = args.value("bus_id");
            const qint64 id = idValue.toInteger(-1);
            if (!idValue.isDouble() || id < 0 || id > UINT32_MAX || idValue.toDouble() != id)
                return {false, {}, "INVALID_ARGS", "bus_id must be an unsigned 32-bit integer"};
            const auto busId = static_cast<uint32_t>(id);
            if (!pm.findVBus(busId))
                return {false, {}, "NOT_FOUND", QString("Bus ID %1 not found").arg(busId)};
            mainWindow->selectBus(busId, true);
            return {true, QJsonObject{{"bus_id", id}, {"selected", true}}};
        });

    manager.setActionHandler("select_project_item", selectHandler);

    manager.setActionHandler("select_item", selectHandler);

    manager.setActionHandler("open_remote_connect", [mainWindow](const QJsonObject&) -> ActionResult {
            if (!mainWindow) return mainWindowNotAvailable();
            mainWindow->showRemoteConnect();
            return {true, QJsonObject{{"opened", true}}};
        });

    manager.setActionHandler("switch_layout", [mainWindow](const QJsonObject& args) -> ActionResult {
            if (!mainWindow) return mainWindowNotAvailable();
            QString layoutName = args["layout"].toString();
            {
                auto sel = VNX::resolveSelector(layoutName);
                if (!sel.ok) return {false, {}, sel.errorCode, sel.errorMessage};
                layoutName = sel.name;
            }
            if (layoutName.isEmpty())
                return {false, {}, "INVALID_ARGS", "Layout name is required"};
            const auto* builtin = VNX::BuiltinLayouts::findByToken(layoutName);
            if (!builtin)
                return {false, {}, "INVALID_LAYOUT", QString("Unknown layout: %1").arg(layoutName)};
            bool success = mainWindow->switchToView(builtin->id);
            if (success)
                return {true, QJsonObject{{"layout", builtin->id}, {"switched", true}}};
            return {false, {}, "SWITCH_LAYOUT_FAILED",
                    QString("Failed to switch to layout: %1").arg(layoutName)};
        });

    manager.setActionHandler("reset_layout", [mainWindow](const QJsonObject& args) -> ActionResult {
            if (!mainWindow) return mainWindowNotAvailable();
            QString target = args["layout"].toString();
            {
                auto sel = VNX::resolveSelector(target);
                if (!sel.ok) return {false, {}, sel.errorCode, sel.errorMessage};
                target = sel.name;
            }
            const auto resolved = VNX::BuiltinLayouts::resolveResetTarget(
                target, mainWindow->currentView());
            if (resolved.kind == VNX::BuiltinLayouts::ResetTargetKind::Invalid)
                return {false, {}, "INVALID_LAYOUT",
                        QString("Unknown layout: %1").arg(resolved.canonical)};
            if (resolved.kind == VNX::BuiltinLayouts::ResetTargetKind::Builtin
                && resolved.canonical != mainWindow->currentView())
                mainWindow->switchToView(resolved.canonical);
            if (!mainWindow->resetCurrentLayout())
                return {false, {}, "RESET_FAILED",
                        QString("Failed to reset layout: %1").arg(resolved.canonical)};
            return {true, QJsonObject{{"layout", resolved.canonical}, {"reset", true}}};
        });

    manager.setActionHandler("toggle_panel", [mainWindow](const QJsonObject& args) -> ActionResult {
            if (!mainWindow) return mainWindowNotAvailable();
            QString panelName = args["name"].toString().trimmed();
            {
                auto sel = VNX::resolveSelector(panelName);
                if (!sel.ok) return {false, {}, sel.errorCode, sel.errorMessage};
                panelName = sel.name;
            }
            if (panelName.isEmpty())
                return {false, {}, "INVALID_ARGS", "name is required (panel objectName)"};
            QVariantMap current;
            for (const QVariantMap& summary : mainWindow->workspaceWindowSummaries()) {
                if (workspaceWindowSelectorMatches(
                        panelName,
                        summary.value(QStringLiteral("typeId")).toString(),
                        summary.value(QStringLiteral("name")).toString(),
                        summary.value(QStringLiteral("title")).toString(),
                        summary.value(QStringLiteral("instanceId")).toInt())) {
                    if (!current.isEmpty())
                        return {false, {}, "AMBIGUOUS_PANEL", "Several panels match. Use a unique name from list_panels."};
                    current = summary;
                }
            }
            if (current.isEmpty())
                return {false, {}, "PANEL_NOT_FOUND", QString("Panel '%1' not found").arg(panelName)};
            bool newState;
            if (args.contains("visible"))
                newState = args["visible"].toBool();
            else
                newState = !current.value(QStringLiteral("visible")).toBool();

            QVariantMap updated;
            if (!mainWindow->setWorkspaceWindowVisible(panelName, newState, &updated))
                return {false, {}, "PANEL_TOGGLE_FAILED",
                        QString("Failed to set panel '%1' visible=%2").arg(panelName).arg(newState)};
            return {true, QJsonObject::fromVariantMap(updated)};
        });

    manager.setActionHandler("open_curve_editor", [mainWindow](const QJsonObject& args) -> ActionResult {
            QString attenuationName = args.value("name").toString().trimmed();
            QString guidText = args.value("guid").toString().trimmed();

            AttenuationDimension dimension = AttenuationDimension::Volume;
            const QJsonValue dimensionValue = args.value("dimension");
            if (!dimensionValue.isUndefined()) {
                const QString dimensionName = dimensionValue.toString().trimmed();
                if (!dimensionValue.isString()
                    || (!dimensionName.isEmpty()
                        && !attenuationDimensionFromName(dimensionName, &dimension))) {
                    return {false, {}, "DIMENSION_REQUIRED",
                            "A valid curve dimension is required."};
                }
            }

            if (!mainWindow) return mainWindowNotAvailable();

            auto* project = ProjectManager::instance().currentProject();
            if (!project)
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};
            const QUuid guid = QUuid::fromString(guidText);
            if (!guidText.isEmpty() && guid.isNull())
                return {false, {}, "INVALID_ARGS", "guid must be a valid UUID"};
            if (guid.isNull() && attenuationName.isEmpty())
                return {false, {}, "INVALID_ARGS", "name or guid is required"};
            const auto* attenuation = AttenuationStore::find(project, guid, attenuationName);
            if (!attenuation)
                return {false, {}, "NOT_FOUND", "Attenuation not found"};
            const auto* item = attenuation->hostItem();
            const CurveSelection selection = CurveSelection::attenuation(
                item->guid, item->name, dimension);
            if (!mainWindow->openCurveEditor(selection))
                return {false, {}, "OPEN_EDITOR_FAILED", "Failed to open the curve editor"};
            return {true, QJsonObject{
                {"opened", true},
                {"name", item->name},
                {"guid", item->guid.toString(QUuid::WithoutBraces)},
                {"dimension", QLatin1String(attenuationDimensionName(dimension))}
            }};
        });

    manager.setActionHandler("save_named_layout", [mainWindow](const QJsonObject& args) -> ActionResult {
            if (!mainWindow) return mainWindowNotAvailable();
            QString name = args["name"].toString().trimmed();
            {
                auto sel = VNX::resolveSelector(name);
                if (!sel.ok) return {false, {}, sel.errorCode, sel.errorMessage};
                name = sel.name;
            }
            if (name.isEmpty())
                return {false, {}, "INVALID_ARGS", "name is required"};
            QString filePath;
            if (!mainWindow->saveNamedLayout(name, &filePath))
                return {false, {}, "FILE_WRITE_ERROR", "Cannot write layout file"};
            return {true, QJsonObject{{"name", name}, {"path", filePath}}};
        });

    manager.setActionHandler("load_named_layout", [mainWindow](const QJsonObject& args) -> ActionResult {
            if (!mainWindow) return mainWindowNotAvailable();
            QString name = args["name"].toString().trimmed();
            {
                auto sel = VNX::resolveSelector(name);
                if (!sel.ok) return {false, {}, sel.errorCode, sel.errorMessage};
                name = sel.name;
            }
            if (name.isEmpty())
                return {false, {}, "INVALID_ARGS", "name is required"};
            if (!mainWindow->loadNamedLayout(name))
                return {false, {}, "LAYOUT_NOT_FOUND", QString("Named layout '%1' not found").arg(name)};
            return {true, QJsonObject{{"name", name}, {"loaded", true}}};
        });

    manager.setActionHandler("delete_named_layout", [mainWindow](const QJsonObject& args) -> ActionResult {
            if (!mainWindow) return mainWindowNotAvailable();
            QString name = args["name"].toString().trimmed();
            {
                auto sel = VNX::resolveSelector(name);
                if (!sel.ok) return {false, {}, sel.errorCode, sel.errorMessage};
                name = sel.name;
            }
            if (name.isEmpty())
                return {false, {}, "INVALID_ARGS", "name is required"};
            if (!mainWindow->deleteNamedLayout(name))
                return {false, {}, "LAYOUT_NOT_FOUND", QString("Named layout '%1' not found").arg(name)};
            return {true, QJsonObject{{"name", name}, {"deleted", true}}};
        });
}

}
