#pragma once

#include "Core/ActionManager.h"
#include "Core/PlacementContext.h"
#include "Core/ProjectTypes.h"
#include "Core/ThemeSnapshot.h"
#include "Core/Timeline/TimelineSemantics.h"
#include "Widgets/Common/TimeAxis.h"

#include <QSet>
#include <QHash>
#include <QFontMetrics>
#include <QPointer>
#include <QUuid>
#include <QVariantMap>
#include <QWidget>

#include <vector>
#include <cstdint>
#include <optional>
#include <memory>

class QEvent;
class QFocusEvent;
class QObject;
class QResizeEvent;
class QLabel;
class QDoubleSpinBox;
class QComboBox;
class QDialog;
class QFrame;
class QHBoxLayout;
class QScrollArea;
class QScrollBar;
class QSpinBox;
class QStackedWidget;
class QToolButton;
class QTimer;
class QMenu;
class QVBoxLayout;
class QWheelEvent;

namespace VNX {

struct ProjectItem;
class ThemeManager;
class TimelineCanvas;
class TimelineHeaderColumn;
class TimelineRuler;
namespace TimelineEditing { struct ClipMove; }

class TimelineEditorPanel final : public QWidget
{
    Q_OBJECT

public:
    enum class ViewKind { Empty, Track, Segment, Unsupported };
    enum class AxisMode { Seconds, MeasureBeats, SecondsWithMeasureBeats, MeasureBeatsWithSeconds };
    Q_ENUM(AxisMode)

    explicit TimelineEditorPanel(QWidget* parent = nullptr,
                                 ThemeManager* themeManager = nullptr);
    ~TimelineEditorPanel() override;

    static bool acceptsItem(const ProjectItem* item);
    static ProjectItem* timelineOwner(ProjectItem* item);
    bool selectLane(const QUuid& guid, bool publish = false);
    bool reorderLane(int from, int to);

    int panelId() const { return panelId_; }
    void setPanelId(int id) { panelId_ = id; }
    bool isPinned() const { return pinned_; }
    void setPinned(bool pinned);

    QUuid currentItemGuid() const { return boundGuid_; }
    ProjectItem* boundItem() const { return boundItem_; }
    ViewKind viewKind() const { return viewKind_; }
    bool hasTimeline() const { return viewKind_ == ViewKind::Track || viewKind_ == ViewKind::Segment; }
    QString stateText() const;
    int laneCount() const { return static_cast<int>(lanes_.size()); }
    QUuid laneGuid(int index) const;
    QString laneName(int index) const;
    int clipCount(int laneIndex) const;
    QUuid clipGuid(int laneIndex, int clipIndex) const;
    QUuid clipTargetGuid(int laneIndex, int clipIndex) const;
    ProjectItem* clipTarget(int laneIndex, int clipIndex) const;
    int64_t clipStart(int laneIndex, int clipIndex) const;
    int64_t clipDuration(int laneIndex, int clipIndex) const;
    std::optional<int64_t> clipSourceDuration(int laneIndex, int clipIndex) const;
    QSet<QUuid> selectedClipGuids() const;
    QUuid selectedClipAnchor() const;
    int findLaneOfClip(const QUuid& guid) const;
    int findClipIndex(int laneIndex, const QUuid& guid) const;
    double zoomPercent() const;
    double pixelsPerUnit() const;
    int pixelOrigin() const;
    int timelineViewportWidth() const;
    double horizontalScroll() const;
    void panHorizontally(double pixels);
    void scrollTracks(QWheelEvent* event);
    QRect trackWaveformRect() const;
    double verticalZoom() const { return segmentVerticalZoom_; }
    int verticalScroll() const;
    void setVerticalScroll(int value);
    void zoomVertically(int canvasY, double factor);
    double amplitudeExponent() const { return amplitudeExponent_; }
    void setAmplitudeExponent(double exponent);
    TimeAxisTicks timeTicks() const;
    AxisMode axisMode() const;
    void setAxisMode(AxisMode mode);
    bool hasMeasureBeatsPrimaryAxis() const;
    bool hasSecondaryAxis() const;
    int64_t viewOriginCoordinate() const;
    int64_t editCursor() const { return editCursor_; }
    bool isEditCursorVisible() const { return editCursorVisible_ && hasTimeline(); }
    void setEditCursorVisible(bool visible);
    bool isCursorDragActive() const { return cursorDrag_.has_value(); }
    bool hitsEditCursor(int viewportX) const;
    bool isCursorHovered() const { return cursorHovered_; }
    void setCursorHovered(bool hovered);
    QString cursorTooltip() const;
    void beginCursorDrag(int canvasX, bool disableSnap, bool locate);
    void updateCursorDrag(int canvasX, bool disableSnap);
    void endCursorDrag(bool cancel = false);
    void editCursorValue();
    void showCursorContextMenu(const QPoint& position);
    std::optional<int64_t> playheadCoordinate() const { return playhead_; }
    QVariantMap snapshotState() const;
    QString lastInteractionError() const { return lastInteractionError_; }
    bool canDrillBack() const { return !drillPath_.empty(); }
    int drillDepth() const { return static_cast<int>(drillPath_.size()); }
    QString formatCoordinate(int laneIndex, int64_t coordinate) const;
    int64_t coordinateAtPixel(int laneIndex, double pixel) const;
    double pixelAtCoordinate(int laneIndex, int64_t coordinate) const;
    int64_t coordinateAtViewportPixel(int laneIndex, double pixel) const;
    double viewportPositionAtCoordinate(int laneIndex, int64_t coordinate) const;
    int viewportPixelAtCoordinate(int laneIndex, int64_t coordinate) const;
    std::optional<int64_t> emergenceCoordinate(int laneIndex, int clipIndex) const;
    SegmentTimeSpec effectiveTimeSpec(int laneIndex) const;
    TimelineDomain timelineDomain() const;
    int64_t tempoMicrosecondsPerQuarter() const;
    int meterNumerator() const;
    int meterDenominator() const;
    int markerCount() const;
    QUuid markerGuid(int index) const;
    MarkerKind markerKind(int index) const;
    QString markerName(int index) const;
    int64_t markerPosition(int index) const;
    int64_t markerDisplayPosition(int index) const;
    QUuid hoveredMarkerGuid() const;
    QUuid markerAtViewportPixel(int x) const;
    void setHoveredMarker(const QUuid& guid);
    bool isMarkerDragActive() const { return markerDrag_.has_value(); }
    bool isMarkerDragging() const { return markerDrag_ && markerDrag_->moved; }
    QUuid draggingMarkerGuid() const;
    int64_t markerDragPreviewPosition() const;
    void beginMarkerDrag(const QUuid& guid, int viewportX, bool disableSnap);
    void updateMarkerDrag(int viewportX, bool disableSnap);
    void endMarkerDrag(bool cancel = false);
    QString markerTooltip(const QUuid& guid) const;
    void editMarkerValue(const QUuid& guid);
    QMenu* createMarkerContextMenu(const QUuid& guid, QWidget* parent = nullptr);
    void showMarkerContextMenu(const QUuid& guid, const QPoint& globalPos);
    void cancelMarkerInteraction();
    QUuid selectedMarkerGuid() const { return selectedMarkerGuid_; }
    void setSelectedMarker(const QUuid& guid);
    bool hasMarkerKind(MarkerKind kind) const;
    TimelineSemanticsResult timelineSemantics() const;
    QString nominalSummary() const;
    std::vector<TimelineSpan> overlapRegions(int laneIndex) const;
    const ThemeMetrics::Timeline& timelineMetrics() const;
    int laneHeaderWidth() const;
    int laneHeaderGap() const;
    bool laneHeaderResizeHighlighted() const { return laneHeaderResizeHighlighted_; }
    QRect laneRect(int index) const;
    int laneHeight(int index) const;
    void setLaneHeight(int index, int height);
    int headerSelectedLane() const;
    void setLaneHeaderWidth(int width);
    void resetLaneHeaderWidth();
    void fitClips();
    void addTrack();

    bool placeItem(ProjectItem* target, int laneIndex, int64_t start,
                   bool disableSnap = false);
    bool validateClipTarget(ProjectItem* target, int laneIndex, QString* error = nullptr) const;
    bool moveClip(int sourceLaneIndex, const QUuid& clipGuid,
                  int destinationLaneIndex, int64_t start,
                  bool disableSnap = false);
    bool resizeClip(int laneIndex, const QUuid& clipGuid,
                    int64_t start, int64_t duration,
                    bool disableSnap = false);
    bool deleteClip(int laneIndex, const QUuid& clipGuid);
    bool setTimelineDomain(TimelineDomain domain);
    bool setTempoMicrosecondsPerQuarter(int64_t microsecondsPerQuarter);
    bool setMeter(int numerator, int denominator);
    bool addMarker(MarkerKind kind, int64_t position, const QString& name = {});
    bool moveMarker(const QUuid& markerGuid, int64_t position, bool disableSnap = false);
    bool renameMarker(const QUuid& markerGuid, const QString& name);
    bool deleteMarker(const QUuid& markerGuid);
    bool drillIntoClip(int laneIndex, const QUuid& clipGuid);
    bool drillBack();
    PlacementContext placementContextForClip(int laneIndex,
                                             const QUuid& clipGuid) const;
    PlacementContext placementContextForLane(int laneIndex) const;
    bool requestBlueprintForClip(int laneIndex, const QUuid& clipGuid);
    bool requestBlueprintForLane(int laneIndex);
    bool inspectClip(int laneIndex, const QUuid& clipGuid);
    bool revealClip(const QUuid& clipGuid);
    bool revealMarker(const QUuid& markerGuid);
    bool inspectItem(ProjectItem* item, const PlacementContext& context);
    int64_t snapCoordinate(int laneIndex, int64_t coordinate,
                           bool disableSnap, bool* ok = nullptr,
                           const QSet<QUuid>& excludedClips = {}) const;
    bool deleteSelectedClips();
    bool importAudioFiles(const QStringList& files, int laneIndex, int64_t start,
                          bool disableSnap = false);
    void zoomAroundPixel(int canvasX, double factor);
    void refreshStatusBar();
    void rejectInteraction(const QString& error);
    bool commitTimelineActions(std::vector<ActionResult> results, const QString& undoText);
    bool commitClipMoves(const std::vector<TimelineEditing::ClipMove>& moves);
    void setStatusMessage(const QString& text, bool error = false);
    void publishClipSelection();
    static int minimumZoomPercent();

public slots:
    void bindItem(ProjectItem* item);
    void bindGuid(const QUuid& guid);
    void clearEditor();
    void setZoomPercent(double percent);
    void setHorizontalScroll(double value);
    void setEditCursor(int64_t coordinate, bool disableSnap = false);
    void setPlayhead(std::optional<int64_t> coordinate);
    void restoreState(const QVariantMap& state);
    void refreshHorizontalRange();
    void refreshTimelinePaint();

signals:
    void markerDragFinished();
    void cursorDragFinished();
    void panelFocused(int panelId);
    void pinToggled(bool pinned);
    void laneSelectionRequested(ProjectItem* track,
                                const PlacementContext& context);
    void clipSelectionRequested(ProjectItem* target, const PlacementContext& context);
    void blueprintRequested(ProjectItem* subject, const PlacementContext& context);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct DrillFrame {
        QUuid timelineGuid;
        QUuid ownerTrackGuid;
        QUuid clipGuid;
        QUuid targetGuid;
        QVariantMap viewport;
    };

    struct LaneView {
        QUuid guid;
        uint32_t objectID = 0;
        QString name;
        ProjectItem* item = nullptr;
    };

    struct TimeTickCache {
        NanosecondPosition start;
        double span;
        int width;
        QFontMetrics secondsFontMetrics;
        QFontMetrics clockFontMetrics;
        SegmentTimeSpec time;
        int labelSpacing;
        int tickSpacing;
        TimeAxisTicks seconds;
        TimeAxisTicks clock;
        int64_t secondsGrid = 1;
        int64_t clockGrid = 1;
    };

    struct CursorDrag {
        int pointerX = 0;
        int grabOffset = 0;
        bool disableSnap = false;
        bool moved = false;
        int64_t originalCoordinate = 0;
    };

    struct HeaderResizeDrag {
        int pointerX;
        int width;
    };

    struct MarkerDrag {
        QUuid guid;
        int pressX = 0;
        int pointerX = 0;
        int grabOffset = 0;
        bool disableSnap = false;
        bool moved = false;
        int64_t originalPosition = 0;
        int64_t previewPosition = 0;
    };

    struct LaneResizeDrag {
        int lane;
        int pointerY;
        int height;
        double storedHeight;
    };

    void restoreViewport(const QVariantMap& state);
    void refreshTickCache() const;
    int markerIndex(const QUuid& guid) const;
    QString markerCaption(const QUuid& guid) const;
    int64_t clampMarkerPosition(const QUuid& guid, int64_t position) const;
    void applyMarkerDrag();
    void scrollMarkerDrag();
    void showMarkerValue();
    void applyCursorDrag();
    void scrollCursorDrag();
    void rebuild();
    void rebuildTimelinePage();
    void refreshTimeControls();
    void refreshLaneChrome();
    void syncLaneGeometry();
    void updateLaneResizeBars();
    void finishLaneResize(bool cancel);
    void refreshVerticalRange();
    double minimumVerticalZoom() const;
    int64_t contentEndCoordinate() const;
    const TimelineSpan& contentBounds() const;
    void invalidateModelCache();
    void showState(ViewKind kind, const QString& text);
    ProjectItem* laneItem(int index) const;
    int64_t authoredDuration(ProjectItem* target, int laneIndex) const;
    bool commitTimelineAction(ActionResult result);
    void applyThemeMetrics();
    void refreshThemeIcons();
    void applyLaneHeaderWidth();
    void setLaneHeaderResizeHighlighted(bool highlighted);
    void elideLaneHeaderText();
    void refreshLaneSelectionChrome();
    void bindDrillItem(ProjectItem* item);
    PlacementContext drillPlacementContext() const;
    bool drillToDepth(int depth);
    void refreshBreadcrumbs();
    void resetView();
    void applyPresentation();
    double maximumZoomPercent() const;
    double horizontalScrollMaximum() const;
    double viewSpan() const;
    NanosecondPosition positionAtPixel(double pixel) const;
    double horizontalRatio() const;
    void setHorizontalRatio(double ratio);
    void setViewStart(NanosecondPosition position);

    int panelId_ = 0;
    bool pinned_ = false;
    QUuid boundGuid_;
    ProjectItem* boundItem_ = nullptr;
    ViewKind viewKind_ = ViewKind::Empty;
    std::vector<LaneView> lanes_;
    mutable std::optional<TimelineSpan> contentBounds_;
    mutable QHash<QUuid, ProjectItem*> clipTargets_;
    mutable std::optional<TimeTickCache> timeTickCache_;
    AxisMode axisMode_ = AxisMode::MeasureBeats;
    std::optional<SegmentTimeSpec> presentedTime_;
    std::vector<DrillFrame> drillPath_;
    QString lastInteractionError_;
    QString statusMessage_;
    bool suppressSelectionPush_ = false;
    double zoomPercent_ = 100.0;
    double segmentVerticalZoom_ = 1.0;
    double amplitudeExponent_ = 1.0;
    NanosecondPosition viewStart_;
    std::optional<double> pendingHorizontalRatio_;
    qreal laneHeaderWidth_ = 0;
    QHash<QUuid, double> laneHeights_;
    std::optional<LaneResizeDrag> laneResizeDrag_;
    std::optional<HeaderResizeDrag> headerResizeDrag_;
    int64_t editCursor_ = 0;
    bool editCursorVisible_ = true;
    bool cursorHovered_ = false;
    std::optional<CursorDrag> cursorDrag_;
    QPointer<QDialog> cursorValueEditor_;
    QTimer* cursorEdgeScroll_ = nullptr;
    std::optional<int64_t> playhead_;
    ThemeManager* themeManager_ = nullptr;

    QWidget* breadcrumbBar_ = nullptr;
    QHBoxLayout* breadcrumbLayout_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QFrame* timelinePage_ = nullptr;
    QFrame* laneHeaderResizeBar_ = nullptr;
    QFrame* laneHeaderRulerResizeBar_ = nullptr;
    bool forwardingRulerPress_ = false;
    bool laneHeaderResizeHighlighted_ = false;
    QWidget* toolbar_ = nullptr;
    QWidget* timeControls_ = nullptr;
    QDoubleSpinBox* tempoSpin_ = nullptr;
    QSpinBox* meterNumeratorSpin_ = nullptr;
    QComboBox* meterDenominatorCombo_ = nullptr;
    QToolButton* addTrackButton_ = nullptr;
    QToolButton* zoomOutButton_ = nullptr;
    QToolButton* zoomInButton_ = nullptr;
    QFrame* verticalTools_ = nullptr;
    QToolButton* verticalZoomInButton_ = nullptr;
    QToolButton* verticalZoomOutButton_ = nullptr;
    QScrollBar* verticalScrollBar_ = nullptr;
    QUuid selectedMarkerGuid_;
    QUuid hoveredMarkerGuid_;
    std::optional<MarkerDrag> markerDrag_;
    QTimer* markerEdgeScroll_ = nullptr;
    QLabel* markerValue_ = nullptr;
    QPointer<QDialog> markerValueEditor_;
    QUuid selectedLaneGuid_;
    QVariantMap pendingRestoreState_;
    TimelineRuler* ruler_ = nullptr;
    QScrollArea* scrollArea_ = nullptr;
    QWidget* laneHost_ = nullptr;
    std::vector<QFrame*> laneResizeBars_;
    TimelineHeaderColumn* headerColumn_ = nullptr;
    TimelineCanvas* canvas_ = nullptr;
    QScrollBar* horizontalScrollBar_ = nullptr;
    QHBoxLayout* scrollRow_ = nullptr;
    QWidget* horizontalSpacer_ = nullptr;
    QFrame* scrollCorner_ = nullptr;
    QVBoxLayout* laneLayout_ = nullptr;
};

}
