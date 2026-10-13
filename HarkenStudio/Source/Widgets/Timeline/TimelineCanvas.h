#pragma once

#include "TimelineRenderer.h"

#include "Core/AssetPipeline/WaveformCache.h"

#include <QHash>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QSet>
#include <QUuid>
#include <QWidget>

#include <cstdint>
#include <vector>

class QKeyEvent;
class QDialog;
class QMenu;
class QMimeData;

namespace VNX {

struct ProjectItem;
class TimelineEditorPanel;

class TimelineCanvas final : public QWidget
{
    Q_OBJECT

public:
    explicit TimelineCanvas(TimelineEditorPanel* panel, QWidget* parent = nullptr);

    QUuid selectedClipGuid() const;
    QSet<QUuid> selectedClipGuids() const { return selectedGuids_; }
    QUuid selectedClipAnchor() const { return selectionAnchor_; }
    int selectedLaneIndex() const { return selectedLane_; }
    void setSelectedClip(int laneIndex, const QUuid& guid);
    void restoreSelectedClip(const QUuid& guid);
    void restoreSelectedClips(const QSet<QUuid>& guids, const QUuid& anchor);
    const TimelineRenderData& lastRenderData() const { return lastRenderData_; }
    bool isGestureActive() const;
    void clearHover();
    void invalidateWaveforms();
    int previewLane() const { return previewLane_; }
    int64_t previewStart() const { return previewStart_; }
    int64_t previewDuration() const { return previewDuration_; }
    bool snapGuideVisible() const { return snapGuideVisible_; }
    int hoveredClipIndex() const { return hoveredClip_; }
    const WaveformPeakData* cachedWaveform(const QString& absolutePath) const
    {
        return waveformForPath(absolutePath);
    }
    QMenu* createClipMenu(int laneIndex, const QUuid& guid);
    QMenu* createLaneBlankMenu(int laneIndex, int64_t position);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    enum class DragMode { None, Move, ResizeEnd, Lasso, Cursor };

    struct Hit {
        int laneIndex = -1;
        int clipIndex = -1;
        bool blank = false;
    };

    struct GestureClip {
        int laneIndex = -1;
        QUuid guid;
        int64_t start = 0;
        int64_t duration = 1;
    };

    TimelineStyleView styleView() const;
    TimelineRenderData buildRenderData() const;
    QRect laneRect(int laneIndex) const;
    QRect blankRect() const;
    QRect rangeRect(int laneIndex, int64_t start, int64_t duration) const;
    Hit hitAt(const QPoint& point) const;
    bool hitsResizeEnd(const Hit& hit, const QPoint& point) const;
    bool hitsEditCursor(const QPoint& point) const;
    int laneAtY(int y) const;
    const ThemeMetrics::Timeline& timelineMetrics() const;
    const WaveformPeakData* waveformForPath(const QString& absolutePath) const;
    void fillClipItem(TimelineClipItem& item, int laneIndex, int clipIndex,
                      const QRect& area, const QUuid& guid, int64_t start,
                      int64_t duration) const;
    void selectSingle(int laneIndex, const QUuid& guid);
    void toggleSelected(int laneIndex, const QUuid& guid);
    void selectRange(int laneIndex, const QUuid& guid);
    void clearSelection();
    void updateLassoSelection(const QPoint& pos);
    void armDrag(const Hit& hit, const QPoint& pos);
    bool applyDragPreview(const QPoint& pos, bool disableSnap);
    void commitDrag(bool disableSnap);
    void cancelGesture();
    int64_t magnetSnap(int laneIndex, int64_t coordinate, int64_t excludeStart,
                       int64_t excludeEnd) const;
    bool clipSelected(const QUuid& guid) const;
    int destinationLaneAt(int y) const;
    int previewLaneForClip(const GestureClip& clip) const;
    void updateHoverCursor(const QPoint& pos);
    void editClipStart(int laneIndex, const QUuid& guid);
    bool acceptDrop(const QMimeData* mime, const QPoint& point, bool commit,
                    Qt::KeyboardModifiers modifiers);
    void setDropFeedback(bool visible, int x, int lane, const QString& message,
                         bool legal);
    TimelineEditorPanel* panel_ = nullptr;
    TimelineRenderData lastRenderData_;
    mutable QHash<QString, WaveformPeakData> waveformCache_;
    int hoveredLane_ = -1;
    int hoveredClip_ = -1;
    QString hoverTipTarget_;
    QSet<QUuid> selectedGuids_;
    QUuid selectionAnchor_;
    int selectedLane_ = -1;
    QUuid activeGuid_;
    int activeLane_ = -1;
    DragMode dragMode_ = DragMode::None;
    bool dragMoved_ = false;
    QPoint pressPos_;
    int64_t pressCoordinate_ = 0;
    int64_t originalStart_ = 0;
    int64_t originalDuration_ = 1;
    int64_t previewStart_ = 0;
    int64_t previewDuration_ = 1;
    int previewLane_ = -1;
    int64_t intendedStart_ = 0;
    std::vector<GestureClip> gestureClips_;
    bool lassoActive_ = false;
    QPoint lassoStart_;
    QRect lassoRect_;
    QSet<QUuid> lassoBaseSelection_;
    Qt::KeyboardModifiers lassoModifiers_;
    bool dropVisible_ = false;
    int dropX_ = 0;
    int dropLane_ = -1;
    bool dropLegal_ = false;
    bool snapGuideVisible_ = false;
    int snapGuideX_ = 0;
    bool ghostVisible_ = false;
    QRect ghostRect_;
    QPointer<QDialog> valueEditor_;
};

}
