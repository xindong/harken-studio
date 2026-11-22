#pragma once

#include "EditorCommandRegistration.h"

namespace harken::studio {

struct DesktopEditorCommandContext {
    AudioEngine* engine = nullptr;
    ProjectManager* project = nullptr;
    IPCServer* dispatcher = nullptr;
    MainWindow* window = nullptr;
    ProjectExplorer* explorer = nullptr;
    BlueprintCanvasWidget* blueprint = nullptr;
    harken::studio::AuditionManager* audition = nullptr;
};

void registerDesktopEditorCommands(const DesktopEditorCommandContext& context);

}
