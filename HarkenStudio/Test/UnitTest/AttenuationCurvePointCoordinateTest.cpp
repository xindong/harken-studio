#include "Widgets/CurveEditor/CurveCanvas.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QMouseEvent>

#include <memory>

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

TEST(AttenuationCurvePointCoordinate, AppliesTypedCoordinatesWithinBounds)
{
    auto canvas = makeCanvas();
    EXPECT_TRUE(canvas->setPointCoordinates(1, 60.0f, -50.0f));
    const auto points = canvas->curve();
    EXPECT_FLOAT_EQ(points.at(1).x, 60.0f);
    EXPECT_FLOAT_EQ(points.at(1).value, -50.0f);
}

TEST(AttenuationCurvePointCoordinate, DistanceClampedInsideNeighborInterval)
{
    auto canvas = makeCanvas();
    EXPECT_TRUE(canvas->setPointCoordinates(1, 500.0f, -40.0f));
    auto points = canvas->curve();
    EXPECT_GT(points.at(1).x, 99.9f);
    EXPECT_LT(points.at(1).x, points.at(2).x);

    EXPECT_TRUE(canvas->setPointCoordinates(1, -500.0f, -40.0f));
    points = canvas->curve();
    EXPECT_LT(points.at(1).x, 0.01f);
    EXPECT_GT(points.at(1).x, points.at(0).x);
}

TEST(AttenuationCurvePointCoordinate, ValueClampedIntoProfileRange)
{
    auto canvas = makeCanvas();
    EXPECT_TRUE(canvas->setPointCoordinates(1, 50.0f, 40.0f));
    EXPECT_FLOAT_EQ(canvas->curve().at(1).value, 0.0f);
    EXPECT_TRUE(canvas->setPointCoordinates(1, 50.0f, -500.0f));
    EXPECT_FLOAT_EQ(canvas->curve().at(1).value, -96.0f);
}

TEST(AttenuationCurvePointCoordinate, EndpointDistanceStaysPinned)
{
    auto canvas = makeCanvas();
    EXPECT_TRUE(canvas->setPointCoordinates(0, 30.0f, -10.0f));
    EXPECT_FLOAT_EQ(canvas->curve().at(0).x, 0.0f);
    EXPECT_FLOAT_EQ(canvas->curve().at(0).value, -10.0f);

    EXPECT_TRUE(canvas->setPointCoordinates(2, 30.0f, -80.0f));
    EXPECT_FLOAT_EQ(canvas->curve().at(2).x, kMaxDistance);
    EXPECT_FLOAT_EQ(canvas->curve().at(2).value, -80.0f);
}

TEST(AttenuationCurvePointCoordinate, InvalidIndexRefused)
{
    auto canvas = makeCanvas();
    EXPECT_FALSE(canvas->setPointCoordinates(-1, 10.0f, -10.0f));
    EXPECT_FALSE(canvas->setPointCoordinates(3, 10.0f, -10.0f));
}

TEST(AttenuationCurvePointCoordinate, UnchangedCoordinatesReturnFalse)
{
    auto canvas = makeCanvas();
    EXPECT_FALSE(canvas->setPointCoordinates(1, 50.0f, -40.0f));
}

TEST(AttenuationCurvePointCoordinate, DoubleClickEditsCoordinatesWithoutSmoothing)
{
    auto canvas = makeCanvas();
    int commits = 0;
    QObject::connect(canvas.get(), &VNX::CurveCanvas::curveEdited, [&] { ++commits; });
    auto* dialog = openPointEditor(*canvas, 1);
    ASSERT_NE(dialog, nullptr);
    EXPECT_FALSE(dialog->isWindow());
    EXPECT_TRUE(canvas->rect().contains(dialog->geometry()));
    EXPECT_EQ(commits, 0);
    auto* x = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("curveCanvasCoordinateXSpin"));
    auto* y = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("curveCanvasCoordinateYSpin"));
    ASSERT_NE(x, nullptr);
    ASSERT_NE(y, nullptr);
    EXPECT_EQ(x->buttonSymbols(), QAbstractSpinBox::NoButtons);
    x->setValue(60.0);
    y->setValue(-20.0);
    EXPECT_EQ(commits, 0);
    dialog->accept();
    EXPECT_EQ(commits, 1);
    EXPECT_FLOAT_EQ(canvas->curve()[1].x, 60.0f);
    EXPECT_FLOAT_EQ(canvas->curve()[1].value, -20.0f);
}

TEST(AttenuationCurvePointCoordinate, EscapeCancelsAndReloadDismissesInput)
{
    auto canvas = makeCanvas();
    int commits = 0;
    QObject::connect(canvas.get(), &VNX::CurveCanvas::curveEdited, [&] { ++commits; });
    auto* dialog = openPointEditor(*canvas, 1);
    ASSERT_NE(dialog, nullptr);
    dialog->findChild<QDoubleSpinBox*>(QStringLiteral("curveCanvasCoordinateYSpin"))->setValue(-10.0);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(dialog, &escape);
    EXPECT_FALSE(dialog->isVisible());
    EXPECT_EQ(commits, 0);
    EXPECT_FLOAT_EQ(canvas->curve()[1].value, -40.0f);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    dialog = openPointEditor(*canvas, 1);
    ASSERT_NE(dialog, nullptr);
    canvas->setCurve({{0.0f, 0.0f}, {100.0f, -80.0f}});
    EXPECT_FALSE(dialog->isVisible());
    EXPECT_EQ(commits, 0);
}

TEST(AttenuationCurvePointCoordinate, DisplayingPositiveDbDoesNotPermitAttenuationGain)
{
    auto canvas = makeCanvas();
    auto profile = canvas->profile();
    profile.yMax = 24.0f;
    profile.valueMaximum = 0.0f;
    canvas->setProfile(profile);
    auto* dialog = openPointEditor(*canvas, 0);
    ASSERT_NE(dialog, nullptr);
    auto* x = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("curveCanvasCoordinateXSpin"));
    auto* y = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("curveCanvasCoordinateYSpin"));
    EXPECT_TRUE(x->isReadOnly());
    EXPECT_DOUBLE_EQ(y->maximum(), 0.0);
    dialog->reject();
    EXPECT_TRUE(canvas->setPointCoordinates(1, 50.0f, 24.0f));
    EXPECT_FLOAT_EQ(canvas->curve()[1].value, 0.0f);
    EXPECT_EQ(canvas->profile().axisScale, VNX::CurveAxisScale::AmplitudeDb);
    EXPECT_FLOAT_EQ(canvas->profile().yMax, 24.0f);
}

TEST(AttenuationCurvePointCoordinate, AcceptingUnchangedRoundedValuesPreservesPrecision)
{
    auto canvas = makeCanvas();
    ASSERT_TRUE(canvas->setPointCoordinates(1, 50.12345f, -40.12345f));
    const auto before = canvas->curve()[1];
    int commits = 0;
    QObject::connect(canvas.get(), &VNX::CurveCanvas::curveEdited, [&] { ++commits; });
    auto* dialog = openPointEditor(*canvas, 1);
    ASSERT_NE(dialog, nullptr);
    dialog->accept();
    EXPECT_EQ(commits, 0);
    EXPECT_FLOAT_EQ(canvas->curve()[1].x, before.x);
    EXPECT_FLOAT_EQ(canvas->curve()[1].value, before.value);
}

TEST(AttenuationCurvePointCoordinate, OutsideClickAndResizeCancelInlineInput)
{
    auto canvas = makeCanvas();
    int commits = 0;
    QObject::connect(canvas.get(), &VNX::CurveCanvas::curveEdited, [&] { ++commits; });
    auto* dialog = openPointEditor(*canvas, 1);
    ASSERT_NE(dialog, nullptr);
    const QPoint inside(1, 1);
    QMouseEvent insidePress(QEvent::MouseButtonPress, inside, dialog->mapToGlobal(inside),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(dialog, &insidePress);
    EXPECT_TRUE(dialog->isVisible());
    dialog->findChild<QDoubleSpinBox*>(QStringLiteral("curveCanvasCoordinateYSpin"))->setValue(-10.0);
    const QPoint outside(2, 2);
    QMouseEvent press(QEvent::MouseButtonPress, outside, canvas->mapToGlobal(outside),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas.get(), &press);
    EXPECT_FALSE(dialog->isVisible());
    EXPECT_EQ(commits, 0);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    dialog = openPointEditor(*canvas, 1);
    ASSERT_NE(dialog, nullptr);
    canvas->resize(200, 100);
    EXPECT_EQ(canvas->size(), QSize(200, 100));
    EXPECT_FALSE(dialog->isVisible());
    EXPECT_EQ(commits, 0);
    EXPECT_FLOAT_EQ(canvas->curve()[1].value, -40.0f);
}
