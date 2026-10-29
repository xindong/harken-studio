#include "SourceEditorWaveformView.h"
#include "Core/AssetPipeline/MediaPool.h"

#include "Core/SoundSourceSemantics.h"
#include "Core/NumericFormat.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/TimeAxis.h"
#include "Widgets/Common/AmplitudeAxis.h"
#include "Widgets/Common/WaveformRenderer.h"

#include <QFileInfo>
#include <QFutureWatcher>
#include <QDialog>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSizePolicy>
#include <QTimer>
#include <QToolTip>

#include <algorithm>
#include <cmath>
#include <vector>

namespace VNX {

namespace {

constexpr int kWaveformInset = 8;
constexpr int kHandleHitRadius = 8;
constexpr int kHandleBandHeight = 18;

bool isTrimHandle(int handle)
{
    return handle == 0 || handle == 3;
}

}

SourceEditorWaveformView::SourceEditorWaveformView(QWidget* parent, Presentation presentation)
    : QWidget(parent)
    , presentation_(presentation)
{
    setObjectName(QStringLiteral("sourceEditorWaveform"));
    setMinimumHeight(160);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    playheadTick_.setInterval(16);
    connect(&playheadTick_, &QTimer::timeout, this, [this]() {
        if (!playheadFrame()) {
            stopPlayhead();
            return;
        }
        update();
    });
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, [this]() { refreshTimelineControls(); update(); });
    connect(&MediaPool::instance(), &MediaPool::waveformReady, this, [this](uint32_t sourceID) {
        if (asset_ && asset_->sourceID == sourceID && WaveformCache::load(path_, peaks_)) {
            ++requestRevision_;
            setState(State::Ready);
        }
    });
    if (presentation_ == Presentation::TrimEditor)
        setupTimeline();
}

void SourceEditorWaveformView::setAsset(const AssetInfo* asset)
{
    const bool sameContent = asset_ && asset
        && asset_->sourceID == asset->sourceID
        && asset_->contentHash == asset->contentHash
        && asset_->lastModified == asset->lastModified
        && asset_->metadata.fileSize == asset->metadata.fileSize
        && asset_->metadata.totalSamples == asset->metadata.totalSamples
        && asset_->metadata.sampleRate == asset->metadata.sampleRate
        && asset_->metadata.channels == asset->metadata.channels;
    if (!sameContent) {
        cancelGesture();
        detailTimer_.stop();
        detail_ = {};
        detailCount_ = 0;
        viewStart_ = 0.0;
        viewCount_ = 0.0;
        verticalZoom_ = 1.0;
        if (channelScroll_)
            channelScroll_->setValue(0);
    }
    asset_ = asset ? std::optional<AssetInfo>{*asset} : std::nullopt;
    refreshTimelineControls();
    const QString nextPath = asset ? asset->absolutePath : QString{};
    if (!asset) {
        if (state_ != State::MissingSource) {
            ++requestRevision_;
            path_.clear();
            peaks_ = {};
            setState(State::MissingSource);
        }
        return;
    }
    if (nextPath == path_ && sameContent && QFileInfo(nextPath).isFile()) {
        if (state_ == State::Generating || state_ == State::Ready
            || (state_ == State::Unavailable && nextPath.isEmpty())) {
            update();
            return;
        }
    }

    ++requestRevision_;
    path_.clear();
    peaks_ = {};

    if (nextPath.isEmpty() || !QFileInfo::exists(nextPath)) {
        setState(State::Unavailable);
        return;
    }

    path_ = nextPath;
    loadAsset(path_);
}

void SourceEditorWaveformView::setSource(const SoundSourceDef& source)
{
    const bool differentEntry = source.entryId != originalSource_.entryId
        || source.sourceId != originalSource_.sourceId;
    if (!differentEntry && source == originalSource_ && activeHandle_ != Handle::None)
        return;
    cancelGesture();
    originalSource_ = source;
    previewSource_ = source;
    activeHandle_ = Handle::None;
    hoveredHandle_ = Handle::None;
    unsetCursor();
    if (differentEntry) {
        verticalZoom_ = 1.0;
        if (channelScroll_)
            channelScroll_->setValue(0);
        setViewRange(0.0, double(totalFrames()));
    }
    refreshTimelineControls();
    update();
    emit previewChanged();
}

void SourceEditorWaveformView::setEditable(bool editable)
{
    editable_ = editable;
    if (!editable_) {
        cancelGesture();
        activeHandle_ = Handle::None;
        hoveredHandle_ = Handle::None;
        unsetCursor();
    }
    update();
}

void SourceEditorWaveformView::setAmplitudeExponent(double exponent)
{
    amplitudeExponent_ = std::clamp(exponent, 0.0, 1.0);
    update();
}

void SourceEditorWaveformView::setCommitHandler(
    std::function<void(const SoundSourceDef&)> handler)
{
    commit_ = std::move(handler);
}

void SourceEditorWaveformView::startPlayhead(uint64_t startFrame, uint64_t windowStartFrame,
                                            uint64_t windowFrameCount, uint32_t sampleRate,
                                            bool loops)
{
    playhead_.startFrame = startFrame;
    playhead_.windowStartFrame = windowStartFrame;
    playhead_.windowFrameCount = windowFrameCount;
    playhead_.sampleRate = sampleRate;
    playhead_.loops = loops;
    playhead_.playing = true;
    playheadClock_.restart();
    if (!SoundSourceSemantics::extrapolatedPlayheadFrame(playhead_, 0)) {
        stopPlayhead();
        return;
    }
    if (!playheadTick_.isActive())
        playheadTick_.start();
    update();
}

void SourceEditorWaveformView::stopPlayhead()
{
    if (!playhead_.playing && !playheadTick_.isActive())
        return;
    playhead_ = {};
    playheadTick_.stop();
    update();
    emit playheadStopped();
}

std::optional<uint64_t> SourceEditorWaveformView::playheadFrame() const
{
    if (!playhead_.playing)
        return std::nullopt;
    return SoundSourceSemantics::extrapolatedPlayheadFrame(
        playhead_, playheadClock_.isValid() ? playheadClock_.nsecsElapsed() : 0);
}

int SourceEditorWaveformView::playheadPosition() const
{
    const auto frame = playheadFrame();
    const uint64_t total = totalFrames();
    if (!frame || total == 0)
        return -1;
    if (presentation_ == Presentation::TrimEditor)
        return positionAt(double(*frame));
    const QRect waveformRect = rect().adjusted(kWaveformInset, kWaveformInset,
                                               -kWaveformInset, -kWaveformInset);
    const double ratio = std::clamp(
        static_cast<double>(*frame) / static_cast<double>(total),
        0.0, 1.0);
    return waveformRect.left()
        + static_cast<int>(std::lround(ratio * std::max(1, waveformRect.width() - 1)));
}

int SourceEditorWaveformView::handlePosition(int handle) const
{
    const uint64_t total = totalFrames();
    if (total == 0)
        return -1;
    const Handle typedHandle = static_cast<Handle>(handle);
    if (presentation_ == Presentation::TrimEditor) {
        if (!isTrimHandle(handle))
            return -1;
        return positionAt(double(std::min(handleFrame(typedHandle), total)));
    }
    const QRect waveformRect = rect().adjusted(kWaveformInset, kWaveformInset,
                                               -kWaveformInset, -kWaveformInset);
    const double ratio = std::clamp(
        static_cast<double>(handleFrame(typedHandle)) / static_cast<double>(total),
        0.0, 1.0);
    return waveformRect.left()
        + static_cast<int>(std::lround(ratio * std::max(1, waveformRect.width() - 1)));
}

QPoint SourceEditorWaveformView::handleHitPoint(int handle) const
{
    const int x = handlePosition(handle);
    if (x < 0)
        return {-1, -1};
    if (presentation_ == Presentation::TrimEditor)
        return trimGripRect(static_cast<Handle>(handle)).center();
    return {x, isTrimHandle(handle) ? kWaveformInset + 6 : height() - kWaveformInset - 7};
}

QSize SourceEditorWaveformView::sizeHint() const
{
    return {640, 240};
}

void SourceEditorWaveformView::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    const ThemeSnapshotPtr theme = ThemeManager::instance().snapshot();
    painter.fillRect(rect(), theme->ui.colors.bgPrimary);

    if (presentation_ == Presentation::TrimEditor) {
        paintTimeline(painter);
        drawAxisCrosshair(painter, theme->domains.curve, crosshairRect(), crosshair());
        painter.setPen(theme->ui.colors.viewBorder);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
        return;
    }

    painter.setPen(theme->ui.colors.viewBorder);
    painter.drawRect(rect().adjusted(0, 0, -1, -1));

    if (state_ == State::Ready && peaks_.isValid()) {
        const QRect waveformRect = rect().adjusted(kWaveformInset, kWaveformInset,
                                                   -kWaveformInset, -kWaveformInset);
        WaveformRenderOptions options;
        options.colorMode = WaveColorMode::MultiBand;
        options.amplitudeExponent = amplitudeExponent_;
        WaveformRenderer::render(painter, waveformRect, peaks_, theme->domains.waveform, options);
        WaveformRenderer::drawDbScale(painter, waveformRect, theme->domains.waveform, amplitudeExponent_);
        if (totalFrames() != 0) {
            const int trimStart = handlePosition(static_cast<int>(Handle::TrimStart));
            const int trimEnd = handlePosition(static_cast<int>(Handle::TrimEnd));
            QColor trimmedColor = theme->ui.colors.bgPrimary;
            trimmedColor.setAlpha(178);
            if (trimStart > waveformRect.left())
                painter.fillRect(QRect(waveformRect.left(), waveformRect.top(),
                                       trimStart - waveformRect.left(), waveformRect.height()),
                                 trimmedColor);
            if (trimEnd < waveformRect.right())
                painter.fillRect(QRect(trimEnd, waveformRect.top(),
                                       waveformRect.right() - trimEnd + 1,
                                       waveformRect.height()), trimmedColor);

            if (previewSource_.loopRegion) {
                const int loopStart = handlePosition(static_cast<int>(Handle::LoopStart));
                const int loopEnd = handlePosition(static_cast<int>(Handle::LoopEnd));
                QColor loopFill = theme->domains.waveform.loopHandle;
                loopFill.setAlpha(34);
                painter.fillRect(QRect(loopStart, waveformRect.top(),
                                       std::max(1, loopEnd - loopStart),
                                       waveformRect.height()), loopFill);
            }

            const auto drawHandle = [&](Handle handle, const QColor& color) {
                const int rawHandle = static_cast<int>(handle);
                const int x = handlePosition(rawHandle);
                if (x < 0)
                    return;
                QColor lineColor = color;
                if (handle == hoveredHandle_ || handle == activeHandle_)
                    lineColor = lineColor.lighter(135);
                painter.setPen(QPen(lineColor, handle == activeHandle_ ? 3 : 2));
                painter.drawLine(x, waveformRect.top(), x, waveformRect.bottom());
                painter.setPen(Qt::NoPen);
                painter.setBrush(lineColor);
                const bool trim = isTrimHandle(rawHandle);
                const QPolygon marker = trim
                    ? QPolygon{{x - 5, waveformRect.top()}, {x + 5, waveformRect.top()},
                               {x, waveformRect.top() + 7}}
                    : QPolygon{{x - 5, waveformRect.bottom()}, {x + 5, waveformRect.bottom()},
                               {x, waveformRect.bottom() - 7}};
                painter.drawPolygon(marker);
            };
            drawHandle(Handle::TrimStart, theme->domains.waveform.trimHandle);
            drawHandle(Handle::TrimEnd, theme->domains.waveform.trimHandle);
            if (previewSource_.loopRegion) {
                drawHandle(Handle::LoopStart, theme->domains.waveform.loopHandle);
                drawHandle(Handle::LoopEnd, theme->domains.waveform.loopHandle);
            }
            const int playheadX = playheadPosition();
            if (playheadX >= 0) {
                painter.setPen(QPen(theme->domains.waveform.previewPlayhead, 1));
                painter.drawLine(playheadX, waveformRect.top(),
                                 playheadX, waveformRect.bottom());
            }
        }
        drawAxisCrosshair(painter, theme->domains.curve, crosshairRect(), crosshair());
        return;
    }

    QString message;
    switch (state_) {
    case State::MissingSource:
        return;
    case State::Unavailable:
        message = tr("Waveform unavailable");
        break;
    case State::Generating:
        message = tr("Generating waveform…");
        break;
    case State::Ready:
        message = tr("Waveform unavailable");
        break;
    }
    painter.setPen(theme->ui.colors.textMuted);
    painter.drawText(rect(), Qt::AlignCenter, message);
}

void SourceEditorWaveformView::mousePressEvent(QMouseEvent* event)
{
    const QPoint position = event->position().toPoint();
    if (isPanArea(position)
        && (event->button() == Qt::MiddleButton
            || (event->button() == Qt::LeftButton && hitTestHandle(position) == Handle::None))) {
        setFocus();
        panOrigin_ = position;
        panButton_ = event->button();
        panStart_ = viewStart_;
        panVerticalStart_.reset();
        if (waveformRect().contains(position))
            panVerticalStart_ = channelScroll_->value();
        hoveredHandle_ = Handle::None;
        QToolTip::hideText();
        setCursor(isTimeRuler(position) ? Qt::ClosedHandCursor : Qt::ArrowCursor);
        update();
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton || totalFrames() == 0)
        return;

    if (editable_) {
        const Handle hit = hitTestHandle(event->position().toPoint());
        if (hit != Handle::None) {
            setFocus();
            originalSource_ = previewSource_;
            activeHandle_ = hit;
            hoveredHandle_ = hit;
            if (presentation_ == Presentation::TrimEditor) {
                dragOffset_ = event->position().toPoint().x() - handlePosition(static_cast<int>(hit));
                dragPosition_ = event->position().toPoint();
                dragMoved_ = false;
                QToolTip::hideText();
                showHandleValue();
                update();
            } else {
                updatePreview(frameAt(event->position().toPoint().x()));
            }
            return;
        }
    }

    setFocus();
    emit locateRequested(frameAt(event->position().toPoint().x()));
}

void SourceEditorWaveformView::mouseMoveEvent(QMouseEvent* event)
{
    if (panOrigin_) {
        const double delta = (event->position().x() - panOrigin_->x())
            / std::max(1, waveformRect().width() - 1) * viewFrameCount();
        setViewRange(panStart_ - delta, viewFrameCount());
        if (panVerticalStart_)
            channelScroll_->setValue(*panVerticalStart_ - (event->position().toPoint().y() - panOrigin_->y()));
        return;
    }
    if (activeHandle_ != Handle::None && (event->buttons() & Qt::LeftButton)) {
        if (presentation_ == Presentation::TrimEditor)
            dragHandle(event->position().toPoint());
        else
            updatePreview(frameAt(event->position().toPoint().x()));
        return;
    }
    updateHover(event->position().toPoint());
}

void SourceEditorWaveformView::mouseReleaseEvent(QMouseEvent* event)
{
    if (panOrigin_ && event->button() == panButton_) {
        panOrigin_.reset();
        panVerticalStart_.reset();
        panButton_ = Qt::NoButton;
        updateHover(event->position().toPoint());
        return;
    }
    if (event->button() != Qt::LeftButton || activeHandle_ == Handle::None)
        return;
    if (presentation_ == Presentation::TrimEditor) {
        if (dragMoved_)
            dragHandle(event->position().toPoint());
        edgeScroll_.stop();
        handleValue_->hide();
    } else {
        updatePreview(frameAt(event->position().toPoint().x()));
    }
    activeHandle_ = Handle::None;
    updateHover(event->position().toPoint());
    if (previewSource_ != originalSource_) {
        originalSource_ = previewSource_;
        if (commit_)
            commit_(previewSource_);
    }
    update();
}

void SourceEditorWaveformView::leaveEvent(QEvent* event)
{
    hoverPosition_.reset();
    if (activeHandle_ == Handle::None && !panOrigin_) {
        hoveredHandle_ = Handle::None;
        unsetCursor();
        update();
    }
    QWidget::leaveEvent(event);
}

void SourceEditorWaveformView::setState(State state)
{
    state_ = state;
    setProperty("waveformState", [&]() {
        switch (state_) {
        case State::MissingSource: return QStringLiteral("missingSource");
        case State::Unavailable: return QStringLiteral("unavailable");
        case State::Generating: return QStringLiteral("generating");
        case State::Ready: return QStringLiteral("ready");
        }
        return QString{};
    }());
    refreshTimelineControls();
    if (state == State::Ready && presentation_ == Presentation::TrimEditor)
        detailTimer_.start();
    update();
}

void SourceEditorWaveformView::loadAsset(const QString& path)
{
    if (WaveformCache::load(path, peaks_)) {
        setState(State::Ready);
        return;
    }

    setState(State::Generating);
    const quint64 revision = requestRevision_;
    auto* watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher, revision, path]() {
        watcher->deleteLater();
        if (revision != requestRevision_ || path != path_)
            return;

        peaks_ = {};
        setState(WaveformCache::load(path, peaks_)
                     ? State::Ready
                     : State::Unavailable);
        if (state_ == State::Ready && asset_)
            emit MediaPool::instance().waveformReady(asset_->sourceID);
    });
    watcher->setFuture(WaveformCache::generateAsync(path));
}

uint64_t SourceEditorWaveformView::totalFrames() const
{
    return asset_ ? asset_->metadata.totalSamples : 0;
}

uint64_t SourceEditorWaveformView::frameAt(int x) const
{
    const uint64_t total = totalFrames();
    if (total == 0)
        return 0;
    if (presentation_ == Presentation::TrimEditor) {
        const double ratio = std::clamp(double(x - waveformRect().left())
            / std::max(1, waveformRect().width() - 1), 0.0, 1.0);
        return static_cast<uint64_t>(std::llround(std::clamp(
            viewStart_ + ratio * viewFrameCount(), 0.0, double(total))));
    }
    const QRect waveformRect = rect().adjusted(kWaveformInset, kWaveformInset,
                                               -kWaveformInset, -kWaveformInset);
    const double ratio = std::clamp(
        static_cast<double>(x - waveformRect.left())
            / std::max(1, waveformRect.width() - 1),
        0.0, 1.0);
    return static_cast<uint64_t>(std::llround(ratio * static_cast<double>(total)));
}

uint64_t SourceEditorWaveformView::handleFrame(Handle handle) const
{
    const uint64_t total = totalFrames();
    switch (handle) {
    case Handle::TrimStart:
        return previewSource_.trimStartFrame;
    case Handle::LoopStart:
        return previewSource_.loopRegion
            ? previewSource_.loopRegion->startFrame : previewSource_.trimStartFrame;
    case Handle::LoopEnd:
        return previewSource_.loopRegion
            ? previewSource_.loopRegion->endFrame
            : previewSource_.trimEndFrame.value_or(total);
    case Handle::TrimEnd:
        return previewSource_.trimEndFrame.value_or(total);
    case Handle::None:
        return 0;
    }
    return 0;
}

SourceEditorWaveformView::Handle SourceEditorWaveformView::hitTestHandle(
    const QPoint& position) const
{
    if (!editable_ || totalFrames() == 0)
        return Handle::None;
    if (presentation_ == Presentation::TrimEditor) {
        const QRect plot = waveformRect();
        if (position.y() < plot.top() || position.y() > plot.bottom())
            return Handle::None;
        Handle nearest = Handle::None;
        int nearestDistance = 13;
        for (Handle handle : {Handle::TrimStart, Handle::TrimEnd}) {
            const int x = handlePosition(static_cast<int>(handle));
            if (x < 0)
                continue;
            const QRect bounds = trimGripRect(handle);
            const int center = bounds.center().x();
            const bool grip = position.y() <= bounds.bottom() + 2;
            const int distance = std::abs(position.x() - (grip ? center : x));
            if (distance <= (grip ? 10 : 5) && distance < nearestDistance) {
                nearest = handle;
                nearestDistance = distance;
            }
        }
        return nearest;
    }
    const bool trimBand = position.y() <= kWaveformInset + kHandleBandHeight;
    const bool loopBand = position.y() >= height() - kWaveformInset - kHandleBandHeight;
    if (!trimBand && !loopBand)
        return Handle::None;

    const std::vector<Handle> candidates = trimBand
        ? std::vector<Handle>{Handle::TrimStart, Handle::TrimEnd}
        : previewSource_.loopRegion
            ? std::vector<Handle>{Handle::LoopStart, Handle::LoopEnd}
            : std::vector<Handle>{};
    Handle nearest = Handle::None;
    int nearestDistance = kHandleHitRadius + 1;
    for (const Handle handle : candidates) {
        const int distance = std::abs(handlePosition(static_cast<int>(handle)) - position.x());
        if (distance <= kHandleHitRadius && distance < nearestDistance) {
            nearest = handle;
            nearestDistance = distance;
        }
    }
    return nearest;
}

void SourceEditorWaveformView::updateHover(const QPoint& position)
{
    hoverPosition_ = position;
    update();
    const Handle next = hitTestHandle(position);
    if (next != Handle::None)
        setCursor(Qt::SizeHorCursor);
    else if (isTimeRuler(position))
        setCursor(Qt::OpenHandCursor);
    else
        unsetCursor();
    if (next == hoveredHandle_)
        return;
    hoveredHandle_ = next;
    if (hoveredHandle_ == Handle::None) {
        QToolTip::hideText();
    } else {
        QToolTip::showText(mapToGlobal(position), handleTooltip(hoveredHandle_), this);
    }
}

QRect SourceEditorWaveformView::crosshairRect() const
{
    return presentation_ == Presentation::TrimEditor
        ? waveformRect().intersected(channelViewportRect()) : waveformRect();
}

AxisCrosshair SourceEditorWaveformView::crosshair() const
{
    AxisCrosshair result;
    if (!hoverPosition_ || !asset_ || totalFrames() == 0
        || activeHandle_ != Handle::None || hoveredHandle_ != Handle::None
        || panOrigin_ || valueEditor_ || !crosshairRect().contains(*hoverPosition_))
        return result;
    const QRect wave = presentation_ == Presentation::TrimEditor
        ? channelWaveformRect() : waveformRect();
    const double center = wave.top() + wave.height() / 2;
    const auto scale = WaveformRenderer::amplitudeScale(
        ThemeManager::instance().snapshot()->domains.waveform, amplitudeExponent_);
    const double position = std::abs(hoverPosition_->y() - center) / std::max(1, wave.height() / 2);
    const double db = scale.positionToDb(position);
    const uint64_t frame = frameAt(hoverPosition_->x());
    const QString time = formatFrame(frame);
    result.visible = true;
    result.position = *hoverPosition_;
    result.label = QStringLiteral("%1 | %2 dB").arg(time, formatAxisValue(db, 1));
    return result;
}

QString SourceEditorWaveformView::handleTooltip(Handle handle) const
{
    QString name;
    switch (handle) {
    case Handle::TrimStart: name = tr("Trim In"); break;
    case Handle::LoopStart: name = tr("Loop In"); break;
    case Handle::LoopEnd: name = tr("Loop Out"); break;
    case Handle::TrimEnd: name = tr("Trim Out"); break;
    case Handle::None: return {};
    }
    const uint64_t frame = handleFrame(handle);
    const uint32_t sampleRate = asset_ ? asset_->metadata.sampleRate : 0;
    const auto seconds = SoundSourceSemantics::secondsFromFrames(frame, sampleRate);
    return seconds
        ? tr("%1 · %2 s · %3 frames")
              .arg(name, formatAxisValue(*seconds, axisReadoutDecimalsForStep(
                  timeAxisTicks().majorStep / (frameDisplay_ ? sampleRate : 1.0), secondsDecimals())),
                  QString::number(frame))
        : tr("%1 · %2 frames").arg(name, QString::number(frame));
}

void SourceEditorWaveformView::updatePreview(uint64_t frame)
{
    switch (activeHandle_) {
    case Handle::TrimStart:
        previewSource_.trimStartFrame = frame;
        break;
    case Handle::TrimEnd:
        previewSource_.trimEndFrame = frame;
        break;
    case Handle::LoopStart:
        if (previewSource_.loopRegion)
            previewSource_.loopRegion->startFrame = frame;
        break;
    case Handle::LoopEnd:
        if (previewSource_.loopRegion)
            previewSource_.loopRegion->endFrame = frame;
        break;
    case Handle::None:
        return;
    }
    canonicalizeActiveHandle();
    if (presentation_ == Presentation::TrimEditor)
        showHandleValue();
    update();
    emit previewChanged();
}

void SourceEditorWaveformView::canonicalizeActiveHandle()
{
    const uint64_t total = totalFrames();
    if (total == 0)
        return;

    uint64_t trimStart = previewSource_.trimStartFrame;
    uint64_t trimEnd = previewSource_.trimEndFrame.value_or(total);
    if (activeHandle_ == Handle::TrimStart) {
        trimEnd = std::clamp(trimEnd, uint64_t{1}, total);
        trimStart = std::min(trimStart, trimEnd - 1);
    } else {
        trimStart = std::min(trimStart, total - 1);
        trimEnd = std::clamp(trimEnd, trimStart + 1, total);
    }
    previewSource_.trimStartFrame = trimStart;
    if (previewSource_.trimEndFrame || activeHandle_ == Handle::TrimEnd)
        previewSource_.trimEndFrame = trimEnd;

    if (presentation_ == Presentation::TrimEditor || !previewSource_.loopRegion)
        return;

    uint64_t loopStart = previewSource_.loopRegion->startFrame;
    uint64_t loopEnd = previewSource_.loopRegion->endFrame;
    if (activeHandle_ == Handle::LoopStart) {
        loopEnd = std::clamp(loopEnd, trimStart + 1, trimEnd);
        loopStart = std::clamp(loopStart, trimStart, loopEnd - 1);
    } else {
        loopStart = std::clamp(loopStart, trimStart, trimEnd - 1);
        loopEnd = std::clamp(loopEnd, loopStart + 1, trimEnd);
    }
    previewSource_.loopRegion = SoundLoopRegion{loopStart, loopEnd};
}

}
