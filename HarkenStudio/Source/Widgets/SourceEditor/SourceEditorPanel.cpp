#include "SourceEditorPanel.h"

#include "SourceEditorWidget.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/Components/VSound.h"
#include "Core/ConversionResults.h"
#include "Core/PlatformContextService.h"
#include "Core/PreviewMediaModeService.h"
#include "Core/ProjectManager.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeLayout.h"
#include "Widgets/Common/DialogFormLayout.h"

#include <QChildEvent>
#include <QFocusEvent>
#include <QLabel>
#include <QLocale>
#include <QSplitter>
#include <QVBoxLayout>

namespace VNX {

QString SourceEditorPanel::typeId()
{
    return QStringLiteral("SourceEditor");
}

QString SourceEditorPanel::dockObjectPrefix()
{
    return QStringLiteral("SourceEditorDock");
}

SourceEditorPanel* SourceEditorPanel::panelContaining(QWidget* widget)
{
    for (QWidget* current = widget; current; current = current->parentWidget()) {
        if (auto* panel = qobject_cast<SourceEditorPanel*>(current))
            return panel;
    }
    return nullptr;
}

SourceEditorPanel::SourceEditorPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("SourceEditorPanel"));
    setFocusPolicy(Qt::StrongFocus);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* splitter = new QSplitter(Qt::Vertical, this);
    editor_ = new SourceEditorWidget(splitter);
    splitter->addWidget(editor_);
    auto* properties = new QWidget(splitter);
    auto* propertiesLayout = new QVBoxLayout(properties);
    ThemeLayout::margins(propertiesLayout, ThemeLayout::Space::Panel);
    ThemeLayout::spacing(propertiesLayout, ThemeLayout::Space::Control);
    auto* title = new QLabel(tr("File Properties"), properties);
    title->setObjectName(QStringLiteral("sourceFilePropertiesTitle"));
    QtControlSkin::setSemanticProperty(title, "textRole", QStringLiteral("title"));
    propertiesLayout->addWidget(title);
    auto* form = new DialogFormLayout;
    const std::array<QString, 6> titles{
        tr("File Path"), tr("Channels"), tr("Sample Rate"), tr("Duration"), tr("Size"), tr("Format")};
    const std::array<const char*, 6> names{
        "sourceFilePath", "sourceFileChannels", "sourceFileSampleRate",
        "sourceFileDuration", "sourceFileSize", "sourceFileFormat"};
    for (size_t index = 0; index < titles.size(); ++index) {
        auto* value = new QLabel(properties);
        value->setObjectName(QString::fromLatin1(names[index]));
        value->setTextFormat(Qt::PlainText);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);
        form->addRow(titles[index], value);
        filePropertyValues_[index] = value;
    }
    propertiesLayout->addLayout(form);
    propertiesLayout->addStretch();
    splitter->addWidget(properties);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter);
    connect(editor_, &SourceEditorWidget::sourceContextChanged,
            this, &SourceEditorPanel::refreshFileProperties);
    auto& manager = ProjectManager::instance();
    connect(&manager, &ProjectManager::itemPropertyChanged,
            this, &SourceEditorPanel::refreshFileProperties, Qt::QueuedConnection);
    connect(&manager, &ProjectManager::defaultConversionChanged,
            this, &SourceEditorPanel::refreshFileProperties);
    connect(&manager, &ProjectManager::itemDeleted,
            this, &SourceEditorPanel::refreshFileProperties, Qt::QueuedConnection);
    connect(&manager, &ProjectManager::itemMoved,
            this, &SourceEditorPanel::refreshFileProperties, Qt::QueuedConnection);
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
            this, &SourceEditorPanel::refreshFileProperties);
    connect(&PreviewMediaModeService::instance(), &PreviewMediaModeService::modeChanged,
            this, &SourceEditorPanel::refreshFileProperties);
    connect(&SoundSourceDerivative::MaterializationEvents::instance(),
            &SoundSourceDerivative::MaterializationEvents::artifactReady,
            this, &SourceEditorPanel::refreshFileProperties, Qt::QueuedConnection);
    installFocusFilterRecursively(this);
}

void SourceEditorPanel::refreshFileProperties()
{
    for (auto* value : filePropertyValues_)
        value->clear();
    const auto* asset = AssetRegistry::instance().getAsset(sourceId());
    if (!asset)
        return;
    ConversionResults::FileProperties properties{
        asset->absolutePath, asset->metadata.originalFormat, asset->metadata.sampleRate,
        asset->metadata.channels, asset->metadata.totalSamples, asset->metadata.fileSize};
    if (!soundGuid().isNull()) {
        auto& manager = ProjectManager::instance();
        const auto* project = manager.currentProject();
        const auto* item = manager.findItemByGuid(soundGuid());
        const auto* sound = item ? item->comp<VSound>() : nullptr;
        const auto* source = sound ? SoundSourceSemantics::findEntry(*sound, entryId()) : nullptr;
        if (!project || !source || source->sourceId != sourceId())
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
    filePropertyValues_[0]->setText(properties.path);
    filePropertyValues_[1]->setText(QString::number(properties.channels));
    filePropertyValues_[2]->setText(tr("%1 Hz").arg(properties.sampleRate));
    if (properties.sampleRate != 0)
        filePropertyValues_[3]->setText(tr("%1 s").arg(
            static_cast<double>(properties.frames) / properties.sampleRate, 0, 'f', 3));
    filePropertyValues_[4]->setText(QLocale().formattedDataSize(static_cast<qint64>(properties.bytes)));
    filePropertyValues_[5]->setText(properties.format);
}

void SourceEditorPanel::installFocusFilterRecursively(QObject* root)
{
    if (!root)
        return;
    root->installEventFilter(this);
    for (QObject* child : root->children())
        installFocusFilterRecursively(child);
}

uint32_t SourceEditorPanel::sourceId() const
{
    return editor_->sourceId();
}

bool SourceEditorPanel::isPreviewPlayheadActive() const
{
    return editor_->isPreviewPlayheadActive();
}

QString SourceEditorPanel::previewObjectName() const
{
    return editor_->previewObjectName();
}

QUuid SourceEditorPanel::soundGuid() const
{
    return editor_->soundGuid();
}

QUuid SourceEditorPanel::entryId() const
{
    return editor_->entryId();
}

void SourceEditorPanel::setPinned(bool pinned)
{
    if (pinned_ == pinned)
        return;
    pinned_ = pinned;
    emit pinToggled(pinned_);
}

void SourceEditorPanel::bindSource(uint32_t sourceId, const QUuid& soundGuid, const QUuid& entryId)
{
    editor_->bindSource(sourceId, soundGuid, entryId);
}

void SourceEditorPanel::setAuditionManager(HarkenEditor::AuditionManager* audition)
{
    editor_->setAuditionManager(audition);
}

bool SourceEditorPanel::handlePlayStopShortcut()
{
    return editor_->handlePlayStopShortcut();
}

void SourceEditorPanel::clearEditor()
{
    editor_->clearSource();
}

QVariantMap SourceEditorPanel::snapshotState() const
{
    QVariantMap state;
    if (sourceId() != 0)
        state[QStringLiteral("sourceId")] = sourceId();
    if (!soundGuid().isNull())
        state[QStringLiteral("soundGuid")] = soundGuid().toString(QUuid::WithoutBraces);
    if (!entryId().isNull())
        state[QStringLiteral("entryId")] = entryId().toString(QUuid::WithoutBraces);
    return state;
}

void SourceEditorPanel::restoreState(const QVariantMap& state)
{
    bindSource(state.value(QStringLiteral("sourceId")).toUInt(),
               QUuid::fromString(state.value(QStringLiteral("soundGuid")).toString()),
               QUuid::fromString(state.value(QStringLiteral("entryId")).toString()));
}

bool SourceEditorPanel::eventFilter(QObject* watched, QEvent* event)
{
    const QEvent::Type type = event->type();
    if (type == QEvent::FocusIn) {
        if (watched != this)
            emit panelFocused(panelId_);
    } else if (type == QEvent::ChildAdded) {
        if (auto* childEvent = static_cast<QChildEvent*>(event))
            installFocusFilterRecursively(childEvent->child());
    }
    return QWidget::eventFilter(watched, event);
}

void SourceEditorPanel::focusInEvent(QFocusEvent* event)
{
    emit panelFocused(panelId_);
    QWidget::focusInEvent(event);
}

}
