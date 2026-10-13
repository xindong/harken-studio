#include "Widgets/CurveEditor/CurveCanvas.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QAction>
#include <QContextMenuEvent>
#include <QJsonArray>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QTimer>

#include <memory>
#include <cmath>

// WP2 acceptance: the bezier mapping canvas hosts any x domain [lo, hi] —
// a normalized fade span [0, 1], a signed variable range, or attenuation
// distance — with the same pinning, clamping, and paste-remap invariants.

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

VNX::CurvePointDef makePoint(float x, float value)
{
    VNX::CurvePointDef p;
    p.x = x;
    p.value = value;
    return p;
}

std::unique_ptr<VNX::CurveCanvas> makeCanvas(float lo, float hi)
{
    ensureApp();
    auto canvas = std::make_unique<VNX::CurveCanvas>();
    canvas->setXDomain(lo, hi);
    return canvas;
}

std::unique_ptr<VNX::CurveCanvas> makeMirroredCanvas()
{
    auto canvas = makeCanvas(0.0f, 10.0f);
    VNX::CurveCanvasProfile profile;
    profile.axisScale = VNX::CurveAxisScale::MirroredAmplitudeDb;
    profile.yMax = 24.0f;
    canvas->setProfile(profile);
    canvas->resize(800, 600);
    return canvas;
}

QPointF mirroredPoint(const VNX::CurveCanvas& canvas, float x, float value)
{
    const QRectF graph = canvas.renderData().graphRect;
    const double ny = VNX::curveAxisNormalizedY(
        VNX::CurveAxisScale::MirroredAmplitudeDb, -96.0f, 24.0f, value);
    return {graph.left() + graph.width() * x / 10.0, graph.bottom() - graph.height() * ny};
}

void sendMouse(VNX::CurveCanvas& canvas, QEvent::Type type, const QPoint& pos,
               Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const Qt::MouseButtons buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, QPointF(pos), QPointF(canvas.mapToGlobal(pos)), button, buttons, modifiers);
    QApplication::sendEvent(&canvas, &event);
}

float mirroredValueAt(const QVector<VNX::CurvePointDef>& points,
                       const QVector<VNX::CurveHandlePairDef>& handles, float x)
{
    return VNX::curveBezierValueAt(points, handles, x, VNX::CurveAxisScale::MirroredAmplitudeDb);
}

void expectStraight(const VNX::CurveCanvas& canvas)
{
    const auto data = canvas.renderData();
    ASSERT_EQ(data.points.size(), 2);
    const QPointF first = data.points.front().position;
    const QPointF last = data.points.back().position;
    ASSERT_GT(data.continuousPath.elementCount(), 2);
    for (int i = 0; i < data.continuousPath.elementCount(); ++i) {
        const auto element = data.continuousPath.elementAt(i);
        const double fraction = (element.x - first.x()) / (last.x() - first.x());
        EXPECT_NEAR(element.y, first.y() + fraction * (last.y() - first.y()), 0.01);
    }
}

QJsonObject makePayload(const QVector<VNX::CurvePointDef>& points)
{
    QJsonArray pointArray;
    for (const auto& p : points)
        pointArray.append(QJsonObject{
            {QStringLiteral("x"), p.x},
            {QStringLiteral("value"), p.value}});
    return QJsonObject{
        {QStringLiteral("format"), QStringLiteral("Harken.Curve.v3")},
        {QStringLiteral("family"), QStringLiteral("volume")},
        {QStringLiteral("points"), pointArray}
    };
}

} // namespace

TEST(CurveCanvasDomain, DefaultCurveSpansUnitDomain)
{
    auto canvas = makeCanvas(0.0f, 1.0f);
    canvas->setCurve({});
    const auto points = canvas->curve();
    ASSERT_EQ(points.size(), 2);
    EXPECT_FLOAT_EQ(points.first().x, 0.0f);
    EXPECT_FLOAT_EQ(points.last().x, 1.0f);
}

TEST(CurveCanvasDomain, BodyDragPreviewsBeforeCommitAndCancelRestoresHandles)
{
    auto canvas = makeMirroredCanvas();
    canvas->setCurve({{0, 0}, {10, -12}});
    const auto original = canvas->curveHandles();
    int previews = 0;
    int commits = 0;
    int finishes = 0;
    QObject::connect(canvas.get(), &VNX::CurveCanvas::curvePreviewed,
        [&] { ++previews; });
    QObject::connect(canvas.get(), &VNX::CurveCanvas::curveEdited,
        [&] { ++commits; });
    QObject::connect(canvas.get(), &VNX::CurveCanvas::curvePreviewFinished,
        [&] { ++finishes; });
    const QPoint start = canvas->renderData().continuousPath.pointAtPercent(0.5).toPoint();
    sendMouse(*canvas, QEvent::MouseButtonPress, start);
    sendMouse(*canvas, QEvent::MouseMove, start + QPoint(0, 50));
    EXPECT_EQ(previews, 1);
    EXPECT_EQ(commits, 0);
    canvas->cancelDrag();
    EXPECT_EQ(finishes, 1);
    const auto restored = canvas->curveHandles();
    ASSERT_EQ(restored.size(), original.size());
    EXPECT_FLOAT_EQ(restored.front().h1x, original.front().h1x);
    EXPECT_FLOAT_EQ(restored.front().h1y, original.front().h1y);
    EXPECT_FLOAT_EQ(restored.front().h2x, original.front().h2x);
    EXPECT_FLOAT_EQ(restored.front().h2y, original.front().h2y);
    sendMouse(*canvas, QEvent::MouseButtonRelease, start + QPoint(0, 50));
    EXPECT_EQ(commits, 0);
    sendMouse(*canvas, QEvent::MouseButtonPress, start);
    sendMouse(*canvas, QEvent::MouseMove, start + QPoint(0, 50));
    EXPECT_EQ(previews, 2);
    sendMouse(*canvas, QEvent::MouseButtonRelease, start + QPoint(0, 50));
    EXPECT_EQ(commits, 1);
}

TEST(CurveCanvasDomain, DefaultCurveSpansSignedDomain)
{
    auto canvas = makeCanvas(-24.0f, 36.0f);
    canvas->setCurve({});
    const auto points = canvas->curve();
    ASSERT_EQ(points.size(), 2);
    EXPECT_FLOAT_EQ(points.first().x, -24.0f);
    EXPECT_FLOAT_EQ(points.last().x, 36.0f);
}

TEST(CurveCanvasDomain, NormalizePinsEndpointsToDomain)
{
    auto canvas = makeCanvas(-10.0f, 10.0f);
    canvas->setCurve({makePoint(-50.0f, 0.0f),
                      makePoint(2.0f, -40.0f),
                      makePoint(50.0f, -96.0f)});
    const auto points = canvas->curve();
    ASSERT_EQ(points.size(), 3);
    EXPECT_FLOAT_EQ(points.first().x, -10.0f);
    EXPECT_FLOAT_EQ(points.at(1).x, 2.0f);
    EXPECT_FLOAT_EQ(points.last().x, 10.0f);
    EXPECT_EQ(canvas->curveHandles().size(), 2);
}

TEST(CurveCanvasDomain, PointCoordinatesClampIntoDomain)
{
    auto canvas = makeCanvas(-10.0f, 10.0f);
    canvas->setCurve({makePoint(-10.0f, 0.0f),
                      makePoint(0.0f, -40.0f),
                      makePoint(10.0f, -96.0f)});

    EXPECT_TRUE(canvas->setPointCoordinates(1, 5.0f, -50.0f));
    EXPECT_FLOAT_EQ(canvas->curve().at(1).x, 5.0f);

    EXPECT_TRUE(canvas->setPointCoordinates(1, -500.0f, -50.0f));
    const auto points = canvas->curve();
    EXPECT_GT(points.at(1).x, points.at(0).x);
    EXPECT_LT(points.at(1).x, -9.9f);

    EXPECT_TRUE(canvas->setPointCoordinates(0, 3.0f, -10.0f));
    EXPECT_FLOAT_EQ(canvas->curve().at(0).x, -10.0f);
    EXPECT_TRUE(canvas->setPointCoordinates(2, 3.0f, -80.0f));
    EXPECT_FLOAT_EQ(canvas->curve().at(2).x, 10.0f);
}

TEST(CurveCanvasDomain, PastePreservesOriginalX)
{
    auto canvas = makeCanvas(-10.0f, 10.0f);
    canvas->setCurve({});
    const QJsonObject payload = makePayload({makePoint(-5.0f, -20.0f),
                                             makePoint(4.0f, -70.0f)});
    ASSERT_TRUE(canvas->pastePayload(payload));
    const auto points = canvas->curve();
    ASSERT_EQ(points.size(), 4);
    EXPECT_FLOAT_EQ(points.at(0).x, -10.0f);
    EXPECT_FLOAT_EQ(points.at(1).x, -5.0f);
    EXPECT_FLOAT_EQ(points.at(2).x, 4.0f);
    EXPECT_FLOAT_EQ(points.at(3).x, 10.0f);
}

TEST(CurveCanvasDomain, PasteRejectsOutOfDomainAtomically)
{
    auto canvas = makeCanvas(0.0f, 1.0f);
    canvas->setCurve({});
    const auto beforePoints = canvas->curve();
    const auto beforeHandles = canvas->curveHandles();
    const QJsonObject payload = makePayload({makePoint(0.4f, -20.0f),
                                             makePoint(100.0f, -96.0f)});
    EXPECT_FALSE(canvas->pastePayload(payload));
    const auto points = canvas->curve();
    ASSERT_EQ(points.size(), beforePoints.size());
    ASSERT_EQ(canvas->curveHandles().size(), beforeHandles.size());
    for (int i = 0; i < points.size(); ++i) {
        EXPECT_FLOAT_EQ(points.at(i).x, beforePoints.at(i).x);
        EXPECT_FLOAT_EQ(points.at(i).value, beforePoints.at(i).value);
    }
}

TEST(CurveCanvasDomain, MirroredPointDragCrossesZeroAndCommitsOnce)
{
    auto canvas = makeMirroredCanvas();
    canvas->setCurve({{0.0f, -24.0f}, {5.0f, -6.0f}, {10.0f, 24.0f}});
    int edits = 0;
    QObject::connect(canvas.get(), &VNX::CurveCanvas::curveEdited, [&edits] { ++edits; });
    const QPoint start = mirroredPoint(*canvas, 5.0f, -6.0f).toPoint();
    const QPoint end = mirroredPoint(*canvas, 5.0f, 6.0f).toPoint();
    sendMouse(*canvas, QEvent::MouseButtonPress, start);
    sendMouse(*canvas, QEvent::MouseMove, end);
    sendMouse(*canvas, QEvent::MouseButtonRelease, end);
    EXPECT_NEAR(canvas->curve()[1].value, 6.0f, 0.1f);
    EXPECT_FLOAT_EQ(canvas->curve()[1].x, 5.0f);
    EXPECT_EQ(edits, 1);
}

TEST(CurveCanvasDomain, RepeatedAnchorDragKeepsDefaultLineAcrossZero)
{
    auto canvas = makeMirroredCanvas();
    canvas->setCurve({{0.0f, 0.0f}, {10.0f, -96.0f}});
    QStringList edits;
    QObject::connect(canvas.get(), &VNX::CurveCanvas::curveEdited,
        [&edits](const auto&, const auto&, const QString& label) { edits.append(label); });
    for (float value : {6.0f, 12.0f, -6.0f}) {
        const QPoint start = canvas->renderData().points.front().position.toPoint();
        const QPoint end = mirroredPoint(*canvas, 0.0f, value).toPoint();
        sendMouse(*canvas, QEvent::MouseButtonPress, start);
        sendMouse(*canvas, QEvent::MouseMove, end);
        sendMouse(*canvas, QEvent::MouseButtonRelease, end);
        EXPECT_NEAR(canvas->curve().front().value, value, 0.1f);
        EXPECT_FLOAT_EQ(canvas->curveHandles().front().h1y, canvas->curve().front().value);
        EXPECT_FLOAT_EQ(canvas->curve().back().value, -96.0f);
        expectStraight(*canvas);
    }
    EXPECT_EQ(edits, QStringList({QStringLiteral("Move Point(s)"),
                                  QStringLiteral("Move Point(s)"), QStringLiteral("Move Point(s)")}));
}

TEST(CurveCanvasDomain, SelectedAttenuationEndpointRemainsAnAnchorOnRepeatedDrags)
{
    auto canvas = makeCanvas(0.0f, 10.0f);
    canvas->resize(800, 600);
    canvas->setCurve({{0.0f, 0.0f}, {10.0f, -96.0f}});
    canvas->setSelectedPointIndex(1);
    for (int dy : {-100, -80, 40}) {
        const QPoint start = canvas->renderData().points.back().position.toPoint();
        const float before = canvas->curve().back().value;
        sendMouse(*canvas, QEvent::MouseButtonPress, start);
        sendMouse(*canvas, QEvent::MouseMove, start + QPoint(0, dy));
        sendMouse(*canvas, QEvent::MouseButtonRelease, start + QPoint(0, dy));
        EXPECT_NE(canvas->curve().back().value, before);
        EXPECT_FLOAT_EQ(canvas->curveHandles().front().h2y, canvas->curve().back().value);
        expectStraight(*canvas);
    }
}

TEST(CurveCanvasDomain, AltPullsUnselectedEndpointHandleWithoutMovingAnchor)
{
    for (int index : {0, 1}) {
        auto canvas = makeMirroredCanvas();
        canvas->setCurve({{0.0f, 0.0f}, {10.0f, -96.0f}});
        int edits = 0;
        QObject::connect(canvas.get(), &VNX::CurveCanvas::curveEdited,
            [&edits](const auto&, const auto&, const QString& label) {
                ++edits;
                EXPECT_EQ(label, QStringLiteral("Move Handle"));
            });
        const QPoint start = canvas->renderData().points.at(index).position.toPoint();
        const QPoint end = start + QPoint(index == 0 ? 80 : -80, -80);
        sendMouse(*canvas, QEvent::MouseButtonPress, start, Qt::AltModifier);
        sendMouse(*canvas, QEvent::MouseMove, end);
        sendMouse(*canvas, QEvent::MouseButtonRelease, end);
        EXPECT_FLOAT_EQ(canvas->curve().front().value, 0.0f);
        EXPECT_FLOAT_EQ(canvas->curve().back().value, -96.0f);
        const auto handle = canvas->curveHandles().front();
        if (index == 0) {
            EXPECT_GT(handle.h1x, 0.0f);
            EXPECT_GT(handle.h1y, 0.0f);
            EXPECT_FLOAT_EQ(handle.h2x, 10.0f);
            EXPECT_FLOAT_EQ(handle.h2y, -96.0f);
        } else {
            EXPECT_LT(handle.h2x, 10.0f);
            EXPECT_GT(handle.h2y, -96.0f);
            EXPECT_FLOAT_EQ(handle.h1x, 0.0f);
            EXPECT_FLOAT_EQ(handle.h1y, 0.0f);
        }
        EXPECT_EQ(edits, 1);
    }
}

TEST(CurveCanvasDomain, AltPullLocksInteriorHandleSideAndDoesNotRequireAltToStayHeld)
{
    for (int direction : {-1, 1}) {
        auto canvas = makeMirroredCanvas();
        canvas->setCurve({{0.0f, -24.0f}, {5.0f, -6.0f}, {10.0f, 12.0f}});
        const QPoint start = mirroredPoint(*canvas, 5.0f, -6.0f).toPoint();
        sendMouse(*canvas, QEvent::MouseButtonPress, start, Qt::AltModifier);
        sendMouse(*canvas, QEvent::MouseMove, start + QPoint(40 * direction, -40), Qt::AltModifier);
        const QPoint end = start + QPoint(-40 * direction, -80);
        sendMouse(*canvas, QEvent::MouseMove, end);
        sendMouse(*canvas, QEvent::MouseButtonRelease, end);
        const auto handles = canvas->curveHandles();
        EXPECT_FLOAT_EQ(canvas->curve()[1].x, 5.0f);
        EXPECT_FLOAT_EQ(canvas->curve()[1].value, -6.0f);
        if (direction < 0) {
            EXPECT_FLOAT_EQ(handles[0].h2x, 5.0f);
            EXPECT_GT(handles[0].h2y, -6.0f);
            EXPECT_FLOAT_EQ(handles[1].h1x, 5.0f);
            EXPECT_FLOAT_EQ(handles[1].h1y, -6.0f);
        } else {
            EXPECT_FLOAT_EQ(handles[1].h1x, 5.0f);
            EXPECT_GT(handles[1].h1y, -6.0f);
            EXPECT_FLOAT_EQ(handles[0].h2x, 5.0f);
            EXPECT_FLOAT_EQ(handles[0].h2y, -6.0f);
        }
    }
}

TEST(CurveCanvasDomain, AltClickDoesNotEditOrCaptureTheNextAnchorDrag)
{
    auto canvas = makeMirroredCanvas();
    canvas->setCurve({{0.0f, 0.0f}, {10.0f, -96.0f}});
    int edits = 0;
    QObject::connect(canvas.get(), &VNX::CurveCanvas::curveEdited, [&edits] { ++edits; });
    const QPoint start = mirroredPoint(*canvas, 0.0f, 0.0f).toPoint();
    sendMouse(*canvas, QEvent::MouseButtonPress, start, Qt::AltModifier);
    sendMouse(*canvas, QEvent::MouseButtonRelease, start, Qt::AltModifier);
    EXPECT_EQ(edits, 0);
    const QPoint end = start + QPoint(0, -50);
    sendMouse(*canvas, QEvent::MouseButtonPress, start);
    sendMouse(*canvas, QEvent::MouseMove, end);
    sendMouse(*canvas, QEvent::MouseButtonRelease, end);
    EXPECT_EQ(edits, 1);
    EXPECT_GT(canvas->curve().front().value, 0.0f);
    expectStraight(*canvas);
}

TEST(CurveCanvasDomain, TypedAnchorCoordinatesTranslateHandlesInCanvasSpace)
{
    auto canvas = makeMirroredCanvas();
    canvas->setCurve({{0.0f, -18.0f}, {5.0f, -6.0f}, {10.0f, 18.0f}},
                     {{1.0f, -18.0f, 4.0f, -12.0f}, {6.0f, -2.0f, 9.0f, 18.0f}});
    const QPointF anchorBefore = mirroredPoint(*canvas, 5.0f, -6.0f);
    const QPointF leftBefore = mirroredPoint(*canvas, 4.0f, -12.0f) - anchorBefore;
    const QPointF rightBefore = mirroredPoint(*canvas, 6.0f, -2.0f) - anchorBefore;
    ASSERT_TRUE(canvas->setPointCoordinates(1, 5.5f, 3.0f));
    const auto handles = canvas->curveHandles();
    const QPointF anchorAfter = mirroredPoint(*canvas, 5.5f, 3.0f);
    const QPointF leftAfter = mirroredPoint(*canvas, handles[0].h2x, handles[0].h2y) - anchorAfter;
    const QPointF rightAfter = mirroredPoint(*canvas, handles[1].h1x, handles[1].h1y) - anchorAfter;
    EXPECT_NEAR(leftAfter.x(), leftBefore.x(), 0.01);
    EXPECT_NEAR(leftAfter.y(), leftBefore.y(), 0.01);
    EXPECT_NEAR(rightAfter.x(), rightBefore.x(), 0.01);
    EXPECT_NEAR(rightAfter.y(), rightBefore.y(), 0.01);
}

TEST(CurveCanvasDomain, MirroredBodyDragFollowsPointerAcrossZero)
{
    auto canvas = makeMirroredCanvas();
    canvas->setCurve({{0.0f, -12.0f}, {10.0f, 6.0f}}, {{3.0f, -12.0f, 7.0f, 6.0f}});
    const QPoint start = mirroredPoint(*canvas, 4.0f,
        mirroredValueAt(canvas->curve(), canvas->curveHandles(), 4.0f)).toPoint();
    const QRectF graph = canvas->renderData().graphRect;
    const float x = static_cast<float>((start.x() - graph.left()) * 10.0 / graph.width());
    const float before = mirroredValueAt(canvas->curve(), canvas->curveHandles(), x);
    const QPoint end = start + QPoint(0, -100);
    QMouseEvent hover(QEvent::MouseMove, QPointF(start), QPointF(canvas->mapToGlobal(start)),
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas.get(), &hover);
    EXPECT_EQ(canvas->cursor().shape(), Qt::SizeVerCursor);
    int edits = 0;
    QObject::connect(canvas.get(), &VNX::CurveCanvas::curveEdited, [&edits] { ++edits; });
    sendMouse(*canvas, QEvent::MouseButtonPress, start);
    sendMouse(*canvas, QEvent::MouseMove, end);
    sendMouse(*canvas, QEvent::MouseButtonRelease, end);
    const float after = mirroredValueAt(canvas->curve(), canvas->curveHandles(), x);
    EXPECT_LT(before, 0.0f);
    EXPECT_GT(after, 0.0f);
    EXPECT_NEAR(mirroredPoint(*canvas, x, after).y(), mirroredPoint(*canvas, x, before).y() - 100.0, 0.05);
    EXPECT_FLOAT_EQ(canvas->curve().front().value, -12.0f);
    EXPECT_FLOAT_EQ(canvas->curve().back().value, 6.0f);
    EXPECT_EQ(edits, 1);
}

TEST(CurveCanvasDomain, MultiSelectionMovesByTheSameCanvasOffset)
{
    auto canvas = makeMirroredCanvas();
    canvas->setCurve({{0.0f, -18.0f}, {5.0f, -6.0f}, {10.0f, 6.0f}});
    canvas->setSelectedPointIndices({0, 1, 2}, 1);
    const auto before = canvas->renderData();
    const QPoint start = before.points[1].position.toPoint();
    const QPoint end = start + QPoint(0, -30);
    sendMouse(*canvas, QEvent::MouseButtonPress, start);
    sendMouse(*canvas, QEvent::MouseMove, end);
    sendMouse(*canvas, QEvent::MouseButtonRelease, end);
    const auto after = canvas->renderData();
    for (int i = 0; i < before.points.size(); ++i) {
        EXPECT_NEAR(after.points[i].position.x(), before.points[i].position.x(), 0.01);
        EXPECT_NEAR(after.points[i].position.y(), before.points[i].position.y() - 30.0, 0.01);
    }
    for (int i = 0; i < before.handles.size(); ++i)
        EXPECT_NEAR(after.handles[i].position.y(), before.handles[i].position.y() - 30.0, 0.01);
}

TEST(CurveCanvasDomain, AltDraggingExpandedHandleKeepsItsPartnerFixed)
{
    auto canvas = makeMirroredCanvas();
    const float rightHandleDb = static_cast<float>(20.0 * std::log10(
        2.0 * std::pow(10.0, -6.0 / 20.0) - std::pow(10.0, -12.0 / 20.0)));
    canvas->setCurve({{0.0f, -18.0f}, {5.0f, -6.0f}, {10.0f, 12.0f}},
                     {{1.0f, -18.0f, 4.0f, -12.0f}, {6.0f, rightHandleDb, 9.0f, 12.0f}});
    canvas->setSelectedPointIndex(1);
    const QPoint start = mirroredPoint(*canvas, 6.0f, rightHandleDb).toPoint();
    const QPoint end = mirroredPoint(*canvas, 7.0f, 3.0f).toPoint();
    sendMouse(*canvas, QEvent::MouseButtonPress, start, Qt::AltModifier);
    sendMouse(*canvas, QEvent::MouseMove, end);
    sendMouse(*canvas, QEvent::MouseButtonRelease, end);
    const auto handles = canvas->curveHandles();
    EXPECT_GT(handles[1].h1x, 6.5f);
    EXPECT_GT(handles[1].h1y, 2.9f);
    EXPECT_FLOAT_EQ(handles[0].h2x, 4.0f);
    EXPECT_FLOAT_EQ(handles[0].h2y, -12.0f);
}

TEST(CurveCanvasDomain, MirroredInsertionPreservesBindingValues)
{
    auto canvas = makeMirroredCanvas();
    canvas->setCurve({{0.0f, -18.0f}, {10.0f, 18.0f}}, {{2.0f, -8.0f, 8.0f, 15.0f}});
    const auto points = canvas->curve();
    const auto handles = canvas->curveHandles();
    const float value = mirroredValueAt(points, handles, 5.0f);
    ASSERT_TRUE(canvas->addPointAt(mirroredPoint(*canvas, 5.0f, value).toPoint()));
    ASSERT_EQ(canvas->curve().size(), 3);
    for (float x = 0.0f; x <= 10.0f; x += 0.25f)
        EXPECT_NEAR(mirroredValueAt(canvas->curve(), canvas->curveHandles(), x),
                    mirroredValueAt(points, handles, x), 1e-3f);
}

TEST(CurveCanvasDomain, MirroredHandleDragKeepsSmoothBindingTangents)
{
    auto canvas = makeMirroredCanvas();
    const float rightHandleDb = static_cast<float>(20.0 * std::log10(
        2.0 * std::pow(10.0, -6.0 / 20.0) - std::pow(10.0, -12.0 / 20.0)));
    canvas->setCurve({{0.0f, -18.0f}, {5.0f, -6.0f}, {10.0f, 12.0f}},
                     {{1.0f, -18.0f, 4.0f, -12.0f}, {6.0f, rightHandleDb, 9.0f, 12.0f}});
    canvas->setSelectedPointIndex(1);
    const QPoint start = mirroredPoint(*canvas, 6.0f, rightHandleDb).toPoint();
    const QPoint end = mirroredPoint(*canvas, 7.0f, 3.0f).toPoint();
    sendMouse(*canvas, QEvent::MouseButtonPress, start);
    sendMouse(*canvas, QEvent::MouseMove, end);
    sendMouse(*canvas, QEvent::MouseButtonRelease, end);
    const auto handles = canvas->curveHandles();
    EXPECT_GT(handles[1].h1x, 6.5f);
    EXPECT_NE(handles[0].h2y, -12.0f);
    const QPointF anchor = mirroredPoint(*canvas, 5.0f, -6.0f);
    const QPointF left = mirroredPoint(*canvas, handles[0].h2x, handles[0].h2y) - anchor;
    const QPointF right = mirroredPoint(*canvas, handles[1].h1x, handles[1].h1y) - anchor;
    const double leftSlope = left.y() / left.x();
    const double rightSlope = right.y() / right.x();
    EXPECT_NEAR(leftSlope, rightSlope, 1e-4);
}

TEST(CurveCanvasDomain, MirroredSmoothCommandAlignsBindingTangents)
{
    auto canvas = makeMirroredCanvas();
    canvas->setCurve({{0.0f, -24.0f}, {5.0f, -6.0f}, {10.0f, 18.0f}},
                     {{1.0f, -24.0f, 4.0f, -15.0f}, {6.0f, 9.0f, 9.0f, 18.0f}});
    bool smoothed = false;
    QTimer::singleShot(0, canvas.get(), [&] {
        for (auto* menu : canvas->findChildren<QMenu*>()) {
            for (auto* action : menu->actions()) {
                if (action->text() == QStringLiteral("Smooth") && action->isEnabled()) {
                    action->trigger();
                    smoothed = true;
                }
            }
            menu->close();
        }
    });
    const QPoint pos = mirroredPoint(*canvas, 5.0f, -6.0f).toPoint();
    QContextMenuEvent event(QContextMenuEvent::Mouse, pos, canvas->mapToGlobal(pos));
    QApplication::sendEvent(canvas.get(), &event);
    ASSERT_TRUE(smoothed);
    const auto handles = canvas->curveHandles();
    const QPointF anchor = mirroredPoint(*canvas, 5.0f, -6.0f);
    const QPointF left = mirroredPoint(*canvas, handles[0].h2x, handles[0].h2y) - anchor;
    const QPointF right = mirroredPoint(*canvas, handles[1].h1x, handles[1].h1y) - anchor;
    const double leftSlope = left.y() / left.x();
    const double rightSlope = right.y() / right.x();
    EXPECT_NEAR(leftSlope, rightSlope, 1e-4);
}
