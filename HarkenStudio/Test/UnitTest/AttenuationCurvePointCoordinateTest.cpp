#include "Widgets/CurveEditor/CurveCanvas.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDialog>
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
