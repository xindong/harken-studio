#include "TimelineCanvas.h"

#include "TimelineEditorPanel.h"
#include "Core/FontManager.h"
#include "TimelineTicks.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AssetPipeline/AudioImportGate.h"
#include "Core/Components/VPiece.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/ItemTypeIcons.h"
#include "Core/LanguageContextService.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/ShortcutManager.h"
#include "Core/SoundSourceEditing.h"
#include "Core/SoundSourceSemantics.h"
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
#include <QSet>
#include <QScreen>
#include <QShortcut>
#include <QToolTip>
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
    ShortcutManager::instance().bindWidgetAction(Shortcuts::Delete, this, [this]() {
        panel_->deleteSelectedClips();
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
    hoveredLane_ = -1;
    hoveredClip_ = -1;
    hoverTipTarget_.clear();
    QToolTip::hideText();
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
    if (panel_->viewKind() != TimelineEditorPanel::ViewKind::Piece)
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
    const int inset = panel_->viewKind() == TimelineEditorPanel::ViewKind::Track
        ? 0 : metrics.clipVerticalInset;
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
    if (WaveformCache::load(absolutePath, peaks) && peaks.isValid()) {
        auto inserted = waveformCache_.insert(absolutePath, peaks);
        return &*inserted;
    }
    return nullptr;
}

void TimelineCanvas::fillClipItem(TimelineClipItem& item, int laneIndex, int clipIndex,
                                  const QRect& area, const QUuid& guid, int64_t start,
                                  int64_t duration) const
{
    item.rect = area;
    item.laneIndex = laneIndex;
    item.clipIndex = clipIndex;
    item.guid = guid;
    item.foreign = panel_->clipIsExternal(laneIndex, clipIndex);
    if (clipSelected(guid))
        item.state = TimelineClipVisualState::Selected;
    else if (laneIndex == hoveredLane_ && clipIndex == hoveredClip_)
        item.state = TimelineClipVisualState::Hover;

    ProjectItem* target = ProjectManager::instance().findItemByGuid(
        panel_->clipTargetGuid(laneIndex, clipIndex));
    item.label = target ? target->name : tr("Missing");
    if (target && target->color != 0)
        item.objectColor = QColor::fromRgba(target->color);
    if (target)
        item.typeIcon = iconForItem(*target);
    if (target && target->type == ProjectItem::Type::Sound) {
        const uint32_t sourceId = SoundSourceEditing::activeSourceId(
            *target, PlatformContextService::instance().context().primary,
            SoundSourceEditing::previewLanguage(
                *target, LanguageContextService::instance().language()));
        if (sourceId != 0) {
            if (const AssetInfo* asset = AssetRegistry::instance().getAsset(sourceId)) {
                item.waveformPath = asset->absolutePath;
                if (panel_->hasTimeline()) {
                    const auto* source = SoundSourceEditing::activeSource(*target,
                        PlatformContextService::instance().context().primary,
                        SoundSourceEditing::previewLanguage(*target, LanguageContextService::instance().language()));
                    const auto range = source ? SoundSourceSemantics::effectiveRange(*source, asset) : std::nullopt;
                    const auto sourceDuration =
                        panel_->clipSourceDuration(laneIndex, clipIndex);
                    if (range && sourceDuration && *sourceDuration > 0) {
                        const double left = panel_->viewportPositionAtCoordinate(laneIndex, start);
                        const int64_t contributionDuration = std::min(
                            duration, *sourceDuration);
                        const double right = panel_->viewportPositionAtCoordinate(
                            laneIndex, saturatingAdd(start, contributionDuration));
                        const double sourceRight = panel_->viewportPositionAtCoordinate(
                            laneIndex, saturatingAdd(start, *sourceDuration));
                        const double visibleLeft = std::clamp(left, 0.0, double(width()));
                        const double visibleRight = std::clamp(right, 0.0, double(width()));
                        if (visibleRight > visibleLeft && sourceRight > left) {
                            const int labelHeight = QFontMetrics(FontManager::instance().getFont(FontRole::Caption)).height();
                            if (panel_->viewKind() == TimelineEditorPanel::ViewKind::Track) {
                                item.waveformViewport = panel_->trackWaveformRect();
                                item.waveformRect = waveformAmplitudeBounds(item.waveformViewport,
                                    int(std::lround(item.waveformViewport.height() * panel_->verticalZoom())),
                                    panel_->verticalScroll(), QFontMetrics(FontManager::instance().getFont(FontRole::Axis)));
                            } else {
                                item.waveformViewport = area.adjusted(1, labelHeight + 3, -1, -2);
                                item.waveformRect = item.waveformViewport;
                            }
                            item.waveformRect.setLeft(int(std::lround(visibleLeft)));
                            item.waveformRect.setWidth(int(std::lround(visibleRight)) - int(std::lround(visibleLeft)));
                            const double frames = double(range->trimEndFrame - range->trimStartFrame);
                            item.waveformStartFrame = range->trimStartFrame
                                + frames * (visibleLeft - left) / (sourceRight - left);
                            item.waveformFrameCount = frames * (visibleRight - visibleLeft) / (sourceRight - left);
                        }
                    }
                }
            }
        }
    }

    QStringList tooltip;
    tooltip << item.label;
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
    if (item.foreign && target) {
        const QString home = target->parent ? target->parent->name : tr("unparented");
        tooltip << tr("Placement only — its home in the tree is %1").arg(home);
        TimelineClipBadge badge;
        badge.kind = TimelineClipBadgeKind::External;
        badge.text = QStringLiteral("EXT");
        item.badges.append(badge);
    }
    if (const auto emergence = panel_->emergenceCoordinate(laneIndex, clipIndex)) {
        const int realX = panel_->viewportPixelAtCoordinate(laneIndex, *emergence);
        item.emergenceCoordinate = *emergence;
        item.emergencePixel = realX;
        item.emergenceClamped = realX != std::clamp(realX, area.left(), area.right());
        tooltip << tr("Emerges at %1").arg(panel_->formatCoordinate(laneIndex, *emergence));
    }
    item.tooltip = tooltip.join(QStringLiteral("\n"));
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
    for (int i = panel_->clipCount(lane) - 1; i >= 0; --i) {
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
    style.placeholderText = snapshot->ui.colors.textMuted;
    style.axisText = snapshot->ui.colors.textSecondary;
    style.trackBackground = snapshot->ui.colors.trackBackground;
    style.trackBackgroundAlt = snapshot->ui.colors.trackBackgroundAlt;
    style.timeline = snapshot->domains.timeline;
    style.music = snapshot->domains.music;
    if (panel_->hasTimeline())
        style.music.cursor = snapshot->ui.colors.success;
    style.waveform = snapshot->domains.waveform;
    style.waveformExponent = panel_->viewKind() == TimelineEditorPanel::ViewKind::Track
        ? panel_->amplitudeExponent() : 1.0;
    style.curve = snapshot->domains.curve;
    style.metrics = snapshot->ui.metrics;
    style.accent = snapshot->ui.colors.accent;
    style.textMuted = snapshot->ui.colors.textMuted;
    return style;
}

TimelineRenderData TimelineCanvas::buildRenderData() const
{
    TimelineRenderData data;
    data.viewport = rect();
    data.frameless = panel_->viewKind() == TimelineEditorPanel::ViewKind::Track;
    if (data.frameless)
        data.amplitudeTicks = TimelineRenderer::amplitudeTicks(panel_->trackWaveformRect(),
            panel_->verticalZoom(), panel_->verticalScroll(), *ThemeManager::instance().snapshot(),
            panel_->amplitudeExponent());
    if (panel_->viewKind() == TimelineEditorPanel::ViewKind::Unsupported) {
        data.placeholder = panel_->stateText();
        return data;
    }
    data.viewOffset = panel_->horizontalScroll();
    data.pxPerUnit = panel_->pixelsPerUnit();
    data.timeAxis = panel_->timeTicks();
    data.domain = panel_->timelineDomain();
    for (int lane = 0; lane < panel_->laneCount(); ++lane) {
        TimelineLaneRect rect;
        rect.rect = laneRect(lane);
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
        const int count = panel_->clipCount(lane);
        for (int i = 0; i < count; ++i) {
            const QUuid guid = panel_->clipGuid(lane, i);
            int64_t start = panel_->clipStart(lane, i);
            int64_t duration = panel_->clipDuration(lane, i);
            const bool dragging = dragMoved_ &&
                ((clipSelected(guid) && dragMode_ == DragMode::Move) ||
                 (guid == activeGuid_ && dragMode_ == DragMode::ResizeEnd));
            if (dragging)
                continue;
            const QRect area = rangeRect(lane, start, duration);
            if (area.isEmpty())
                continue;
            TimelineClipItem item;
            fillClipItem(item, lane, i, area, guid, start, duration);
            data.clipItems.append(item);
        }

    }
    if (dragMoved_ && dragMode_ == DragMode::Move && previewLane_ >= 0) {
        const int laneDelta = previewLane_ - activeLane_;
        for (const GestureClip& clip : gestureClips_) {
            const int64_t start = saturatingAdd(previewStart_,
                                                saturatingSubtract(clip.start, originalStart_));
            const int destLane = clip.laneIndex + laneDelta;
            const bool illegal = destLane < 0 || destLane >= panel_->laneCount();
            const int clipIndex = panel_->findClipIndex(clip.laneIndex, clip.guid);
            if (clipIndex < 0)
                continue;
            const int drawLane = illegal ? clip.laneIndex : destLane;
            const QRect area = rangeRect(drawLane, start, clip.duration);
            if (area.isEmpty())
                continue;
            TimelineClipItem item;
            fillClipItem(item, clip.laneIndex, clipIndex, area, clip.guid, start,
                         clip.duration);
            item.residual = false;
            item.state = data.frameless ? TimelineClipVisualState::Dragging : TimelineClipVisualState::Selected;
            item.foreign = item.foreign || illegal;
            data.clipItems.append(item);
            if (!data.frameless) {
                TimelineGhost ghost;
                ghost.visible = true;
                ghost.rect = area;
                ghost.laneIndex = destLane;
                ghost.illegal = illegal;
                data.ghosts.append(ghost);
            }
        }
    }
    if (dragMoved_ && dragMode_ == DragMode::ResizeEnd && activeLane_ >= 0) {
        const int clipIndex = panel_->findClipIndex(activeLane_, activeGuid_);
        const QRect area = rangeRect(
            activeLane_, originalStart_, previewDuration_);
        if (clipIndex >= 0 && !area.isEmpty()) {
            TimelineClipItem item;
            fillClipItem(item, activeLane_, clipIndex, area, activeGuid_,
                         originalStart_, previewDuration_);
            item.state = data.frameless
                ? TimelineClipVisualState::Dragging
                : TimelineClipVisualState::Selected;
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
    if (!data.frameless && ghostVisible_ && !ghostRect_.isEmpty() && data.ghosts.isEmpty()) {
        TimelineGhost ghost;
        ghost.visible = true;
        ghost.rect = ghostRect_;
        data.ghosts.append(ghost);
    }
    return data;
}

void TimelineCanvas::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    lastRenderData_ = buildRenderData();
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
        const Hit hit = hitsEditCursor(event->position().toPoint()) ? Hit{} : hitAt(event->position().toPoint());
        if (hoveredLane_ != hit.laneIndex || hoveredClip_ != hit.clipIndex) {
            hoveredLane_ = hit.laneIndex;
            hoveredClip_ = hit.clipIndex;
            update();
        }
        updateHoverCursor(event->position().toPoint());
        if (panel_->hasTimeline())
            return;
        if (hit.clipIndex >= 0) {
            QString tip;
            QString target;
            for (const TimelineClipItem& item : lastRenderData_.clipItems) {
                if (item.rect.contains(event->position().toPoint())
                    && !item.tooltip.isEmpty() && !item.residual) {
                    tip = item.tooltip;
                    target = item.guid.toString(QUuid::WithoutBraces);
                    break;
                }
            }
            if (!tip.isEmpty() && hoverTipTarget_ != target) {
                hoverTipTarget_ = target;
                QToolTip::showText(event->globalPosition().toPoint(), tip, this);
            }
        } else if (!hoverTipTarget_.isEmpty()) {
            hoverTipTarget_.clear();
            QToolTip::hideText();
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
    const Hit hit = hitAt(event->position().toPoint());
    if (hit.clipIndex >= 0) {
        const QUuid guid = panel_->clipGuid(hit.laneIndex, hit.clipIndex);
        ProjectItem* target = ProjectManager::instance().findItemByGuid(
            panel_->clipTargetGuid(hit.laneIndex, hit.clipIndex));
        if (target && target->type == ProjectItem::Type::Piece
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
        if (panel_->viewKind() == TimelineEditorPanel::ViewKind::Track
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
                end = std::max(end, panel_->clipStart(lane, clip)
                                        + panel_->clipDuration(lane, clip));
        }
        panel_->setEditCursor(end);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TimelineCanvas::hideEvent(QHideEvent* event)
{
    if (dragMode_ == DragMode::Cursor)
        panel_->endCursorDrag();
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
    intendedStart_ = originalStart_;
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

int TimelineCanvas::destinationLaneAt(int) const
{
    return activeLane_;
}

int TimelineCanvas::previewLaneForClip(const GestureClip& clip) const
{
    return clip.laneIndex;
}

bool TimelineCanvas::applyDragPreview(const QPoint& pos, bool disableSnap)
{
    const int destLane = destinationLaneAt(pos.y());
    const int64_t current = panel_->coordinateAtViewportPixel(std::max(0, destLane), pos.x());
    const int64_t delta = saturatingSubtract(current, pressCoordinate_);
    snapGuideVisible_ = false;
    ghostVisible_ = false;
    if (dragMode_ == DragMode::Move) {
        intendedStart_ = saturatingAdd(originalStart_, delta);
        previewLane_ = destLane;
        int64_t applied = intendedStart_;
        if (!disableSnap)
            applied = magnetSnap(destLane, intendedStart_, originalStart_,
                                 saturatingAdd(originalStart_, originalDuration_));
        previewStart_ = applied;
        previewDuration_ = originalDuration_;
        if (applied != intendedStart_) {
            ghostVisible_ = true;
            ghostRect_ = rangeRect(destLane, intendedStart_, originalDuration_);
        }
        const int snappedPixel = panel_->viewportPixelAtCoordinate(destLane, applied);
        snapGuideVisible_ = !disableSnap && applied != intendedStart_;
        snapGuideX_ = snappedPixel;
        panel_->setStatusMessage(
            tr("Move to %1").arg(panel_->formatCoordinate(destLane, applied)), false);
        if (panel_->hasTimeline())
            QToolTip::showText(mapToGlobal(pos), tr("Start: %1").arg(panel_->formatCoordinate(destLane, applied)), this);
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
        if (gestureClips_.size() == 1) {
            panel_->moveClip(activeLane_, activeGuid_, previewLane_, previewStart_, true);
            return;
        }
        std::vector<ActionResult> results;
        results.reserve(gestureClips_.size());
        auto& manager = ProjectManager::instance();
        for (const GestureClip& clip : gestureClips_) {
            const int destLane = previewLaneForClip(clip);
            if (destLane < 0 || destLane >= panel_->laneCount()) {
                panel_->rejectInteraction(tr("The selection cannot move outside the timeline."));
                return;
            }
        }
        for (const GestureClip& clip : gestureClips_) {
            const int64_t start = saturatingAdd(
                previewStart_, saturatingSubtract(clip.start, originalStart_));
            const int destLane = previewLaneForClip(clip);
            ProjectItem* source = manager.findItemByGuid(panel_->laneGuid(clip.laneIndex));
            ProjectItem* destination = manager.findItemByGuid(panel_->laneGuid(destLane));
            if (!source || !destination) {
                panel_->rejectInteraction(tr("The Clip or destination Track is unavailable."));
                return;
            }
            ActionResult result = source == destination
                ? TimelineEditing::moveClip(&manager, source, clip.guid, start, panel_)
                : TimelineEditing::moveClipAcrossTracks(&manager, source, destination,
                                                        clip.guid, start, panel_);
            results.push_back(std::move(result));
        }
        panel_->commitTimelineActions(std::move(results), tr("Move Clips"));
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
    ghostVisible_ = false;
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
    if (!panel_->isEditCursorVisible() || !rect().contains(point))
        return false;
    const int x = panel_->viewportPixelAtCoordinate(0, panel_->editCursor());
    return x >= 0 && x < width() && std::abs(point.x() - x) <= 3;
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
            QToolTip::showText(mapToGlobal(QPoint(std::clamp(x, 0, width()), 0)), message, this);
        else
            QToolTip::hideText();
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
    const int64_t start = panel_->snapCoordinate(coordinateLane, rawStart, disableSnap);
    const int dropX = panel_->viewportPixelAtCoordinate(coordinateLane, start);
    const bool blank = hit.blank
        || (hit.laneIndex < 0
            && panel_->viewKind() == TimelineEditorPanel::ViewKind::Piece);

    if (AudioImportGate::mimeHasAudio(mime)) {
        const QStringList files = AudioImportGate::extractAudioFiles(mime);
        if (files.isEmpty()) {
            setDropFeedback(true, dropX, hit.laneIndex,
                            tr("No audio files in the drop."), false);
            setCursor(Qt::ForbiddenCursor);
            return false;
        }
        const QString blankAudioError =
            tr("Piece blank does not import audio. Drop onto a Track lane.");
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
    enter->setEnabled(target && target->type == ProjectItem::Type::Piece
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
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(8);
    layout->addWidget(new QLabel(tr("Start"), dialog));
    auto* spin = new DecimalSpinBox(dialog);
    spin->setObjectName(QStringLiteral("timelineClipStartInput"));
    spin->setKeyboardTracking(false);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setDecimals(9);
    const double divisor = TimelineCoordinates::kNanosecondsPerSecond;
    const double limit = std::nextafter(double(std::numeric_limits<int64_t>::max()) / divisor, 0.0);
    spin->setRange(-limit, limit);
    spin->setSingleStep(0.001);
    spin->setSuffix(QStringLiteral(" s"));
    spin->setValue(double(initial) / divisor);
    const double initialValue = spin->value();
    spin->setMinimumWidth(158);
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
        connect(add, &QAction::triggered, this, [this, laneIndex, position] {
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
            SelectorWindow::open(request, this);
        });
    }
    if (panel_->viewKind() == TimelineEditorPanel::ViewKind::Piece
        && ItemTypeCatalog::isUserFacingType(ProjectItem::Type::Track)) {
        menu->addSeparator();
        QAction* addTrack = menu->addAction(tr("Add Track"));
        addTrack->setObjectName(QStringLiteral("timelineAddTrack"));
        connect(addTrack, &QAction::triggered, this, [this] {
            ProjectManager::instance().createItem(
                panel_->boundItem(),
                QStringLiteral("Track"),
                ProjectItem::Type::Track);
        });
    }
    return menu;
}

}
