#include "Widgets/CurveEditor/CurveCanvas.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QContextMenuEvent>
#include <QDialog>
#include <QMenu>
#include <QMouseEvent>
#include <QTimer>

#include <memory>
#include <limits>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

constexpr float kMaxDistance = 100.0f;

VNX::CurvePointDef makePoint(float distance, float value)
{
    VNX::CurvePointDef p;
    p.x = distance;
    p.value = value;
    return p;
}

std::unique_ptr<VNX::CurveCanvas> makeCanvas()
{
    ensureApp();
    auto canvas = std::make_unique<VNX::CurveCanvas>();
    canvas->setXDomain(0.0f, kMaxDistance);
    canvas->setCurve({makePoint(0.0f, 0.0f),
                      makePoint(50.0f, -40.0f),
                      makePoint(kMaxDistance, -96.0f)});
    return canvas;
}

QDialog* openPointEditor(VNX::CurveCanvas& canvas, int index)
{
    canvas.resize(420, 300);
    canvas.show();
    const QPointF point = canvas.renderData().points.at(index).position;
    QMouseEvent click(QEvent::MouseButtonDblClick, point, canvas.mapToGlobal(point.toPoint()),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &click);
    return canvas.findChild<QDialog*>(QStringLiteral("curveCanvasPointEditor"));
}

} // namespace

TEST(AttenuationCurvePointCoordinate, AcceptingUnchangedRoundedValuesPreservesPrecision)
{
    auto canvas = makeCanvas();
    ASSERT_TRUE(canvas->setPointCoordinates(1, 50.12345f, -40.12345f));
    const auto before = canvas->curve()[1];
    auto* dialog = openPointEditor(*canvas, 1);
    ASSERT_NE(dialog, nullptr);
    dialog->accept();
    EXPECT_FLOAT_EQ(canvas->curve()[1].x, before.x);
    EXPECT_FLOAT_EQ(canvas->curve()[1].value, before.value);
}

TEST(CurveCanvasSelection, PresetsOnlyChangeThePrimaryPointsRightSegment)
{
    ensureApp();
    for (const auto preset : VNX::kCurvePresets) {
        SCOPED_TRACE(static_cast<int>(preset));
        VNX::CurveCanvas canvas;
        canvas.setXDomain(0.0f, 100.0f);
        canvas.setCurve({{0.0f, 0.0f}, {25.0f, -40.0f}, {60.0f, -10.0f}, {100.0f, -96.0f}});
        canvas.setSelectedPointIndices({0, 1, 3}, 1);
        const auto points = canvas.curve();
        const auto handles = canvas.curveHandles();
        const auto selection = canvas.selectedPointIndices();
        int edits = 0;
        QObject::connect(&canvas, &VNX::CurveCanvas::curveEdited, &canvas,
            [&edits] { ++edits; });
        ASSERT_TRUE(canvas.applySelectedCurvePreset(preset));
        EXPECT_EQ(canvas.curve(), points);
        EXPECT_EQ(canvas.curveHandles()[0], handles[0]);
        EXPECT_EQ(canvas.curveHandles()[2], handles[2]);
        EXPECT_NE(canvas.curveHandles()[1], handles[1]);
        EXPECT_EQ(canvas.selectedPointIndices(), selection);
        EXPECT_EQ(canvas.selectedPointIndex(), 1);
        EXPECT_EQ(edits, 1);
        EXPECT_FALSE(canvas.applySelectedCurvePreset(preset));
        EXPECT_EQ(edits, 1);
        const auto applied = canvas.curveHandles();
        for (const int index : {3, -1}) {
            canvas.setSelectedPointIndex(index);
            EXPECT_FALSE(canvas.applySelectedCurvePreset(preset));
            EXPECT_EQ(canvas.curve(), points);
            EXPECT_EQ(canvas.curveHandles(), applied);
            EXPECT_EQ(edits, 1);
        }
    }
}

TEST(CurveCanvasSelection, ContextMenuTargetsTheClickedAnchorAndItsRightSegment)
{
    ensureApp();
    VNX::CurveCanvas canvas;
    canvas.setXDomain(0.0f, 100.0f);
    canvas.resize(600, 400);
    canvas.show();
    for (const int index : {1, 2, 3}) {
        canvas.setCurve({{0.0f, 0.0f}, {25.0f, -40.0f}, {60.0f, -10.0f}, {100.0f, -96.0f}});
        canvas.setSelectedPointIndex(0);
        const auto points = canvas.curve();
        const auto handles = canvas.curveHandles();
        bool inspected = false;
        QTimer closeMenu;
        QObject::connect(&closeMenu, &QTimer::timeout, &canvas, [] {
            if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()))
                menu->close();
        });
        closeMenu.start(1000);
        QTimer::singleShot(0, &canvas, [&] {
            auto* menu = canvas.findChild<QMenu*>(QString(), Qt::FindDirectChildrenOnly);
            ASSERT_NE(menu, nullptr);
            auto* presets = menu->findChild<QMenu*>(QStringLiteral("curvePresetMenu"));
            EXPECT_EQ(canvas.selectedPointIndex(), index);
            if (index < handles.size()) {
                EXPECT_NE(presets, nullptr);
                if (presets) {
                    for (auto* action : presets->actions()) {
                        if (action->data().toInt() == static_cast<int>(VNX::CurvePreset::Convex)) {
                            action->trigger();
                            inspected = true;
                            break;
                        }
                    }
                }
            } else {
                EXPECT_TRUE(!presets || !presets->menuAction()->isEnabled());
                inspected = true;
            }
            menu->close();
        });
        const QPoint position = canvas.renderData().points.at(index).position.toPoint();
        QContextMenuEvent event(QContextMenuEvent::Mouse, position, canvas.mapToGlobal(position));
        QApplication::sendEvent(&canvas, &event);
        EXPECT_TRUE(inspected);
        EXPECT_EQ(canvas.curve(), points);
        for (int segment = 0; segment < handles.size(); ++segment) {
            if (segment == index)
                EXPECT_NE(canvas.curveHandles()[segment], handles[segment]);
            else
                EXPECT_EQ(canvas.curveHandles()[segment], handles[segment]);
        }
    }
}

TEST(CurveCanvasDomain, CoordinateEditingPreservesOrderingAtDifferentScales)
{
    ensureApp();
    for (const float span : {100.0f, 0.01f, 0.0001f}) {
        for (const bool pinned : {false, true}) {
            SCOPED_TRACE(::testing::Message() << span << ", pinned=" << pinned);
            VNX::CurveCanvas canvas;
            auto profile = canvas.profile();
            profile.endpointsFollowDomain = pinned;
            canvas.setProfile(profile);
            canvas.setXDomain(0.0f, span);
            canvas.setCurve({{0.0f, 0.0f}, {span * 0.4f, -30.0f}, {span, -96.0f}});
            ASSERT_TRUE(canvas.setPointCoordinates(1, span * 0.4f, -40.0f));
            EXPECT_FLOAT_EQ(canvas.curve()[1].x, span * 0.4f);
            ASSERT_TRUE(canvas.setPointCoordinates(1, span * 0.6f, -40.0f));
            EXPECT_FLOAT_EQ(canvas.curve()[1].x, span * 0.6f);
            ASSERT_TRUE(canvas.setPointCoordinates(1, span * 2.0f, -40.0f));
            EXPECT_LT(canvas.curve()[1].x, canvas.curve()[2].x);
            EXPECT_GT(canvas.curve()[1].x, canvas.curve()[0].x);
        }
    }
}

TEST(CurveCanvasDomain, ExistingOutOfRangeCoordinatesRemainEditable)
{
    auto canvas = makeCanvas();
    auto profile = canvas->profile();
    profile.endpointsFollowDomain = false;
    canvas->setProfile(profile);
    canvas->setCurve({{20.0f, 0.0f}, {50.0f, -40.0f}, {100.0f, -96.0f}});
    canvas->setXDomain(0.0f, 10.0f);
    ASSERT_TRUE(canvas->setPointCoordinates(1, 40.0f, -30.0f));
    EXPECT_FLOAT_EQ(canvas->curve()[1].x, 40.0f);
    EXPECT_LT(canvas->curve()[0].x, canvas->curve()[1].x);
    EXPECT_LT(canvas->curve()[1].x, canvas->curve()[2].x);
}

TEST(CurveCanvasDomain, ClippedPointsCanBeSeparatedAfterRangeContraction)
{
    auto canvas = makeCanvas();
    canvas->setCurve({{0.0f, 0.0f}, {50.0f, -30.0f}, {75.0f, -60.0f}, {100.0f, -96.0f}});
    canvas->setXDomain(0.0f, 10.0f);
    ASSERT_TRUE(canvas->setPointCoordinates(1, 3.0f, -30.0f));
    ASSERT_TRUE(canvas->setPointCoordinates(2, 6.0f, -60.0f));
    for (int index = 1; index < canvas->curve().size(); ++index)
        EXPECT_GT(canvas->curve()[index].x, canvas->curve()[index - 1].x);
}

TEST(CurveCanvasDomain, RenderingIsBoundedNearFloatingPointIntegerLimits)
{
    ensureApp();
    VNX::CurveCanvas canvas;
    canvas.resize(420, 300);
    auto profile = canvas.profile();
    profile.discretePreview = true;
    canvas.setProfile(profile);
    canvas.setXDomain(16777216.0f, 16777224.0f);
    canvas.setCurve({{16777216.0f, 0.0f}, {16777224.0f, -96.0f}});
    const auto data = canvas.renderData();
    ASSERT_TRUE(data.discretePath.has_value());
    EXPECT_LT(data.discretePath->elementCount(), 100);
    for (const float value : {16777216.0f, std::numeric_limits<float>::max()}) {
        canvas.setXDomain(value, value);
        EXPECT_GT(canvas.xMax(), canvas.xMin());
    }
}
