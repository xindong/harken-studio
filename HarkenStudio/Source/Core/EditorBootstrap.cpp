#include "EditorBootstrap.h"

#include "AppSettings.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AudioEngine.h"
#include "PlatformContextService.h"
#include "ProjectManager.h"
#include "ActionManager.h"
#include "ThemeManager.h"
#include "FontManager.h"

namespace VNX {

void initializeEditorSingletons()
{
    AppSettings::instance();
    ThemeManager::instance();
    FontManager::instance();
    AssetRegistry::instance();
    AudioEngine::instance();
    ProjectManager::instance();
    ActionManager::instance();
    PlatformContextService::instance();
}

}
