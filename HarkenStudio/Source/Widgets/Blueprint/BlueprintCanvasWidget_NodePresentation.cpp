#include "BlueprintCanvasWidget.h"

#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AssetPipeline/MediaPool.h"
#include "Core/AssetPipeline/WaveformCache.h"
#include "Core/LanguageContextService.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/SoundSourceEditing.h"
#include "Core/ThemeManager.h"

namespace VNX {

void BlueprintCanvasWidget::refreshNodeWaveforms_(uint32_t sourceID)
{
    const auto refresh = [](const QString& path, const std::vector<WaveformNode>& nodes) {
        const auto peaks = path.isEmpty() ? std::shared_ptr<const WaveformPeakData>{}
                                         : WaveformCache::loadShared(path);
        for (const auto& node : nodes)
            node.item->setWaveformPeakData(peaks, node.range);
    };
    if (sourceID != 0) {
        const auto* asset = AssetRegistry::instance().getAsset(sourceID);
        if (!asset) return;
        const auto nodes = waveformNodes_.constFind(asset->absolutePath);
        if (nodes != waveformNodes_.cend()) refresh(nodes.key(), nodes.value());
        return;
    }
    for (auto nodes = waveformNodes_.cbegin(); nodes != waveformNodes_.cend(); ++nodes)
        refresh(nodes.key(), nodes.value());
}

void BlueprintCanvasWidget::refreshNodeTitles_()
{
    auto& manager = ProjectManager::instance();
    const auto platform = PlatformContextService::instance().context().primary;
    const auto& contextLanguage = LanguageContextService::instance().language();
    const auto& colors = ThemeManager::instance().colors();
    std::vector<uint32_t> objectIDs;
    objectIDs.reserve(nodeMap_.size());
    for (const auto& [key, node] : nodeMap_)
        if (key.kind != NodeKind::Bus) objectIDs.push_back(key.id);
    const auto items = manager.findItemsByObjectID(objectIDs);
    size_t itemIndex = 0;
    for (const auto& [key, node] : nodeMap_) {
        const auto* item = key.kind == NodeKind::Bus ? manager.findBusItem(key.id)
            : items[itemIndex++];
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
