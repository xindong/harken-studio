#include "BlueprintCanvasWidget.h"

#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AssetPipeline/MediaPool.h"
#include "Core/LanguageContextService.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/SoundSourceEditing.h"
#include "Core/ThemeManager.h"

namespace VNX {

void BlueprintCanvasWidget::refreshNodeTitles_()
{
    auto& manager = ProjectManager::instance();
    const auto platform = PlatformContextService::instance().context().primary;
    const auto& contextLanguage = LanguageContextService::instance().language();
    const auto& colors = ThemeManager::instance().colors();
    for (const auto& [key, node] : nodeMap_) {
        const auto* item = key.kind == NodeKind::Bus ? manager.findBusItem(key.id)
            : manager.findItemByObjectID(key.id);
        if (!item) continue;
        node->setTitle(item->name);
        node->setTitleDirty(item->isDirtyObject());
        QColor color;
        if (item->type == ProjectItemType::Sound) {
            const auto language = SoundSourceEditing::previewLanguage(*item, contextLanguage);
            const auto source = SoundSourceEditing::activeSourceId(*item, platform, language);
            if (source != 0) {
                switch (MediaPool::instance().cachedStatus(source)) {
                case MediaStatus::Outdated: color = colors.warning; break;
                case MediaStatus::Missing:
                case MediaStatus::Error: color = colors.error; break;
                case MediaStatus::Orphaned: color = colors.textMuted; break;
                default: break;
                }
            }
        }
        node->setTitleColor(color);
    }
}

}
