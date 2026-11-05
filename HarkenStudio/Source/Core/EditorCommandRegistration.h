#pragma once

#include "ActionManager.h"

namespace VNX {

struct EditorCommandContext {
    AudioEngine* engine = nullptr;
    ProjectManager* project = nullptr;
    IPCServer* dispatcher = nullptr;
    HarkenEditor::AuditionManager* audition = nullptr;
    std::function<bool(PhysicalFolder*)> selectFolder;
};

void registerEditorCommands(const EditorCommandContext& context);

}
