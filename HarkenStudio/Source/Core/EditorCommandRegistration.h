#pragma once

#include "ActionManager.h"

namespace harken::studio {

struct EditorCommandContext {
    AudioEngine* engine = nullptr;
    ProjectManager* project = nullptr;
    IPCServer* dispatcher = nullptr;
    harken::studio::AuditionManager* audition = nullptr;
    std::function<bool(PhysicalFolder*)> selectFolder;
};

void registerEditorCommands(const EditorCommandContext& context);

}
