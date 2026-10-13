#include "Widgets/Blueprint/BlueprintCanvasView.h"
#include "Widgets/Blueprint/NodeContentEditor.h"
#include "Widgets/Common/ParameterControlHost.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QEnterEvent>
#include <QGraphicsProxyWidget>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QScopeGuard>
#include <QSlider>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

using VNX::BlueprintCanvasView;
using Mode = BlueprintCanvasView::InteractionMode;

class TestBlueprintCanvasView : public BlueprintCanvasView
{
public:
    using BlueprintCanvasView::drawBackground;
};

class BlueprintCanvasViewTest : public ::testing::Test
{
protected:
    QVariant interactionSetting = QSettings().value(QStringLiteral("Blueprint/InteractionMode"));
    QGraphicsScene scene;
    TestBlueprintCanvasView view;
    QGraphicsRectItem* node = nullptr;
    QGraphicsProxyWidget* proxy = nullptr;
    QLineEdit* number = nullptr;
    QSlider* slider = nullptr;

    void SetUp() override
    {
        view.setInteractionMode(Mode::Select);
        view.setScene(&scene);
        view.setSceneRect(-2000, -2000, 4000, 4000);
        view.resize(800, 600);
        node = scene.addRect(-120, -90, 240, 40);
        node->setCursor(Qt::SizeAllCursor);
        auto* fields = new QWidget;
        auto* layout = new QVBoxLayout(fields);
        number = new QLineEdit(QStringLiteral("-14.3"), fields);
        slider = new QSlider(Qt::Horizontal, fields);
        slider->setRange(0, 100);
        slider->setValue(50);
        slider->setCursor(Qt::SizeHorCursor);
        layout->addWidget(number);
        layout->addWidget(slider);
        fields->setFixedSize(240, 100);
        proxy = scene.addWidget(fields);
        proxy->setPos(-120, -40);
        view.show();
        QApplication::processEvents();
        view.centerOn(0, 0);
    }

    void TearDown() override
    {
        QSettings settings;
        if (interactionSetting.isValid())
            settings.setValue(QStringLiteral("Blueprint/InteractionMode"), interactionSetting);
        else
            settings.remove(QStringLiteral("Blueprint/InteractionMode"));
    }

    QPoint pointOn(QWidget* widget) const
    {
        return view.mapFromScene(proxy->mapToScene(
            widget->mapTo(proxy->widget(), widget->rect().center())));
    }

    void mouse(QEvent::Type type, QPoint position, Qt::MouseButton button = Qt::NoButton,
        Qt::MouseButtons buttons = Qt::NoButton)
    {
        const QPoint global = view.viewport()->mapToGlobal(position);
        QMouseEvent event(type, QPointF(position), QPointF(global), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &event);
        QApplication::processEvents();
    }
};

TEST_F(BlueprintCanvasViewTest, GridChangesDensityWithoutFadedDots)
{
    const auto background = VNX::ThemeManager::instance().snapshot()->domains.blueprint.canvasBackground;
    const auto render = [&](double zoom, QPointF origin = QPointF(96, 96)) {
        view.setZoomFactor(zoom);
        QImage image(192, 192, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate(origin);
        painter.scale(zoom, zoom);
        view.drawBackground(&painter, painter.worldTransform().inverted().mapRect(QRectF(image.rect())));
        painter.end();
        return image;
    };
    for (const double zoom : {0.15, 0.3, 0.6, 1.2, 2.4}) {
        SCOPED_TRACE(zoom);
        const auto image = render(zoom);
        EXPECT_DOUBLE_EQ(view.gridSpacing() * zoom, 24.0);
        EXPECT_EQ(image, render(1.2));
        EXPECT_NE(image.pixelColor(96, 96), background);
        EXPECT_EQ(image.pixelColor(108, 96), background);
        EXPECT_EQ(image.pixelColor(84, 96), background);
        EXPECT_EQ(image.pixelColor(120, 96), image.pixelColor(96, 96));
        EXPECT_EQ(image.pixelColor(72, 96), image.pixelColor(96, 96));
        EXPECT_EQ(render(zoom, QPointF(96.5, 95.5)), render(1.2, QPointF(96.5, 95.5)));
    }
    for (const double zoom : {0.1, 0.2, 0.4, 0.8, 1.6, 3.2}) {
        SCOPED_TRACE(zoom);
        const auto image = render(zoom);
        EXPECT_DOUBLE_EQ(view.gridSpacing() * zoom, 16.0);
        EXPECT_NE(image.pixelColor(112, 96), background);
        if (zoom > BlueprintCanvasView::kMinZoom) {
            view.setZoomFactor(zoom - 0.001);
            const double coarse = view.gridSpacing();
            view.setZoomFactor(zoom);
            EXPECT_DOUBLE_EQ(view.gridSpacing(), coarse / 2.0);
        }
    }
}

TEST_F(BlueprintCanvasViewTest, GridTextureKeepsItsOriginAcrossPanDpiAndPartialRepaint)
{
    const auto compare = [](const QImage& first, const QImage& second) {
        ASSERT_EQ(first.size(), second.size());
        int largestDifference = 0;
        for (int y = 0; y < first.height(); ++y) {
            for (int x = 0; x < first.width(); ++x) {
                const auto a = first.pixelColor(x, y);
                const auto b = second.pixelColor(x, y);
                const int difference = std::max({std::abs(a.red() - b.red()), std::abs(a.green() - b.green()),
                    std::abs(a.blue() - b.blue()), std::abs(a.alpha() - b.alpha())});
                largestDifference = std::max(largestDifference, difference);
            }
        }
        EXPECT_LE(largestDifference, 1);
    };
    const auto render = [&](double zoom, double dpr, QPointF origin, bool partial) {
        view.setZoomFactor(zoom);
        QImage image(qRound(192 * dpr), qRound(192 * dpr), QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF whole(0, 0, 192, 192);
        const QList<QRectF> pieces = partial
            ? QList<QRectF>{QRectF(0, 0, 71, 113), QRectF(71, 0, 121, 113),
                           QRectF(0, 113, 71, 79), QRectF(71, 113, 121, 79)}
            : QList<QRectF>{whole};
        for (const auto& piece : pieces) {
            painter.save();
            painter.setClipRect(piece);
            painter.translate(origin);
            painter.scale(zoom, zoom);
            view.drawBackground(&painter, painter.worldTransform().inverted().mapRect(piece));
            painter.restore();
        }
        painter.end();
        return image;
    };
    for (const double zoom : {0.4, 0.55, 0.79, 1.0, 1.15, 2.4}) {
        for (const double dpr : {1.0, 1.25, 1.5, 2.0}) {
            SCOPED_TRACE(zoom);
            SCOPED_TRACE(dpr);
            const QPointF origin(47.25, 63.5);
            const auto full = render(zoom, dpr, origin, false);
            compare(full, render(zoom, dpr, origin, true));
            const auto panned = render(zoom, dpr, origin + QPointF(8, 12), false);
            const QPoint delta(qRound(8 * dpr), qRound(12 * dpr));
            const QSize overlap(full.width() - delta.x(), full.height() - delta.y());
            compare(full.copy(QRect(QPoint(), overlap)), panned.copy(QRect(delta, overlap)));
        }
    }
}

TEST_F(BlueprintCanvasViewTest, PanCursorOverridesHoveredItemsAndEmbeddedControls)
{
    mouse(QEvent::MouseMove, pointOn(number));
    ASSERT_EQ(view.viewport()->cursor().shape(), Qt::IBeamCursor);
    view.setInteractionMode(Mode::Pan);
    EXPECT_EQ(view.viewport()->cursor().shape(), Qt::OpenHandCursor);

    const QPoint points[] = {
        view.mapFromScene(QPointF(-300, -200)),
        view.mapFromScene(node->sceneBoundingRect().center()),
        pointOn(number), pointOn(slider)
    };
    for (const auto& point : points) {
        mouse(QEvent::MouseMove, point);
        EXPECT_EQ(view.viewport()->cursor().shape(), Qt::OpenHandCursor);
    }

    view.viewport()->setCursor(Qt::IBeamCursor);
    EXPECT_EQ(view.viewport()->cursor().shape(), Qt::OpenHandCursor);
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(view.viewport(), &leave);
    const QPoint position = pointOn(number);
    QEnterEvent enter(QPointF(position), QPointF(position),
        QPointF(view.viewport()->mapToGlobal(position)));
    QApplication::sendEvent(view.viewport(), &enter);
    EXPECT_EQ(view.viewport()->cursor().shape(), Qt::OpenHandCursor);
}

TEST_F(BlueprintCanvasViewTest, PanOverFieldsKeepsClosedHandAndDoesNotEdit)
{
    mouse(QEvent::MouseMove, pointOn(number));
    view.setInteractionMode(Mode::Pan);
    for (auto* field : {static_cast<QWidget*>(number), static_cast<QWidget*>(slider)}) {
        const auto start = pointOn(field);
        const auto end = start + QPoint(50, 30);
        const auto initialTransform = view.viewportTransform();
        mouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        EXPECT_EQ(view.viewport()->cursor().shape(), Qt::ClosedHandCursor);
        EXPECT_EQ(scene.mouseGrabberItem(), nullptr);
        mouse(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
        EXPECT_EQ(view.viewport()->cursor().shape(), Qt::ClosedHandCursor);
        EXPECT_NE(view.viewportTransform(), initialTransform);
        mouse(QEvent::MouseButtonRelease, end, Qt::LeftButton);
        EXPECT_EQ(view.viewport()->cursor().shape(), Qt::OpenHandCursor);
        mouse(QEvent::MouseButtonDblClick, end, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, end, Qt::LeftButton);
        EXPECT_EQ(scene.mouseGrabberItem(), nullptr);
    }
    EXPECT_EQ(number->text(), QStringLiteral("-14.3"));
    EXPECT_EQ(slider->value(), 50);
    EXPECT_EQ(node->pos(), QPointF());
    EXPECT_EQ(proxy->pos(), QPointF(-120, -40));
}

TEST_F(BlueprintCanvasViewTest, RightButtonPanOverridesFieldCursorAndRestoresSelectHover)
{
    const auto start = pointOn(number);
    const auto end = start + QPoint(40, 20);
    mouse(QEvent::MouseMove, start);
    ASSERT_EQ(view.viewport()->cursor().shape(), Qt::IBeamCursor);
    mouse(QEvent::MouseButtonPress, start, Qt::RightButton, Qt::RightButton);
    EXPECT_EQ(view.viewport()->cursor().shape(), Qt::ClosedHandCursor);
    mouse(QEvent::MouseMove, end, Qt::NoButton, Qt::RightButton);
    EXPECT_EQ(view.viewport()->cursor().shape(), Qt::ClosedHandCursor);
    mouse(QEvent::MouseButtonRelease, end, Qt::RightButton);
    EXPECT_EQ(view.viewport()->cursor().shape(), Qt::ArrowCursor);
    mouse(QEvent::MouseMove, pointOn(number));
    EXPECT_EQ(view.viewport()->cursor().shape(), Qt::IBeamCursor);

    view.setInteractionMode(Mode::Pan);
    view.setInteractionMode(Mode::Select);
    EXPECT_EQ(view.viewport()->cursor().shape(), Qt::ArrowCursor);
    mouse(QEvent::MouseMove, pointOn(slider));
    EXPECT_EQ(view.viewport()->cursor().shape(), Qt::SizeHorCursor);
}

TEST_F(BlueprintCanvasViewTest, BlueprintAdjustmentCursorOnlyCoversTheVisibleSlider)
{
    const auto previousStyle = qApp->styleSheet();
    const auto restore = qScopeGuard([&]() { qApp->setStyleSheet(previousStyle); });
    qApp->setStyleSheet(VNX::QtControlSkin::current().applicationStyleSheet());
    VNX::NodeContentInfo info;
    info.volume = 0.0f;
    info.pitch = 0.0f;
    auto* content = new VNX::NodeContentEditor(info, VNX::kAllNodeContentFields,
        VNX::NodeContentEditor::labelWidth(VNX::kAllNodeContentFields));
    auto* contentProxy = scene.addWidget(content);
    contentProxy->setPos(300, 100);
    view.centerOn(contentProxy);
    QApplication::processEvents();
    auto* host = content->findChild<VNX::ParameterControlHost*>("nodeSlider_volume");
    auto* control = host->findChild<QSlider*>();
    ASSERT_EQ(control->height(), 6);
    EXPECT_EQ(host->cursor().shape(), Qt::ArrowCursor);
    const auto position = [&](QWidget* widget, const QPoint& local) {
        return view.mapFromScene(contentProxy->mapToScene(widget->mapTo(content, local)));
    };
    const auto overSlider = position(control, control->rect().center());
    mouse(QEvent::MouseMove, overSlider);
    EXPECT_EQ(view.viewport()->cursor().shape(), Qt::SizeHorCursor);
    auto* value = content->findChild<QLabel*>("nodeValue_volume");
    const QPoint outside[] = {
        position(host, QPoint(host->width() / 2, 0)),
        position(host, QPoint(host->width() / 2, host->height() - 1)),
        position(host, QPoint(-4, host->height() / 2)),
        position(host, QPoint(host->width() + 4, host->height() / 2)),
        position(value, value->rect().center())
    };
    for (const auto& point : outside) {
        mouse(QEvent::MouseMove, overSlider);
        ASSERT_EQ(view.viewport()->cursor().shape(), Qt::SizeHorCursor);
        mouse(QEvent::MouseMove, point);
        EXPECT_EQ(view.viewport()->cursor().shape(), Qt::ArrowCursor);
        const int before = control->value();
        mouse(QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, point, Qt::LeftButton);
        EXPECT_EQ(control->value(), before);
        EXPECT_FALSE(control->isSliderDown());
    }
    mouse(QEvent::MouseButtonPress, overSlider, Qt::LeftButton, Qt::LeftButton);
    ASSERT_TRUE(control->isSliderDown());
    const int beforeDrag = control->value();
    const auto belowSlider = position(control, QPoint(control->width() / 4, control->height() + 2));
    mouse(QEvent::MouseMove, belowSlider, Qt::NoButton, Qt::LeftButton);
    EXPECT_NE(control->value(), beforeDrag);
    mouse(QEvent::MouseButtonRelease, belowSlider, Qt::LeftButton);
    EXPECT_FALSE(control->isSliderDown());
    mouse(QEvent::MouseMove, belowSlider);
    EXPECT_EQ(view.viewport()->cursor().shape(), Qt::ArrowCursor);
    view.setInteractionMode(Mode::Pan);
    mouse(QEvent::MouseMove, overSlider);
    EXPECT_EQ(view.viewport()->cursor().shape(), Qt::OpenHandCursor);
}

}
