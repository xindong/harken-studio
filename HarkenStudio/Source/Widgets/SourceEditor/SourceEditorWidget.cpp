#include "SourceEditorWidget.h"

#include "SourceEditorWaveformView.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AudioEngine.h"
#include "Core/Components/VSound.h"
#include "Core/ProjectManager.h"
#include "Core/SoundSourceSemantics.h"
#include "Core/AuditionManager.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeAssetResolver.h"
#include "Core/ThemeManager.h"

#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace VNX {

namespace {

QString formatFileSize(uint64_t bytes)
{
    static constexpr double kKilobyte = 1024.0;
    static constexpr double kMegabyte = kKilobyte * 1024.0;
    static constexpr double kGigabyte = kMegabyte * 1024.0;

    if (bytes == 0)
        return QObject::tr("Unknown");
    if (bytes < 1024)
        return QObject::tr("%1 B").arg(bytes);
    if (bytes < static_cast<uint64_t>(kMegabyte))
        return QObject::tr("%1 KB").arg(QString::number(bytes / kKilobyte, 'f', 1));
    if (bytes < static_cast<uint64_t>(kGigabyte))
        return QObject::tr("%1 MB").arg(QString::number(bytes / kMegabyte, 'f', 1));
    return QObject::tr("%1 GB").arg(QString::number(bytes / kGigabyte, 'f', 1));
}

QString formatDuration(double seconds)
{
    return std::isfinite(seconds) && seconds > 0.0
        ? QObject::tr("%1 s").arg(QString::number(seconds, 'f', 2))
        : QObject::tr("Unknown");
}

QString formatEmbeddedLoop(const AudioMetadata& metadata)
{
    const LoopInfo& loop = metadata.loopInfo;
    if (!loop.enabled)
        return QObject::tr("None");
    if (metadata.sampleRate == 0)
        return QObject::tr("%1 – %2 frames")
            .arg(QString::number(loop.loopStart), QString::number(loop.loopEnd));

    const double startSeconds = static_cast<double>(loop.loopStart) / metadata.sampleRate;
    const double endSeconds = static_cast<double>(loop.loopEnd) / metadata.sampleRate;
    return QObject::tr("%1 – %2 s  (%3 – %4 frames)")
        .arg(QString::number(startSeconds, 'f', 3),
             QString::number(endSeconds, 'f', 3),
             QString::number(loop.loopStart),
             QString::number(loop.loopEnd));
}

QString formatLevel(float value, const QString& suffix, float unavailableValue = 0.0f)
{
    if (!std::isfinite(value) || value == unavailableValue)
        return QObject::tr("Unknown");
    return QObject::tr("%1 %2").arg(QString::number(value, 'f', 1), suffix);
}

QString formatRangeValue(uint64_t frame, uint32_t sampleRate)
{
    const auto seconds = SoundSourceSemantics::secondsFromFrames(frame, sampleRate);
    return seconds
        ? QObject::tr("%1 s\n%2 frames")
              .arg(QString::number(*seconds, 'f', 3), QString::number(frame))
        : QObject::tr("%1 frames").arg(QString::number(frame));
}

std::pair<QLabel*, QLabel*> addMetadataValue(QWidget* parent, const QString& title,
                                             const QString& objectName)
{
    auto* titleLabel = new QLabel(title, parent);
    titleLabel->setProperty("textRole", QStringLiteral("formLabel"));
    titleLabel->setProperty("textScale", QStringLiteral("caption"));

    auto* valueLabel = new QLabel(parent);
    valueLabel->setObjectName(objectName);
    valueLabel->setProperty("textRole", QStringLiteral("primary"));
    valueLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    valueLabel->setToolTip(title);
    return {titleLabel, valueLabel};
}

QLabel* addRangeValue(QGridLayout* layout, QWidget* parent, int column,
                      const QString& title, const QString& objectName,
                      std::vector<QLabel*>* titles)
{
    auto* titleLabel = new QLabel(title, parent);
    titleLabel->setProperty("textRole", QStringLiteral("formLabel"));
    titleLabel->setProperty("textScale", QStringLiteral("caption"));
    titleLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(titleLabel, 0, column);
    titles->push_back(titleLabel);

    auto* valueLabel = new QLabel(parent);
    valueLabel->setObjectName(objectName);
    valueLabel->setProperty("textRole", QStringLiteral("primary"));
    valueLabel->setAlignment(Qt::AlignCenter);
    valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    valueLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(valueLabel, 1, column);
    return valueLabel;
}

}

SourceEditorWidget::SourceEditorWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("SourceEditorWidget"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    sourceIdentityStrip_ = new QWidget(this);
    sourceIdentityStrip_->setObjectName(QStringLiteral("sourceEditorSourceIdentity"));
    auto* identityLayout = new QVBoxLayout(sourceIdentityStrip_);
    identityLayout->setContentsMargins(2, 0, 2, 0);
    identityLayout->setSpacing(1);
    sourceTitle_ = new QLabel(sourceIdentityStrip_);
    sourceTitle_->setObjectName(QStringLiteral("sourceEditorSourceTitle"));
    sourceTitle_->setProperty("textRole", QStringLiteral("primary"));
    sourceTitle_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    sourceTitle_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sourceContext_ = new QLabel(sourceIdentityStrip_);
    sourceContext_->setObjectName(QStringLiteral("sourceEditorSourceContext"));
    sourceContext_->setProperty("textRole", QStringLiteral("muted"));
    sourceContext_->setProperty("textScale", QStringLiteral("caption"));
    sourceContext_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    sourceContext_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    identityLayout->addWidget(sourceTitle_);
    identityLayout->addWidget(sourceContext_);
    layout->addWidget(sourceIdentityStrip_);

    metadataStrip_ = new QWidget(this);
    metadataStrip_->setObjectName(QStringLiteral("sourceEditorMetadataStrip"));
    metadataStrip_->setProperty("surfaceRole", QStringLiteral("secondary"));
    metadataLayout_ = new QGridLayout(metadataStrip_);
    metadataLayout_->setContentsMargins(8, 6, 8, 6);
    metadataLayout_->setHorizontalSpacing(6);
    metadataLayout_->setVerticalSpacing(3);

    const auto addField = [this](const QString& title, const QString& name) {
        const auto field = addMetadataValue(metadataStrip_, title, name);
        metadataFields_.push_back(field);
        return field.second;
    };
    formatValue_ = addField(tr("Format"), QStringLiteral("audioMetadataFormat"));
    sampleRateValue_ = addField(tr("Sample Rate"), QStringLiteral("audioMetadataSampleRate"));
    channelsValue_ = addField(tr("Channels"), QStringLiteral("audioMetadataChannels"));
    bitDepthValue_ = addField(tr("Bit Depth"), QStringLiteral("audioMetadataBitDepth"));
    fileSizeValue_ = addField(tr("File Size"), QStringLiteral("audioMetadataFileSize"));
    durationValue_ = addField(tr("Duration"), QStringLiteral("audioMetadataDuration"));
    embeddedLoopValue_ = addField(tr("Embedded Loop"), QStringLiteral("audioMetadataEmbeddedLoop"));
    peakValue_ = addField(tr("Peak"), QStringLiteral("audioMetadataPeak"));
    lufsValue_ = addField(tr("LUFS"), QStringLiteral("audioMetadataLufs"));
    layout->addWidget(metadataStrip_);

    rangePanel_ = new QWidget(this);
    rangePanel_->setObjectName(QStringLiteral("sourceEditorRangePanel"));
    rangePanel_->setProperty("surfaceRole", QStringLiteral("secondary"));
    auto* rangeLayout = new QVBoxLayout(rangePanel_);
    rangeLayout->setContentsMargins(8, 6, 8, 6);
    rangeLayout->setSpacing(5);
    auto* rangeHeader = new QHBoxLayout();
    rangeSourceValue_ = new QLabel(rangePanel_);
    rangeSourceValue_->setObjectName(QStringLiteral("sourceEditorRangeSource"));
    rangeSourceValue_->setProperty("textRole", QStringLiteral("muted"));
    rangeSourceValue_->setProperty("textScale", QStringLiteral("caption"));
    rangeSourceValue_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    rangeHeader->addWidget(rangeSourceValue_, 1);
    rangeLayout->addLayout(rangeHeader);

    rangeValues_ = new QWidget(rangePanel_);
    rangeValuesLayout_ = new QGridLayout(rangeValues_);
    rangeValuesLayout_->setContentsMargins(0, 0, 0, 0);
    rangeValuesLayout_->setHorizontalSpacing(6);
    rangeValuesLayout_->setVerticalSpacing(2);
    trimStartValue_ = addRangeValue(rangeValuesLayout_, rangeValues_, 0, tr("Trim In"),
                                    QStringLiteral("sourceEditorTrimStart"), &rangeTitleLabels_);
    loopStartValue_ = addRangeValue(rangeValuesLayout_, rangeValues_, 1, tr("Loop In"),
                                    QStringLiteral("sourceEditorLoopStart"), &rangeTitleLabels_);
    loopEndValue_ = addRangeValue(rangeValuesLayout_, rangeValues_, 2, tr("Loop Out"),
                                  QStringLiteral("sourceEditorLoopEnd"), &rangeTitleLabels_);
    trimEndValue_ = addRangeValue(rangeValuesLayout_, rangeValues_, 3, tr("Trim Out"),
                                  QStringLiteral("sourceEditorTrimEnd"), &rangeTitleLabels_);
    rangeValueLabels_ = {trimStartValue_, loopStartValue_, loopEndValue_, trimEndValue_};
    rangeLayout->addWidget(rangeValues_);

    integrityWarning_ = new QLabel(rangePanel_);
    integrityWarning_->setObjectName(QStringLiteral("sourceEditorIntegrityWarning"));
    integrityWarning_->setProperty("textRole", QStringLiteral("warning"));
    integrityWarning_->setWordWrap(true);
    rangeLayout->addWidget(integrityWarning_);
    layout->addWidget(rangePanel_);

    auditionStrip_ = new QWidget(this);
    auditionStrip_->setObjectName(QStringLiteral("sourceEditorAudition"));
    auto* auditionLayout = new QHBoxLayout(auditionStrip_);
    auditionLayout->setContentsMargins(0, 0, 0, 0);
    auditionLayout->setSpacing(6);
    playButton_ = new QToolButton(auditionStrip_);
    playButton_->setObjectName(QStringLiteral("sourceEditorPlay"));
    playButton_->setAutoRaise(true);
    playButton_->setCursor(Qt::PointingHandCursor);
    playButton_->setProperty("flatIconButton", true);
    playButton_->setFixedSize(28, 28);
    playButton_->setIconSize(QSize(20, 20));
    playButton_->setToolTip(tr("Play"));
    stopButton_ = new QToolButton(auditionStrip_);
    stopButton_->setObjectName(QStringLiteral("sourceEditorStop"));
    stopButton_->setAutoRaise(true);
    stopButton_->setCursor(Qt::PointingHandCursor);
    stopButton_->setProperty("flatIconButton", true);
    stopButton_->setFixedSize(28, 28);
    stopButton_->setIconSize(QSize(20, 20));
    stopButton_->setToolTip(tr("Stop"));
    auditionLayout->addWidget(playButton_);
    auditionLayout->addWidget(stopButton_);
    auditionLayout->addStretch(1);
    layout->addWidget(auditionStrip_);

    waveformView_ = new SourceEditorWaveformView(this);
    waveformView_->setEditable(false);
    connect(waveformView_, &SourceEditorWaveformView::previewChanged, this, [this]() {
        if (refreshingDisplayedSource_)
            return;
        const AssetInfo* asset = sourceId_ == 0
            ? nullptr : AssetRegistry::instance().getAsset(sourceId_);
        refreshRangePresentation(waveformView_->previewSource(), asset,
                                 !soundGuid_.isNull() && !entryId_.isNull());
    });
    layout->addWidget(waveformView_, 1);

    emptyStateLabel_ = new QLabel(this);
    emptyStateLabel_->setObjectName(QStringLiteral("sourceEditorEmptyState"));
    emptyStateLabel_->setProperty("textRole", QStringLiteral("muted"));
    emptyStateLabel_->setAlignment(Qt::AlignCenter);
    emptyStateLabel_->setWordWrap(true);
    layout->addWidget(emptyStateLabel_);

    connect(playButton_, &QToolButton::clicked, this, [this]() {
        startPreviewPlayhead();
    });
    connect(stopButton_, &QToolButton::clicked,
            this, &SourceEditorWidget::stopPreviewPlayhead);
    connect(waveformView_, &SourceEditorWaveformView::locateRequested,
            this, [this](uint64_t frame) {
        const auto slice = previewPcmSlice();
        if (!slice || slice->windowFrameCount == 0)
            return;
        const uint64_t windowEnd = slice->windowStartFrame + slice->windowFrameCount;
        if (frame < slice->windowStartFrame)
            frame = slice->windowStartFrame;
        else if (frame >= windowEnd)
            frame = windowEnd - 1;
        startPreviewPlayhead(frame);
    });
    connect(waveformView_, &SourceEditorWaveformView::playheadStopped,
            this, [this]() {
        if (previewPlaybackHandle_ != 0)
            stopPreviewPlayhead();
        else
            refreshAuditionControls();
    });
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &SourceEditorWidget::applyAuditionTheme);

    auto& assets = AssetRegistry::instance();
    connect(&assets, &AssetRegistry::assetRegistered, this, [this](uint32_t sourceId) {
        if (sourceId == sourceId_)
            refreshMetadata();
    });
    connect(&assets, &AssetRegistry::assetModified, this, [this](uint32_t sourceId) {
        if (sourceId == sourceId_)
            refreshMetadata();
    });
    connect(&assets, &AssetRegistry::assetUnregistered, this, [this](uint32_t sourceId) {
        if (sourceId == sourceId_)
            refreshMetadata();
    });
    connect(&ProjectManager::instance(), &ProjectManager::projectOpened,
            this, [this](ProjectData*) { refreshMetadata(); }, Qt::QueuedConnection);
    connect(&ProjectManager::instance(), &ProjectManager::itemPropertyChanged,
            this, [this](ProjectItem* item, const QString& property, const QObject*) {
        if ((property == QStringLiteral("sources") || property == QStringLiteral("distinguishLanguage"))
            && !soundGuid_.isNull()
            && item && item->guid == soundGuid_) {
            refreshMetadata();
        }
    }, Qt::QueuedConnection);
    connect(&ProjectManager::instance(), &ProjectManager::itemDeleted,
            this, [this](ProjectItem*) {
        if (!soundGuid_.isNull())
            refreshMetadata();
    }, Qt::QueuedConnection);
    connect(&ProjectManager::instance(), &ProjectManager::projectClosed,
            this, &SourceEditorWidget::clearSource);

    applyAuditionTheme();
    refreshMetadata();
}

SourceEditorWidget::~SourceEditorWidget()
{
    stopPreviewPlayhead();
}

void SourceEditorWidget::bindSource(uint32_t sourceId, const QUuid& soundGuid,
                                   const QUuid& entryId)
{
    sourceId_ = sourceId;
    if (sourceId_ != 0 && !soundGuid.isNull() && !entryId.isNull()) {
        soundGuid_ = soundGuid;
        entryId_ = entryId;
    } else {
        soundGuid_ = QUuid();
        entryId_ = QUuid();
    }
    stopPreviewPlayhead();
    refreshMetadata();
}

void SourceEditorWidget::clearSource()
{
    bindSource(0);
}

void SourceEditorWidget::setAuditionManager(HarkenEditor::AuditionManager* audition)
{
    if (audition_ == audition)
        return;
    if (audition_)
        disconnect(audition_, nullptr, this, nullptr);
    audition_ = audition;
    if (!audition_)
        return;
    connect(audition_, &HarkenEditor::AuditionManager::auditionStateChanged,
            this, [this](uint32_t playingId, int state) {
        if (playingId == 0 || playingId != auditionPlayingId_ || state != 0)
            return;
        auditionPlayingId_ = 0;
        if (previewPlaybackHandle_ == 0)
            return;
        previewPlaybackHandle_ = 0;
        if (previewSoundId_ != harkenstudio::ObjectId{}) {
            AudioEngine::instance().unregisterRawPcm(previewSoundId_);
            previewSoundId_ = {};
        }
        waveformView_->stopPlayhead();
        refreshAuditionControls();
    });
}

QString SourceEditorWidget::auditionObjectName(const AssetInfo* asset) const
{
    if (asset && !asset->relativePath.isEmpty())
        return asset->relativePath;
    if (asset && !asset->absolutePath.isEmpty())
        return QFileInfo(asset->absolutePath).fileName();
    return tr("Audio source %1").arg(sourceId_);
}

void SourceEditorWidget::retireRawFileAudition()
{
    if (auditionPlayingId_ == 0)
        return;
    if (audition_) {
        const uint32_t playingId = auditionPlayingId_;
        auditionPlayingId_ = 0;
        const auto info = audition_->getAudition(playingId);
        if (info.playingId == playingId)
            audition_->stop(playingId, 0);
        return;
    }
    auditionPlayingId_ = 0;
}

bool SourceEditorWidget::registerRawFileAudition(const AssetInfo* asset)
{
    if (!audition_)
        return true;
    if (previewPlaybackHandle_ == 0)
        return false;
    auditionPlayingId_ = audition_->registerRawFilePlayback(
        auditionObjectName(asset),
        static_cast<quintptr>(previewPlaybackHandle_),
        sourceId_);
    return auditionPlayingId_ != 0;
}

std::optional<SoundSourceSemantics::PcmSlice> SourceEditorWidget::previewPcmSlice(
    uint64_t startFrame) const
{
    if (sourceId_ == 0)
        return std::nullopt;
    const AssetInfo* asset = AssetRegistry::instance().getAsset(sourceId_);
    if (!asset)
        return std::nullopt;
    if (const SoundSourceDef* source = boundSource())
        return SoundSourceSemantics::pcmSlice(*source, asset, startFrame);
    if (referenceIsDangling())
        return std::nullopt;

    SoundSourceDef fileSource;
    fileSource.sourceId = asset->sourceID;
    fileSource.rangeContentHash = asset->contentHash;
    fileSource.loopRegion = SoundSourceSemantics::fileLoopRegion(asset);
    if (fileSource.loopRegion)
        fileSource.loopProvenance = SoundLoopProvenance::File;
    return SoundSourceSemantics::pcmSlice(fileSource, asset, startFrame);
}

std::optional<SoundSourceSemantics::PcmSlice> SourceEditorWidget::previewLoopPcmSlice(
    uint64_t startFrame) const
{
    const auto slice = previewPcmSlice(startFrame);
    if (!slice || !slice->loops)
        return std::nullopt;
    return slice;
}

bool SourceEditorWidget::startPreviewPlayhead(uint64_t startFrame)
{
    const auto slice = previewPcmSlice(startFrame);
    const AssetInfo* asset = sourceId_ == 0
        ? nullptr : AssetRegistry::instance().getAsset(sourceId_);
    if (!slice || !asset || asset->absolutePath.isEmpty()) {
        stopPreviewPlayhead();
        return false;
    }

    auto& engine = AudioEngine::instance();
    if (previewSoundId_ == harkenstudio::ObjectId{})
        previewSoundId_ = engine.mintRawPcmSoundId();
    if (previewSoundId_ == harkenstudio::ObjectId{}) {
        stopPreviewPlayhead();
        return false;
    }

    if (previewPlaybackHandle_ != 0) {
        retireRawFileAudition();
        engine.stopRawPcm(previewPlaybackHandle_, 0);
        previewPlaybackHandle_ = 0;
    }

    const auto registered = engine.registerRawPcmSlice(
        previewSoundId_, asset->absolutePath, slice->windowStartFrame,
        slice->windowFrameCount, slice->loops);
    if (registered.result != HarkenEditor::Bridge::kEngineOk) {
        stopPreviewPlayhead();
        return false;
    }

    if (slice->startFrame != slice->windowStartFrame) {
        const auto relocated = engine.relocateRawPcmSlice(previewSoundId_, slice->startFrame);
        if (relocated.result != HarkenEditor::Bridge::kEngineOk) {
            stopPreviewPlayhead();
            return false;
        }
    }

    previewPlaybackHandle_ = engine.playRawPcm(previewSoundId_, slice->loops);
    if (previewPlaybackHandle_ == 0) {
        stopPreviewPlayhead();
        return false;
    }
    if (!registerRawFileAudition(asset)) {
        stopPreviewPlayhead();
        return false;
    }

    waveformView_->startPlayhead(slice->startFrame, slice->windowStartFrame,
                                 slice->windowFrameCount, slice->sampleRate, slice->loops);
    refreshAuditionControls();
    return waveformView_->isPlayheadActive();
}

void SourceEditorWidget::stopPreviewPlayhead()
{
    auto& engine = AudioEngine::instance();
    retireRawFileAudition();
    if (previewPlaybackHandle_ != 0) {
        engine.stopRawPcm(previewPlaybackHandle_, 0);
        previewPlaybackHandle_ = 0;
    }
    if (previewSoundId_ != harkenstudio::ObjectId{}) {
        engine.unregisterRawPcm(previewSoundId_);
        previewSoundId_ = {};
    }
    waveformView_->stopPlayhead();
    refreshAuditionControls();
}

bool SourceEditorWidget::handlePlayStopShortcut()
{
    if (isPreviewPlayheadActive()) {
        stopPreviewPlayhead();
        return true;
    }
    return startPreviewPlayhead();
}

QString SourceEditorWidget::previewObjectName() const
{
    const AssetInfo* asset = sourceId_ == 0
        ? nullptr : AssetRegistry::instance().getAsset(sourceId_);
    return auditionObjectName(asset);
}

std::optional<uint64_t> SourceEditorWidget::previewPlayheadFrame() const
{
    return waveformView_->playheadFrame();
}

bool SourceEditorWidget::isPreviewPlayheadActive() const
{
    return waveformView_->isPlayheadActive();
}

void SourceEditorWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateResponsiveLayouts(event->size().width() - 16);
}

const SoundSourceDef* SourceEditorWidget::boundSource() const
{
    if (sourceId_ == 0 || soundGuid_.isNull() || entryId_.isNull())
        return nullptr;
    const ProjectItem* item = ProjectManager::instance().findItemByGuid(soundGuid_);
    const auto* sound = item && item->type == ProjectItem::Type::Sound
        ? item->comp<VSound>() : nullptr;
    if (!sound)
        return nullptr;
    const auto* source = SoundSourceSemantics::findEntry(*sound, entryId_);
    return source && source->sourceId == sourceId_ ? source : nullptr;
}

bool SourceEditorWidget::referenceIsDangling() const
{
    return !soundGuid_.isNull() && !entryId_.isNull() && boundSource() == nullptr;
}

void SourceEditorWidget::setDisplayedSource(const SoundSourceDef& source)
{
    refreshingDisplayedSource_ = true;
    waveformView_->setSource(source);
    refreshingDisplayedSource_ = false;
}

void SourceEditorWidget::refreshMetadata()
{
    const bool hasSource = sourceId_ != 0;
    const AssetInfo* asset = hasSource ? AssetRegistry::instance().getAsset(sourceId_) : nullptr;
    const bool hasReferenceContext = !soundGuid_.isNull() && !entryId_.isNull();
    const SoundSourceDef* source = boundSource();
    const bool dangling = hasReferenceContext && !source;
    const bool hasAsset = asset != nullptr && !dangling;
    if (dangling)
        stopPreviewPlayhead();

    refreshSourceIdentity(asset);
    sourceIdentityStrip_->setVisible(hasSource);
    metadataStrip_->setVisible(hasAsset);
    rangePanel_->setVisible(hasAsset);
    auditionStrip_->setVisible(hasAsset);
    waveformView_->setVisible(hasAsset);

    SoundSourceDef displaySource;
    if (source) {
        displaySource = *source;
    } else if (hasAsset) {
        displaySource.sourceId = asset->sourceID;
        displaySource.rangeContentHash = asset->contentHash;
        displaySource.loopRegion = SoundSourceSemantics::fileLoopRegion(asset);
        if (displaySource.loopRegion)
            displaySource.loopProvenance = SoundLoopProvenance::File;
    }
    waveformView_->setEditable(false);
    setDisplayedSource(displaySource);
    waveformView_->setAsset(asset);
    refreshRangePresentation(displaySource, asset, hasReferenceContext);

    emptyStateLabel_->setVisible(hasSource && !hasAsset);
    emptyStateLabel_->setText(!hasSource ? QString()
                              : !asset   ? tr("Audio source is unavailable")
                                         : tr("This audio reference no longer exists"));
    refreshAuditionControls();
    if (!hasAsset)
        return;

    const AudioMetadata& metadata = asset->metadata;
    formatValue_->setText(metadata.originalFormat.isEmpty() ? tr("Unknown")
                                                            : metadata.originalFormat);
    sampleRateValue_->setText(metadata.sampleRate == 0
                                  ? tr("Unknown")
                                  : tr("%1 Hz").arg(metadata.sampleRate));
    channelsValue_->setText(metadata.channels == 0
                                ? tr("Unknown") : QString::number(metadata.channels));
    bitDepthValue_->setText(metadata.bitDepth == 0
                                ? tr("Unknown") : tr("%1-bit").arg(metadata.bitDepth));
    fileSizeValue_->setText(formatFileSize(metadata.fileSize));
    durationValue_->setText(formatDuration(metadata.durationSec));
    embeddedLoopValue_->setText(formatEmbeddedLoop(metadata));
    peakValue_->setText(formatLevel(metadata.validation.peakDb, QStringLiteral("dBFS")));
    lufsValue_->setText(formatLevel(metadata.validation.integratedLUFS,
                                    QStringLiteral("LUFS"), -100.0f));
    updateResponsiveLayouts(width() - 16);
}

void SourceEditorWidget::refreshSourceIdentity(const AssetInfo* asset)
{
    if (sourceId_ == 0) {
        sourceTitle_->clear();
        sourceContext_->clear();
        return;
    }
    const QString path = asset
        ? (!asset->relativePath.isEmpty() ? asset->relativePath : asset->absolutePath)
        : QString{};
    sourceTitle_->setText(path.isEmpty()
                              ? tr("Audio Source %1").arg(sourceId_)
                              : QFileInfo(path).fileName());
    QStringList context;
    if (const ProjectItem* item = ProjectManager::instance().findItemByGuid(soundGuid_))
        context.push_back(item->name);
    if (!path.isEmpty() && QFileInfo(path).fileName() != path)
        context.push_back(path);
    context.push_back(tr("Source %1").arg(sourceId_));
    sourceContext_->setText(context.join(QStringLiteral(" · ")));
    sourceTitle_->setToolTip(asset ? asset->absolutePath : QString{});
    sourceContext_->setToolTip(asset ? asset->absolutePath : QString{});
}

void SourceEditorWidget::refreshRangePresentation(const SoundSourceDef& source,
                                                 const AssetInfo* asset,
                                                 bool hasReferenceContext)
{
    if (!asset) {
        rangeSourceValue_->clear();
        integrityWarning_->clear();
        integrityWarning_->hide();
        return;
    }
    const uint64_t totalFrames = asset->metadata.totalSamples;
    rangeSourceValue_->setText(tr("File defaults · Read-only"));

    trimStartValue_->setText(formatRangeValue(source.trimStartFrame,
                                               asset->metadata.sampleRate));
    trimEndValue_->setText(formatRangeValue(source.trimEndFrame.value_or(totalFrames),
                                             asset->metadata.sampleRate));
    loopStartValue_->setText(source.loopRegion
        ? formatRangeValue(source.loopRegion->startFrame, asset->metadata.sampleRate)
        : tr("Off"));
    loopEndValue_->setText(source.loopRegion
        ? formatRangeValue(source.loopRegion->endFrame, asset->metadata.sampleRate)
        : tr("Off"));
    const QString warning = hasReferenceContext
        ? SoundSourceSemantics::integrityText(source, asset) : QString{};
    integrityWarning_->setText(warning);
    integrityWarning_->setVisible(!warning.isEmpty());
}

void SourceEditorWidget::applyAuditionTheme()
{
    auto& manager = ThemeManager::instance();
    const auto theme = manager.snapshot();
    const auto& colors = theme->ui.colors;
    const ThemeAssetVariant playVariant{
        playButton_->iconSize(), playButton_->devicePixelRatioF(), 0, colors.auditionPlayActive};
    playButton_->setIcon(manager.resolveIcon(
        QString::fromLatin1(ThemeAssetIds::AuditionPlay), playVariant).icon);
    const ThemeAssetVariant stopVariant{
        stopButton_->iconSize(), stopButton_->devicePixelRatioF(), 0, colors.auditionStopActive};
    stopButton_->setIcon(manager.resolveIcon(
        QString::fromLatin1(ThemeAssetIds::AuditionStop), stopVariant).icon);
}

void SourceEditorWidget::refreshAuditionControls()
{
    const bool hasSlice = previewPcmSlice().has_value();
    const bool playing = isPreviewPlayheadActive();
    playButton_->setEnabled(hasSlice && !playing);
    stopButton_->setEnabled(playing);
    playButton_->setProperty("playing", playing);
    playButton_->style()->unpolish(playButton_);
    playButton_->style()->polish(playButton_);
}

void SourceEditorWidget::updateResponsiveLayouts(int availableWidth)
{
    int metadataColumns = 1;
    if (availableWidth >= 920)
        metadataColumns = 5;
    else if (availableWidth >= 620)
        metadataColumns = 3;
    else if (availableWidth >= 380)
        metadataColumns = 2;
    if (metadataStrip_->property("columnCount").toInt() != metadataColumns) {
        while (QLayoutItem* item = metadataLayout_->takeAt(0))
            delete item;
        for (int column = 0; column < 10; ++column)
            metadataLayout_->setColumnStretch(column, 0);
        for (size_t index = 0; index < metadataFields_.size(); ++index) {
            const int row = static_cast<int>(index) / metadataColumns;
            const int column = static_cast<int>(index) % metadataColumns;
            metadataLayout_->addWidget(metadataFields_[index].first, row, column * 2,
                                       Qt::AlignRight | Qt::AlignVCenter);
            metadataLayout_->addWidget(metadataFields_[index].second, row, column * 2 + 1);
            metadataLayout_->setColumnStretch(column * 2 + 1, 1);
        }
        metadataStrip_->setProperty("columnCount", metadataColumns);
    }

    const int rangeColumns = availableWidth >= 540 ? 4 : 2;
    if (rangeValues_->property("columnCount").toInt() != rangeColumns) {
        for (int index = 0; index < static_cast<int>(rangeValueLabels_.size()); ++index) {
            rangeValuesLayout_->setColumnStretch(index, index < rangeColumns ? 1 : 0);
            QLabel* title = rangeTitleLabels_[index];
            rangeValuesLayout_->removeWidget(title);
            rangeValuesLayout_->removeWidget(rangeValueLabels_[index]);
            const int row = index / rangeColumns;
            const int column = index % rangeColumns;
            rangeValuesLayout_->addWidget(title, row * 2, column);
            rangeValuesLayout_->addWidget(rangeValueLabels_[index], row * 2 + 1, column);
        }
        rangeValues_->setProperty("columnCount", rangeColumns);
    }
}

}
