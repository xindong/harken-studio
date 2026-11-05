#include "EditorCommandRegistration.h"
#include "IPC/IPCServer.h"

namespace VNX {

void registerTemplateActions(ActionManager& manager, ProjectManager* project);

void registerEditorCommands(const EditorCommandContext& context)
{
    auto& manager = ActionManager::instance();
    registerMixerActions(manager, context.engine, context.project, nullptr);
    registerProjectActions(manager, context.project, context.selectFolder);
    registerBindingActions(manager);
    registerContainerActions(manager, context.project);
    registerEditorActions(manager, context.project, context.engine,
                          nullptr, context.dispatcher);
    registerTemplateActions(manager, context.project);
    registerAuditionActions(manager, context.audition, context.engine);
    registerSpatialActions(manager, context.engine, context.project);
    registerAdvancedActions(manager, context.engine);
    registerAssetActions(manager, context.engine, context.project);
    registerSystemActions(manager, context.engine, context.audition,
                          context.dispatcher->activePlaybacksMap());
    registerReadonlyActions(manager, context.dispatcher);
}

}
