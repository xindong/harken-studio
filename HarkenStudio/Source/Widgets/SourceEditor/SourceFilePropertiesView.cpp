#include "SourceFilePropertiesView.h"

#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/Components/VSound.h"
#include "Core/ConversionResults.h"
#include "Core/PlatformContextService.h"
#include "Core/PreviewMediaModeService.h"
#include "Core/ProjectManager.h"
#include "Core/QtControlSkin.h"
#include "Core/SoundSourceSemantics.h"
#include "Core/ThemeLayout.h"
#include "Tooling/Harkenizer/Core/SoundSourceDerivative.h"
#include "Widgets/Common/DialogFormLayout.h"

#include <QLabel>
#include <QLocale>
#include <QVBoxLayout>

namespace VNX {

SourceFilePropertiesView::SourceFilePropertiesView(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("sourceFileProperties"));
    auto* layout = new QVBoxLayout(this);
    ThemeLayout::margins(layout, ThemeLayout::Space::Panel);
    ThemeLayout::spacing(layout, ThemeLayout::Space::Control);
    auto* title = new QLabel(tr("File Properties"), this);
    title->setObjectName(QStringLiteral("sourceFilePropertiesTitle"));
    QtControlSkin::setSemanticProperty(title, "textRole", QStringLiteral("title"));
    layout->addWidget(title);
    auto* form = new DialogFormLayout;
    const std::array<QString, 6> titles{
        tr("File Path"), tr("Channels"), tr("Sample Rate"), tr("Duration"), tr("Size"), tr("Format")};
    const std::array<const char*, 6> names{
        "sourceFilePath", "sourceFileChannels", "sourceFileSampleRate",
        "sourceFileDuration", "sourceFileSize", "sourceFileFormat"};
    for (size_t index = 0; index < titles.size(); ++index) {
        auto* value = new QLabel(this);
        value->setObjectName(QString::fromLatin1(names[index]));
        value->setTextFormat(Qt::PlainText);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);
        form->addRow(titles[index], value);
        values_[index] = value;
    }
    layout->addLayout(form);
    layout->addStretch();
    auto& manager = ProjectManager::instance();
    connect(&manager, &ProjectManager::itemPropertyChanged, this, &SourceFilePropertiesView::refresh, Qt::QueuedConnection);
    connect(&manager, &ProjectManager::defaultConversionChanged, this, &SourceFilePropertiesView::refresh);
    connect(&manager, &ProjectManager::itemDeleted, this, &SourceFilePropertiesView::refresh, Qt::QueuedConnection);
    connect(&manager, &ProjectManager::itemMoved, this, &SourceFilePropertiesView::refresh, Qt::QueuedConnection);
    connect(&AssetRegistry::instance(), &AssetRegistry::assetModified, this, &SourceFilePropertiesView::refresh);
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
            this, &SourceFilePropertiesView::refresh);
    connect(&PreviewMediaModeService::instance(), &PreviewMediaModeService::modeChanged,
            this, &SourceFilePropertiesView::refresh);
    connect(&SoundSourceDerivative::MaterializationEvents::instance(),
            &SoundSourceDerivative::MaterializationEvents::artifactReady,
            this, &SourceFilePropertiesView::refresh, Qt::QueuedConnection);
}

void SourceFilePropertiesView::setSource(const QUuid& soundGuid, const QUuid& entryId, uint32_t sourceId)
{
    soundGuid_ = soundGuid;
    entryId_ = entryId;
    sourceId_ = sourceId;
    refresh();
}

void SourceFilePropertiesView::refresh()
{
    for (auto* value : values_)
        value->clear();
    const auto* asset = AssetRegistry::instance().getAsset(sourceId_);
    if (!asset)
        return;
    ConversionResults::FileProperties properties{
        asset->absolutePath, asset->metadata.originalFormat, asset->metadata.sampleRate,
        asset->metadata.channels, asset->metadata.totalSamples, asset->metadata.fileSize};
    if (!soundGuid_.isNull()) {
        auto& manager = ProjectManager::instance();
        const auto* project = manager.currentProject();
        const auto* item = manager.findItemByGuid(soundGuid_);
        const auto* sound = item ? item->comp<VSound>() : nullptr;
        const auto* source = sound ? SoundSourceSemantics::findEntry(*sound, entryId_) : nullptr;
        if (!project || !source || source->sourceId != sourceId_)
            return;
        if (PreviewMediaModeService::instance().mode() == PreviewMediaMode::Converted) {
            const auto result = ConversionResults::inspectSource(*project, *item, *source, *asset,
                PlatformContextService::instance().context().primary);
            if (!result)
                return;
            if (!result->presetGuid.isNull()) {
                if (!result->converted)
                    return;
                properties = *result->converted;
            }
        }
    }
    values_[0]->setText(properties.path);
    values_[1]->setText(QString::number(properties.channels));
    values_[2]->setText(tr("%1 Hz").arg(properties.sampleRate));
    if (properties.sampleRate != 0)
        values_[3]->setText(tr("%1 s").arg(
            static_cast<double>(properties.frames) / properties.sampleRate, 0, 'f', 3));
    values_[4]->setText(QLocale().formattedDataSize(static_cast<qint64>(properties.bytes)));
    values_[5]->setText(properties.format);
}

}
