#include "SelectionWaveform.h"

#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/LanguageContextService.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/SoundSourceEditing.h"

namespace VNX {

SelectionWaveform::SelectionWaveform(const QUuid& targetGuid, QWidget* parent)
    : SourceRangeThumb(parent), targetGuid_(targetGuid)
{
    connect(this, &SourceRangeThumb::clicked, this, [this] {
        if (isEnabled() && !entryId_.isNull()
            && AssetRegistry::instance().sourceFileExists(sourceId_))
            emit sourceEditorRequested(targetGuid_, entryId_);
    });
    auto& manager = ProjectManager::instance();
    connect(&manager, &ProjectManager::itemPropertyChanged, this,
            [this](ProjectItem* item, const QString& property, const QObject*) {
        if (item && item->guid == targetGuid_
            && (property == QStringLiteral("sources")
                || property == QStringLiteral("distinguishLanguage")))
            refresh();
    });
    connect(&manager, &ProjectManager::itemCreated, this, [this](ProjectItem* item) {
        if (item && item->guid == targetGuid_) refresh();
    });
    connect(&manager, &ProjectManager::itemDeleted, this, [this](ProjectItem* item) {
        if (item && item->guid == targetGuid_) clear();
    });
    connect(&manager, &ProjectManager::projectClosed, this, &SelectionWaveform::clear);
    connect(&manager, &ProjectManager::projectLanguagesChanged, this,
            [this] { refresh(); });
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
            this, [this] { refresh(); });
    connect(&LanguageContextService::instance(), &LanguageContextService::languageChanged,
            this, [this] { refresh(); });
    auto& assets = AssetRegistry::instance();
    const auto refreshAsset = [this](uint32_t sourceId) {
        if (sourceId == sourceId_) refresh();
    };
    connect(&assets, &AssetRegistry::assetRegistered, this, refreshAsset);
    connect(&assets, &AssetRegistry::assetModified, this, refreshAsset);
    connect(&assets, &AssetRegistry::assetUnregistered, this, refreshAsset);
    refresh();
}

void SelectionWaveform::clear()
{
    entryId_ = QUuid{};
    sourceId_ = 0;
    clearSource();
    setToolTip({});
    hide();
}

void SelectionWaveform::refresh()
{
    const auto* item = ProjectManager::instance().findItemByGuid(targetGuid_);
    if (!item || item->type != ProjectItem::Type::Sound) {
        clear();
        return;
    }
    entryId_ = QUuid{};
    sourceId_ = 0;
    setVisible(true);
    setToolTip({});
    const auto& context = PlatformContextService::instance().context();
    if (context.selected.isEmpty()) {
        clearSource();
        return;
    }
    const auto language = SoundSourceEditing::previewLanguage(
        *item, LanguageContextService::instance().language());
    const auto* source = SoundSourceEditing::activeSource(*item, context.primary, language);
    if (context.all) {
        for (const auto platform : context.selected) {
            const auto* other = SoundSourceEditing::activeSource(*item, platform, language);
            if ((source ? source->entryId : QUuid{}) != (other ? other->entryId : QUuid{})) {
                clearSource(tr("Mixed"));
                setToolTip(tr("Active sources differ across platforms. Select a single platform to view its waveform."));
                return;
            }
        }
    }
    if (!source) {
        clearSource();
        return;
    }
    entryId_ = source->entryId;
    sourceId_ = source->sourceId;
    const auto* asset = AssetRegistry::instance().getAsset(sourceId_);
    setSource(*source, asset);
    if (isEnabled()) setToolTip(tr("Open Source Editor"));
}

}
