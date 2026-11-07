#include "TimelineCanvas.h"
#include "Core/ThemeLayout.h"

#include "TimelineEditorPanel.h"
#include "Core/FontManager.h"
#include "TimelineTicks.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AssetPipeline/AudioImportGate.h"
#include "Core/Components/VSegment.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/ItemTypeIcons.h"
#include "Core/LanguageContextService.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/ShortcutManager.h"
#include "Core/SoundSourceEditing.h"
#include "Core/ThemeManager.h"
#include "Core/FontManager.h"
#include "Core/Timeline/TimelineCoordinates.h"
#include "Core/Timeline/TimelineEditing.h"
#include "Core/Timeline/TimelineSemantics.h"
#include "Widgets/Shared/ProjectItemMime.h"
#include "Widgets/Selector/SelectorWindow.h"
#include "Widgets/Common/DecimalSpinBox.h"

#include <QContextMenuEvent>
#include <QEvent>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QSet>
#include <QScreen>
#include <QShortcut>
#include "Core/ToolTip.h"
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace VNX {
namespace {

int64_t saturatingAdd(int64_t left, int64_t right)
{
    if (right > 0 && left > std::numeric_limits<int64_t>::max() - right)
        return std::numeric_limits<int64_t>::max();
    if (right < 0 && left < std::numeric_limits<int64_t>::min() - right)
        return std::numeric_limits<int64_t>::min();
    return left + right;
}

int64_t saturatingSubtract(int64_t left, int64_t right)
{
    if (right > 0 && left < std::numeric_limits<int64_t>::min() + right)
        return std::numeric_limits<int64_t>::min();
    if (right < 0 && left > std::numeric_limits<int64_t>::max() + right)
        return std::numeric_limits<int64_t>::max();
    return left - right;
}

}

TimelineCanvas::TimelineCanvas(TimelineEditorPanel* panel, QWidget* parent)
    : QWidget(parent)
    , panel_(panel)
{
    setObjectName(QStringLiteral("timelineLaneViewport"));
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connect(panel_, &TimelineEditorPanel::cursorDragFinished, this, [this] {
        if (dragMode_ == DragMode::Cursor) {
            dragMode_ = DragMode::None;
            unsetCursor();
        }
    });
    connect(panel_, &TimelineEditorPanel::markerDragFinished, this, [this] {
        if (dragMode_ == DragMode::Marker) {
            dragMode_ = DragMode::None;
            unsetCursor();
        }
    });
    ShortcutManager::instance().bindWidgetAction(Shortcuts::Delete, this, [this]() {
        const QUuid marker = panel_->selectedMarkerGuid();
        if (!marker.isNull()) {
            for (int index = 0; index < panel_->markerCount(); ++index) {
                if (panel_->markerGuid(index) != marker)
                    continue;
                if (panel_->markerKind(index) != MarkerKind::Custom && panel_->deleteMarker(marker))
                    panel_->setSelectedMarker({});
                break;
            }
            return;
        }
        panel_->deleteSelectedClips();
    });
    connect(&WaveformCache::instance(), &WaveformCache::cacheReady, this,
        [this](const QString& path) {
            if (waveformCache_.remove(path) > 0)
                update();
        });
}

QUuid TimelineCanvas::selectedClipGuid() const
{
    return selectedGuids_.size() == 1 ? *selectedGuids_.constBegin() : QUuid();
}

bool TimelineCanvas::isGestureActive() const
{
    return dragMode_ != DragMode::None || lassoActive_ || dropVisible_;
}

void TimelineCanvas::clearHover()
{
    panel_->setCursorHovered(false);
    panel_->setHoveredMarker({});
    hoveredLane_ = -1;
    hoveredClip_ = -1;
    hoverTipTarget_.clear();
    ToolTip::hideText();
    update();
}

void TimelineCanvas::invalidateWaveforms()
{
    waveformCache_.clear();
    update();
}

void TimelineCanvas::setSelectedClip(int laneIndex, const QUuid& guid)
{
    selectSingle(laneIndex, guid);
}

void TimelineCanvas::restoreSelectedClip(const QUuid& guid)
{
    restoreSelectedClips(guid.isNull() ? QSet<QUuid>() : QSet<QUuid>{guid}, guid);
}

void TimelineCanvas::restoreSelectedClips(const QSet<QUuid>& guids, const QUuid& anchor)
{
    if (valueEditor_ && guids != selectedGuids_)
        valueEditor_->reject();
    selectedGuids_.clear();
    selectionAnchor_ = QUuid();
    selectedLane_ = -1;
    for (const QUuid& guid : guids) {
        const int lane = panel_->findLaneOfClip(guid);
        if (lane < 0)
            continue;
        selectedGuids_.insert(guid);
        if (selectedLane_ < 0)
            selectedLane_ = lane;
    }
    if (!anchor.isNull() && selectedGuids_.contains(anchor)) {
        selectionAnchor_ = anchor;
        selectedLane_ = panel_->findLaneOfClip(anchor);
    } else if (!selectedGuids_.isEmpty()) {
        selectionAnchor_ = *selectedGuids_.constBegin();
        selectedLane_ = panel_->findLaneOfClip(selectionAnchor_);
    }
}

const ThemeMetrics::Timeline& TimelineCanvas::timelineMetrics() const
{
    return panel_->timelineMetrics();
}

QRect TimelineCanvas::laneRect(int laneIndex) const
{
    if (laneIndex < 0 || laneIndex >= panel_->laneCount())
        return {};
    return panel_->laneRect(laneIndex);
}

QRect TimelineCanvas::blankRect() const
{
    if (panel_->viewKind() != TimelineEditorPanel::ViewKind::Segment)
        return {};
    const int y = panel_->laneCount() > 0 ? laneRect(panel_->laneCount() - 1).bottom() + 1 : 0;
    return QRect(0, y, width(), std::max(0, height() - y));
}

int TimelineCanvas::laneAtY(int y) const
{
    const QRect blank = blankRect();
    if (blank.contains(QPoint(0, y)))
        return -1;
    for (int lane = 0; lane < panel_->laneCount(); ++lane)
        if (laneRect(lane).contains(QPoint(0, y)))
            return lane;
    return -2;
}

QRect TimelineCanvas::rangeRect(int laneIndex, int64_t start, int64_t duration) const
{
    const QRect lane = laneRect(laneIndex);
    if (lane.isEmpty())
        return {};
    const int left = panel_->viewportPixelAtCoordinate(laneIndex, start);
    const int right = panel_->viewportPixelAtCoordinate(
        laneIndex, saturatingAdd(start, duration));
    if (right < 0 || left > width())
        return {};
    const auto& metrics = timelineMetrics();
    const int minWidth = metrics.clipMinimumWidth;
    const int visibleLeft = std::max(-minWidth, left);
    const int visibleRight = std::min(width() + minWidth, right);
    const int inset = metrics.clipVerticalInset;
    return {visibleLeft, lane.top() + inset,
            std::max(minWidth, visibleRight - visibleLeft),
            lane.height() - inset * 2};
}

const WaveformPeakData* TimelineCanvas::waveformForPath(const QString& absolutePath) const
{
    if (absolutePath.isEmpty())
        return nullptr;
    const auto it = waveformCache_.constFind(absolutePath);
    if (it != waveformCache_.constEnd())
        return it->isValid() ? &*it : nullptr;
    WaveformPeakData peaks;
    if (!WaveformCache::load(absolutePath, peaks))
        peaks = {};
    const auto inserted = waveformCache_.insert(absolutePath, std::move(peaks));
    return inserted->isValid() ? &*inserted : nullptr;
}

QString TimelineCanvas::clipTooltip(int laneIndex, int clipIndex) const
{
    const auto* target = panel_->clipTarget(laneIndex, clipIndex);
    QStringList tooltip;
    tooltip << (target ? target->name : tr("Missing"));
    tooltip << tr("Scheduled start: %1").arg(
        panel_->formatCoordinate(laneIndex, panel_->clipStart(laneIndex, clipIndex)));
    tooltip << tr("Authored duration: %1").arg(
        panel_->formatCoordinate(laneIndex, panel_->clipDuration(laneIndex, clipIndex)));
    if (const auto duration = panel_->clipSourceDuration(laneIndex, clipIndex))
        tooltip << tr("Source reference span: %1").arg(
            panel_->formatCoordinate(laneIndex, *duration));
    else
        tooltip << tr("Source reference span varies or is unavailable.");
    tooltip << tr("Playback ends at the authored Clip boundary or an earlier natural source end. Overlapping Clips play together without an automatic crossfade.");
    if (const auto emergence = panel_->emergenceCoordinate(laneIndex, clipIndex))
        tooltip << tr("Emerges at %1").arg(panel_->formatCoordinate(laneIndex, *emergence));
    return tooltip.join(QStringLiteral("\n"));
}

QVector<int> TimelineCanvas::clipDrawOrder(int laneIndex) const
{
    QVector<int> order;
    const int count = panel_->clipCount(laneIndex);
    QVector<int64_t> starts;
    order.reserve(count);
    starts.reserve(count);
    for (int i = 0; i < count; ++i) {
        order.append(i);
        starts.append(panel_->clipStart(laneIndex, i));
    }
    std::stable_sort(order.begin(), order.end(), [&starts](int left, int right) {
        return starts[left] < starts[right];
    });
    return order;
}

TimelineCanvas::Hit TimelineCanvas::hitAt(const QPoint& point) const
{
    Hit hit;
    const int lane = laneAtY(point.y());
    if (lane == -1) {
        hit.blank = true;
        hit.laneIndex = -1;
        return hit;
    }
    if (lane < 0)
        return hit;
    hit.laneIndex = lane;
    const auto order = clipDrawOrder(lane);
    for (auto it = order.crbegin(); it != order.crend(); ++it) {
        const int i = *it;
        if (rangeRect(lane, panel_->clipStart(lane, i),
                      panel_->clipDuration(lane, i)).contains(point)) {
            hit.clipIndex = i;
            break;
        }
    }
    return hit;
}

bool TimelineCanvas::hitsResizeEnd(const Hit& hit, const QPoint& point) const
{
    if (hit.laneIndex < 0 || hit.clipIndex < 0)
        return false;
    const int64_t start = panel_->clipStart(hit.laneIndex, hit.clipIndex);
    const int64_t duration = panel_->clipDuration(hit.laneIndex, hit.clipIndex);
    const QRect area = rangeRect(hit.laneIndex, start, duration);
    if (area.isEmpty())
        return false;
    const int end = area.right();
    return end >= 0 && end < width()
        && std::abs(point.x() - end)
            <= std::max(1, timelineMetrics().resizeHandleWidth);
}

TimelineStyleView TimelineCanvas::styleView() const
{
    TimelineStyleView style;
    const auto snapshot = ThemeManager::instance().snapshot();
    style.canvasBackground = snapshot->ui.colors.bgPrimary;
    style.canvasBorder = snapshot->ui.colors.borderPrimary;
    style.placeholderText = snapshot->ui.colors.textSecondary;
    style.axisText = snapshot->ui.colors.textSecondary;
    style.trackBackground = snapshot->ui.colors.trackBackground;
    style.trackBackgroundAlt = snapshot->ui.colors.trackBackgroundAlt;
    style.timeline = snapshot->domains.timeline;
    style.music = snapshot->domains.music;
    style.waveform = snapshot->domains.waveform;
    style.waveformExponent = panel_->viewKind() == TimelineEditorPanel::ViewKind::Track
        ? panel_->amplitudeExponent() : 1.0;
    style.curve = snapshot->domains.curve;
    style.metrics = snapshot->ui.metrics;
    style.accent = snapshot->ui.colors.accent;
    style.textPrimary = snapshot->ui.colors.textPrimary;
    return style;
}

TimelineRenderData TimelineCanvas::buildRenderData(const QRect& exposed) const
{
    TimelineRenderData data;
    data.viewport = rect();
    data.frameless = panel_->viewKind() == TimelineEditorPanel::ViewKind::Track;
    if (data.frameless)
        data.amplitudeTicks = TimelineRenderer::amplitudeTicks(panel_->trackWaveformRect(),
            1.0, 0, *ThemeManager::instance().snapshot(),
            panel_->amplitudeExponent());
    if (panel_->viewKind() == TimelineEditorPanel::ViewKind::Unsupported) {
        data.placeholder = panel_->stateText();
        return data;
    }
    data.viewOffset = panel_->horizontalScroll();
    data.pxPerUnit = panel_->pixelsPerUnit();
    data.timeAxis = panel_->timeTicks();
    data.domain = panel_->timelineDomain();
    for (int index = 0; index < panel_->markerCount(); ++index) {
        TimelineMarker marker;
        marker.x = panel_->viewportPixelAtCoordinate(0, panel_->markerDisplayPosition(index));
        marker.height = height();
        marker.kind = panel_->markerKind(index);
        marker.selected = panel_->markerGuid(index) == panel_->selectedMarkerGuid();
        marker.hovered = panel_->markerGuid(index) == panel_->hoveredMarkerGuid();
        data.markers.append(marker);
    }
    const auto theme = ThemeManager::instance().snapshot();
    const int titleHeight = TimelineRenderer::clipTitleHeight(theme->ui.metrics);
    const QRect trackViewport = data.frameless ? panel_->trackWaveformRect() : QRect();
    const QRect trackWaveform = data.frameless
        ? waveformAmplitudeBounds(trackViewport,
            trackViewport.height(), 0, QFontMetrics(FontManager::instance().getFont(FontRole::Caption)), theme->ui.metrics.timeAxis.labelPadding)
        : QRect();
    struct ClipContent {
        QString label;
        QColor color;
        QIcon icon;
        QString waveformPath;
        int64_t sourceDuration = 0;
        std::optional<SourceWaveformView> waveformView;
        double emergenceOffsetSec = 0.0;
        bool waveformResolved = false;
    };
    QHash<QUuid, ClipContent> contents;
    const auto fillClipItem = [&](TimelineClipItem& item, int laneIndex, int clipIndex,
                                  const QRect& area, const QUuid& guid, int64_t start,
                                  int64_t duration) {
        const QUuid targetGuid = panel_->clipTargetGuid(laneIndex, clipIndex);
        auto entry = contents.find(targetGuid);
        if (entry == contents.end()) {
            ClipContent content;
            const auto* target = panel_->clipTarget(laneIndex, clipIndex);
            content.label = target ? target->name : tr("Missing");
            if (target && target->color != 0)
                content.color = QColor::fromRgba(target->color);
            if (target)
                content.icon = iconForItem(*target);
            const auto* sound = target ? target->comp<VSound>() : nullptr;
            if (sound)
                content.emergenceOffsetSec = sound->scheduleOffsetSec;
            entry = contents.insert(targetGuid, std::move(content));
        }
        const bool waveformVisible = area.height() > titleHeight
            && (!data.frameless || !trackViewport.isEmpty());
        if (waveformVisible && !entry->waveformResolved) {
            ClipContent& content = *entry;
            content.waveformResolved = true;
            const auto* target = panel_->clipTarget(laneIndex, clipIndex);
            if (target && target->comp<VSound>()) {
                const auto* source = SoundSourceEditing::activeSource(*target,
                    PlatformContextService::instance().context().primary,
                    SoundSourceEditing::previewLanguage(*target, LanguageContextService::instance().language()));
                const auto* asset = source ? AssetRegistry::instance().getAsset(source->sourceId) : nullptr;
                if (asset && !asset->absolutePath.isEmpty() && asset->metadata.totalSamples > 0) {
                    content.waveformPath = asset->absolutePath;
                    content.waveformView.emplace(*source, asset->metadata.totalSamples);
                    content.sourceDuration = panel_->clipSourceDuration(laneIndex, clipIndex).value_or(0);
                }
            }
        }
        const ClipContent& content = *entry;
        item.rect = area;
        item.laneIndex = laneIndex;
        item.clipIndex = clipIndex;
        item.guid = guid;
        item.label = content.label;
        item.objectColor = content.color;
        item.typeIcon = content.icon;
        if (clipSelected(guid))
            item.state = TimelineClipVisualState::Selected;
        else if (laneIndex == hoveredLane_ && clipIndex == hoveredClip_)
            item.state = TimelineClipVisualState::Hover;
        if (waveformVisible && content.sourceDuration > 0 && content.waveformView) {
            const double left = panel_->viewportPositionAtCoordinate(laneIndex, start);
            const double right = panel_->viewportPositionAtCoordinate(laneIndex,
                saturatingAdd(start, std::min(duration, content.sourceDuration)));
            const double sourceRight = panel_->viewportPositionAtCoordinate(laneIndex,
                saturatingAdd(start, content.sourceDuration));
            const double visibleLeft = std::clamp(left, 0.0, double(width()));
            const double visibleRight = std::clamp(right, 0.0, double(width()));
            if (visibleRight > visibleLeft && sourceRight > left) {
                item.waveformPath = content.waveformPath;
                item.waveformView = content.waveformView;
                item.waveformViewport = data.frameless ? trackViewport : area.adjusted(0, titleHeight, 0, 0);
                item.waveformRect = data.frameless ? trackWaveform : item.waveformViewport;
                item.waveformRect.setLeft(int(std::lround(visibleLeft)));
                item.waveformRect.setWidth(int(std::lround(visibleRight)) - int(std::lround(visibleLeft)));
                const double sourceFrames = double(content.waveformView->endFrame() - content.waveformView->startFrame());
                item.waveformStartFrame = double(content.waveformView->startFrame())
                    + sourceFrames * (visibleLeft - left) / (sourceRight - left);
                item.waveformFrameCount = sourceFrames * (visibleRight - visibleLeft) / (sourceRight - left);
            }
        }
        if (waveformVisible && content.emergenceOffsetSec > 0.0) {
            const auto offset = TimelineCoordinates::fromSeconds(content.emergenceOffsetSec,
                panel_->effectiveTimeSpec(laneIndex));
            if (offset) {
                const int64_t emergence = saturatingAdd(panel_->clipStart(laneIndex, clipIndex), offset.value);
                const int realX = panel_->viewportPixelAtCoordinate(laneIndex, emergence);
                item.emergenceCoordinate = emergence;
                item.emergencePixel = realX;
                item.emergenceClamped = realX != std::clamp(realX, area.left(), area.right());
            }
        }
    };
    for (int lane = 0; lane < panel_->laneCount(); ++lane) {
        TimelineLaneRect rect;
        rect.rect = laneRect(lane);
        if (rect.rect.bottom() < exposed.top() || rect.rect.top() > exposed.bottom())
            continue;
        rect.alternate = lane % 2 == 1;
        rect.highlighted = !data.frameless && ((dropVisible_ && dropLegal_ && dropLane_ == lane)
            || (dragMode_ == DragMode::Move && previewLane_ == lane));
        rect.selected = false;
        ProjectItem* track = ProjectManager::instance().findItemByGuid(
            panel_->laneGuid(lane));
        if (!data.frameless && track && track->color != 0)
            rect.accent = QColor::fromRgba(track->color);
        data.laneRects.append(rect);
    }
    for (int lane = 0; lane < panel_->laneCount(); ++lane) {
        const QRect laneArea = laneRect(lane);
        if (laneArea.bottom() < exposed.top() || laneArea.top() > exposed.bottom())
            continue;
        for (int i : clipDrawOrder(lane)) {
            const QUuid guid = panel_->clipGuid(lane, i);
            int64_t start = panel_->clipStart(lane, i);
            int64_t duration = panel_->clipDuration(lane, i);
            const bool dragging = dragMoved_ &&
                ((clipSelected(guid) && dragMode_ == DragMode::Move) ||
                 (guid == activeGuid_ && dragMode_ == DragMode::ResizeEnd));
            if (dragging)
                continue;
            const QRect area = rangeRect(lane, start, duration);
            if (area.isEmpty() || area.bottom() < exposed.top() || area.top() > exposed.bottom())
                continue;
            TimelineClipItem item;
            fillClipItem(item, lane, i, area, guid, start, duration);
            data.clipItems.append(item);
        }

    }
    if (dragMoved_ && dragMode_ == DragMode::Move && previewLane_ >= 0) {
        for (const GestureClip& clip : gestureClips_) {
            const int64_t start = saturatingAdd(previewStart_,
                                                saturatingSubtract(clip.start, originalStart_));
            const int destLane = previewLaneForClip(clip);
            const bool illegal = destLane < 0 || destLane >= panel_->laneCount();
            const int clipIndex = panel_->findClipIndex(clip.laneIndex, clip.guid);
            if (clipIndex < 0)
                continue;
            const int drawLane = illegal ? clip.laneIndex : destLane;
            const QRect area = rangeRect(drawLane, start, clip.duration);
            if (area.isEmpty() || area.bottom() < exposed.top() || area.top() > exposed.bottom())
                continue;
            TimelineClipItem item;
            fillClipItem(item, clip.laneIndex, clipIndex, area, clip.guid, start,
                         clip.duration);
            item.residual = false;
            item.state = TimelineClipVisualState::Selected;
            data.clipItems.append(item);
        }
    }
    if (dragMoved_ && dragMode_ == DragMode::ResizeEnd && activeLane_ >= 0) {
        const int clipIndex = panel_->findClipIndex(activeLane_, activeGuid_);
        const QRect area = rangeRect(
            activeLane_, originalStart_, previewDuration_);
        if (clipIndex >= 0 && !area.isEmpty()
            && area.bottom() >= exposed.top() && area.top() <= exposed.bottom()) {
            TimelineClipItem item;
            fillClipItem(item, activeLane_, clipIndex, area, activeGuid_,
                         originalStart_, previewDuration_);
            item.state = TimelineClipVisualState::Selected;
            data.clipItems.append(item);
        }
    }

    QSet<QString> waveformPaths;
    for (const TimelineClipItem& item : data.clipItems) {
        if (!item.waveformPath.isEmpty())
            waveformPaths.insert(item.waveformPath);
    }
    for (const QString& path : waveformPaths)
        waveformForPath(path);
    for (TimelineClipItem& item : data.clipItems)
        item.waveform = waveformForPath(item.waveformPath);

    data.editCursor.visible = panel_->isEditCursorVisible();
    data.editCursor.x = panel_->viewportPixelAtCoordinate(0, panel_->editCursor());
    data.editCursor.highlighted = panel_->isCursorHovered() || panel_->isCursorDragActive();
    if (const auto playhead = panel_->playheadCoordinate()) {
        data.playhead.visible = true;
        data.playhead.x = panel_->viewportPixelAtCoordinate(0, *playhead);
    }
    data.dropIndicator.visible = dropVisible_ && dropLegal_;
    data.dropIndicator.x = dropX_;
    data.snapGuide.visible = snapGuideVisible_;
    data.snapGuide.x = snapGuideX_;
    data.lasso.visible = lassoActive_ && !lassoRect_.isNull();
    data.lasso.rect = lassoRect_;
    return data;
}

void TimelineCanvas::paintEvent(QPaintEvent* event)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    lastRenderData_ = buildRenderData(event->rect());
    TimelineRenderer::render(painter, styleView(), lastRenderData_);
    for (TimelineClipItem& item : lastRenderData_.clipItems)
        item.waveform = nullptr;
}

void TimelineCanvas::resizeEvent(QResizeEvent*)
{
    panel_->refreshHorizontalRange();
    panel_->refreshTimelinePaint();
}

void TimelineCanvas::dragEnterEvent(QDragEnterEvent* event)
{
    if (acceptDrop(event->mimeData(), event->position().toPoint(), false,
                   event->modifiers()))
        event->acceptProposedAction();
    else
        event->ignore();
}

void TimelineCanvas::dragMoveEvent(QDragMoveEvent* event)
{
    if (acceptDrop(event->mimeData(), event->position().toPoint(), false,
                   event->modifiers()))
        event->acceptProposedAction();
    else
        event->ignore();
}

void TimelineCanvas::dragLeaveEvent(QDragLeaveEvent*)
{
    setDropFeedback(false, 0, -1, {}, false);
    unsetCursor();
}

void TimelineCanvas::dropEvent(QDropEvent* event)
{
    if (acceptDrop(event->mimeData(), event->position().toPoint(), true,
                   event->modifiers()))
        event->acceptProposedAction();
    else
        event->ignore();
    setDropFeedback(false, 0, -1, {}, false);
    unsetCursor();
}

void TimelineCanvas::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus();
    clearHover();
    if (hitsEditCursor(event->position().toPoint())) {
        dragMode_ = DragMode::Cursor;
        panel_->beginCursorDrag(int(event->position().x()),
            event->modifiers().testFlag(Qt::AltModifier), false);
        setCursor(Qt::SizeHorCursor);
        event->accept();
        return;
    }
    const QUuid marker = panel_->markerAtViewportPixel(int(event->position().x()));
    if (!marker.isNull()) {
        dragMode_ = DragMode::Marker;
        panel_->beginMarkerDrag(marker, int(event->position().x()),
            event->modifiers().testFlag(Qt::AltModifier));
        setCursor(Qt::SizeHorCursor);
        event->accept();
        return;
    }
    panel_->setSelectedMarker({});
    const Hit hit = hitAt(event->position().toPoint());
    if (hit.clipIndex < 0) {
        lassoActive_ = true;
        lassoStart_ = event->position().toPoint();
        lassoRect_ = QRect(lassoStart_, QSize());
        lassoBaseSelection_ = selectedGuids_;
        lassoModifiers_ = event->modifiers();
        dragMode_ = DragMode::Lasso;
        if ((lassoModifiers_ & (Qt::ControlModifier | Qt::AltModifier)) == 0)
            clearSelection();
        update();
        return;
    }
    const bool resizeEnd =
        !event->modifiers().testFlag(Qt::ControlModifier) &&
        !event->modifiers().testFlag(Qt::ShiftModifier) &&
        hitsResizeEnd(hit, event->position().toPoint());
    const QUuid guid = panel_->clipGuid(hit.laneIndex, hit.clipIndex);
    if (event->modifiers().testFlag(Qt::ControlModifier))
        toggleSelected(hit.laneIndex, guid);
    else if (event->modifiers().testFlag(Qt::ShiftModifier))
        selectRange(hit.laneIndex, guid);
    else if (!clipSelected(guid))
        selectSingle(hit.laneIndex, guid);
    else {
        selectionAnchor_ = guid;
        selectedLane_ = hit.laneIndex;
    }
    armDrag(hit, event->position().toPoint());
    if (resizeEnd && selectedGuids_.size() == 1)
        dragMode_ = DragMode::ResizeEnd;
    update();
}

void TimelineCanvas::mouseMoveEvent(QMouseEvent* event)
{
    if (dragMode_ == DragMode::Marker) {
        panel_->updateMarkerDrag(int(event->position().x()),
            event->modifiers().testFlag(Qt::AltModifier));
        event->accept();
        return;
    }
    if (dragMode_ == DragMode::Cursor) {
        panel_->updateCursorDrag(int(event->position().x()),
            event->modifiers().testFlag(Qt::AltModifier));
        event->accept();
        return;
    }
    if (lassoActive_) {
        updateLassoSelection(event->position().toPoint());
        update();
        return;
    }
    if (dragMode_ == DragMode::None || !(event->buttons() & Qt::LeftButton)) {
        const bool overHandle = hitsEditCursor(event->position().toPoint())
            || !panel_->markerAtViewportPixel(int(event->position().x())).isNull();
        const Hit hit = overHandle ? Hit{} : hitAt(event->position().toPoint());
        if (hoveredLane_ != hit.laneIndex || hoveredClip_ != hit.clipIndex) {
            hoveredLane_ = hit.laneIndex;
            hoveredClip_ = hit.clipIndex;
            update();
        }
        updateHoverCursor(event->position().toPoint());
        if (panel_->hasTimeline())
            return;
        if (hit.clipIndex >= 0) {
            const QString target = panel_->clipGuid(hit.laneIndex, hit.clipIndex).toString(QUuid::WithoutBraces);
            if (hoverTipTarget_ != target) {
                hoverTipTarget_ = target;
                ToolTip::showText(event->globalPosition().toPoint(),
                    clipTooltip(hit.laneIndex, hit.clipIndex), this);
            }
        } else if (!hoverTipTarget_.isEmpty()) {
            hoverTipTarget_.clear();
            ToolTip::hideText();
        }
        return;
    }
    if (!dragMoved_
        && (event->position().toPoint() - pressPos_).manhattanLength() < 3)
        return;
    dragMoved_ = true;
    applyDragPreview(event->position().toPoint(),
                     event->modifiers().testFlag(Qt::AltModifier));
    update();
}

void TimelineCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    if (dragMode_ == DragMode::Marker) {
        panel_->updateMarkerDrag(int(event->position().x()),
            event->modifiers().testFlag(Qt::AltModifier));
        panel_->endMarkerDrag();
        dragMode_ = DragMode::None;
        updateHoverCursor(event->position().toPoint());
        event->accept();
        return;
    }
    if (dragMode_ == DragMode::Cursor) {
        panel_->updateCursorDrag(int(event->position().x()),
            event->modifiers().testFlag(Qt::AltModifier));
        panel_->endCursorDrag();
        dragMode_ = DragMode::None;
        updateHoverCursor(event->position().toPoint());
        update();
        event->accept();
        return;
    }
    if (lassoActive_) {
        updateLassoSelection(event->position().toPoint());
        lassoActive_ = false;
        lassoRect_ = {};
        lassoBaseSelection_.clear();
        dragMode_ = DragMode::None;
        panel_->publishClipSelection();
        update();
        return;
    }
    if (dragMode_ == DragMode::None) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const bool disableSnap = event->modifiers().testFlag(Qt::AltModifier);
    if (dragMoved_)
        commitDrag(disableSnap);
    else
        panel_->publishClipSelection();
    cancelGesture();
    update();
}

void TimelineCanvas::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && hitsEditCursor(event->position().toPoint())) {
        cancelGesture();
        panel_->editCursorValue();
        event->accept();
        return;
    }
    const QUuid marker = panel_->markerAtViewportPixel(int(event->position().x()));
    if (event->button() == Qt::LeftButton && !marker.isNull()) {
        cancelGesture();
        panel_->editMarkerValue(marker);
        event->accept();
        return;
    }
    const Hit hit = hitAt(event->position().toPoint());
    if (hit.clipIndex >= 0) {
        const QUuid guid = panel_->clipGuid(hit.laneIndex, hit.clipIndex);
        ProjectItem* target = ProjectManager::instance().findItemByGuid(
            panel_->clipTargetGuid(hit.laneIndex, hit.clipIndex));
        if (target && target->type == ProjectItem::Type::Segment
            && ItemTypeCatalog::isUserFacingType(target->type)) {
            QTimer::singleShot(0, panel_, [panel = panel_, lane = hit.laneIndex, guid] {
                panel->drillIntoClip(lane, guid);
            });
        } else {
            panel_->inspectClip(hit.laneIndex, guid);
        }
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TimelineCanvas::contextMenuEvent(QContextMenuEvent* event)
{
    clearHover();
    if (hitsEditCursor(event->pos())) {
        cancelGesture();
        panel_->showCursorContextMenu(event->globalPos());
        event->accept();
        return;
    }
    const QUuid marker = panel_->markerAtViewportPixel(event->pos().x());
    if (!marker.isNull()) {
        cancelGesture();
        panel_->showMarkerContextMenu(marker, event->globalPos());
        event->accept();
        return;
    }
    const Hit hit = hitAt(event->pos());
    const int64_t position = panel_->coordinateAtViewportPixel(
        std::max(0, hit.laneIndex),
        event->pos().x());
    QMenu* menu = nullptr;
    if (hit.clipIndex >= 0) {
        const QUuid guid = panel_->clipGuid(hit.laneIndex, hit.clipIndex);
        if (!clipSelected(guid))
            selectSingle(hit.laneIndex, guid);
        menu = createClipMenu(hit.laneIndex, guid);
    } else {
        menu = createLaneBlankMenu(hit.blank ? -1 : hit.laneIndex, position);
    }
    menu->exec(event->globalPos());
    menu->deleteLater();
    event->accept();
}

void TimelineCanvas::wheelEvent(QWheelEvent* event)
{
    const QPoint delta = event->angleDelta().isNull() ? event->pixelDelta() : event->angleDelta();
    if (event->modifiers().testFlag(Qt::ShiftModifier)) {
        const int pixels = delta.x() != 0 ? delta.x() : delta.y();
        panel_->panHorizontally(-pixels / (event->angleDelta().isNull() ? 1.0 : 8.0));
    } else {
        const double steps = double(delta.y() != 0 ? delta.y() : delta.x())
            / (event->angleDelta().isNull() ? 40.0 : 120.0);
        if (panel_->viewKind() == TimelineEditorPanel::ViewKind::Segment
            && event->modifiers().testFlag(Qt::ControlModifier))
            panel_->zoomVertically(int(event->position().y()), std::pow(1.25, steps));
        else
            panel_->zoomAroundPixel(int(event->position().x()), std::pow(1.25, steps));
    }
    event->accept();
}

void TimelineCanvas::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && isGestureActive()) {
        cancelGesture();
        update();
        event->accept();
        return;
    }
    if (panel_->isEditCursorVisible() && event->key() == Qt::Key_Home) {
        panel_->setEditCursor(panel_->viewOriginCoordinate());
        event->accept();
        return;
    }
    if (panel_->isEditCursorVisible() && event->key() == Qt::Key_End) {
        int64_t end = panel_->viewOriginCoordinate();
        for (int lane = 0; lane < panel_->laneCount(); ++lane) {
            for (int clip = 0; clip < panel_->clipCount(lane); ++clip)
                end = std::max(end, saturatingAdd(panel_->clipStart(lane, clip),
                    panel_->clipDuration(lane, clip)));
        }
        for (int marker = 0; marker < panel_->markerCount(); ++marker)
            end = std::max(end, panel_->markerPosition(marker));
        panel_->setEditCursor(end, true);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TimelineCanvas::hideEvent(QHideEvent* event)
{
    if (dragMode_ == DragMode::Cursor)
        panel_->endCursorDrag();
    if (dragMode_ == DragMode::Marker)
        panel_->endMarkerDrag(true);
    clearHover();
    QWidget::hideEvent(event);
}

void TimelineCanvas::leaveEvent(QEvent* event)
{
    clearHover();
    QWidget::leaveEvent(event);
}

bool TimelineCanvas::clipSelected(const QUuid& guid) const
{
    return !guid.isNull() && selectedGuids_.contains(guid);
}

void TimelineCanvas::selectSingle(int laneIndex, const QUuid& guid)
{
    if (valueEditor_)
        valueEditor_->reject();
    selectedGuids_.clear();
    if (!guid.isNull())
        selectedGuids_.insert(guid);
    selectionAnchor_ = guid;
    selectedLane_ = laneIndex;
    panel_->publishClipSelection();
    update();
}

void TimelineCanvas::toggleSelected(int laneIndex, const QUuid& guid)
{
    if (valueEditor_)
        valueEditor_->reject();
    if (guid.isNull())
        return;
    if (selectedGuids_.contains(guid))
        selectedGuids_.remove(guid);
    else
        selectedGuids_.insert(guid);
    selectionAnchor_ = guid;
    selectedLane_ = laneIndex;
    panel_->publishClipSelection();
    update();
}

void TimelineCanvas::selectRange(int laneIndex, const QUuid& guid)
{
    if (valueEditor_)
        valueEditor_->reject();
    if (guid.isNull())
        return;
    if (selectionAnchor_.isNull() || panel_->findLaneOfClip(selectionAnchor_) != laneIndex) {
        selectSingle(laneIndex, guid);
        return;
    }
    struct Row {
        QUuid guid;
        int64_t start = 0;
    };
    std::vector<Row> rows;
    const int count = panel_->clipCount(laneIndex);
    rows.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        rows.push_back({panel_->clipGuid(laneIndex, i), panel_->clipStart(laneIndex, i)});
    std::sort(rows.begin(), rows.end(),
              [](const Row& left, const Row& right) { return left.start < right.start; });
    int anchor = -1;
    int current = -1;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        if (rows[i].guid == selectionAnchor_)
            anchor = i;
        if (rows[i].guid == guid)
            current = i;
    }
    if (anchor < 0 || current < 0) {
        selectSingle(laneIndex, guid);
        return;
    }
    selectedGuids_.clear();
    const int from = std::min(anchor, current);
    const int to = std::max(anchor, current);
    for (int i = from; i <= to; ++i)
        selectedGuids_.insert(rows[i].guid);
    selectionAnchor_ = guid;
    selectedLane_ = laneIndex;
    panel_->publishClipSelection();
    update();
}

void TimelineCanvas::clearSelection()
{
    if (valueEditor_)
        valueEditor_->reject();
    selectedGuids_.clear();
    selectionAnchor_ = QUuid();
    selectedLane_ = -1;
}

void TimelineCanvas::updateLassoSelection(const QPoint& pos)
{
    lassoRect_ = QRect(lassoStart_, pos).normalized();
    QSet<QUuid> hits;
    for (int lane = 0; lane < panel_->laneCount(); ++lane) {
        for (int clip = 0; clip < panel_->clipCount(lane); ++clip) {
            const QRect area = rangeRect(lane, panel_->clipStart(lane, clip),
                                         panel_->clipDuration(lane, clip));
            if (area.intersects(lassoRect_))
                hits.insert(panel_->clipGuid(lane, clip));
        }
    }
    selectedGuids_ = lassoBaseSelection_;
    if ((lassoModifiers_ & Qt::ControlModifier) != 0) {
        for (const QUuid& guid : hits) {
            if (selectedGuids_.contains(guid))
                selectedGuids_.remove(guid);
            else
                selectedGuids_.insert(guid);
        }
    } else if ((lassoModifiers_ & Qt::AltModifier) != 0) {
        for (const QUuid& guid : hits)
            selectedGuids_.remove(guid);
    } else {
        selectedGuids_ = hits;
    }
    if (!selectedGuids_.isEmpty()) {
        selectionAnchor_ = *selectedGuids_.constBegin();
        selectedLane_ = panel_->findLaneOfClip(selectionAnchor_);
    } else {
        selectionAnchor_ = QUuid();
        selectedLane_ = -1;
    }
}

void TimelineCanvas::armDrag(const Hit& hit, const QPoint& pos)
{
    activeGuid_ = panel_->clipGuid(hit.laneIndex, hit.clipIndex);
    activeLane_ = hit.laneIndex;
    originalStart_ = panel_->clipStart(hit.laneIndex, hit.clipIndex);
    originalDuration_ = panel_->clipDuration(hit.laneIndex, hit.clipIndex);
    previewStart_ = originalStart_;
    previewDuration_ = originalDuration_;
    previewLane_ = hit.laneIndex;
    pressCoordinate_ = panel_->coordinateAtViewportPixel(hit.laneIndex, pos.x());
    pressPos_ = pos;
    dragMoved_ = false;
    gestureClips_.clear();
    for (const QUuid& guid : selectedGuids_) {
        const int lane = panel_->findLaneOfClip(guid);
        const int clip = panel_->findClipIndex(lane, guid);
        if (lane < 0 || clip < 0)
            continue;
        gestureClips_.push_back({lane, guid, panel_->clipStart(lane, clip),
                                 panel_->clipDuration(lane, clip)});
    }
    dragMode_ = DragMode::Move;
}

int TimelineCanvas::destinationLaneAt(int y) const
{
    if (panel_->laneCount() == 0)
        return -1;
    int destination = laneAtY(y);
    if (destination < 0)
        destination = y < laneRect(0).top() ? 0 : panel_->laneCount() - 1;
    int firstLane = activeLane_;
    int lastLane = activeLane_;
    for (const GestureClip& clip : gestureClips_) {
        firstLane = std::min(firstLane, clip.laneIndex);
        lastLane = std::max(lastLane, clip.laneIndex);
    }
    if (firstLane < 0 || lastLane >= panel_->laneCount())
        return -1;
    return activeLane_ + std::clamp(destination - activeLane_,
        -firstLane, panel_->laneCount() - 1 - lastLane);
}

int TimelineCanvas::previewLaneForClip(const GestureClip& clip) const
{
    return clip.laneIndex + previewLane_ - activeLane_;
}

bool TimelineCanvas::applyDragPreview(const QPoint& pos, bool disableSnap)
{
    const int destLane = destinationLaneAt(pos.y());
    const int64_t current = panel_->coordinateAtViewportPixel(std::max(0, destLane), pos.x());
    const int64_t delta = saturatingSubtract(current, pressCoordinate_);
    snapGuideVisible_ = false;
    if (dragMode_ == DragMode::Move) {
        int64_t earliestStart = originalStart_;
        for (const GestureClip& clip : gestureClips_)
            earliestStart = std::min(earliestStart, clip.start);
        const int64_t minimumStart = saturatingSubtract(originalStart_, earliestStart);
        const int64_t intendedStart = std::max(minimumStart,
            saturatingAdd(originalStart_, delta));
        previewLane_ = destLane;
        int64_t applied = intendedStart;
        if (!disableSnap)
            applied = magnetSnap(destLane, intendedStart, originalStart_,
                                 saturatingAdd(originalStart_, originalDuration_));
        applied = std::max(minimumStart, applied);
        previewStart_ = applied;
        previewDuration_ = originalDuration_;
        const int snappedPixel = panel_->viewportPixelAtCoordinate(destLane, applied);
        snapGuideVisible_ = !disableSnap && applied != intendedStart;
        snapGuideX_ = snappedPixel;
        panel_->setStatusMessage(
            tr("Move to %1").arg(panel_->formatCoordinate(destLane, applied)), false);
        return true;
    }
    if (dragMode_ == DragMode::ResizeEnd) {
        const int64_t minimumEnd = saturatingAdd(originalStart_, 1);
        const int64_t originalEnd =
            saturatingAdd(originalStart_, originalDuration_);
        const int64_t intendedEnd = std::max(
            saturatingAdd(originalEnd, delta), minimumEnd);
        int64_t appliedEnd = intendedEnd;
        if (!disableSnap) {
            appliedEnd = magnetSnap(
                activeLane_, intendedEnd, originalStart_,
                saturatingAdd(originalStart_, originalDuration_));
            appliedEnd = std::max(appliedEnd, minimumEnd);
        }
        previewStart_ = originalStart_;
        previewDuration_ = std::max<int64_t>(
            1, saturatingSubtract(appliedEnd, originalStart_));
        previewLane_ = activeLane_;
        snapGuideVisible_ = !disableSnap && appliedEnd != intendedEnd;
        snapGuideX_ = panel_->viewportPixelAtCoordinate(
            activeLane_, appliedEnd);
        panel_->setStatusMessage(
            tr("Duration: %1").arg(
                panel_->formatCoordinate(activeLane_, previewDuration_)),
            false);
        return true;
    }
    return false;
}

void TimelineCanvas::commitDrag(bool disableSnap)
{
    Q_UNUSED(disableSnap);
    if (dragMode_ == DragMode::Move) {
        const auto clips = gestureClips_;
        const int laneDelta = previewLane_ - activeLane_;
        const int64_t start = previewStart_;
        const int64_t originalStart = originalStart_;
        QVector<QUuid> lanes;
        lanes.reserve(panel_->laneCount());
        for (int lane = 0; lane < panel_->laneCount(); ++lane)
            lanes.append(panel_->laneGuid(lane));
        cancelGesture();
        std::vector<TimelineEditing::ClipMove> moves;
        moves.reserve(clips.size());
        for (const GestureClip& clip : clips) {
            const int destLane = clip.laneIndex + laneDelta;
            if (clip.laneIndex < 0 || clip.laneIndex >= lanes.size()
                || destLane < 0 || destLane >= lanes.size()) {
                panel_->rejectInteraction(tr("The selection cannot move outside the timeline."));
                return;
            }
            const int64_t clipStart = saturatingAdd(
                start, saturatingSubtract(clip.start, originalStart));
            moves.push_back({lanes[clip.laneIndex], lanes[destLane], clip.guid, clipStart});
        }
        panel_->commitClipMoves(moves);
        return;
    }
    if (dragMode_ == DragMode::ResizeEnd) {
        panel_->resizeClip(activeLane_, activeGuid_, originalStart_,
                           previewDuration_, true);
        return;
    }

}

void TimelineCanvas::cancelGesture()
{
    if (dragMode_ == DragMode::Cursor)
        panel_->endCursorDrag(true);
    if (dragMode_ == DragMode::Marker)
        panel_->endMarkerDrag(true);
    clearHover();
    if (valueEditor_)
        valueEditor_->reject();
    dragMode_ = DragMode::None;
    dragMoved_ = false;
    lassoActive_ = false;
    lassoRect_ = {};
    lassoBaseSelection_.clear();
    gestureClips_.clear();
    activeGuid_ = QUuid();
    activeLane_ = -1;
    previewLane_ = -1;
    snapGuideVisible_ = false;
    dropVisible_ = false;
    unsetCursor();
}

int64_t TimelineCanvas::magnetSnap(int laneIndex, int64_t coordinate,
    int64_t, int64_t) const
{
    bool ok = false;
    const int64_t snapped = panel_->snapCoordinate(laneIndex, coordinate, false, &ok, selectedGuids_);
    return ok ? snapped : coordinate;
}

void TimelineCanvas::updateHoverCursor(const QPoint& pos)
{
    const bool cursor = hitsEditCursor(pos);
    const QUuid marker = !cursor && rect().contains(pos) ? panel_->markerAtViewportPixel(pos.x()) : QUuid{};
    panel_->setCursorHovered(cursor);
    panel_->setHoveredMarker(marker);
    const QString target = cursor ? QStringLiteral("cursor") : marker.toString(QUuid::WithoutBraces);
    if (cursor || !marker.isNull()) {
        setCursor(Qt::SizeHorCursor);
        if (hoverTipTarget_ != target) {
            hoverTipTarget_ = target;
            ToolTip::showText(mapToGlobal(pos), cursor ? panel_->cursorTooltip() : panel_->markerTooltip(marker), this);
        }
        return;
    }
    if (!hoverTipTarget_.isEmpty()) {
        hoverTipTarget_.clear();
        ToolTip::hideText();
    }
    if (panel_->hasTimeline()) {
        const Hit hit = hitAt(pos);
        const QUuid guid = hit.clipIndex >= 0
            ? panel_->clipGuid(hit.laneIndex, hit.clipIndex)
            : QUuid{};
        if (hitsEditCursor(pos) ||
            (hitsResizeEnd(hit, pos) &&
             (!clipSelected(guid) || selectedGuids_.size() == 1)))
            setCursor(Qt::SizeHorCursor);
        else
            unsetCursor();
        return;
    }
    const Hit hit = hitAt(pos);
    if (hit.clipIndex < 0) {
        unsetCursor();
        return;
    }
    const QUuid guid = panel_->clipGuid(hit.laneIndex, hit.clipIndex);
    if (hitsResizeEnd(hit, pos) &&
        (!clipSelected(guid) || selectedGuids_.size() == 1)) {
        setCursor(Qt::SizeHorCursor);
        return;
    }
    setCursor(Qt::OpenHandCursor);
}

bool TimelineCanvas::hitsEditCursor(const QPoint& point) const
{
    return rect().contains(point) && panel_->hitsEditCursor(point.x());
}

void TimelineCanvas::setDropFeedback(bool visible, int x, int lane,
                                    const QString& message, bool legal)
{
    dropVisible_ = visible;
    dropX_ = x;
    dropLane_ = lane;
    dropLegal_ = legal;
    if (visible)
        panel_->setStatusMessage(message, !legal);
    else
        panel_->setStatusMessage({}, false);
    if (panel_->hasTimeline()) {
        if (visible)
            ToolTip::showText(mapToGlobal(QPoint(std::clamp(x, 0, width()), 0)), message, this);
        else
            ToolTip::hideText();
    }
    update();
}

bool TimelineCanvas::acceptDrop(const QMimeData* mime, const QPoint& point, bool commit,
                               Qt::KeyboardModifiers modifiers)
{
    const Hit hit = hitAt(point);
    const int coordinateLane = std::max(0, hit.laneIndex);
    const int64_t rawStart = panel_->coordinateAtViewportPixel(coordinateLane, point.x());
    const bool disableSnap = modifiers.testFlag(Qt::AltModifier);
    const int64_t start = std::max<int64_t>(0,
        panel_->snapCoordinate(coordinateLane, rawStart, disableSnap));
    const int dropX = panel_->viewportPixelAtCoordinate(coordinateLane, start);
    const bool blank = hit.blank
        || (hit.laneIndex < 0
            && panel_->viewKind() == TimelineEditorPanel::ViewKind::Segment);

    if (AudioImportGate::mimeHasAudio(mime)) {
        const QStringList files = AudioImportGate::extractAudioFiles(mime);
        if (files.isEmpty()) {
            setDropFeedback(true, dropX, hit.laneIndex,
                            tr("No audio files in the drop."), false);
            setCursor(Qt::ForbiddenCursor);
            return false;
        }
        const QString blankAudioError =
            tr("Segment blank does not import audio. Drop onto a Track lane.");
        if (blank) {
            setDropFeedback(true, dropX, -1, blankAudioError, false);
            setCursor(Qt::ForbiddenCursor);
            if (commit)
                panel_->rejectInteraction(blankAudioError);
            return false;
        }
        if (hit.laneIndex < 0) {
            const QString error = tr("Audio can only be imported into a Track.");
            setDropFeedback(true, dropX, hit.laneIndex, error, false);
            setCursor(Qt::ForbiddenCursor);
            if (commit)
                panel_->rejectInteraction(error);
            return false;
        }
        ProjectItem* track = ProjectManager::instance().findItemByGuid(
            panel_->laneGuid(hit.laneIndex));
        const ImportPlacement placement = AudioImportGate::resolveImportParent(track);
        if (placement.kind != ImportPlacementKind::Parent) {
            setDropFeedback(true, dropX, hit.laneIndex,
                            tr("Audio cannot be imported into this Track."), false);
            setCursor(Qt::ForbiddenCursor);
            return false;
        }
        setDropFeedback(true, dropX, hit.laneIndex,
                        tr("Import into %1 and place a Clip at %2")
                            .arg(track ? track->name : tr("Track"),
                                 panel_->formatCoordinate(hit.laneIndex, start)),
                        true);
        setCursor(Qt::DragCopyCursor);
        if (!commit)
            return true;
        return panel_->importAudioFiles(files, hit.laneIndex, start, disableSnap);
    }

    if (!ProjectItemMime::hasObjectIDs(mime)) {
        setDropFeedback(true, dropX, hit.laneIndex,
                        tr("This drop is not a project object."), false);
        setCursor(Qt::ForbiddenCursor);
        return false;
    }
    const uint32_t id = ProjectItemMime::readSingleObjectID(mime);
    ProjectItem* target = ProjectManager::instance().findItemByObjectID(id);
    QString error;
    if (!panel_->validateClipTarget(target, blank ? -1 : hit.laneIndex, &error)) {
        setDropFeedback(true, dropX, hit.laneIndex, error, false);
        setCursor(Qt::ForbiddenCursor);
        if (commit)
            panel_->rejectInteraction(error);
        return false;
    }
    setDropFeedback(true, dropX, hit.laneIndex,
                    tr("Place %1 at %2")
                        .arg(target->name,
                             panel_->formatCoordinate(hit.laneIndex, start)),
                    true);
    setCursor(Qt::DragCopyCursor);
    if (!commit)
        return true;
    return panel_->placeItem(target, hit.laneIndex, start, disableSnap);
}

QMenu* TimelineCanvas::createClipMenu(int laneIndex, const QUuid& guid)
{
    ProjectItem* target = ProjectManager::instance().findItemByGuid(
        panel_->clipTargetGuid(laneIndex, panel_->findClipIndex(laneIndex, guid)));
    auto* menu = new QMenu(this);
    QAction* enter = menu->addAction(tr("Enter Timeline"));
    enter->setEnabled(target && target->type == ProjectItem::Type::Segment
                      && ItemTypeCatalog::isUserFacingType(target->type));
    connect(enter, &QAction::triggered, this, [this, laneIndex, guid] {
        panel_->drillIntoClip(laneIndex, guid);
    });
    QAction* inspect = menu->addAction(tr("Inspect Object"));
    inspect->setObjectName(QStringLiteral("timelineInspectObject"));
    connect(inspect, &QAction::triggered, this, [this, laneIndex, guid] {
        panel_->inspectClip(laneIndex, guid);
    });
    QAction* blueprint = menu->addAction(tr("Open in Blueprint"));
    connect(blueprint, &QAction::triggered, this, [this, laneIndex, guid] {
        panel_->requestBlueprintForClip(laneIndex, guid);
    });
    menu->addSeparator();
    if (panel_->hasTimeline()) {
        auto* start = menu->addAction(tr("Enter Start..."));
        start->setObjectName(QStringLiteral("timelineEditClipStart"));
        connect(start, &QAction::triggered, this, [this, laneIndex, guid] {
            editClipStart(laneIndex, guid);
        });
    }
    QAction* remove = menu->addAction(tr("Remove from Timeline"));
    remove->setObjectName(QStringLiteral("timelineRemoveClip"));
    remove->setToolTip(tr("Remove the Clip; keep the referenced object"));
    connect(remove, &QAction::triggered, this, [this] {
        panel_->deleteSelectedClips();
    });
    return menu;
}

void TimelineCanvas::editClipStart(int laneIndex, const QUuid& guid)
{
    const int index = panel_->findClipIndex(laneIndex, guid);
    if (!panel_->hasTimeline() || index < 0)
        return;
    cancelGesture();
    selectSingle(laneIndex, guid);
    const QUuid owner = panel_->currentItemGuid();
    const QUuid lane = panel_->laneGuid(laneIndex);
    const QUuid target = panel_->clipTargetGuid(laneIndex, index);
    const int64_t initial = panel_->clipStart(laneIndex, index);
    auto* dialog = new QDialog(this, Qt::Popup);
    valueEditor_ = dialog;
    dialog->setObjectName(QStringLiteral("timelineClipStartEditor"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QHBoxLayout(dialog);
    ThemeLayout::margins(layout, ThemeLayout::Space::Small);
    ThemeLayout::spacing(layout, ThemeLayout::Space::Small);
    auto* label = new QLabel(tr("Start"), dialog);
    label->setProperty("textRole", QStringLiteral("formLabel"));
    layout->addWidget(label);
    auto* spin = new DecimalSpinBox(dialog);
    spin->setObjectName(QStringLiteral("timelineClipStartInput"));
    spin->setKeyboardTracking(false);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setDecimals(9);
    const bool musical = panel_->timelineDomain() == TimelineDomain::Musical;
    const double divisor = musical ? TimelineCoordinates::kPpq : TimelineCoordinates::kNanosecondsPerSecond;
    const double limit = std::nextafter(double(std::numeric_limits<int64_t>::max()) / divisor, 0.0);
    spin->setRange(0.0, limit);
    spin->setSingleStep(musical ? 1.0 / TimelineCoordinates::kPpq : 0.001);
    spin->setSuffix(musical ? tr(" beats") : QStringLiteral(" s"));
    spin->setValue(double(initial) / divisor);
    const double initialValue = spin->value();
    ThemeManager::instance().bindMetrics(spin, [spin](const ThemeMetrics& metrics) {
        spin->setMinimumWidth(metrics.popup.minimumWidth);
    });
    layout->addWidget(spin);
    for (int key : {Qt::Key_Return, Qt::Key_Enter}) {
        auto* shortcut = new QShortcut(QKeySequence(key), dialog);
        connect(shortcut, &QShortcut::activated, dialog, [dialog, spin] {
            spin->interpretText();
            dialog->accept();
        });
    }
    connect(dialog, &QDialog::accepted, this,
        [this, laneIndex, guid, owner, lane, target, initial, initialValue, spin, divisor] {
        valueEditor_.clear();
        const int current = panel_->findClipIndex(laneIndex, guid);
        if (!panel_->hasTimeline()
            || panel_->currentItemGuid() != owner || panel_->laneGuid(laneIndex) != lane
            || selectedClipGuid() != guid || current < 0
            || panel_->clipTargetGuid(laneIndex, current) != target
            || panel_->clipStart(laneIndex, current) != initial || spin->value() == initialValue)
            return;
        panel_->moveClip(laneIndex, guid, laneIndex, int64_t(std::llround(spin->value() * divisor)), true);
    });
    connect(dialog, &QDialog::finished, this, [this] { valueEditor_.clear(); update(); });
    dialog->adjustSize();
    const QPoint anchor = mapToGlobal(rangeRect(laneIndex, initial, panel_->clipDuration(laneIndex, index))
        .intersected(rect()).topLeft());
    QPoint position(anchor.x(), anchor.y() - dialog->height() - 6);
    if (const auto* screen = QGuiApplication::screenAt(anchor)) {
        const QRect available = screen->availableGeometry();
        position.setX(std::clamp(position.x(), available.left(),
            std::max(available.left(), available.right() - dialog->width() + 1)));
        position.setY(std::max(available.top(), position.y()));
    }
    dialog->move(position);
    dialog->show();
    spin->setFocus();
    spin->selectAll();
}

QMenu* TimelineCanvas::createLaneBlankMenu(int laneIndex, int64_t position)
{
    auto* menu = new QMenu(this);
    ProjectItem* track = laneIndex >= 0
        ? ProjectManager::instance().findItemByGuid(panel_->laneGuid(laneIndex))
        : nullptr;
    if (track) {
        QAction* add = menu->addAction(tr("Add Clip..."));
        add->setObjectName(QStringLiteral("timelineAddClip"));
        connect(add, &QAction::triggered, this, [this, menu, laneIndex, position] {
            const QPointer<TimelineCanvas> self(this);
            const QUuid owner = panel_->currentItemGuid();
            const QUuid trackGuid = panel_->laneGuid(laneIndex);
            SelectorRequest request;
            request.title = tr("Add Clip");
            request.directChildParentGuid = trackGuid;
            request.filterTypes = ItemTypeCatalog::userFacingChildTypesOf(ProjectItem::Type::Track);
            request.isSelectable = [self, owner, trackGuid, laneIndex](uint32_t id) {
                return self && self->panel_->currentItemGuid() == owner
                    && self->panel_->laneGuid(laneIndex) == trackGuid
                    && self->panel_->validateClipTarget(
                        ProjectManager::instance().findItemByObjectID(id), laneIndex);
            };
            request.onSelected = [self, owner, trackGuid, laneIndex, position](uint32_t id) {
                if (!self || self->panel_->currentItemGuid() != owner
                    || self->panel_->laneGuid(laneIndex) != trackGuid) return;
                self->panel_->placeItem(ProjectManager::instance().findItemByObjectID(id),
                                       laneIndex, position);
            };
            SelectorWindow::open(request, this, QRect(menu->pos(), QSize(1, 1)));
        });
    }
    if (panel_->viewKind() == TimelineEditorPanel::ViewKind::Segment
        && ItemTypeCatalog::isUserFacingType(ProjectItem::Type::Track)) {
        menu->addSeparator();
        QAction* addTrack = menu->addAction(tr("Add Track"));
        addTrack->setObjectName(QStringLiteral("timelineAddTrack"));
        connect(addTrack, &QAction::triggered, panel_, &TimelineEditorPanel::addTrack);
    }
    return menu;
}

}
