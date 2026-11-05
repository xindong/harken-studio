#pragma once

#include "ActionManager.h"

namespace VNX {

struct EditorCommandContext {
    AudioEngine* engine = nullptr;
    ProjectManager* project = nullptr;
    IPCServer* dispatcher = nullptr;
    MainWindow* window = nullptr;
    ProjectExplorer* explorer = nullptr;
    BlueprintCanvasWidget* blueprint = nullptr;
    HarkenEditor::AuditionManager* audition = nullptr;
};

void registerEditorCommands(const EditorCommandContext& context);

}
