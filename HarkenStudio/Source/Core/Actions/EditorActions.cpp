
#include "../CommandSchemas.h"
#include "../IPC/DesktopCommandSchemas.h"
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
#include "../ProjectTypes.h"

#include <QJsonArray>
#include <QMetaObject>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <cstring>

namespace VNX {



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
        def.requiresDesktop = true;
        def.displayName = "Connect Nodes";
        def.category = "editor";
        def.paramSchema = desktopCommandInputSchema(def.id);
        def.undoable = false;

        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "disconnect_nodes";
        def.requiresDesktop = true;
        def.displayName = "Disconnect Nodes";
        def.category = "editor";
        def.paramSchema = desktopCommandInputSchema(def.id);
        def.undoable = false;

        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_node_override";
        def.requiresDesktop = true;
        def.displayName = "Set Node Override";
        def.category = "editor";
        def.paramSchema = desktopCommandInputSchema(def.id);
        def.undoable = false;

        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_canvas_flow";
        def.requiresDesktop = true;
        def.displayName = "Set Blueprint Flow Direction";
        def.category = "editor";
        def.paramSchema = desktopCommandInputSchema(def.id);
        def.undoable = false;

        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "get_canvas_flow";
        def.paramSchema = desktopCommandInputSchema(def.id);
        def.requiresDesktop = true;
        def.displayName = "Get Blueprint Flow Direction";
        def.category = "editor";
        def.undoable = false;

        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "create_action";
        def.displayName = "Create Action";
        def.category = "editor";
        def.paramSchema = commandInputSchema(def.id);
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
