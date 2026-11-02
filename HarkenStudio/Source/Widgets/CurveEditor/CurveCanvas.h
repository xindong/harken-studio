#pragma once

#include "Core/CurveSemantics.h"
#include "CurveRenderer.h"
#include "CurvePlotGeometry.h"
#include "Core/VariableSemantics.h"

#include <QPointF>
#include <QPointer>
#include <QRect>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QWidget>
#include <QVector>
#include <optional>
#include <vector>

class QJsonObject;
class QDialog;

namespace VNX {

// Walks from anchor along direction for min(length, distance to the box
// boundary), preserving direction instead of clamping components. Returns
// anchor when direction degenerates or anchor sits outside the box.
QPointF curveHandleRayIntoRect(const QPointF& anchor, const QPointF& direction,
                               double length, const QRectF& box);

// Smooth-anchor predicate: both handle vectors longer than 1px and opposed
// within 3 degrees. Derived from geometry only, never persisted.
bool curveHandlesMirrorAligned(const QPointF& anchor, const QPointF& handleA,
                               const QPointF& handleB);

// Least-norm H1/H2 deltas moving the cubic Bezier point at t by delta:
// d_i = delta * w_i / (w1^2 + w2^2) with w1 = 3(1-t)^2 t, w2 = 3(1-t) t^2.
// Caller pre-clamps t into [0.05, 0.95] so the denominator never degenerates.
void curveDistributeBodyDelta(double t, const QPointF& delta, QPointF* d1, QPointF* d2);

struct CurveCanvasProfile : CurvePlotProfile
{
    void setVariableAxis(VariableKind kind, const std::vector<VariableLabel>& labels);

    float defaultEndValue = -96.0f;
    CurveColorRole colorRole = CurveColorRole::ParameterBinding;
    QColor color;
    QString clipboardFamily{QStringLiteral("volume")};
    bool endpointsFollowDomain = true;
    bool preserveOutOfRangeValues = false;
};

struct CurveOverlay
{
    QVector<CurvePointDef> points;
    QVector<CurveHandlePairDef> handles;
    CurveColorRole colorRole = CurveColorRole::ParameterBinding;
    QColor color;
    bool selectable = false;
};

// Generic bezier mapping canvas (blueprint D1): edits the unified curve form
// over an arbitrary x domain [xMin, xMax] set by the host — attenuation
// distance, a variable range, or a normalized fade span.
class CurveCanvas : public QWidget
{
    Q_OBJECT

public:
    explicit CurveCanvas(QWidget* parent = nullptr);

    void setProfile(const CurveCanvasProfile& profile);
    const CurveCanvasProfile& profile() const { return profile_; }

    void setXDomain(float lo, float hi);
    float xMin() const { return xMin_; }
    float xMax() const { return xMax_; }

    void setOverlays(const QVector<CurveOverlay>& overlays);
    const QVector<CurveOverlay>& overlays() const { return overlays_; }
    void setCurveVisible(bool visible);
    void setPlayhead(std::optional<float> x);
    std::optional<float> playhead() const { return playhead_; }

    void setCurve(const QVector<CurvePointDef>& points,
                  const QVector<CurveHandlePairDef>& handles = {});
    QVector<CurvePointDef> curve() const { return points_; }
    QVector<CurveHandlePairDef> curveHandles() const { return handles_; }

    int selectedPointIndex() const { return selectedPoint_; }
    QVector<int> selectedPointIndices() const;
    void setSelectedPointIndex(int index);
    void setSelectedPointIndices(const QVector<int>& indices, int primaryIndex);
    bool setPointCoordinates(int index, float x, float value);
    bool addPointAt(const QPoint& pos);
    bool deleteSelectedPoint();
    bool resetCurveToDefault();
    bool copySelectedPointsToClipboard() const;
    bool cutSelectedPointsToClipboard();
    bool pasteFromClipboard();
    bool pastePayload(const QJsonObject& payload);
    bool canPasteFromClipboard() const;
    CurveRenderData renderData() const;
    void dismissValueEditor();
    virtual void cancelDrag();

signals:
    void curvePreviewed(const QVector<CurvePointDef>& points,
                        const QVector<CurveHandlePairDef>& handles);
    void curvePreviewFinished();
    void curveEdited(const QVector<CurvePointDef>& points,
                     const QVector<CurveHandlePairDef>& handles,
                     const QString& editLabel);
    void xMaxEdited(float xMax);
    void yRangeEdited(float low, float high);
    void selectionChanged();
    void overlaySelected(int index);

protected:
    virtual QRectF graphRect() const;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    CurvePlotGeometry plotGeometry(bool useVirtualGraph = true) const;
    CurveAxisScale axisScale() const { return profile_.axisScale; }
    void beginAxisBoundEdit(CurveAxisBound bound, const QRectF& labelRect);
    QDialog* createValueEditor();
    void showValueEditor(QDialog* dialog, const QPointF& anchor, QWidget* input);
    float yMin() const { return profile_.yMin; }
    float yMax() const { return profile_.yMax; }
    float valueMin() const { return profile_.valueMinimum.value_or(yMin()); }
    float valueMax() const { return profile_.valueMaximum.value_or(yMax()); }

    QPointF pointToScreen(const CurvePointDef& point) const;
    CurvePointDef screenToPoint(const QPoint& pos) const;
    CurvePointDef screenToPoint(const QPointF& pos) const;
    void translateAnchorHandles(QVector<CurveHandlePairDef>& handles, int index,
                                const CurvePointDef& before, const CurvePointDef& after) const;
    float curveValueAtX(float x) const;
    bool isNearCurveSegment(const QPoint& pos) const;
    int segmentIndexNear(const QPoint& pos) const;
    QVector<QPointF> segmentScreenPolyline(int leftIndex) const;
    double clampXDeltaForMovingPoints(double xDelta,
                                      const QVector<CurvePointDef>& anchorPoints,
                                      QVector<int> indices) const;
    int findPointAt(const QPoint& pos) const;
    QPointF handleScreenPos(int segment, bool isH1) const;
    bool findHandleAt(const QPoint& pos, int* segment, bool* isH1) const;
    bool selectedHandleValid() const;
    QRectF handleConstraintBoxScreen(int segment) const;
    bool applySmoothPartner(int segment, bool isH1, double partnerLengthScreen);
    void smoothAnchorAt(int index);
    void beginHandleDrag(int segment, bool isH1, const QPointF& mousePos, bool independent);
    void moveDraggedHandle(QMouseEvent* event);
    void moveBodyDrag(QMouseEvent* event);
    int segmentIndexNearCoarse(const QPoint& pos) const;
    void updateGhost(const QPointF& applied, const QPointF& intended);
    void applyCurvePreset(int segment, float u1, float v1, float u2, float v2);
    int insertPointBySplit(float x);
    void removePointMerge(int index);
    void beginPointCoordinateEdit(int index);
    void setSingleSelection(int index);
    void updateLassoSelection(const QPoint& pos);
    bool writePointsToClipboard(const QVector<int>& indices) const;
    void showPointLimitToolTip(const QPoint& globalPos);
    void normalizeCurve();
    QVector<CurvePointDef> defaultCurve() const;

    CurveCanvasProfile profile_;
    bool curveVisible_ = true;
    float xMin_ = 0.0f;
    float xMax_ = 100.0f;
    QVector<CurveOverlay> overlays_;
    std::optional<float> playhead_;
    QPointer<QDialog> valueEditor_;
    QVector<CurvePointDef> points_;
    QVector<CurveHandlePairDef> handles_;
    int selectedPoint_ = -1;
    int draggedPoint_ = -1;
    int draggedHandleSegment_ = -1;
    bool draggedHandleIsH1_ = false;
    int pulledHandleAnchor_ = -1;
    int selectedHandleSegment_ = -1;
    bool selectedHandleIsH1_ = false;
    QSet<int> selectedPoints_;
    QVector<CurvePointDef> dragStartPoints_;
    QVector<CurveHandlePairDef> dragStartHandles_;
    QVector<CurvePointDef> dragAnchorPoints_;
    QVector<CurveHandlePairDef> dragAnchorHandles_;
    QPointF dragStartPos_;
    QPointF dragMouseOrigin_;
    CurvePointDef dragAnchorPoint_;
    bool dragMoved_ = false;
    QPointF dragHandleGrabOffset_;
    bool dragHandleSmooth_ = false;
    double dragPartnerLength_ = 0.0;
    int bodyDragSegment_ = -1;
    double bodyDragT_ = 0.0;
    QPointF bodyDragControls_[4];
    QPointF bodyDragMouseOrigin_;
    QVector<CurveHandlePairDef> bodyDragBaseHandles_;
    QPointF ghostFrom_;
    QPointF ghostTo_;
    bool ghostActive_ = false;
    bool ctrlTogglePending_ = false;
    int ctrlTogglePoint_ = -1;
    bool lassoActive_ = false;
    QPoint lassoStart_;
    QRect lassoRect_;
    QSet<int> lassoBaseSelection_;
    Qt::KeyboardModifiers lassoModifiers_;
    QPointF hoverPos_;
    bool hoverActive_ = false;
};

}
