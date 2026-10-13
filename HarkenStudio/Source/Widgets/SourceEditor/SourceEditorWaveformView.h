#pragma once

#include "Core/AssetPipeline/AssetTypes.h"
#include "Core/AssetPipeline/WaveformCache.h"
#include "Core/Components/VSound.h"
#include "Core/SoundSourceSemantics.h"
#include "Widgets/Common/AxisCrosshair.h"

#include <QElapsedTimer>
#include <QPoint>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <optional>

class QDialog;
class QLabel;
class QPainter;
class QScrollBar;
class QToolButton;

namespace VNX {

struct TimeAxisTicks;

class SourceEditorWaveformView final : public QWidget
{
    Q_OBJECT

public:
    enum class Presentation { Preview, TrimEditor };

    explicit SourceEditorWaveformView(QWidget* parent = nullptr,
                                     Presentation presentation = Presentation::Preview);

    void setAsset(const AssetInfo* asset);
    void setSource(const SoundSourceDef& source);
    void setEditable(bool editable);
    void setCommitHandler(std::function<void(const SoundSourceDef&)> handler);
    void startPlayhead(uint64_t startFrame, uint64_t windowStartFrame,
                       uint64_t windowFrameCount, uint32_t sampleRate, bool loops);
    void stopPlayhead();
    std::optional<uint64_t> playheadFrame() const;
    bool isPlayheadActive() const { return playhead_.playing; }
    const SoundSourceDef& previewSource() const { return previewSource_; }
    int handlePosition(int handle) const;
    QPoint handleHitPoint(int handle) const;
    int playheadPosition() const;
    bool hasWaveform() const { return peaks_.isValid(); }
    bool hasBandData() const { return peaks_.hasBandData(); }
    bool isGenerating() const { return state_ == State::Generating; }
    QSize sizeHint() const override;
    QRect waveformRect() const;
    AxisCrosshair crosshair() const;
    double amplitudeExponent() const { return amplitudeExponent_; }
    void setAmplitudeExponent(double exponent);
    double viewStartFrame() const { return viewStart_; }
    double viewFrameCount() const;
    void setViewRange(double startFrame, double frameCount);
    void zoomAround(int x, double factor);

signals:
    void previewChanged();
    void playheadStopped();
    void locateRequested(uint64_t frame);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    enum class Handle { None = -1, TrimStart = 0, LoopStart = 1, LoopEnd = 2, TrimEnd = 3 };

    enum class State {
        MissingSource,
        Unavailable,
        Generating,
        Ready
    };

    void setState(State state);
    void loadAsset(const QString& path);
    uint64_t totalFrames() const;
    uint64_t frameAt(int x) const;
    uint64_t handleFrame(Handle handle) const;
    QRect trimGripRect(Handle handle) const;
    Handle hitTestHandle(const QPoint& position) const;
    void updateHover(const QPoint& position);
    QString handleTooltip(Handle handle) const;
    void updatePreview(uint64_t frame);
    void canonicalizeActiveHandle();
    void setupTimeline();
    void layoutTimeline();
    void refreshTimelineControls();
    void zoomVertically(int y, double factor);
    QRect channelViewportRect() const;
    QRect channelWaveformRect() const;
    QRect crosshairRect() const;
    int channelContentHeight() const;
    void paintTimeline(QPainter& painter);
    void requestDetail();
    void showHandleValue();
    void editHandleValue(Handle handle);
    void cancelGesture();
    void dragHandle(const QPoint& position);
    int positionAt(double frame) const;
    int secondsDecimals() const;
    TimeAxisTicks timeAxisTicks() const;
    QString formatFrame(uint64_t frame) const;
    bool isTimeRuler(const QPoint& position) const;
    bool isPanArea(const QPoint& position) const;

    QString path_;
    WaveformPeakData peaks_;
    std::optional<AssetInfo> asset_;
    SoundSourceDef originalSource_;
    SoundSourceDef previewSource_;
    SoundSourceSemantics::PlayheadExtrapolation playhead_;
    QElapsedTimer playheadClock_;
    QTimer playheadTick_;
    Handle activeHandle_ = Handle::None;
    Handle hoveredHandle_ = Handle::None;
    std::optional<QPoint> hoverPosition_;
    bool editable_ = false;
    std::function<void(const SoundSourceDef&)> commit_;
    State state_ = State::MissingSource;
    quint64 requestRevision_ = 0;
    Presentation presentation_;
    double viewStart_ = 0.0;
    double viewCount_ = 0.0;
    double verticalZoom_ = 1.0;
    double amplitudeExponent_ = 1.0;
    bool frameDisplay_ = false;
    std::optional<QPoint> panOrigin_;
    Qt::MouseButton panButton_ = Qt::NoButton;
    double panStart_ = 0.0;
    std::optional<int> panVerticalStart_;
    int dragOffset_ = 0;
    QPoint dragPosition_;
    bool dragMoved_ = false;
    QTimer edgeScroll_;
    QToolButton* units_ = nullptr;
    QScrollBar* scroll_ = nullptr;
    QScrollBar* channelScroll_ = nullptr;
    QToolButton* zoomOut_ = nullptr;
    QToolButton* zoomIn_ = nullptr;
    QToolButton* verticalZoomIn_ = nullptr;
    QToolButton* verticalZoomOut_ = nullptr;
    QLabel* handleValue_ = nullptr;
    QPointer<QDialog> valueEditor_;
    WaveformPeakData detail_;
    uint64_t detailStart_ = 0;
    uint64_t detailCount_ = 0;
    QTimer detailTimer_;
    bool detailLoading_ = false;
};

}
