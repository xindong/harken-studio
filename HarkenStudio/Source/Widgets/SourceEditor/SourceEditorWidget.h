#pragma once

#include "Utils/ObjectId.h"

#include "Core/SoundSourceSemantics.h"

#include <QPointer>
#include <QUuid>
#include <QWidget>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

class QGridLayout;
class QLabel;
class QResizeEvent;
class QToolButton;

namespace HarkenEditor {
class AuditionManager;
}

namespace VNX {

struct AssetInfo;
class SourceEditorWaveformView;

class SourceEditorWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit SourceEditorWidget(QWidget* parent = nullptr);
    ~SourceEditorWidget() override;

    uint32_t sourceId() const { return sourceId_; }
    QUuid soundGuid() const { return soundGuid_; }
    QUuid entryId() const { return entryId_; }
    void bindSource(uint32_t sourceId, const QUuid& soundGuid = {}, const QUuid& entryId = {});
    void clearSource();
    void setAuditionManager(HarkenEditor::AuditionManager* audition);
    uint32_t auditionPlayingId() const { return auditionPlayingId_; }
    std::optional<SoundSourceSemantics::PcmSlice> previewPcmSlice(uint64_t startFrame = 0) const;
    std::optional<SoundSourceSemantics::PcmSlice> previewLoopPcmSlice(uint64_t startFrame = 0) const;
    bool startPreviewPlayhead(uint64_t startFrame = 0);
    void stopPreviewPlayhead();
    bool handlePlayStopShortcut();
    QString previewObjectName() const;
    std::optional<uint64_t> previewPlayheadFrame() const;
    bool isPreviewPlayheadActive() const;
    harkenstudio::ObjectId previewSoundId() const { return previewSoundId_; }
    uint64_t previewPlaybackHandle() const { return previewPlaybackHandle_; }

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    const SoundSourceDef* boundSource() const;
    bool referenceIsDangling() const;
    void setDisplayedSource(const SoundSourceDef& source);
    void refreshMetadata();
    void refreshSourceIdentity(const AssetInfo* asset);
    void refreshRangePresentation(const SoundSourceDef& source, const AssetInfo* asset,
                                  bool hasReferenceContext);
    void applyAuditionTheme();
    void refreshAuditionControls();
    void updateResponsiveLayouts(int availableWidth);
    QString auditionObjectName(const AssetInfo* asset) const;
    void retireRawFileAudition();
    bool registerRawFileAudition(const AssetInfo* asset);

    uint32_t sourceId_ = 0;
    QUuid soundGuid_;
    QUuid entryId_;
    harkenstudio::ObjectId previewSoundId_{};
    uint64_t previewPlaybackHandle_ = 0;
    uint32_t auditionPlayingId_ = 0;
    QPointer<HarkenEditor::AuditionManager> audition_;
    bool refreshingDisplayedSource_ = false;
    QWidget* sourceIdentityStrip_ = nullptr;
    QLabel* sourceTitle_ = nullptr;
    QLabel* sourceContext_ = nullptr;
    QWidget* metadataStrip_ = nullptr;
    QGridLayout* metadataLayout_ = nullptr;
    std::vector<std::pair<QLabel*, QLabel*>> metadataFields_;
    QLabel* formatValue_ = nullptr;
    QLabel* sampleRateValue_ = nullptr;
    QLabel* channelsValue_ = nullptr;
    QLabel* bitDepthValue_ = nullptr;
    QLabel* fileSizeValue_ = nullptr;
    QLabel* durationValue_ = nullptr;
    QLabel* embeddedLoopValue_ = nullptr;
    QLabel* peakValue_ = nullptr;
    QLabel* lufsValue_ = nullptr;
    QWidget* rangePanel_ = nullptr;
    QLabel* rangeSourceValue_ = nullptr;
    QWidget* rangeValues_ = nullptr;
    QGridLayout* rangeValuesLayout_ = nullptr;
    std::vector<QLabel*> rangeTitleLabels_;
    std::vector<QLabel*> rangeValueLabels_;
    QLabel* trimStartValue_ = nullptr;
    QLabel* loopStartValue_ = nullptr;
    QLabel* loopEndValue_ = nullptr;
    QLabel* trimEndValue_ = nullptr;
    QLabel* integrityWarning_ = nullptr;
    QWidget* auditionStrip_ = nullptr;
    QToolButton* playButton_ = nullptr;
    QToolButton* stopButton_ = nullptr;
    SourceEditorWaveformView* waveformView_ = nullptr;
    QLabel* emptyStateLabel_ = nullptr;
};

}
