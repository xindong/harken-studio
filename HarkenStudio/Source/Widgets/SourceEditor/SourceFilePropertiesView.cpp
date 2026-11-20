#include "SourceFilePropertiesView.h"

#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AssetPipeline/PathUtils.h"
#include "Core/Components/VSound.h"
#include "Core/ConversionResults.h"
#include "Core/PlatformContextService.h"
#include "Core/PreviewMediaModeService.h"
#include "Core/ProjectManager.h"
#include "Core/QtControlSkin.h"
#include "Core/SoundSourceSemantics.h"
#include "Core/ThemeLayout.h"
#include "Tooling/Harkenizer/Core/SoundSourceDerivative.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include "Widgets/Common/TableWidget.h"

#include <QDir>
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
    auto* table = new TableWidget(TableOptions::display(), this);
    table->setColumnsMovable(false);
    auto* view = table->createItemView(QStringLiteral("sourceFilePropertiesTable"),
        {tr("Property"), tr("Value")}, {1, 5});
    view->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    view->setSelectionMode(QAbstractItemView::NoSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(table, 1);
    const std::array<QString, PropertyCount> titles{tr("Relative Path"), tr("Absolute Path"),
        tr("Channels"), tr("Sample Rate"), tr("Duration"), tr("Size"), tr("Format")};
    const std::array<const char*, PropertyCount> names{"sourceFileRelativePath", "sourceFileAbsolutePath",
        "sourceFileChannels", "sourceFileSampleRate", "sourceFileDuration", "sourceFileSize", "sourceFileFormat"};
    for (size_t index = 0; index < titles.size(); ++index) {
        auto* row = new QTreeWidgetItem(view, {titles[index]});
        auto* value = new QLabel(view);
        value->setObjectName(QString::fromLatin1(names[index]));
        value->setTextFormat(Qt::PlainText);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        view->setItemWidget(row, 1, value);
        values_[index] = value;
    }
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
    for (auto* value : values_) {
        value->clear();
        value->setToolTip({});
    }
    auto& registry = AssetRegistry::instance();
    const auto* asset = registry.getAsset(sourceId_);
    if (!asset)
        return;
    auto properties = ConversionResults::FileProperties::fromSource(*asset);
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
                PlatformContextService::instance().context().primary, {}, sound->scheduleOffsetSec);
            if (!result)
                return;
            if (!result->presetGuid.isNull()) {
                if (!result->converted)
                    return;
                properties = *result->converted;
            }
        }
    }
    const auto displayPath = [](QLabel* label, const QString& path) {
        const auto nativePath = QDir::toNativeSeparators(path);
        label->setText(nativePath);
        label->setToolTip(nativePath);
    };
    displayPath(values_[RelativePath], PathUtils::toRelativePath(properties.path, registry.projectRoot()));
    displayPath(values_[AbsolutePath], properties.path);
    values_[Channels]->setText(QString::number(properties.channels));
    values_[SampleRate]->setText(tr("%1 Hz").arg(properties.sampleRate));
    if (properties.sampleRate != 0)
        values_[Duration]->setText(tr("%1 s").arg(
            static_cast<double>(properties.frames) / properties.sampleRate, 0, 'f', 3));
    values_[Size]->setText(QLocale().formattedDataSize(static_cast<qint64>(properties.bytes)));
    values_[Format]->setText(properties.format);
}

}
