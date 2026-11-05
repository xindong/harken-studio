#pragma once

#include "EditorCommandRegistration.h"

namespace VNX {

struct DesktopEditorCommandContext {
    AudioEngine* engine = nullptr;
    ProjectManager* project = nullptr;
    IPCServer* dispatcher = nullptr;
    MainWindow* window = nullptr;
    ProjectExplorer* explorer = nullptr;
    BlueprintCanvasWidget* blueprint = nullptr;
    HarkenEditor::AuditionManager* audition = nullptr;
};

void registerDesktopEditorCommands(const DesktopEditorCommandContext& context);

}
