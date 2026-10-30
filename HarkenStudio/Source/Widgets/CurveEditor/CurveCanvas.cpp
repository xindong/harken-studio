#include "CurveCanvas.h"
#include "CurveRenderer.h"
#include "Core/BindingSemantics.h"
#include "Core/FontManager.h"
#include "Core/NumericFormat.h"
#include "Core/QtControlSkin.h"
#include "Core/ShortcutManager.h"
#include "Core/ThemeManager.h"
#include "Core/VariableSemantics.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDialog>
#include "Widgets/Common/DecimalSpinBox.h"
#include <QFormLayout>
#include <QHideEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QSet>
#include <QResizeEvent>
#include <QShortcut>
#include <QToolTip>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace VNX {
namespace {

constexpr const char* kCurveClipboardFormat = "Harken.Curve.v3";
constexpr double kMinCurveXInterval = 0.001;

double tickStep(double minimum)
{
    const double magnitude = std::pow(10.0, std::floor(std::log10(minimum)));
    for (double multiple : {1.0, 2.0, 5.0, 10.0}) {
        if (multiple * magnitude >= minimum)
            return multiple * magnitude;
    }
    return 10.0 * magnitude;
}

struct ClipboardPoint
{
    CurvePointDef point;
    QPointF leftHandle;
    QPointF rightHandle;
    bool hasLeftHandle = false;
    bool hasRightHandle = false;
};

QJsonObject pointToJson(const CurvePointDef& point)
{
    return QJsonObject{
        {QStringLiteral("x"), point.x},
        {QStringLiteral("value"), point.value}
    };
}

bool readHandle(const QJsonValue& value, QPointF* handle)
{
    const QJsonArray values = value.toArray();
    if (values.size() != 2 || !values.at(0).isDouble() || !values.at(1).isDouble())
        return false;
    const double x = values.at(0).toDouble();
    const double y = values.at(1).toDouble();
    if (!std::isfinite(x) || !std::isfinite(y)
        || std::abs(x) > std::numeric_limits<float>::max()
        || std::abs(y) > std::numeric_limits<float>::max())
        return false;
    *handle = QPointF(x, y);
    return true;
}

bool readClipboardPoint(const QJsonValue& value, ClipboardPoint* point)
{
    if (!value.isObject())
        return false;
    const QJsonObject object = value.toObject();
    const QJsonValue xValue = object.value(QStringLiteral("x"));
    const QJsonValue yValue = object.value(QStringLiteral("value"));
    if (!xValue.isDouble() || !yValue.isDouble())
        return false;
    const double x = xValue.toDouble();
    const double y = yValue.toDouble();
    if (!std::isfinite(x) || !std::isfinite(y)
        || std::abs(x) > std::numeric_limits<float>::max()
        || std::abs(y) > std::numeric_limits<float>::max())
        return false;
    point->point = {static_cast<float>(x), static_cast<float>(y)};
    if (object.contains(QStringLiteral("leftHandle"))) {
        if (!readHandle(object.value(QStringLiteral("leftHandle")), &point->leftHandle))
            return false;
        point->hasLeftHandle = true;
    }
    if (object.contains(QStringLiteral("rightHandle"))) {
        if (!readHandle(object.value(QStringLiteral("rightHandle")), &point->rightHandle))
            return false;
        point->hasRightHandle = true;
    }
    return true;
}

bool sameHandlePair(const CurveHandlePairDef& lhs, const CurveHandlePairDef& rhs)
{
    return lhs.h1x == rhs.h1x && lhs.h1y == rhs.h1y
        && lhs.h2x == rhs.h2x && lhs.h2y == rhs.h2y;
}

QJsonObject readCurveClipboardObject()
{
    const QString text = QApplication::clipboard() ? QApplication::clipboard()->text() : QString();
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    if (!doc.isObject())
        return {};
    const QJsonObject obj = doc.object();
    if (obj.value(QStringLiteral("format")).toString() != QString::fromLatin1(kCurveClipboardFormat))
        return {};
    return obj;
}

int pointIndexAtX(const QVector<CurvePointDef>& points, float x, double tolerance)
{
    int closest = -1;
    double closestDistance = tolerance;
    for (int i = 0; i < points.size(); ++i) {
        const double distance = std::abs(static_cast<double>(points.at(i).x) - x);
        if (distance == 0.0)
            return i;
        if (distance < closestDistance) {
            closest = i;
            closestDistance = distance;
        }
    }
    return closest;
}

bool readPointPayload(const QJsonObject& obj,
                      const QString& family,
                      float xMin,
                      float xMax,
                      const QVector<CurvePointDef>& existingPoints,
                      QVector<ClipboardPoint>* incoming)
{
    if (obj.value(QStringLiteral("format")).toString()
            != QString::fromLatin1(kCurveClipboardFormat)
        || obj.value(QStringLiteral("family")).toString() != family)
        return false;

    const QJsonArray pointArray = obj.value(QStringLiteral("points")).toArray();
    if (pointArray.isEmpty()
        || pointArray.size() > std::max<qsizetype>(existingPoints.size(), kCurveMaxPoints))
        return false;

    QVector<ClipboardPoint> parsed;
    parsed.reserve(pointArray.size());
    int insertCount = 0;
    for (const QJsonValue& value : pointArray) {
        ClipboardPoint point;
        if (!readClipboardPoint(value, &point)
            || point.point.x < xMin || point.point.x > xMax)
            return false;
        const int index = pointIndexAtX(existingPoints, point.point.x, kMinCurveXInterval);
        if (index >= 0) {
            const double dx = static_cast<double>(existingPoints.at(index).x) - point.point.x;
            point.point.x = existingPoints.at(index).x;
            point.leftHandle.rx() += dx;
            point.rightHandle.rx() += dx;
        } else {
            ++insertCount;
        }
        parsed.push_back(point);
    }

    std::sort(parsed.begin(), parsed.end(), [](const ClipboardPoint& lhs,
                                               const ClipboardPoint& rhs) {
        return lhs.point.x < rhs.point.x;
    });
    for (int i = 1; i < parsed.size(); ++i) {
        if (parsed.at(i).point.x == parsed.at(i - 1).point.x)
            return false;
    }

    if (insertCount > 0
        && (existingPoints.size() < 2
            || existingPoints.size() + insertCount > kCurveMaxPoints))
        return false;

    if (incoming)
        *incoming = std::move(parsed);
    return true;
}

}

namespace {

constexpr double kPi = 3.14159265358979323846;
double bezierCoordinate(double p0, double p1, double p2, double p3, double t)
{
    const double u = 1.0 - t;
    return u * u * u * p0 + 3.0 * u * u * t * p1 +
           3.0 * u * t * t * p2 + t * t * t * p3;
}

}

QPointF curveHandleRayIntoRect(const QPointF& anchor, const QPointF& direction,
                               double length, const QRectF& box)
{
    const double norm = std::hypot(direction.x(), direction.y());
    if (norm < 1e-9 || !box.contains(anchor))
        return anchor;
    const QPointF dir(direction.x() / norm, direction.y() / norm);
    double limit = std::max(length, 0.0);
    if (dir.x() > 0.0)
        limit = std::min(limit, (box.right() - anchor.x()) / dir.x());
    else if (dir.x() < 0.0)
        limit = std::min(limit, (box.left() - anchor.x()) / dir.x());
    if (dir.y() > 0.0)
        limit = std::min(limit, (box.bottom() - anchor.y()) / dir.y());
    else if (dir.y() < 0.0)
        limit = std::min(limit, (box.top() - anchor.y()) / dir.y());
    return anchor + dir * std::max(limit, 0.0);
}

bool curveHandlesMirrorAligned(const QPointF& anchor, const QPointF& handleA,
                               const QPointF& handleB)
{
    const QPointF va = handleA - anchor;
    const QPointF vb = handleB - anchor;
    const double lenA = std::hypot(va.x(), va.y());
    const double lenB = std::hypot(vb.x(), vb.y());
    if (lenA <= 1.0 || lenB <= 1.0)
        return false;
    const double dot = (va.x() * vb.x() + va.y() * vb.y()) / (lenA * lenB);
    return dot <= -std::cos(3.0 * kPi / 180.0);
}

void curveDistributeBodyDelta(double t, const QPointF& delta, QPointF* d1, QPointF* d2)
{
    const double u = 1.0 - t;
    const double w1 = 3.0 * u * u * t;
    const double w2 = 3.0 * u * t * t;
    const double denom = w1 * w1 + w2 * w2;
    *d1 = delta * (w1 / denom);
    *d2 = delta * (w2 / denom);
}

CurveCanvas::CurveCanvas(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("CurveCanvas");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setFont(FontManager::instance().getFont(FontRole::Caption));
    connect(&FontManager::instance(), &FontManager::fontChanged, this, [this] {
        setFont(FontManager::instance().getFont(FontRole::Caption));
        update();
    });
    ShortcutManager& shortcuts = ShortcutManager::instance();
    shortcuts.bindWidgetAction(Shortcuts::SelectAll, this, [this]() {
        selectedPoints_.clear();
        selectedHandleSegment_ = -1;
        for (int i = 0; i < points_.size(); ++i)
            selectedPoints_.insert(i);
        selectedPoint_ = points_.isEmpty() ? -1 : 0;
        emit selectionChanged();
        update();
    });
    shortcuts.bindWidgetAction(Shortcuts::Cut, this,
                               [this]() { cutSelectedPointsToClipboard(); });
    shortcuts.bindWidgetAction(Shortcuts::Copy, this,
                               [this]() { copySelectedPointsToClipboard(); });
    shortcuts.bindWidgetAction(Shortcuts::Paste, this,
                               [this]() { pasteFromClipboard(); });
    shortcuts.bindWidgetAction(Shortcuts::Delete, this,
                               [this]() { deleteSelectedPoint(); });
    connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged,
            this, [this]() { update(); });
    setCurve({});
}

void CurveCanvas::setProfile(const CurveCanvasProfile& profile)
{
    cancelDrag();
    dismissValueEditor();
    profile_ = profile;
    setSingleSelection(-1);
    normalizeCurve();
    update();
}

void CurveCanvas::setXDomain(float lo, float hi)
{
    cancelDrag();
    dismissValueEditor();
    xMin_ = lo;
    xMax_ = hi;
    if (!(xMax_ > xMin_))
        xMax_ = xMin_ + 1.0f;
    normalizeCurve();
    update();
}

void CurveCanvas::setOverlays(const QVector<CurveOverlay>& overlays)
{
    overlays_ = overlays;
    update();
}

void CurveCanvas::setCurveVisible(bool visible)
{
    cancelDrag();
    dismissValueEditor();
    curveVisible_ = visible;
    update();
}

void CurveCanvas::setPlayhead(std::optional<float> x)
{
    playhead_ = x;
    update();
}

void CurveCanvas::setCurve(const QVector<CurvePointDef>& points,
                          const QVector<CurveHandlePairDef>& handles)
{
    cancelDrag();
    dismissValueEditor();
    points_ = points;
    handles_ = handles;
    if (points_.isEmpty()) {
        points_ = defaultCurve();
        handles_.clear();
    }
    normalizeCurve();
    const int lastIndex = static_cast<int>(points_.size()) - 1;
    selectedPoint_ = points_.isEmpty() ? -1 : std::clamp(selectedPoint_, -1, lastIndex);
    QSet<int> validSelection;
    for (int index : selectedPoints_)
        if (index >= 0 && index <= lastIndex)
            validSelection.insert(index);
    selectedPoints_ = validSelection;
    if (selectedPoint_ >= 0)
        selectedPoints_.insert(selectedPoint_);
    update();
}

QVector<int> CurveCanvas::selectedPointIndices() const
{
    QVector<int> indices;
    indices.reserve(selectedPoints_.size());
    for (int index : selectedPoints_)
        indices.push_back(index);
    std::sort(indices.begin(), indices.end());
    return indices;
}

void CurveCanvas::setSelectedPointIndex(int index)
{
    setSingleSelection(index);
    emit selectionChanged();
    update();
}

void CurveCanvas::setSelectedPointIndices(const QVector<int>& indices, int primaryIndex)
{
    selectedPoints_.clear();
    for (int index : indices) {
        if (index >= 0 && index < points_.size())
            selectedPoints_.insert(index);
    }

    selectedPoint_ = selectedPoints_.contains(primaryIndex)
        ? primaryIndex
        : (selectedPoints_.isEmpty() ? -1 : *selectedPoints_.constBegin());
    emit selectionChanged();
    update();
}

// Single authority for injected coordinates: endpoint x stays pinned,
// interior x clamps against the neighbor interval, values clamp into
// the profile range — typed input can never break curve invariants.
bool CurveCanvas::setPointCoordinates(int index, float x, float value)
{
    if (index < 0 || index >= points_.size())
        return false;

    const CurvePointDef before = points_.at(index);
    double xDelta = static_cast<double>(x) - static_cast<double>(before.x);
    if (profile_.endpointsFollowDomain && (index == 0 || index == points_.size() - 1))
        xDelta = 0.0;
    else
        xDelta = clampXDeltaForMovingPoints(xDelta, points_, {index});

    CurvePointDef& point = points_[index];
    point.x = std::clamp(static_cast<float>(static_cast<double>(before.x) + xDelta),
                         xMin_,
                         xMax_);
    if (!profile_.endpointsFollowDomain && x == before.x)
        point.x = before.x;
    point.value = std::clamp(value, valueMin(), valueMax());
    if (point.x == before.x && point.value == before.value)
        return false;

    translateAnchorHandles(handles_, index, before, point);
    normalizeCurve();
    emit curveEdited(points_, handles_, tr("Edit Point"));
    update();
    return true;
}

void CurveCanvas::translateAnchorHandles(QVector<CurveHandlePairDef>& handles, int index,
                                         const CurvePointDef& before, const CurvePointDef& after) const
{
    const double delta = curveAxisCoordinate(axisScale(), after.value)
        - curveAxisCoordinate(axisScale(), before.value);
    const double minimum = curveAxisCoordinate(axisScale(), valueMin());
    const double maximum = curveAxisCoordinate(axisScale(), valueMax());
    const auto translate = [&](float& x, float& value) {
        x += after.x - before.x;
        if (value == before.value)
            value = after.value;
        else if (delta != 0.0)
            value = curveAxisValueFromCoordinate(axisScale(), std::clamp(
                curveAxisCoordinate(axisScale(), value) + delta, minimum, maximum));
    };
    if (index > 0 && index - 1 < handles.size())
        translate(handles[index - 1].h2x, handles[index - 1].h2y);
    if (index < handles.size())
        translate(handles[index].h1x, handles[index].h1y);
}

bool CurveCanvas::addPointAt(const QPoint& pos)
{
    if (!isNearCurveSegment(pos))
        return false;

    if (points_.size() >= kCurveMaxPoints) {
        showPointLimitToolTip(mapToGlobal(pos));
        return false;
    }

    const CurvePointDef p = screenToPoint(pos);
    if (p.x <= xMin_ || p.x >= xMax_)
        return false;
    const int newIndex = insertPointBySplit(p.x);
    if (newIndex < 0)
        return false;
    normalizeCurve();
    setSingleSelection(newIndex);
    emit selectionChanged();
    emit curveEdited(points_, handles_, tr("Add Point"));
    update();
    return true;
}

bool CurveCanvas::deleteSelectedPoint()
{
    QVector<int> indices = selectedPointIndices();
    indices.erase(std::remove_if(indices.begin(), indices.end(), [this](int index) {
        return index <= 0 || index >= points_.size() - 1;
    }), indices.end());
    if (indices.isEmpty())
        return false;

    std::sort(indices.begin(), indices.end(), std::greater<int>());
    for (int index : indices)
        removePointMerge(index);
    normalizeCurve();
    setSingleSelection(-1);
    emit selectionChanged();
    emit curveEdited(points_, handles_, tr("Remove Point(s)"));
    update();
    return true;
}

bool CurveCanvas::resetCurveToDefault()
{
    points_ = defaultCurve();
    handles_.clear();
    normalizeCurve();
    setSingleSelection(-1);
    emit selectionChanged();
    emit curveEdited(points_, handles_, tr("Reset Curve"));
    update();
    return true;
}

void CurveCanvas::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    const ThemeSnapshotPtr snapshot = ThemeManager::instance().snapshot();
    CurveRenderer::render(painter,
                          snapshot->domains.curve,
                          profile_.colorRole,
                          renderData());
}

void CurveCanvas::contextMenuEvent(QContextMenuEvent* event)
{
    if (!curveVisible_) return;
    setFocus();

    const int pointIndex = findPointAt(event->pos());
    const int segmentIndex = segmentIndexNear(event->pos());
    if (pointIndex < 0 && segmentIndex < 0) {
        event->ignore();
        return;
    }

    QMenu menu(this);
    if (pointIndex >= 0) {
        QAction* enterCoordinates = menu.addAction(tr("Enter Coordinates..."));
        connect(enterCoordinates, &QAction::triggered, this,
                [this, pointIndex]() { beginPointCoordinateEdit(pointIndex); });
        QAction* smooth = menu.addAction(tr("Smooth"));
        smooth->setEnabled(pointIndex > 0 && pointIndex + 1 < points_.size());
        connect(smooth, &QAction::triggered, this,
                [this, pointIndex]() { smoothAnchorAt(pointIndex); update(); });
        menu.addSeparator();
    }

    const QVector<int> selection = selectedPointIndices();
    const bool hasEditableSelection = std::any_of(
        selection.cbegin(), selection.cend(), [this](int index) {
            return index > 0 && index < points_.size() - 1;
        });
    QAction* cutAction = menu.addAction(tr("Cut"));
    cutAction->setEnabled(hasEditableSelection);
    connect(cutAction, &QAction::triggered, this,
            [this]() { cutSelectedPointsToClipboard(); });
    QAction* copyAction = menu.addAction(tr("Copy"));
    copyAction->setEnabled(!selection.isEmpty());
    connect(copyAction, &QAction::triggered, this,
            [this]() { copySelectedPointsToClipboard(); });
    QAction* pasteAction = menu.addAction(tr("Paste"));
    pasteAction->setEnabled(canPasteFromClipboard());
    connect(pasteAction, &QAction::triggered, this,
            [this]() { pasteFromClipboard(); });
    QAction* deleteAction = menu.addAction(tr("Delete"));
    deleteAction->setEnabled(hasEditableSelection);
    connect(deleteAction, &QAction::triggered, this,
            [this]() { deleteSelectedPoint(); });

    if (segmentIndex >= 0 && segmentIndex < handles_.size()) {
        menu.addSeparator();
        QMenu* presetMenu = menu.addMenu(tr("Presets"));
        struct PresetTemplate {
            const char* name;
            float u1, v1, u2, v2;
        };
        static constexpr PresetTemplate kPresets[] = {
            {QT_TR_NOOP("Linear"),   0.0f,  0.0f, 1.0f,  1.0f},
            {QT_TR_NOOP("Ease-In"),  0.42f, 0.0f, 1.0f,  1.0f},
            {QT_TR_NOOP("Ease-Out"), 0.0f,  0.0f, 0.58f, 1.0f},
            {QT_TR_NOOP("S-Curve"),  0.42f, 0.0f, 0.58f, 1.0f},
        };
        for (const PresetTemplate& preset : kPresets) {
            QAction* action = presetMenu->addAction(tr(preset.name));
            connect(action, &QAction::triggered, this, [this, segmentIndex, preset]() {
                applyCurvePreset(segmentIndex, preset.u1, preset.v1, preset.u2, preset.v2);
            });
        }
    }

    menu.exec(event->globalPos());
}

void CurveCanvas::mousePressEvent(QMouseEvent* event)
{
    cancelDrag();
    setFocus();
    draggedHandleSegment_ = -1;
    selectedHandleSegment_ = -1;
    pulledHandleAnchor_ = -1;
    draggedPoint_ = -1;
    bodyDragSegment_ = -1;
    dragStartPoints_.clear();
    dragAnchorPoints_.clear();
    dragAnchorHandles_.clear();
    dragMoved_ = false;
    dragStartPos_ = QPointF(event->pos());
    dragMouseOrigin_ = dragStartPos_;
    dragAnchorPoint_ = {};
    lassoActive_ = false;
    lassoRect_ = {};
    ctrlTogglePending_ = false;
    ctrlTogglePoint_ = -1;
    if (event->button() != Qt::LeftButton)
        return;

    int hitSegment = -1;
    bool hitH1 = false;
    if ((!curveVisible_ || (findPointAt(event->pos()) < 0
        && !findHandleAt(event->pos(), &hitSegment, &hitH1)
        && !isNearCurveSegment(event->pos()))) && graphRect().contains(event->pos())) {
        const auto drawn = renderData();
        QPainterPathStroker stroke;
        stroke.setWidth(10.0);
        int drawnIndex = 0;
        for (int i = 0; i < overlays_.size(); ++i) {
            if (overlays_[i].points.size() < 2) continue;
            const auto& overlay = drawn.overlays[drawnIndex];
            if (overlays_[i].selectable
                && (stroke.createStroke(overlay.path).contains(event->pos())
                    || (overlay.discretePath && stroke.createStroke(*overlay.discretePath).contains(event->pos())))) {
                emit overlaySelected(i);
                return;
            }
            ++drawnIndex;
        }
    }
    if (!curveVisible_) return;

    dragStartPoints_ = points_;
    dragStartHandles_ = handles_;
    const int pointIndex = findPointAt(event->pos());
    const bool alt = (event->modifiers() & Qt::AltModifier) != 0;
    int handleSegment = -1;
    bool handleIsH1 = false;
    if (pointIndex >= 0 && alt && !handles_.isEmpty()) {
        setSingleSelection(pointIndex);
        pulledHandleAnchor_ = pointIndex;
    } else if (pointIndex < 0 && findHandleAt(event->pos(), &handleSegment, &handleIsH1)) {
        beginHandleDrag(handleSegment, handleIsH1, event->position(), alt);
    } else {
        draggedPoint_ = pointIndex;
    }
    if (pulledHandleAnchor_ >= 0 || draggedHandleSegment_ >= 0) {
        emit selectionChanged();
        update();
        return;
    }
    if (draggedPoint_ >= 0) {
        if ((event->modifiers() & Qt::ControlModifier) != 0) {
            ctrlTogglePending_ = true;
            ctrlTogglePoint_ = draggedPoint_;
            selectedPoint_ = draggedPoint_;
        } else {
            if (!selectedPoints_.contains(draggedPoint_))
                setSingleSelection(draggedPoint_);
            else
                selectedPoint_ = draggedPoint_;
        }
        dragStartPoints_ = points_;
        dragAnchorPoints_ = points_;
        dragAnchorHandles_ = handles_;
        dragAnchorPoint_ = points_.at(draggedPoint_);
    } else {
        const int bodySeg = (event->buttons() & Qt::LeftButton) != 0
            ? segmentIndexNear(event->pos())
            : -1;
        if (bodySeg >= 0 && bodySeg < handles_.size() && bodySeg + 1 < points_.size()) {
            bodyDragSegment_ = bodySeg;
            bodyDragControls_[0] = pointToScreen(points_.at(bodySeg));
            bodyDragControls_[1] = handleScreenPos(bodySeg, true);
            bodyDragControls_[2] = handleScreenPos(bodySeg, false);
            bodyDragControls_[3] = pointToScreen(points_.at(bodySeg + 1));
            double lo = 0.0;
            double hi = 1.0;
            for (int i = 0; i < 32; ++i) {
                const double mid = (lo + hi) * 0.5;
                const double bx = bezierCoordinate(bodyDragControls_[0].x(), bodyDragControls_[1].x(),
                                                   bodyDragControls_[2].x(), bodyDragControls_[3].x(), mid);
                if (bx < event->pos().x())
                    lo = mid;
                else
                    hi = mid;
            }
            bodyDragT_ = std::clamp((lo + hi) * 0.5, 0.05, 0.95);
            bodyDragMouseOrigin_ = QPointF(event->pos());
            bodyDragBaseHandles_ = handles_;
        } else if ((event->buttons() & Qt::LeftButton) != 0) {
            lassoActive_ = true;
            lassoStart_ = event->pos();
            lassoRect_ = QRect(lassoStart_, QSize());
            lassoBaseSelection_ = selectedPoints_;
            lassoModifiers_ = event->modifiers();
            if ((lassoModifiers_ & (Qt::ControlModifier | Qt::ShiftModifier)) == 0)
                setSingleSelection(-1);
        } else {
            setSingleSelection(-1);
        }
    }
    emit selectionChanged();
    update();
}

void CurveCanvas::mouseMoveEvent(QMouseEvent* event)
{
    if (!curveVisible_) return;
    const bool dragSession = lassoActive_ || draggedPoint_ >= 0
        || draggedHandleSegment_ >= 0 || pulledHandleAnchor_ >= 0 || bodyDragSegment_ >= 0;
    if (event->buttons() == Qt::NoButton && !dragSession) {
        int hoverSeg = -1;
        bool hoverIsH1 = false;
        if (findHandleAt(event->pos(), &hoverSeg, &hoverIsH1) || findPointAt(event->pos()) >= 0)
            setCursor(Qt::PointingHandCursor);
        else if (segmentIndexNearCoarse(event->pos()) >= 0)
            setCursor(Qt::SizeVerCursor);
        else
            unsetCursor();
        hoverActive_ = true;
        hoverPos_ = QPointF(event->pos());
        update();
        return;
    }

    if (lassoActive_) {
        updateLassoSelection(event->pos());
        emit selectionChanged();
        update();
        return;
    }

    if (pulledHandleAnchor_ >= 0) {
        const QPointF delta = event->position() - dragStartPos_;
        const int index = pulledHandleAnchor_;
        const bool interior = index > 0 && index + 1 < points_.size();
        if (std::hypot(delta.x(), delta.y()) < 2.0 || (interior && std::abs(delta.x()) < 2.0))
            return;
        const bool isH1 = index == 0 || (interior && delta.x() > 0.0);
        beginHandleDrag(isH1 ? index : index - 1, isH1, dragStartPos_, true);
        dragHandleGrabOffset_ = pointToScreen(points_.at(index)) - dragStartPos_;
        pulledHandleAnchor_ = -1;
    }

    if (draggedHandleSegment_ >= 0) {
        moveDraggedHandle(event);
        return;
    }

    if (bodyDragSegment_ >= 0) {
        moveBodyDrag(event);
        return;
    }

    if (draggedPoint_ < 0 || draggedPoint_ >= points_.size())
        return;

    const QPointF currentPos(event->pos());
    const CurvePointDef originPoint = screenToPoint(dragMouseOrigin_);
    const CurvePointDef effectivePoint = screenToPoint(currentPos);
    double xDelta = static_cast<double>(effectivePoint.x - originPoint.x);
    double valueDelta = (dragMouseOrigin_.y() - currentPos.y()) / graphRect().height();

    if ((event->modifiers() & Qt::ShiftModifier) != 0) {
        const double dx = currentPos.x() - dragStartPos_.x();
        const double dy = currentPos.y() - dragStartPos_.y();
        if (std::abs(dx) <= std::abs(dy))
            xDelta = 0.0;
        else
            valueDelta = 0.0;
    }

    const bool anchorsValid = dragAnchorPoints_.size() == points_.size()
        && dragAnchorHandles_.size() == handles_.size();
    const QVector<CurvePointDef>& anchorPoints = anchorsValid
        ? dragAnchorPoints_
        : points_;
    QVector<CurvePointDef> moved = anchorPoints;
    QVector<CurveHandlePairDef> movedHandles = anchorsValid
        ? dragAnchorHandles_
        : handles_;
    QVector<int> indices = selectedPointIndices();
    if (!indices.contains(draggedPoint_))
        indices.push_back(draggedPoint_);
    xDelta = clampXDeltaForMovingPoints(xDelta, anchorPoints, indices);
    const double minimum = curveAxisNormalizedY(axisScale(), yMin(), yMax(), valueMin());
    const double maximum = curveAxisNormalizedY(axisScale(), yMin(), yMax(), valueMax());
    for (int index : indices) {
        const double value = curveAxisNormalizedY(axisScale(), yMin(), yMax(), anchorPoints.at(index).value);
        valueDelta = std::clamp(valueDelta, minimum - value, maximum - value);
    }

    bool changed = false;
    for (int index : indices) {
        if (index < 0 || index >= moved.size())
            continue;
        CurvePointDef& point = moved[index];
        const CurvePointDef anchorPoint = anchorPoints.at(index);
        const CurvePointDef oldPoint = points_.at(index);
        point.x = static_cast<float>(static_cast<double>(point.x) + xDelta);
        if (valueDelta != 0.0)
            point.value = curveAxisValueFromNormalizedY(axisScale(), yMin(), yMax(), std::clamp(
                curveAxisNormalizedY(axisScale(), yMin(), yMax(), point.value) + valueDelta,
                minimum, maximum));
        if (profile_.endpointsFollowDomain && index == 0)
            point.x = xMin_;
        if (profile_.endpointsFollowDomain && index == moved.size() - 1)
            point.x = xMax_;
        translateAnchorHandles(movedHandles, index, anchorPoint, point);
        changed = changed
            || oldPoint.x != point.x
            || oldPoint.value != point.value;
    }

    points_ = moved;
    handles_ = movedHandles;
    normalizeCurve();
    selectedPoint_ = draggedPoint_;
    if (selectedPoint_ >= 0)
        selectedPoints_.insert(selectedPoint_);
    emit selectionChanged();
    dragMoved_ = dragMoved_ || changed;
    dragMouseOrigin_ = currentPos;
    dragAnchorPoints_ = points_;
    dragAnchorHandles_ = handles_;
    dragAnchorPoint_ = points_.at(draggedPoint_);
    if (changed) emit curvePreviewed(points_, handles_);
    update();
}

void CurveCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    hoverActive_ = true;
    hoverPos_ = QPointF(event->pos());
    const bool commitHandleDrag = dragMoved_ && draggedHandleSegment_ >= 0;
    const bool commitBodyDrag = dragMoved_ && bodyDragSegment_ >= 0;
    const bool commitDrag = dragMoved_ && !dragStartPoints_.isEmpty();
    const bool applyCtrlToggle = ctrlTogglePending_ && !dragMoved_;
    const int ctrlTogglePoint = ctrlTogglePoint_;
    draggedPoint_ = -1;
    draggedHandleSegment_ = -1;
    pulledHandleAnchor_ = -1;
    bodyDragSegment_ = -1;
    bodyDragBaseHandles_.clear();
    ghostActive_ = false;
    dragMoved_ = false;
    dragStartPoints_.clear();
    dragStartHandles_.clear();
    dragAnchorPoints_.clear();
    dragAnchorHandles_.clear();
    ctrlTogglePending_ = false;
    ctrlTogglePoint_ = -1;
    lassoActive_ = false;
    lassoRect_ = {};
    lassoBaseSelection_.clear();
    if (applyCtrlToggle && ctrlTogglePoint >= 0 && ctrlTogglePoint < points_.size()) {
        if (selectedPoints_.contains(ctrlTogglePoint))
            selectedPoints_.remove(ctrlTogglePoint);
        else
            selectedPoints_.insert(ctrlTogglePoint);
        selectedPoint_ = selectedPoints_.contains(ctrlTogglePoint)
            ? ctrlTogglePoint
            : (selectedPoints_.isEmpty() ? -1 : *selectedPoints_.constBegin());
        emit selectionChanged();
    }
    if (commitHandleDrag)
        emit curveEdited(points_, handles_, tr("Move Handle"));
    else if (commitBodyDrag)
        emit curveEdited(points_, handles_, tr("Shape Segment"));
    else if (commitDrag)
        emit curveEdited(points_, handles_, tr("Move Point(s)"));
    emit curvePreviewFinished();
    update();
}

void CurveCanvas::cancelDrag()
{
    if (dragStartPoints_.isEmpty()) return;
    if (dragMoved_) {
        points_ = dragStartPoints_;
        handles_ = dragStartHandles_;
    }
    draggedPoint_ = -1;
    draggedHandleSegment_ = -1;
    pulledHandleAnchor_ = -1;
    bodyDragSegment_ = -1;
    bodyDragBaseHandles_.clear();
    dragStartPoints_.clear();
    dragStartHandles_.clear();
    dragAnchorPoints_.clear();
    dragAnchorHandles_.clear();
    dragMoved_ = false;
    ghostActive_ = false;
    ctrlTogglePending_ = false;
    ctrlTogglePoint_ = -1;
    lassoActive_ = false;
    lassoRect_ = {};
    lassoBaseSelection_.clear();
    emit curvePreviewFinished();
    emit selectionChanged();
    update();
}

void CurveCanvas::keyPressEvent(QKeyEvent* event)
{
    if (!curveVisible_) return;
    if (event->key() == Qt::Key_Escape && !dragStartPoints_.isEmpty()) {
        cancelDrag();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void CurveCanvas::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (!curveVisible_) return;
    if (event->button() == Qt::LeftButton) {
        const CurveRenderData data = renderData();
        for (const auto& tick : data.textTicks) {
            if (tick.editable && tick.rect.contains(event->position())) {
                beginAxisBoundEdit(tick.bound, tick.rect);
                return;
            }
        }
        const int anchorIndex = findPointAt(event->pos());
        if (anchorIndex >= 0) {
            setSingleSelection(anchorIndex);
            emit selectionChanged();
            beginPointCoordinateEdit(anchorIndex);
            update();
            return;
        }
        if (addPointAt(event->pos()))
            return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void CurveCanvas::leaveEvent(QEvent* event)
{
    hoverActive_ = false;
    update();
    QWidget::leaveEvent(event);
}

void CurveCanvas::hideEvent(QHideEvent* event)
{
    cancelDrag();
    dismissValueEditor();
    QWidget::hideEvent(event);
}

void CurveCanvas::resizeEvent(QResizeEvent* event)
{
    dismissValueEditor();
    QWidget::resizeEvent(event);
}

bool CurveCanvas::eventFilter(QObject* watched, QEvent* event)
{
    if (valueEditor_ && event->type() == QEvent::MouseButtonPress) {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (widget && widget != valueEditor_ && !valueEditor_->isAncestorOf(widget))
            dismissValueEditor();
    }
    if (event->type() == QEvent::ApplicationDeactivate)
        dismissValueEditor();
    return QWidget::eventFilter(watched, event);
}

void CurveCanvas::dismissValueEditor()
{
    if (valueEditor_)
        valueEditor_->reject();
    valueEditor_.clear();
    qApp->removeEventFilter(this);
}

QDialog* CurveCanvas::createValueEditor()
{
    dismissValueEditor();
    draggedPoint_ = -1;
    draggedHandleSegment_ = -1;
    pulledHandleAnchor_ = -1;
    bodyDragSegment_ = -1;
    lassoActive_ = false;
    dragMoved_ = false;
    ghostActive_ = false;
    hoverActive_ = false;
    QToolTip::hideText();
    auto* dialog = new QDialog(this);
    dialog->setWindowFlags(Qt::Widget);
    valueEditor_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAttribute(Qt::WA_StyledBackground);
    dialog->setAttribute(Qt::WA_NoMousePropagation);
    qApp->installEventFilter(this);
    QtControlSkin::setSemanticProperty(dialog, "surfaceRole", QStringLiteral("secondary"));
    for (int key : {Qt::Key_Return, Qt::Key_Enter}) {
        auto* shortcut = new QShortcut(QKeySequence(key), dialog);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, dialog, [dialog] {
            for (auto* spin : dialog->findChildren<QDoubleSpinBox*>())
                spin->interpretText();
            dialog->accept();
        });
    }
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (valueEditor_ == dialog)
            valueEditor_.clear();
        qApp->removeEventFilter(this);
        setFocus(Qt::OtherFocusReason);
        update();
    });
    return dialog;
}

void CurveCanvas::showValueEditor(QDialog* dialog, const QPointF& anchor, QWidget* input)
{
    dialog->adjustSize();
    dialog->resize(dialog->size().boundedTo(size()));
    QPoint corner(qRound(anchor.x()) - dialog->width() / 2,
                   qRound(anchor.y()) - dialog->height() - 8);
    if (corner.y() < 0)
        corner.setY(qRound(anchor.y()) + 8);
    corner.setX(std::clamp(corner.x(), 0, std::max(0, width() - dialog->width())));
    corner.setY(std::clamp(corner.y(), 0, std::max(0, height() - dialog->height())));
    dialog->move(corner);
    dialog->show();
    dialog->raise();
    input->setFocus();
    update();
}

void CurveCanvas::beginAxisBoundEdit(CurveAxisBound bound, const QRectF& labelRect)
{
    const bool xBound = bound == CurveAxisBound::XMax;
    const bool upper = bound == CurveAxisBound::YMax;
    auto* dialog = createValueEditor();
    dialog->setObjectName(QStringLiteral("curveCanvasRangeEditor"));
    auto* form = new QFormLayout(dialog);
    form->setSizeConstraint(QLayout::SetNoConstraint);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setContentsMargins(8, 6, 8, 6);
    auto* spin = new DecimalSpinBox(dialog);
    spin->setObjectName(QStringLiteral("curveCanvasRangeInput"));
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setKeyboardTracking(false);
    spin->setDecimals(xBound ? profile_.xDecimals : profile_.valueDecimals);
    spin->setRange(xBound ? profile_.xMaxMinimum : (upper ? yMin() : profile_.yRangeMinimum),
                   xBound ? profile_.xMaxMaximum : (upper ? profile_.yRangeMaximum : yMax()));
    const float initial = xBound ? profile_.xMaxEditValue.value_or(xMax_)
        : (upper ? yMax() : yMin());
    spin->setValue(initial);
    const double initialValue = spin->value();
    const QString title = xBound ? tr("X Max") : (upper ? tr("Y Max") : tr("Y Min"));
    form->addRow(title, spin);
    QtControlSkin::setSemanticProperty(form->labelForField(spin), "textRole",
                                      QStringLiteral("formLabel"));
    connect(dialog, &QDialog::accepted, this, [this, spin, initial, initialValue, xBound, upper] {
        valueEditor_.clear();
        if (spin->value() == initialValue)
            return;
        const float next = static_cast<float>(spin->value());
        if (next == initial)
            return;
        if (xBound)
            emit xMaxEdited(next);
        else
            emit yRangeEdited(upper ? yMin() : next, upper ? next : yMax());
    });
    showValueEditor(dialog, labelRect.center(), spin);
    spin->selectAll();
}

QRectF CurveCanvas::graphRect() const
{
    const auto& metrics = ThemeManager::instance().snapshot()->domains.curve.metrics;
    const int minimumLeft = axisScale() != CurveAxisScale::LinearValue
        ? metrics.amplitudeGraphLeftInset
        : metrics.linearGraphLeftInset;
    const QFontMetrics fm(FontManager::instance().getFont(FontRole::Axis));
    const int labelWidth = std::max(fm.horizontalAdvance(formatDecimal(yMin(), profile_.valueDecimals)),
                                    fm.horizontalAdvance(formatDecimal(yMax(), profile_.valueDecimals)));
    const qreal left = std::min<qreal>(std::max(minimumLeft, labelWidth + 12), width() * 0.4);
    const qreal right = std::min<qreal>(metrics.graphRightInset, width() * 0.1);
    const qreal top = std::min<qreal>(std::max(metrics.graphTopInset, fm.height() / 2 + 2), height() * 0.2);
    const qreal bottom = std::min<qreal>(std::max(metrics.graphBottomInset, fm.height() + 8), height() * 0.3);
    return QRectF(left, top, std::max(1.0, width() - left - right),
                   std::max(1.0, height() - top - bottom));
}

QPointF CurveCanvas::pointToScreen(const CurvePointDef& point) const
{
    const QRectF graph = graphRect();
    const double nx = std::clamp((static_cast<double>(point.x) - xMin_)
                                     / (static_cast<double>(xMax_) - xMin_),
                                 0.0, 1.0);
    const double ny = yMin() == yMax() ? 0.5
        : std::clamp(curveAxisNormalizedY(axisScale(), yMin(), yMax(), point.value),
                                 0.0, 1.0);
    return QPointF(graph.left() + nx * graph.width(),
                   graph.bottom() - ny * graph.height());
}

CurvePointDef CurveCanvas::screenToPoint(const QPoint& pos) const
{
    return screenToPoint(QPointF(pos));
}

CurvePointDef CurveCanvas::screenToPoint(const QPointF& pos) const
{
    const QRectF graph = graphRect();
    const double nx = std::clamp((pos.x() - graph.left()) / graph.width(), 0.0, 1.0);
    const double ny = std::clamp((graph.bottom() - pos.y()) / graph.height(), 0.0, 1.0);
    CurvePointDef p;
    p.x = static_cast<float>(xMin_ + nx * (static_cast<double>(xMax_) - xMin_));
    p.value = curveAxisValueFromNormalizedY(axisScale(), yMin(), yMax(), ny);
    return p;
}

float CurveCanvas::curveValueAtX(float x) const
{
    if (points_.isEmpty())
        return yMin();
    if (!profile_.endpointsFollowDomain && (x < points_.front().x || x > points_.back().x))
        return 0.0f;
    return curveBezierValueAt(points_, handles_, x, axisScale());
}

QVector<QPointF> CurveCanvas::segmentScreenPolyline(int leftIndex) const
{
    QVector<QPointF> polyline;
    if (leftIndex < 0 || leftIndex + 1 >= points_.size())
        return polyline;

    const CurvePointDef& left = points_.at(leftIndex);
    const CurvePointDef& right = points_.at(leftIndex + 1);
    polyline.append(pointToScreen(left));

    const double screenWidth =
        std::abs(pointToScreen(right).x() - polyline.first().x()) * devicePixelRatioF();
    const int samples = std::clamp(static_cast<int>(std::ceil(screenWidth)), 16, 512);
    for (int step = 1; step < samples; ++step) {
        const float t = static_cast<float>(step) / samples;
        CurvePointDef sample;
        sample.x = left.x + (right.x - left.x) * t;
        sample.value = curveValueAtX(sample.x);
        polyline.append(pointToScreen(sample));
    }

    polyline.append(pointToScreen(right));
    return polyline;
}

bool CurveCanvas::isNearCurveSegment(const QPoint& pos) const
{
    return segmentIndexNear(pos) >= 0;
}

int CurveCanvas::segmentIndexNear(const QPoint& pos) const
{
    if (points_.size() < 2)
        return -1;

    constexpr double kSegmentHitRadius = 6.0;
    const QRectF hitGraph = graphRect().adjusted(-kSegmentHitRadius,
                                                 -kSegmentHitRadius,
                                                 kSegmentHitRadius,
                                                 kSegmentHitRadius);
    const QPointF test(pos);
    if (!hitGraph.contains(test))
        return -1;

    for (int i = 0; i + 1 < points_.size(); ++i) {
        const QVector<QPointF> polyline = segmentScreenPolyline(i);
        for (int j = 1; j < polyline.size(); ++j) {
            const QPointF begin = polyline.at(j - 1);
            const QPointF end = polyline.at(j);
            const QPointF segment = end - begin;
            const double lengthSq = QPointF::dotProduct(segment, segment);
            if (lengthSq <= 0.000001)
                continue;

            const QPointF fromBegin = test - begin;
            const double t = std::clamp(QPointF::dotProduct(fromBegin, segment) / lengthSq,
                                        0.0,
                                        1.0);
            const QPointF nearest = begin + segment * t;
            const QPointF delta = test - nearest;
            if (QPointF::dotProduct(delta, delta) <= kSegmentHitRadius * kSegmentHitRadius)
                return i;
        }
    }

    return -1;
}

// Hover-only coarse hit test: 16 uniform-t samples on the projected control
// polygon instead of the per-pixel polyline segmentIndexNear walks.
int CurveCanvas::segmentIndexNearCoarse(const QPoint& pos) const
{
    if (points_.size() < 2 || handles_.size() != points_.size() - 1)
        return -1;

    constexpr double kSegmentHitRadius = 6.0;
    const QRectF hitGraph = graphRect().adjusted(-kSegmentHitRadius,
                                                 -kSegmentHitRadius,
                                                 kSegmentHitRadius,
                                                 kSegmentHitRadius);
    const QPointF test(pos);
    if (!hitGraph.contains(test))
        return -1;

    for (int i = 0; i + 1 < points_.size(); ++i) {
        const QPointF p0 = pointToScreen(points_.at(i));
        const QPointF h1 = handleScreenPos(i, true);
        const QPointF h2 = handleScreenPos(i, false);
        const QPointF p3 = pointToScreen(points_.at(i + 1));
        QPointF prev = p0;
        for (int step = 1; step <= 16; ++step) {
            const double t = step / 16.0;
            const QPointF cur(bezierCoordinate(p0.x(), h1.x(), h2.x(), p3.x(), t),
                              bezierCoordinate(p0.y(), h1.y(), h2.y(), p3.y(), t));
            const QPointF segment = cur - prev;
            const double lengthSq = QPointF::dotProduct(segment, segment);
            if (lengthSq > 0.000001) {
                const QPointF fromBegin = test - prev;
                const double u = std::clamp(QPointF::dotProduct(fromBegin, segment) / lengthSq,
                                            0.0,
                                            1.0);
                const QPointF delta = test - (prev + segment * u);
                if (QPointF::dotProduct(delta, delta) <= kSegmentHitRadius * kSegmentHitRadius)
                    return i;
            }
            prev = cur;
        }
    }

    return -1;
}

double CurveCanvas::clampXDeltaForMovingPoints(
    double xDelta,
    const QVector<CurvePointDef>& anchorPoints,
    QVector<int> indices) const
{
    if (indices.isEmpty() || anchorPoints.isEmpty())
        return xDelta;

    std::sort(indices.begin(), indices.end());
    QSet<int> movingIndices;
    for (int index : indices)
        movingIndices.insert(index);

    double minXDelta = -std::numeric_limits<double>::infinity();
    double maxXDelta = std::numeric_limits<double>::infinity();
    bool hasValidIndex = false;
    for (int index : indices) {
        if (index < 0 || index >= anchorPoints.size())
            continue;

        hasValidIndex = true;
        const double anchorX = anchorPoints.at(index).x;
        const double minimum = profile_.endpointsFollowDomain ? xMin_ : std::min<double>(xMin_, anchorX);
        const double maximum = profile_.endpointsFollowDomain ? xMax_ : std::max<double>(xMax_, anchorX);
        minXDelta = std::max(minXDelta, minimum - anchorX);
        maxXDelta = std::min(maxXDelta, maximum - anchorX);

        const int leftIndex = index - 1;
        if (leftIndex >= 0 && !movingIndices.contains(leftIndex)) {
            minXDelta = std::max(minXDelta,
                static_cast<double>(anchorPoints.at(leftIndex).x) + kMinCurveXInterval - anchorX);
        }

        const int rightIndex = index + 1;
        if (rightIndex < anchorPoints.size() && !movingIndices.contains(rightIndex)) {
            maxXDelta = std::min(maxXDelta,
                static_cast<double>(anchorPoints.at(rightIndex).x) - kMinCurveXInterval - anchorX);
        }
    }

    if (!hasValidIndex)
        return xDelta;
    return std::clamp(xDelta, minXDelta, maxXDelta);
}

int CurveCanvas::findPointAt(const QPoint& pos) const
{
    for (int i = 0; i < points_.size(); ++i) {
        const QPointF sp = pointToScreen(points_[i]);
        const double dx = sp.x() - pos.x();
        const double dy = sp.y() - pos.y();
        if (std::sqrt(dx * dx + dy * dy) <= 9.0)
            return i;
    }
    return -1;
}

QPointF CurveCanvas::handleScreenPos(int segment, bool isH1) const
{
    const CurveHandlePairDef& h = handles_.at(segment);
    CurvePointDef p;
    p.x = isH1 ? h.h1x : h.h2x;
    p.value = isH1 ? h.h1y : h.h2y;
    return pointToScreen(p);
}

bool CurveCanvas::findHandleAt(const QPoint& pos, int* segment, bool* isH1) const
{
    const auto hit = [this, pos, segment, isH1](int seg, bool h1) {
        if (seg < 0 || seg >= handles_.size())
            return false;
        const QPointF hp = handleScreenPos(seg, h1);
        const double dx = hp.x() - pos.x();
        const double dy = hp.y() - pos.y();
        if (std::sqrt(dx * dx + dy * dy) > 8.0)
            return false;
        *segment = seg;
        *isH1 = h1;
        return true;
    };
    for (int i : selectedPoints_) {
        if (hit(i, true) || hit(i - 1, false))
            return true;
    }
    return false;
}

bool CurveCanvas::selectedHandleValid() const
{
    if (selectedHandleSegment_ < 0 || selectedHandleSegment_ >= handles_.size())
        return false;
    const int owner = selectedHandleIsH1_ ? selectedHandleSegment_ : selectedHandleSegment_ + 1;
    return selectedPoints_.contains(owner);
}

QRectF CurveCanvas::handleConstraintBoxScreen(int segment) const
{
    const QRectF graph = graphRect();
    return QRectF(QPointF(pointToScreen(points_.at(segment)).x(), graph.top()),
                  QPointF(pointToScreen(points_.at(segment + 1)).x(), graph.bottom()));
}

bool CurveCanvas::applySmoothPartner(int segment, bool isH1, double partnerLengthScreen)
{
    const int partnerSeg = isH1 ? segment - 1 : segment + 1;
    if (partnerSeg < 0 || partnerSeg >= handles_.size() || partnerSeg + 1 >= points_.size())
        return false;

    const QPointF anchorScreen = pointToScreen(points_.at(isH1 ? segment : segment + 1));
    const QPointF direction = anchorScreen - handleScreenPos(segment, isH1);
    const QPointF target = curveHandleRayIntoRect(anchorScreen, direction, partnerLengthScreen,
                                                  handleConstraintBoxScreen(partnerSeg));
    const CurvePointDef mapped = screenToPoint(target);
    const float px = std::clamp(mapped.x,
                                points_.at(partnerSeg).x,
                                points_.at(partnerSeg + 1).x);
    const float py = std::clamp(mapped.value, valueMin(), valueMax());
    CurveHandlePairDef next = handles_.at(partnerSeg);
    if (isH1) {
        next.h2x = px;
        next.h2y = py;
    } else {
        next.h1x = px;
        next.h1y = py;
    }
    if (sameHandlePair(handles_.at(partnerSeg), next))
        return false;
    handles_[partnerSeg] = next;
    return true;
}

// Screen-space re-smooth: degenerate handles regrow along the neighbor
// secant (or the surviving handle's tangent) at one third of their segment
// chord; healthy pairs realign on normalize(vR - vL) keeping their lengths.
void CurveCanvas::smoothAnchorAt(int index)
{
    if (index <= 0 || index >= points_.size() - 1 || handles_.size() != points_.size() - 1)
        return;

    const int leftSeg = index - 1;
    const int rightSeg = index;
    const QPointF anchor = pointToScreen(points_.at(index));
    const QPointF leftHandle = handleScreenPos(leftSeg, false);
    const QPointF rightHandle = handleScreenPos(rightSeg, true);
    const QPointF vL = leftHandle - anchor;
    const QPointF vR = rightHandle - anchor;
    const double lenL = std::hypot(vL.x(), vL.y());
    const double lenR = std::hypot(vR.x(), vR.y());
    const bool leftDegenerate = lenL <= 1.0;
    const bool rightDegenerate = lenR <= 1.0;

    const QPointF leftChord = anchor - pointToScreen(points_.at(index - 1));
    const QPointF rightChord = pointToScreen(points_.at(index + 1)) - anchor;

    QPointF tangent;
    double leftLen = lenL;
    double rightLen = lenR;
    if (leftDegenerate && rightDegenerate) {
        tangent = leftChord + rightChord;
        leftLen = std::hypot(leftChord.x(), leftChord.y()) / 3.0;
        rightLen = std::hypot(rightChord.x(), rightChord.y()) / 3.0;
    } else if (leftDegenerate) {
        tangent = vR;
        leftLen = std::hypot(leftChord.x(), leftChord.y()) / 3.0;
    } else if (rightDegenerate) {
        tangent = -vL;
        rightLen = std::hypot(rightChord.x(), rightChord.y()) / 3.0;
    } else {
        tangent = vR - vL;
    }
    const double tangentNorm = std::hypot(tangent.x(), tangent.y());
    if (tangentNorm < 1e-9)
        return;
    tangent /= tangentNorm;

    const QPointF leftTarget = curveHandleRayIntoRect(anchor, -tangent, leftLen,
                                                      handleConstraintBoxScreen(leftSeg));
    const QPointF rightTarget = curveHandleRayIntoRect(anchor, tangent, rightLen,
                                                       handleConstraintBoxScreen(rightSeg));
    constexpr double kNoChangeEpsilonPx = 0.01;
    if (std::hypot(leftTarget.x() - leftHandle.x(), leftTarget.y() - leftHandle.y()) <= kNoChangeEpsilonPx
        && std::hypot(rightTarget.x() - rightHandle.x(), rightTarget.y() - rightHandle.y()) <= kNoChangeEpsilonPx)
        return;

    const CurvePointDef leftMapped = screenToPoint(leftTarget);
    const CurvePointDef rightMapped = screenToPoint(rightTarget);
    handles_[leftSeg].h2x = std::clamp(leftMapped.x,
                                       points_.at(leftSeg).x, points_.at(index).x);
    handles_[leftSeg].h2y = std::clamp(leftMapped.value, valueMin(), valueMax());
    handles_[rightSeg].h1x = std::clamp(rightMapped.x,
                                        points_.at(index).x, points_.at(index + 1).x);
    handles_[rightSeg].h1y = std::clamp(rightMapped.value, valueMin(), valueMax());
    normalizeCurve();
    emit curveEdited(points_, handles_, tr("Smooth Anchor"));
    update();
}

void CurveCanvas::beginHandleDrag(int segment, bool isH1, const QPointF& mousePos, bool independent)
{
    draggedHandleSegment_ = segment;
    draggedHandleIsH1_ = isH1;
    selectedHandleSegment_ = segment;
    selectedHandleIsH1_ = isH1;
    dragHandleGrabOffset_ = handleScreenPos(segment, isH1) - mousePos;
    dragHandleSmooth_ = false;
    dragPartnerLength_ = 0.0;
    const int partnerSeg = isH1 ? segment - 1 : segment + 1;
    if (!independent && partnerSeg >= 0 && partnerSeg < handles_.size()) {
        const QPointF anchor = pointToScreen(points_.at(isH1 ? segment : segment + 1));
        const QPointF grabbed = handleScreenPos(segment, isH1);
        const QPointF partner = handleScreenPos(partnerSeg, !isH1);
        if (curveHandlesMirrorAligned(anchor, grabbed, partner)) {
            dragHandleSmooth_ = true;
            dragPartnerLength_ = std::hypot(partner.x() - anchor.x(), partner.y() - anchor.y());
        }
    }
}

void CurveCanvas::moveDraggedHandle(QMouseEvent* event)
{
    const int seg = draggedHandleSegment_;
    if (seg < 0 || seg >= handles_.size() || seg + 1 >= points_.size())
        return;

    const QPointF anchorScreen = pointToScreen(points_.at(draggedHandleIsH1_ ? seg : seg + 1));
    QPointF desired = QPointF(event->pos()) + dragHandleGrabOffset_;
    if ((event->modifiers() & Qt::ShiftModifier) != 0) {
        const QPointF v = desired - anchorScreen;
        if (std::hypot(v.x(), v.y()) > 0.0) {
            constexpr double kSnapStep = kPi / 4.0;
            const double snapped = std::round(std::atan2(v.y(), v.x()) / kSnapStep) * kSnapStep;
            const QPointF dir(std::cos(snapped), std::sin(snapped));
            desired = curveHandleRayIntoRect(anchorScreen, dir,
                                             v.x() * dir.x() + v.y() * dir.y(),
                                             handleConstraintBoxScreen(seg));
        }
    }

    const CurvePointDef mapped = screenToPoint(desired);
    CurveHandlePairDef next = handles_.at(seg);
    const float hx = std::clamp(mapped.x,
                                points_.at(seg).x, points_.at(seg + 1).x);
    const float hy = std::clamp(mapped.value, valueMin(), valueMax());
    if (draggedHandleIsH1_) {
        next.h1x = hx;
        next.h1y = hy;
    } else {
        next.h2x = hx;
        next.h2y = hy;
    }

    // Alt breaks the pair for the rest of the drag session; the next press
    // re-derives smoothness from geometry.
    if ((event->modifiers() & Qt::AltModifier) != 0)
        dragHandleSmooth_ = false;

    bool changed = !sameHandlePair(handles_.at(seg), next);
    handles_[seg] = next;
    if (dragHandleSmooth_)
        changed = applySmoothPartner(seg, draggedHandleIsH1_, dragPartnerLength_) || changed;
    dragMoved_ = dragMoved_ || changed;
    CurvePointDef appliedPoint;
    appliedPoint.x = hx;
    appliedPoint.value = hy;
    updateGhost(pointToScreen(appliedPoint), desired);
    if (changed) emit curvePreviewed(points_, handles_);
    update();
}

void CurveCanvas::moveBodyDrag(QMouseEvent* event)
{
    const int seg = bodyDragSegment_;
    if (seg < 0 || seg >= handles_.size() || seg + 1 >= points_.size()
        || bodyDragBaseHandles_.size() != handles_.size())
        return;

    const QPointF delta = QPointF(event->pos()) - bodyDragMouseOrigin_;
    const QPointF grab(bezierCoordinate(bodyDragControls_[0].x(), bodyDragControls_[1].x(),
                                        bodyDragControls_[2].x(), bodyDragControls_[3].x(), bodyDragT_),
                       bezierCoordinate(bodyDragControls_[0].y(), bodyDragControls_[1].y(),
                                        bodyDragControls_[2].y(), bodyDragControls_[3].y(), bodyDragT_));
    const QPointF intended = grab + delta;
    QPointF d1;
    QPointF d2;
    curveDistributeBodyDelta(bodyDragT_, delta, &d1, &d2);
    const CurvePointDef m1 = screenToPoint(bodyDragControls_[1] + d1);
    const CurvePointDef m2 = screenToPoint(bodyDragControls_[2] + d2);
    CurveHandlePairDef next = bodyDragBaseHandles_.at(seg);
    next.h1x = std::clamp(m1.x, points_.at(seg).x, points_.at(seg + 1).x);
    next.h1y = std::clamp(m1.value, valueMin(), valueMax());
    next.h2x = std::clamp(m2.x, points_.at(seg).x, points_.at(seg + 1).x);
    next.h2y = std::clamp(m2.value, valueMin(), valueMax());
    const bool changed = !sameHandlePair(handles_.at(seg), next);
    dragMoved_ = dragMoved_ || changed;
    handles_[seg] = next;

    const QPointF p0 = pointToScreen(points_.at(seg));
    const QPointF p3 = pointToScreen(points_.at(seg + 1));
    const QPointF h1 = handleScreenPos(seg, true);
    const QPointF h2 = handleScreenPos(seg, false);
    const QPointF applied(bezierCoordinate(p0.x(), h1.x(), h2.x(), p3.x(), bodyDragT_),
                          bezierCoordinate(p0.y(), h1.y(), h2.y(), p3.y(), bodyDragT_));
    updateGhost(applied, intended);
    if (changed) emit curvePreviewed(points_, handles_);
    update();
}

void CurveCanvas::updateGhost(const QPointF& applied, const QPointF& intended)
{
    ghostActive_ = std::hypot(intended.x() - applied.x(), intended.y() - applied.y()) > 2.0;
    ghostFrom_ = applied;
    ghostTo_ = intended;
}

int CurveCanvas::insertPointBySplit(float x)
{
    if (points_.size() < 2 || handles_.size() != points_.size() - 1)
        return -1;

    int seg = -1;
    for (int i = 0; i + 1 < points_.size(); ++i) {
        if (x > points_.at(i).x && x < points_.at(i + 1).x) {
            seg = i;
            break;
        }
    }
    if (seg < 0)
        return -1;

    const auto toDomain = [this](float value) {
        return curveAxisNormalizedY(axisScale(), yMin(), yMax(), value);
    };
    const auto fromDomain = [this](double y) {
        return curveAxisValueFromNormalizedY(axisScale(), yMin(), yMax(), y);
    };

    const CurvePointDef& a = points_.at(seg);
    const CurvePointDef& b = points_.at(seg + 1);
    const CurveHandlePairDef& h = handles_.at(seg);
    const double px[4] = {a.x, h.h1x, h.h2x, b.x};
    const double py[4] = {toDomain(a.value), toDomain(h.h1y),
                          toDomain(h.h2y), toDomain(b.value)};

    double lo = 0.0;
    double hi = 1.0;
    for (int i = 0; i < 32; ++i) {
        const double mid = (lo + hi) * 0.5;
        const double bx = bezierCoordinate(px[0], px[1], px[2], px[3], mid);
        if (bx < static_cast<double>(x))
            lo = mid;
        else
            hi = mid;
    }
    const double t = (lo + hi) * 0.5;

    const auto lerp = [t](double p, double q) { return p + (q - p) * t; };
    const double q0x = lerp(px[0], px[1]), q0y = lerp(py[0], py[1]);
    const double q1x = lerp(px[1], px[2]), q1y = lerp(py[1], py[2]);
    const double q2x = lerp(px[2], px[3]), q2y = lerp(py[2], py[3]);
    const double r0x = lerp(q0x, q1x),     r0y = lerp(q0y, q1y);
    const double r1x = lerp(q1x, q2x),     r1y = lerp(q1y, q2y);
    const double sy  = lerp(r0y, r1y);

    CurvePointDef split;
    split.x = x;
    split.value = static_cast<float>(fromDomain(sy));
    points_.insert(seg + 1, split);
    handles_[seg] = {static_cast<float>(q0x), static_cast<float>(fromDomain(q0y)),
                     static_cast<float>(r0x), static_cast<float>(fromDomain(r0y))};
    handles_.insert(seg + 1,
                    {static_cast<float>(r1x), static_cast<float>(fromDomain(r1y)),
                     static_cast<float>(q2x), static_cast<float>(fromDomain(q2y))});
    return seg + 1;
}

void CurveCanvas::applyCurvePreset(int segment, float u1, float v1, float u2, float v2)
{
    if (segment < 0 || segment >= handles_.size() || segment + 1 >= points_.size())
        return;

    const CurvePointDef& a = points_.at(segment);
    const CurvePointDef& b = points_.at(segment + 1);
    const auto vToValue = [&](float v) -> float {
        if (v == 0.0f)
            return a.value;
        if (v == 1.0f)
            return b.value;
        const double ya = curveAxisNormalizedY(axisScale(), yMin(), yMax(), a.value);
        const double yb = curveAxisNormalizedY(axisScale(), yMin(), yMax(), b.value);
        return curveAxisValueFromNormalizedY(axisScale(), yMin(), yMax(), ya + v * (yb - ya));
    };

    CurveHandlePairDef next;
    next.h1x = a.x + u1 * (b.x - a.x);
    next.h1y = vToValue(v1);
    next.h2x = a.x + u2 * (b.x - a.x);
    next.h2y = vToValue(v2);
    if (sameHandlePair(handles_.at(segment), next))
        return;

    handles_[segment] = next;
    normalizeCurve();
    emit curveEdited(points_, handles_, tr("Apply Curve Preset"));
    update();
}

// Removing an interior anchor merges its two segments: the merged pair keeps
// the left segment's h1 and the right segment's h2 (normalize re-clamps x).
void CurveCanvas::removePointMerge(int index)
{
    if (index <= 0 || index >= points_.size() - 1)
        return;
    if (handles_.size() == points_.size() - 1) {
        handles_[index - 1].h2x = handles_.at(index).h2x;
        handles_[index - 1].h2y = handles_.at(index).h2y;
        handles_.removeAt(index);
    }
    points_.removeAt(index);
}

void CurveCanvas::beginPointCoordinateEdit(int index)
{
    if (index < 0 || index >= points_.size())
        return;

    const bool collapsedX = profile_.xMaxEditValue == std::optional<float>(xMin_);
    const bool movable = (!profile_.endpointsFollowDomain || (index > 0 && index < points_.size() - 1)) && !collapsedX;
    const CurvePointDef point = points_.at(index);

    auto* dialog = createValueEditor();
    dialog->setObjectName(QStringLiteral("curveCanvasPointEditor"));
    auto* form = new QFormLayout(dialog);
    form->setSizeConstraint(QLayout::SetNoConstraint);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setContentsMargins(8, 6, 8, 6);
    form->setSpacing(6);

    auto* xSpin = new DecimalSpinBox(dialog);
    xSpin->setObjectName("curveCanvasCoordinateXSpin");
    xSpin->setDecimals(std::max(3, profile_.xDecimals));
    xSpin->setSingleStep(1.0);
    xSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    xSpin->setKeyboardTracking(false);
    if (movable)
        xSpin->setRange(index > 0 ? static_cast<double>(points_.at(index - 1).x) + kMinCurveXInterval
                                  : std::min<double>(xMin_, point.x),
                        index + 1 < points_.size() ? static_cast<double>(points_.at(index + 1).x) - kMinCurveXInterval
                                                   : std::max<double>(xMax_, point.x));
    else
        xSpin->setRange(std::min<double>(xMin_, point.x), std::max<double>(xMax_, point.x));
    xSpin->setValue(collapsedX ? xMin_ : static_cast<double>(point.x));
    xSpin->setReadOnly(!movable);
    xSpin->setFocusPolicy(movable ? Qt::StrongFocus : Qt::NoFocus);
    form->addRow(QStringLiteral("X"), xSpin);

    const bool amplitudeDb = axisScale() != CurveAxisScale::LinearValue;
    auto* ySpin = new DecimalSpinBox(dialog);
    ySpin->setObjectName("curveCanvasCoordinateYSpin");
    ySpin->setDecimals(amplitudeDb ? 2 : profile_.valueDecimals);
    ySpin->setSingleStep(amplitudeDb ? 0.1 : 1.0);
    ySpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    ySpin->setKeyboardTracking(false);
    ySpin->setRange(static_cast<double>(valueMin()), static_cast<double>(valueMax()));
    ySpin->setValue(static_cast<double>(point.value));
    form->addRow(QStringLiteral("Y"), ySpin);

    for (auto* spin : {xSpin, ySpin}) {
        QtControlSkin::setSemanticProperty(form->labelForField(spin), "textRole",
                                          QStringLiteral("formLabel"));
        spin->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        spin->setFont(FontManager::instance().getFont(FontRole::Mono));
        connect(&FontManager::instance(), &FontManager::fontChanged, spin, [spin] {
            spin->setFont(FontManager::instance().getFont(FontRole::Mono));
        });
        const QString unit = spin == ySpin ? profile_.valueUnit.trimmed() : QString();
        const auto updateToolTip = [spin, unit] {
            spin->setToolTip(QStringLiteral("%1 %2").arg(spin->text(), unit).trimmed());
        };
        connect(spin, &QDoubleSpinBox::textChanged, spin, updateToolTip);
        updateToolTip();
    }

    const double initialX = xSpin->value();
    const double initialY = ySpin->value();
    connect(dialog, &QDialog::accepted, this, [this, index, point, xSpin, ySpin, initialX, initialY] {
        valueEditor_.clear();
        setPointCoordinates(index, xSpin->value() == initialX ? point.x : static_cast<float>(xSpin->value()),
                             ySpin->value() == initialY ? point.value : static_cast<float>(ySpin->value()));
    });
    auto* focused = movable ? xSpin : ySpin;
    showValueEditor(dialog, pointToScreen(point), focused);
    focused->selectAll();
}

void CurveCanvas::setSingleSelection(int index)
{
    selectedPoints_.clear();
    selectedHandleSegment_ = -1;
    selectedPoint_ = (index >= 0 && index < points_.size()) ? index : -1;
    if (selectedPoint_ >= 0)
        selectedPoints_.insert(selectedPoint_);
}

void CurveCanvas::updateLassoSelection(const QPoint& pos)
{
    lassoRect_ = QRect(lassoStart_, pos).normalized();
    QSet<int> hits;
    for (int i = 0; i < points_.size(); ++i) {
        QRectF pointRect(pointToScreen(points_[i]), QSizeF(0.0, 0.0));
        pointRect.adjust(-4.0, -4.0, 4.0, 4.0);
        if (QRectF(lassoRect_).intersects(pointRect))
            hits.insert(i);
    }

    selectedPoints_ = lassoBaseSelection_;
    if ((lassoModifiers_ & Qt::ControlModifier) != 0) {
        for (int index : hits) {
            if (selectedPoints_.contains(index))
                selectedPoints_.remove(index);
            else
                selectedPoints_.insert(index);
        }
    } else if ((lassoModifiers_ & Qt::ShiftModifier) != 0) {
        selectedPoints_.unite(hits);
    } else {
        selectedPoints_ = hits;
    }
    selectedPoint_ = selectedPoints_.isEmpty() ? -1 : *selectedPoints_.constBegin();
}

bool CurveCanvas::writePointsToClipboard(const QVector<int>& indices) const
{
    if (indices.isEmpty() || !QApplication::clipboard())
        return false;

    QJsonArray pointArray;
    for (int index : indices) {
        if (index < 0 || index >= points_.size())
            continue;
        QJsonObject pointObject = pointToJson(points_.at(index));
        if (index > 0 && index - 1 < handles_.size()) {
            const CurveHandlePairDef& handle = handles_.at(index - 1);
            pointObject.insert(QStringLiteral("leftHandle"),
                               QJsonArray{handle.h2x, handle.h2y});
        }
        if (index < points_.size() - 1 && index < handles_.size()) {
            const CurveHandlePairDef& handle = handles_.at(index);
            pointObject.insert(QStringLiteral("rightHandle"),
                               QJsonArray{handle.h1x, handle.h1y});
        }
        pointArray.append(pointObject);
    }
    if (pointArray.isEmpty())
        return false;

    const QJsonObject obj{
        {QStringLiteral("format"), QString::fromLatin1(kCurveClipboardFormat)},
        {QStringLiteral("family"), profile_.clipboardFamily},
        {QStringLiteral("points"), pointArray}
    };
    QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    return true;
}

bool CurveCanvas::copySelectedPointsToClipboard() const
{
    return writePointsToClipboard(selectedPointIndices());
}

bool CurveCanvas::cutSelectedPointsToClipboard()
{
    QVector<int> indices = selectedPointIndices();
    indices.erase(std::remove_if(indices.begin(), indices.end(), [this](int index) {
        return index <= 0 || index >= points_.size() - 1;
    }), indices.end());
    if (!writePointsToClipboard(indices))
        return false;

    std::sort(indices.begin(), indices.end(), std::greater<int>());
    for (int index : indices)
        removePointMerge(index);
    normalizeCurve();
    setSingleSelection(-1);
    emit selectionChanged();
    emit curveEdited(points_, handles_, tr("Cut Point(s)"));
    update();
    return true;
}

bool CurveCanvas::pasteFromClipboard()
{
    return pastePayload(readCurveClipboardObject());
}

bool CurveCanvas::pastePayload(const QJsonObject& obj)
{
    QVector<ClipboardPoint> incoming;
    if (!readPointPayload(obj, profile_.clipboardFamily, xMin_, xMax_, points_, &incoming))
        return false;

    const QVector<CurvePointDef> originalPoints = points_;
    const QVector<CurveHandlePairDef> originalHandles = handles_;
    for (const ClipboardPoint& point : incoming) {
        if (pointIndexAtX(points_, point.point.x, 0.0) < 0
            && insertPointBySplit(point.point.x) < 0) {
            points_ = originalPoints;
            handles_ = originalHandles;
            return false;
        }
    }

    int originalIndex = 0;
    for (int index = 0; index < points_.size() && originalIndex < originalPoints.size(); ++index) {
        if (points_.at(index).x != originalPoints.at(originalIndex).x)
            continue;
        if (originalIndex > 0 && originalIndex - 1 < originalHandles.size()) {
            handles_[index - 1].h2x = originalHandles.at(originalIndex - 1).h2x;
            handles_[index - 1].h2y = originalHandles.at(originalIndex - 1).h2y;
        }
        if (originalIndex < originalHandles.size()) {
            handles_[index].h1x = originalHandles.at(originalIndex).h1x;
            handles_[index].h1y = originalHandles.at(originalIndex).h1y;
        }
        ++originalIndex;
    }

    QSet<int> pastedIndices;
    for (const ClipboardPoint& point : incoming) {
        const int index = pointIndexAtX(points_, point.point.x, 0.0);
        if (index < 0) {
            points_ = originalPoints;
            handles_ = originalHandles;
            return false;
        }
        const float value = std::clamp(point.point.value, valueMin(), valueMax());
        translateAnchorHandles(handles_, index, points_.at(index), {points_.at(index).x, value});
        points_[index].value = value;
        if (point.hasLeftHandle && index > 0 && index - 1 < handles_.size()) {
            handles_[index - 1].h2x = static_cast<float>(point.leftHandle.x());
            handles_[index - 1].h2y = static_cast<float>(point.leftHandle.y());
        }
        if (point.hasRightHandle && index < points_.size() - 1 && index < handles_.size()) {
            handles_[index].h1x = static_cast<float>(point.rightHandle.x());
            handles_[index].h1y = static_cast<float>(point.rightHandle.y());
        }
        pastedIndices.insert(index);
    }

    normalizeCurve();
    selectedPoints_ = pastedIndices;
    selectedHandleSegment_ = -1;
    selectedPoint_ = *std::min_element(selectedPoints_.cbegin(), selectedPoints_.cend());
    emit selectionChanged();
    const bool changed = !std::equal(points_.cbegin(), points_.cend(),
                                     originalPoints.cbegin(), originalPoints.cend(),
                                     [](const CurvePointDef& lhs, const CurvePointDef& rhs) {
                                         return lhs.x == rhs.x && lhs.value == rhs.value;
                                     })
        || !std::equal(handles_.cbegin(), handles_.cend(),
                        originalHandles.cbegin(), originalHandles.cend(), sameHandlePair);
    if (changed)
        emit curveEdited(points_, handles_, tr("Paste Point(s)"));
    update();
    return true;
}

bool CurveCanvas::canPasteFromClipboard() const
{
    return readPointPayload(readCurveClipboardObject(), profile_.clipboardFamily,
                            xMin_, xMax_, points_, nullptr);
}

void CurveCanvas::showPointLimitToolTip(const QPoint& globalPos)
{
    QToolTip::showText(globalPos,
                       tr("Curves carry at most %1 points").arg(kCurveMaxPoints),
                       this);
}

void CurveCanvas::normalizeCurve()
{
    if (profile_.endpointsFollowDomain)
        for (auto& p : points_)
            p.x = std::clamp(p.x, xMin_, xMax_);
    std::sort(points_.begin(), points_.end(),
        [](const CurvePointDef& a, const CurvePointDef& b) {
            return a.x < b.x;
        });
    if (profile_.endpointsFollowDomain && !points_.isEmpty()) {
        points_.front().x = xMin_;
        points_.back().x = xMax_;
    }
    curveNormalize(valueMin(), valueMax(), points_, handles_);
}

QVector<CurvePointDef> CurveCanvas::defaultCurve() const
{
    CurvePointDef start;
    CurvePointDef end;
    start.x = xMin_;
    end.x = xMax_;
    start.value = 0.0f;
    end.value = profile_.defaultEndValue;
    return {start, end};
}

void CurveCanvasProfile::setVariableAxis(VariableKind kind, const std::vector<VariableLabel>& labels)
{
    discretePreview = kind == VariableKind::Step;
    xTicksUseLabels = discretePreview;
    xTickLabels = discretePreview ? labels : std::vector<VariableLabel>{};
}

CurveRenderData CurveCanvas::renderData() const
{
    const auto& style = ThemeManager::instance().snapshot()->domains.curve;
    CurveRenderData data;
    data.tickFont = FontManager::instance().getFont(FontRole::Axis);
    const QFontMetrics fontMetrics(data.tickFont);
    data.canvasRect = rect();
    data.graphRect = graphRect();
    data.xAxisLabel = profile_.xAxisVisible ? profile_.xAxisLabel : QString{};
    data.yAxisLabel = profile_.yAxisLabel;

    const auto& metrics = style.metrics;
    const QRectF& graph = data.graphRect;
    const qreal textHeight = fontMetrics.height();
    const qreal gap = metrics.amplitudeTickLabelRightGap;
    const qreal inset = metrics.xAxisLabelTopOffset;
    const qreal titleHeight = QFontMetrics(font()).height();
    data.yAxisLabelRect = QRectF(graph.left() + inset, graph.top() + inset,
        std::max(0.0, graph.width() - inset * 2.0), titleHeight);
    data.xAxisLabelRect = QRectF(graph.left() + inset, graph.bottom() - inset - titleHeight,
        std::max(0.0, graph.width() - inset * 2.0), titleHeight);
    if (!graph.contains(data.yAxisLabelRect))
        data.yAxisLabel.clear();
    if (!graph.contains(data.xAxisLabelRect) || data.yAxisLabelRect.intersects(data.xAxisLabelRect))
        data.xAxisLabel.clear();
    const CurveAxisScale scale = axisScale();
    const bool amplitudeAxis = scale != CurveAxisScale::LinearValue;
    const auto addTick = [&](const QRectF& rect, Qt::Alignment alignment,
                             const QString& text, CurveAxisBound bound = CurveAxisBound::None) {
        if (rect.width() < fontMetrics.horizontalAdvance(QStringLiteral("0"))
            || rect.top() < 0.0 || rect.bottom() > height())
            return;
        for (const auto& existing : data.textTicks) {
            if (existing.rect.adjusted(-gap / 2.0, -2.0, gap / 2.0, 2.0).intersects(rect))
                return;
        }
        CurveRenderTextTick tick;
        tick.placement = CurveRenderTextTick::Placement::Rect;
        tick.rect = rect;
        tick.alignment = alignment;
        tick.text = fontMetrics.elidedText(text, Qt::ElideRight, static_cast<int>(rect.width()));
        tick.bound = bound;
        tick.editable = (bound == CurveAxisBound::XMax && profile_.xMaxEditable)
            || ((bound == CurveAxisBound::YMin || bound == CurveAxisBound::YMax)
                && profile_.yRangeEditable);
        data.textTicks.append(tick);
    };
    QVector<double> yValues{yMax(), yMin()};
    const int rows = std::clamp(static_cast<int>(std::round(graph.height()
        / std::max(metrics.amplitudeGridTargetRowSpacing, textHeight + 8.0))),
        amplitudeAxis ? metrics.amplitudeGridMinimumRows : 1, metrics.amplitudeGridMaximumRows);
    const double ySpan = static_cast<double>(yMax()) - yMin();
    const double yStep = ySpan > 0.0 ? tickStep(ySpan / rows) : 1.0;
    const double firstY = std::ceil(yMin() / yStep) * yStep;
    const bool mirrored = scale == CurveAxisScale::MirroredAmplitudeDb;
    const double zeroY = mirrored ? curveAxisNormalizedY(scale, yMin(), yMax(), 0.0f) : 0.0;
    if (mirrored) {
        const qreal y = pointToScreen({xMin_, 0.0f}).y();
        data.gridLines.append(QLineF(graph.left(), y, graph.right(), y));
        yValues.append(0.0);
    }
    for (int i = mirrored ? -rows : 0; i <= rows; ++i) {
        const double normalizedY = zeroY + double(i) / rows;
        if (mirrored && (i == 0 || normalizedY <= 0.0 || normalizedY >= 1.0))
            continue;
        const float value = amplitudeAxis
            ? curveAxisValueFromNormalizedY(scale, yMin(), yMax(), normalizedY)
            : static_cast<float>(firstY + i * yStep);
        if (value <= yMin() || value >= yMax())
            continue;
        const qreal y = pointToScreen({xMin_, value}).y();
        data.gridLines.append(QLineF(graph.left(), y, graph.right(), y));
        yValues.append(value);
    }
    const QStringList yLabels = formatAxisValues(yValues, profile_.valueDecimals);
    for (int i = 0; i < yValues.size(); ++i) {
        const qreal y = pointToScreen({xMin_, static_cast<float>(yValues[i])}).y();
        const auto bound = i == 0 ? CurveAxisBound::YMax
            : i == 1 ? CurveAxisBound::YMin : CurveAxisBound::None;
        addTick(QRectF(0.0, y - textHeight / 2.0, std::max(0.0, graph.left() - gap), textHeight),
            Qt::AlignRight | Qt::AlignVCenter, yLabels[i], bound);
    }

    const auto xTickText = [this](float x) {
        return profile_.xTicksUseLabels
            ? BindingSemantics::axisTickLabel(x, profile_.xTickLabels)
            : QString::number(x, 'f', profile_.xDecimals);
    };
    QVector<float> xValues;
    if (profile_.xAxisVisible) xValues = {xMin_, xMax_};
    const qreal columnSpacing = std::max<qreal>(48.0,
        std::max(fontMetrics.horizontalAdvance(xTickText(xMin_)),
                  fontMetrics.horizontalAdvance(xTickText(xMax_))) + gap * 2.0);
    const int columns = std::max(1, static_cast<int>(graph.width() / columnSpacing));
    double xStep = tickStep((static_cast<double>(xMax_) - xMin_) / columns);
    if (profile_.discretePreview)
        xStep = std::max(1.0, xStep);
    const double firstX = std::ceil(xMin_ / xStep) * xStep;
    for (int i = 0; profile_.xAxisVisible && i <= columns; ++i) {
        const float value = static_cast<float>(firstX + i * xStep);
        if (value <= xMin_ || value >= xMax_)
            continue;
        const qreal x = pointToScreen({value, yMin()}).x();
        data.gridLines.append(QLineF(x, graph.top(), x, graph.bottom()));
        if (profile_.xMaxEditValue.value_or(xMax_) != xMin_)
            xValues.append(value);
    }
    if (profile_.xAxisVisible && profile_.xTicksUseLabels) {
        for (const auto& label : profile_.xTickLabels) {
            if (label.anchor > xMin_ && label.anchor < xMax_)
                xValues.append(static_cast<float>(label.anchor));
        }
    }
    QStringList xLabels;
    for (int i = 0; i < xValues.size(); ++i)
        xLabels.append(xTickText(i == 1 ? profile_.xMaxEditValue.value_or(xValues[i]) : xValues[i]));
    if (!profile_.xTicksUseLabels)
        xLabels = compactAxisLabels(std::move(xLabels));
    for (int i = 0; i < xValues.size(); ++i) {
        const auto bound = i == 0 ? CurveAxisBound::XMin
            : i == 1 ? CurveAxisBound::XMax : CurveAxisBound::None;
        const qreal w = std::min<qreal>(fontMetrics.horizontalAdvance(xLabels[i]) + 4.0,
            std::max(0.0, graph.width() / 2.0 - gap));
        const qreal x = pointToScreen({xValues[i], yMin()}).x();
        const qreal left = i == 0 ? x : i == 1 ? x - w : x - w / 2.0;
        addTick(QRectF(left, graph.bottom() + metrics.xMaxLabelTopOffset, w, textHeight),
            Qt::AlignCenter, xLabels[i], bound);
    }

    const auto discretePath = [&](const QVector<CurvePointDef>& points,
                                  const QVector<CurveHandlePairDef>& handles) {
        const auto valueAt = [&](float x) {
            if (!profile_.endpointsFollowDomain && (x < points.front().x || x > points.back().x))
                return 0.0f;
            return curveBezierValueAt(points, handles, x, axisScale());
        };
        QPainterPath steps;
        const float start = profile_.endpointsFollowDomain ? xMin_ : std::max(xMin_, points.front().x);
        const float end = profile_.endpointsFollowDomain ? xMax_ : std::min(xMax_, points.back().x);
        if (start >= end) return steps;
        int sample = VariableSemantics::discreteRead(start);
        steps.moveTo(pointToScreen(
            {start, valueAt(static_cast<float>(sample))}));
        const int boundaryCount = std::max(0, static_cast<int>(
            std::ceil(end - (static_cast<float>(sample) + 0.5f))));
        if (boundaryCount <= static_cast<int>(graph.width() * 2.0)) {
            for (float boundary = static_cast<float>(sample) + 0.5f;
                 boundary < end;
                 boundary += 1.0f) {
                const float before = valueAt(static_cast<float>(sample));
                const float after = valueAt(static_cast<float>(sample + 1));
                steps.lineTo(pointToScreen({boundary, before}));
                steps.lineTo(pointToScreen({boundary, after}));
                ++sample;
            }
            steps.lineTo(pointToScreen(
                {end, valueAt(static_cast<float>(sample))}));
        } else {
            const int samples = std::max(1, static_cast<int>(graph.width()));
            float previousValue = valueAt(static_cast<float>(sample));
            for (int i = 1; i <= samples; ++i) {
                const float x = start + (end - start)
                    * static_cast<float>(i) / static_cast<float>(samples);
                const int pointSample = VariableSemantics::discreteRead(x);
                const float value = valueAt(static_cast<float>(pointSample));
                steps.lineTo(pointToScreen({x, previousValue}));
                steps.lineTo(pointToScreen({x, value}));
                previousValue = value;
            }
        }
        return steps;
    };

    if (points_.size() >= 2) {
        data.continuousPath.moveTo(pointToScreen({profile_.endpointsFollowDomain
            ? std::min(xMin_, points_.front().x) : points_.front().x, points_.front().value}));
        data.continuousPath.lineTo(pointToScreen(points_.front()));
        for (int i = 0; i + 1 < points_.size(); ++i) {
            const QVector<QPointF> polyline = segmentScreenPolyline(i);
            for (int j = 1; j < polyline.size(); ++j)
                data.continuousPath.lineTo(polyline.at(j));
        }
        if (profile_.endpointsFollowDomain)
            data.continuousPath.lineTo(pointToScreen({std::max(xMax_, points_.back().x), points_.back().value}));

        if (profile_.discretePreview) data.discretePath = discretePath(points_, handles_);

        for (const CurveOverlay& overlay : overlays_) {
            if (overlay.points.size() < 2)
                continue;
            CurveRenderOverlay drawn;
            drawn.colorRole = overlay.colorRole;
            drawn.color = overlay.color;
            drawn.selectable = overlay.selectable;
            drawn.path.moveTo(pointToScreen({profile_.endpointsFollowDomain
                ? std::min(xMin_, overlay.points.front().x) : overlay.points.front().x, overlay.points.front().value}));
            drawn.path.lineTo(pointToScreen(overlay.points.front()));
            for (int i = 0; i + 1 < overlay.points.size(); ++i) {
                const CurvePointDef& left = overlay.points.at(i);
                const CurvePointDef& right = overlay.points.at(i + 1);
                const double screenWidth =
                    std::abs(pointToScreen(right).x() - pointToScreen(left).x())
                    * devicePixelRatioF();
                const int samples = std::clamp(
                    static_cast<int>(std::ceil(screenWidth)), 16, 512);
                for (int step = 1; step < samples; ++step) {
                    const float t = static_cast<float>(step) / samples;
                    const float x = left.x + (right.x - left.x) * t;
                    const float value = curveBezierValueAt(overlay.points, overlay.handles, x, axisScale());
                    drawn.path.lineTo(pointToScreen({x, value}));
                }
                drawn.path.lineTo(pointToScreen(right));
            }
            if (profile_.endpointsFollowDomain)
                drawn.path.lineTo(pointToScreen({std::max(xMax_, overlay.points.back().x), overlay.points.back().value}));
            if (profile_.discretePreview) drawn.discretePath = discretePath(overlay.points, overlay.handles);
            data.overlays.append(drawn);
        }

        const bool activeHandleVisible = selectedHandleValid();
        for (int i : selectedPoints_) {
            if (i < 0 || i >= points_.size())
                continue;
            const QPointF anchor = pointToScreen(points_.at(i));
            const auto addHandle = [&](int segment, bool isH1) {
                if (segment < 0 || segment >= handles_.size())
                    return;
                CurveRenderHandle handle;
                handle.anchor = anchor;
                handle.position = handleScreenPos(segment, isH1);
                handle.active = activeHandleVisible
                    && segment == selectedHandleSegment_
                    && isH1 == selectedHandleIsH1_;
                data.handles.append(handle);
            };
            addHandle(i - 1, false);
            addHandle(i, true);
        }

        const int hoveredPoint = hoverActive_ ? findPointAt(hoverPos_.toPoint()) : -1;
        for (int i = 0; i < points_.size(); ++i) {
            CurveRenderPoint point;
            point.position = pointToScreen(points_.at(i));
            point.selected = selectedPoints_.contains(i);
            point.hovered = hoveredPoint == i;
            data.points.append(point);
        }
    }

    data.curveColor = profile_.color;
    if (!curveVisible_) {
        data.continuousPath = {};
        data.discretePath.reset();
        data.points.clear();
        data.handles.clear();
    }
    const bool interactionActive = draggedPoint_ >= 0
        || draggedHandleSegment_ >= 0
        || pulledHandleAnchor_ >= 0
        || bodyDragSegment_ >= 0
        || lassoActive_;
    data.crosshair.visible = isEnabled() && profile_.xAxisVisible && hoverActive_
        && !interactionActive
        && graph.contains(hoverPos_);
    if (data.crosshair.visible) {
        const CurvePointDef hoverData = screenToPoint(hoverPos_);
        data.crosshair.position = hoverPos_;
        data.crosshair.label = QStringLiteral("%1 · %2 %3")
            .arg(formatAxisValue(profile_.xMaxEditValue == std::optional<float>(xMin_)
                     ? xMin_ : static_cast<double>(hoverData.x), axisReadoutDecimalsForStep(xStep, profile_.xDecimals)))
            .arg(formatAxisValue(hoverData.value, amplitudeAxis ? profile_.valueDecimals
                : axisReadoutDecimalsForStep(yStep, profile_.valueDecimals)))
            .arg(profile_.valueUnit);
    }

    data.ghost.visible = ghostActive_;
    data.ghost.from = ghostFrom_;
    data.ghost.to = ghostTo_;
    data.lasso.visible = lassoActive_ && !lassoRect_.isNull();
    data.lasso.rect = lassoRect_;
    if (profile_.xAxisVisible && playhead_ && xMax_ > xMin_) {
        const float x = std::clamp(*playhead_, xMin_, xMax_);
        const double nx = static_cast<double>(x - xMin_)
            / static_cast<double>(xMax_ - xMin_);
        data.playhead.visible = true;
        data.playhead.x = graph.left() + graph.width() * nx;
    }
    return data;
}

}
