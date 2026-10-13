#include "Widgets/Blueprint/ConnectionPath.h"
#include "Widgets/Blueprint/NodeContentEditor.h"
#include "Core/ContextInfo.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterControlHost.h"
#include "Core/ThemeManager.h"
#include "Core/QtControlSkin.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QDoubleSpinBox>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QScopeGuard>
#include <QSlider>
#include <QStackedWidget>
#include <QStyleOptionGraphicsItem>
#include <QStyleOptionSlider>

#include <array>
#include <cmath>
#include <memory>

namespace {

using namespace VNX;

class BlueprintNodeGraphVisualTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (qApp) return;
        static int argc = 1;
        static char name[] = "harken_editor_tests";
        static char* argv[] = {name, nullptr};
        static QApplication app(argc, argv);
    }
};

TEST_F(BlueprintNodeGraphVisualTest, NodeContentUsesCurrentApplicationThemeWithoutLocalStyles)
{
    const auto previousStyle = qApp->styleSheet();
    const auto restoreStyle = qScopeGuard([&] { qApp->setStyleSheet(previousStyle); });
    NodeContentEditor editor({.volume = -3.0f}, NodeContentField::Volume,
        NodeContentEditor::labelWidth(NodeContentField::Volume));
    editor.show();
    auto* value = editor.findChild<QLabel*>(QStringLiteral("nodeValue_volume"));
    auto* slider = editor.findChild<QSlider*>();
    ASSERT_NE(value, nullptr);
    ASSERT_NE(slider, nullptr);
    for (const auto& accent : {QColor("#238941"), QColor("#dfd039")}) {
        auto theme = std::make_shared<ThemeSnapshot>(*ThemeManager::instance().snapshot());
        theme->ui.colors.accent = accent;
        theme->domains.nodeGraphNode.contentLabel = accent;
        theme->domains.nodeGraphNode.contentValue = QColor(accent.blue(), accent.red(), accent.green());
        qApp->setStyleSheet(QtControlSkin(theme).applicationStyleSheet());
        QApplication::processEvents();
        EXPECT_EQ(value->palette().color(QPalette::WindowText),
                  theme->domains.nodeGraphNode.contentValue);
        for (auto* label : editor.findChildren<QLabel*>()) {
            if (label->property("nodeContentRole") == QStringLiteral("label"))
                EXPECT_EQ(label->palette().color(QPalette::WindowText),
                          theme->domains.nodeGraphNode.contentLabel);
        }
        EXPECT_TRUE(editor.styleSheet().isEmpty());
        for (auto* child : editor.findChildren<QWidget*>())
            EXPECT_TRUE(child->styleSheet().isEmpty());
        QStyleOptionSlider option;
        option.initFrom(slider);
        option.orientation = Qt::Horizontal;
        option.state |= QStyle::State_Horizontal;
        EXPECT_EQ(slider->style()->subControlRect(QStyle::CC_Slider, &option,
            QStyle::SC_SliderGroove, slider).height(), 6);
        EXPECT_EQ(slider->style()->subControlRect(QStyle::CC_Slider, &option,
            QStyle::SC_SliderHandle, slider).width(), 5);
    }
}

TEST_F(BlueprintNodeGraphVisualTest, ParallelConnectionsKeepCommonEndpointsAndSingleCubicInEveryDirection)
{
    NodePin from(NodePin::Output);
    NodePin to(NodePin::Input);
    from.setPos(20, 30);
    to.setPos(220, 130);
    ConnectionPath center(&from, &to);
    ConnectionPath lower(&from, &to);
    ConnectionPath upper(&from, &to);
    lower.setParallelOffset(-12.0);
    upper.setParallelOffset(12.0);
    constexpr std::array directions{CanvasFlowDirection::LR, CanvasFlowDirection::RL,
        CanvasFlowDirection::TB, CanvasFlowDirection::BT};
    for (const auto direction : directions) {
        SCOPED_TRACE(static_cast<int>(direction));
        center.setBlueprintFlowDirection(direction);
        lower.setBlueprintFlowDirection(direction);
        upper.setBlueprintFlowDirection(direction);
        const bool vertical = direction == CanvasFlowDirection::TB || direction == CanvasFlowDirection::BT;
        for (const auto* edge : {&center, &lower, &upper}) {
            ASSERT_EQ(edge->path().elementCount(), 4);
            EXPECT_EQ(edge->path().elementAt(1).type, QPainterPath::CurveToElement);
            EXPECT_EQ(edge->path().pointAtPercent(0.0), from.scenePos());
            EXPECT_EQ(edge->path().pointAtPercent(1.0), to.scenePos());
            const auto shift = edge->path().pointAtPercent(0.5) - center.path().pointAtPercent(0.5);
            EXPECT_NEAR(vertical ? shift.x() : shift.y(), edge->parallelOffset(), 0.000001);
            EXPECT_NEAR(vertical ? shift.y() : shift.x(), 0.0, 0.000001);
        }
        EXPECT_NE(lower.path(), upper.path());
        EXPECT_FALSE(lower.shape().contains(upper.path().pointAtPercent(0.5)));
        EXPECT_FALSE(upper.shape().contains(lower.path().pointAtPercent(0.5)));
        upper.setParallelOffset(0.0);
        EXPECT_EQ(upper.path(), center.path());
        upper.setParallelOffset(12.0);
    }
}

TEST_F(BlueprintNodeGraphVisualTest, ParallelConnectionsFollowMovingNodesAndContentResizing)
{
    QGraphicsScene scene;
    auto* from = new NodeItem(QStringLiteral("Source"));
    auto* to = new NodeItem(QStringLiteral("Destination"));
    scene.addItem(from);
    scene.addItem(to);
    from->addOutputPin();
    to->addInputPin();
    to->setPos(400, 100);
    auto* center = new ConnectionPath(from->outputPin(), to->inputPin());
    auto* parallel = new ConnectionPath(from->outputPin(), to->inputPin());
    scene.addItem(center);
    scene.addItem(parallel);
    parallel->setParallelOffset(18.0);
    const auto before = parallel->path();
    from->setPos(35, 80);
    to->setPos(510, 210);
    from->setContentInfo({.volume = 0.0, .pitch = 1200.0});
    auto* content = new NodeContentEditor(from->contentInfo(), kAllNodeContentFields,
        NodeContentEditor::labelWidth(kAllNodeContentFields));
    from->setContentWidget(content);
    EXPECT_NE(parallel->path(), before);
    ASSERT_EQ(parallel->path().elementCount(), 4);
    EXPECT_EQ(parallel->path().pointAtPercent(0.0), from->outputPin()->scenePos());
    EXPECT_EQ(parallel->path().pointAtPercent(1.0), to->inputPin()->scenePos());
    EXPECT_NEAR(parallel->path().pointAtPercent(0.5).y() - center->path().pointAtPercent(0.5).y(),
        18.0, 0.000001);
    EXPECT_DOUBLE_EQ(parallel->parallelOffset(), 18.0);
}

TEST_F(BlueprintNodeGraphVisualTest, FocusDimmingIncludesFieldsAndPinsWithoutChangingGeometryOrInteraction)
{
    QGraphicsScene scene;
    auto* node = new NodeItem(QStringLiteral("Source"));
    scene.addItem(node);
    node->addInputPin();
    node->addOutputPin();
    node->setContentInfo({.volume = -6.0, .pitch = 1200.0});
    node->setWaveformPeaks({0.25f, 0.75f, 0.5f});
    auto* content = new NodeContentEditor(node->contentInfo(), kAllNodeContentFields,
        NodeContentEditor::labelWidth(kAllNodeContentFields));
    node->setContentWidget(content);
    node->setSelected(true);
    const auto bounds = node->boundingRect();
    const auto shape = node->shape();
    node->setFocusDimmed(true);
    EXPECT_DOUBLE_EQ(node->opacity(), 0.35);
    EXPECT_DOUBLE_EQ(node->inputPin()->effectiveOpacity(), 0.35);
    EXPECT_DOUBLE_EQ(content->graphicsProxyWidget()->effectiveOpacity(), 0.35);
    EXPECT_EQ(node->boundingRect(), bounds);
    EXPECT_EQ(node->shape(), shape);
    EXPECT_TRUE(node->isSelected());
    EXPECT_TRUE(node->isEnabled());
    EXPECT_TRUE(scene.items(QPointF(12, 12)).contains(node));
    node->setPlaybackActive(true);
    EXPECT_DOUBLE_EQ(node->opacity(), 0.35);
    node->setFocusDimmed(false);
    EXPECT_TRUE(node->playbackActive());
    EXPECT_TRUE(node->isSelected());
    EXPECT_DOUBLE_EQ(content->graphicsProxyWidget()->effectiveOpacity(), 1.0);
    EXPECT_DOUBLE_EQ(node->inputPin()->effectiveOpacity(), 1.0);
}

TEST_F(BlueprintNodeGraphVisualTest, ConnectionDimmingPreservesStateColorsSelectionAndHitArea)
{
    QGraphicsScene scene;
    NodePin from(NodePin::Output);
    NodePin to(NodePin::Input);
    to.setPos(300, 40);
    auto* edge = new ConnectionPath(&from, &to);
    scene.addItem(edge);
    edge->setBlueprintEdgeKind(BlueprintEdgeKind::Send);
    edge->setLabel(QStringLiteral("Send (-6.0 dB)"));
    const auto shape = edge->shape();
    const auto bounds = edge->boundingRect();
    edge->setFocusDimmed(true);
    EXPECT_DOUBLE_EQ(edge->opacity(), 0.35);
    EXPECT_EQ(edge->displayColor(), ConnectionPath::defaultColor());
    EXPECT_EQ(edge->shape(), shape);
    EXPECT_EQ(edge->boundingRect(), bounds);
    EXPECT_TRUE(scene.items(edge->path().pointAtPercent(0.5)).contains(edge));
    edge->setSelected(true);
    EXPECT_EQ(edge->displayColor(), ThemeManager::instance().colors().accent);
    edge->setPlaybackActive(true);
    EXPECT_DOUBLE_EQ(edge->opacity(), 0.35);
    EXPECT_EQ(edge->displayColor(), ThemeManager::instance().colors().success);
    edge->setFocusDimmed(false);
    EXPECT_DOUBLE_EQ(edge->opacity(), 1.0);
    EXPECT_TRUE(edge->isSelected());
    EXPECT_TRUE(edge->playbackActive());
    EXPECT_TRUE(edge->isEnabled());
}

TEST_F(BlueprintNodeGraphVisualTest, DropOverlayPreservesSelectionPlaybackAndContentInteraction)
{
    QGraphicsScene scene;
    auto* node = new NodeItem(QStringLiteral("Source"));
    scene.addItem(node);
    node->setContentInfo({.volume = -6.0, .pitch = 1200.0});
    auto* content = new NodeContentEditor(node->contentInfo(), kAllNodeContentFields,
        NodeContentEditor::labelWidth(kAllNodeContentFields));
    node->setContentWidget(content);
    node->setSelected(true);
    node->setPlaybackActive(true);
    const auto body = node->shape().boundingRect();
    const auto bounds = node->boundingRect();
    const auto shape = node->shape();
    const auto render = [&]() {
        QImage image(body.width() + 24, body.height() + 24, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        scene.render(&painter, QRectF(image.rect()), body.adjusted(-12, -12, 12, 12));
        return image;
    };
    const auto before = render();
    node->setDropFeedback(NodeItem::DropFeedback::Valid);
    const auto during = render();
    EXPECT_NE(before, during);
    EXPECT_EQ(node->dropFeedback(), NodeItem::DropFeedback::Valid);
    EXPECT_EQ(node->boundingRect(), bounds);
    EXPECT_EQ(node->shape(), shape);
    EXPECT_TRUE(node->isSelected());
    EXPECT_TRUE(node->playbackActive());
    EXPECT_TRUE(content->isEnabled());
    const int centerY = 12 + body.height() * 0.5;
    EXPECT_EQ(during.pixelColor(12, centerY), ThemeManager::instance().colors().accent);
    EXPECT_EQ(during.pixelColor(15, centerY), ThemeManager::instance().colors().success);
    for (int x = 12; x < 18; ++x)
        EXPECT_EQ(before.pixelColor(x, centerY), during.pixelColor(x, centerY));
    const QRect contentPixels(22, 22, body.width() - 20, body.height() - 20);
    EXPECT_NE(before.copy(contentPixels), during.copy(contentPixels));
    bool foundOverlay = false;
    for (auto* child : node->childItems()) {
        if (!dynamic_cast<QGraphicsRectItem*>(child)) continue;
        foundOverlay = true;
        EXPECT_TRUE(child->isVisible());
        EXPECT_EQ(child->acceptedMouseButtons(), Qt::NoButton);
        EXPECT_FALSE(child->acceptHoverEvents());
        EXPECT_TRUE(body.contains(child->boundingRect()));
        node->setFocusDimmed(true);
        EXPECT_DOUBLE_EQ(child->effectiveOpacity(), 1.0);
        node->setFocusDimmed(false);
    }
    EXPECT_TRUE(foundOverlay);
    node->setDropFeedback(NodeItem::DropFeedback::None);
    EXPECT_EQ(render(), before);
}

TEST_F(BlueprintNodeGraphVisualTest, DropOverlayPreservesPinHitPriorityAtInsetOverlapInEveryDirection)
{
    QGraphicsScene scene;
    auto* node = new NodeItem(QStringLiteral("Destination"));
    scene.addItem(node);
    node->addInputPin();
    node->addOutputPin();
    for (const auto direction : {CanvasFlowDirection::LR, CanvasFlowDirection::RL,
                                CanvasFlowDirection::TB, CanvasFlowDirection::BT}) {
        node->setCanvasFlowDirection(direction);
        const bool vertical = direction == CanvasFlowDirection::TB || direction == CanvasFlowDirection::BT;
        const auto body = node->shape().boundingRect();
        for (const auto feedback : {NodeItem::DropFeedback::Valid, NodeItem::DropFeedback::Invalid}) {
            node->setDropFeedback(feedback);
            for (auto* pin : {node->inputPin(), node->outputPin()}) {
                const auto center = pin->scenePos();
                const auto position = vertical
                    ? center + QPointF(0.0, center.y() < body.center().y() ? 7.0 : -7.0)
                    : center + QPointF(center.x() < body.center().x() ? 7.0 : -7.0, 0.0);
                const auto hits = scene.items(position, Qt::IntersectsItemShape, Qt::DescendingOrder);
                ASSERT_FALSE(hits.isEmpty());
                EXPECT_EQ(hits.front(), pin);
            }
        }
    }
}

TEST_F(BlueprintNodeGraphVisualTest, DropOverlayPreservesValidAndInvalidDropZoneStyle)
{
    QGraphicsScene scene;
    auto* node = new NodeItem(QStringLiteral("Destination"));
    scene.addItem(node);
    const auto body = node->shape().boundingRect();
    const auto render = [&](NodeItem::DropFeedback feedback) {
        node->setDropFeedback(feedback);
        QImage image(body.size().toSize(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        scene.render(&painter, QRectF(image.rect()), body);
        return image;
    };
    const auto before = render(NodeItem::DropFeedback::None);
    const auto valid = render(NodeItem::DropFeedback::Valid);
    const auto invalid = render(NodeItem::DropFeedback::Invalid);
    EXPECT_NE(before, valid);
    EXPECT_NE(valid, invalid);
    const QRect contentPixels(10, 10, body.width() - 20, body.height() - 20);
    EXPECT_NE(before.copy(contentPixels), valid.copy(contentPixels));
    EXPECT_NE(before.copy(contentPixels), invalid.copy(contentPixels));
    int accentPixels = 0;
    int errorPixels = 0;
    for (int y = 6; y < invalid.height() - 6; ++y) {
        for (int x = 6; x < invalid.width() - 6; ++x) {
            if (valid.pixelColor(x, y) == ThemeManager::instance().colors().accent) ++accentPixels;
            if (invalid.pixelColor(x, y) == ThemeManager::instance().colors().error) ++errorPixels;
        }
    }
    EXPECT_GT(accentPixels, 20);
    EXPECT_GT(errorPixels, 20);
}

TEST_F(BlueprintNodeGraphVisualTest, ValidPinTargetUsesAccentAndPreservesPlaybackIndicator)
{
    NodePin pin(NodePin::Output);
    pin.setConnectState(NodePin::ConnectState::Ok);
    const auto render = [&]() {
        QImage image(32, 32, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.translate(10, 10);
        QStyleOptionGraphicsItem option;
        pin.paint(&painter, &option, nullptr);
        return image;
    };
    const auto valid = render();
    pin.setPlaybackActive(true);
    const auto playing = render();
    int accentBefore = 0;
    int playbackBefore = 0;
    int playbackDuring = 0;
    for (int y = 0; y < valid.height(); ++y) {
        for (int x = 0; x < valid.width(); ++x) {
            const auto before = valid.pixelColor(x, y);
            const auto during = playing.pixelColor(x, y);
            if (before == ThemeManager::instance().colors().accent) ++accentBefore;
            if (before == ThemeManager::instance().colors().success) ++playbackBefore;
            if (during == ThemeManager::instance().colors().success) ++playbackDuring;
        }
    }
    EXPECT_GT(accentBefore, 0);
    EXPECT_EQ(playbackBefore, 0);
    EXPECT_GT(playbackDuring, 0);
}

TEST_F(BlueprintNodeGraphVisualTest, PinPreviewFillsOnlyValidSocketsAndRestoresExistingConnectionColor)
{
    NodePin source(NodePin::Output);
    NodePin target(NodePin::Input);
    const auto render = [&]() {
        QImage image(32, 32, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.translate(10, 10);
        QStyleOptionGraphicsItem option;
        target.paint(&painter, &option, nullptr);
        return image;
    };
    const auto empty = render();
    target.setConnectState(NodePin::ConnectState::Ok);
    EXPECT_EQ(render().pixelColor(17, 16), ConnectionPath::defaultColor());
    target.setConnectState(NodePin::ConnectState::Bad);
    EXPECT_EQ(render().pixelColor(17, 16), empty.pixelColor(17, 16));
    target.setConnectState(NodePin::ConnectState::None);
    EXPECT_EQ(render(), empty);

    ConnectionPath edge(&source, &target);
    edge.setPlaybackActive(true);
    EXPECT_EQ(render().pixelColor(17, 16), ThemeManager::instance().colors().success);
    target.setConnectState(NodePin::ConnectState::Ok);
    EXPECT_EQ(render().pixelColor(17, 16), ConnectionPath::defaultColor());
    target.setConnectState(NodePin::ConnectState::None);
    EXPECT_EQ(render().pixelColor(17, 16), ThemeManager::instance().colors().success);
}

TEST_F(BlueprintNodeGraphVisualTest, PinTargetFeedbackStaysOpaqueWhileItsNodeRemainsDimmed)
{
    NodeItem node(QStringLiteral("Target"));
    node.addInputPin();
    node.addOutputPin();
    node.setFocusDimmed(true);
    for (const auto direction : {CanvasFlowDirection::LR, CanvasFlowDirection::RL,
                                CanvasFlowDirection::TB, CanvasFlowDirection::BT}) {
        node.setCanvasFlowDirection(direction);
        for (auto* pin : {node.inputPin(), node.outputPin()}) {
            const auto shape = pin->shape();
            for (const auto state : {NodePin::ConnectState::Ok, NodePin::ConnectState::Bad}) {
                pin->setConnectState(state);
                EXPECT_DOUBLE_EQ(pin->effectiveOpacity(), 1.0);
                EXPECT_DOUBLE_EQ(node.opacity(), 0.35);
                EXPECT_EQ(pin->shape(), shape);
            }
            pin->setConnectState(NodePin::ConnectState::None);
            EXPECT_DOUBLE_EQ(pin->effectiveOpacity(), 0.35);
        }
    }
}

TEST_F(BlueprintNodeGraphVisualTest, NormalPinsUseHeaderMidpointAndKeepVerticalColumnSpacing)
{
    NodeItem node(QStringLiteral("Source"));
    node.addInputPin();
    node.addInputPin();
    node.addOutputPin();
    node.addOutputPin();
    for (const auto direction : {CanvasFlowDirection::LR, CanvasFlowDirection::RL}) {
        node.setCanvasFlowDirection(direction);
        EXPECT_DOUBLE_EQ(node.inputPin()->scenePos().y(), 17.0);
        EXPECT_DOUBLE_EQ(node.outputPin()->scenePos().y(), 17.0);
        EXPECT_DOUBLE_EQ(node.inputPin(1)->scenePos().y(), 49.0);
        EXPECT_DOUBLE_EQ(node.outputPin(1)->scenePos().y(), 49.0);
    }
    for (int i = 2; i < 5; ++i) node.addInputPin();
    for (const auto direction : {CanvasFlowDirection::TB, CanvasFlowDirection::BT}) {
        node.setCanvasFlowDirection(direction);
        const auto size = node.shape().boundingRect().size();
        EXPECT_DOUBLE_EQ(size.width(), 186.0);
        EXPECT_DOUBLE_EQ(node.inputPin()->scenePos().x(), 29.0);
        EXPECT_DOUBLE_EQ(node.inputPin(4)->scenePos().x(), 157.0);
    }
}

TEST_F(BlueprintNodeGraphVisualTest, RouteGainUsesTwoEqualWidthRowsAndCentersItsPortsInEveryDirection)
{
    NodeItem node(QStringLiteral("Output"));
    node.setContentInfo({.outputVolume = -6.0f});
    auto* editor = new NodeContentEditor(node.contentInfo(), NodeContentField::OutputVolume,
        NodeContentEditor::labelWidth(NodeContentField::OutputVolume));
    node.setContentWidget(editor);
    node.addInputPin();
    node.addOutputPin();
    auto* slider = editor->findChild<ParameterControlHost*>(QStringLiteral("nodeSlider_outputBusVolume"));
    auto* value = editor->findChild<QLabel*>(QStringLiteral("nodeValue_outputBusVolume"));
    ASSERT_NE(slider, nullptr);
    ASSERT_NE(value, nullptr);
    node.setRouteGainMode(true);
    EXPECT_TRUE(node.routeGainMode());
    EXPECT_TRUE(editor->routeGainMode());
    for (const auto direction : {CanvasFlowDirection::LR, CanvasFlowDirection::RL,
                                CanvasFlowDirection::TB, CanvasFlowDirection::BT}) {
        node.setCanvasFlowDirection(direction);
        const auto size = node.shape().boundingRect().size();
        const bool vertical = direction == CanvasFlowDirection::TB || direction == CanvasFlowDirection::BT;
        EXPECT_EQ(node.inputPin()->scenePos(),
            NodeItem::pinCenter(NodePin::Input, 0, 1, size, direction, true));
        EXPECT_EQ(node.outputPin()->scenePos(),
            NodeItem::pinCenter(NodePin::Output, 0, 1, size, direction, true));
        EXPECT_DOUBLE_EQ(vertical ? node.inputPin()->scenePos().x() : node.inputPin()->scenePos().y(),
            vertical ? size.width() * 0.5 : size.height() * 0.5);
        EXPECT_TRUE(node.shape().contains(vertical ? QPointF(size.width() * 0.5, 1.0)
                                                   : QPointF(1.0, size.height() * 0.5)));
        EXPECT_TRUE(node.shape().contains(QPointF(1.0, 1.0)));
    }
    node.setRouteGainMode(false);
    EXPECT_FALSE(editor->routeGainMode());
}

TEST_F(BlueprintNodeGraphVisualTest, ProxyBackedRowsResolveTheFieldUnderThePointer)
{
    NodeItem node(QStringLiteral("Source"));
    node.setContentInfo({.volume = -3.0f, .pitch = 2.0f});
    auto* editor = new NodeContentEditor(node.contentInfo(), node.contentInfo().fields(),
        NodeContentEditor::labelWidth(node.contentInfo().fields()));
    node.setContentWidget(editor);
    ASSERT_NE(editor->graphicsProxyWidget(), nullptr);
    editor->layout()->activate();

    const auto at = [&](const QWidget* widget) {
        const QPointF local = node.mapFromItem(editor->graphicsProxyWidget(),
                                                QPointF(widget->geometry().center()));
        return node.contentFieldAt(local);
    };
    QLabel* volume = nullptr;
    QLabel* pitch = nullptr;
    for (auto* label : editor->findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (label->text() == QLatin1String("Volume"))
            volume = label;
        if (label->text() == QLatin1String("Pitch"))
            pitch = label;
    }
    ASSERT_NE(volume, nullptr);
    ASSERT_NE(pitch, nullptr);
    ASSERT_NE(volume->geometry().top(), pitch->geometry().top());
    EXPECT_EQ(at(volume), NodeContentField::Volume);
    EXPECT_EQ(at(pitch), NodeContentField::Pitch);
    auto* slider = editor->findChild<ParameterControlHost*>(QStringLiteral("nodeSlider_volume"));
    ASSERT_NE(slider, nullptr);
    EXPECT_EQ(at(slider), NodeContentField::Volume);

    const auto topic = ContextTopic::forField(ComponentType::MixNode, QStringLiteral("volume"),
        QStringLiteral("Volume"), QUuid(), ProjectItem::Type::Sound, QStringLiteral("Source"));
    editor->setFieldTopic(NodeContentField::Volume, topic);
    EXPECT_EQ(ContextInfo::tagOf(slider), topic);
    EXPECT_EQ(ContextInfo::tagOf(volume), topic);

    NodeItem gain(QStringLiteral("Gain"));
    gain.setRouteGainMode(true);
    gain.setContentInfo({.outputVolume = -6.0f});
    auto* gainEditor = new NodeContentEditor(gain.contentInfo(), NodeContentField::OutputVolume,
        NodeContentEditor::labelWidth(NodeContentField::OutputVolume));
    gain.setContentWidget(gainEditor);
    gainEditor->layout()->activate();
    auto* gainSlider = gainEditor->findChild<ParameterControlHost*>(
        QStringLiteral("nodeSlider_outputBusVolume"));
    auto* gainValue = gainEditor->findChild<QLabel*>(QStringLiteral("nodeValue_outputBusVolume"));
    ASSERT_NE(gainSlider, nullptr);
    ASSERT_NE(gainValue, nullptr);
    const auto gainAt = [&](const QWidget* widget) {
        const QPointF local = gain.mapFromItem(gainEditor->graphicsProxyWidget(),
                                                QPointF(widget->geometry().center()));
        return gain.contentFieldAt(local);
    };
    EXPECT_EQ(gainAt(gainSlider), NodeContentField::OutputVolume);
    EXPECT_EQ(gainAt(gainValue), NodeContentField::OutputVolume);
}

TEST_F(BlueprintNodeGraphVisualTest, RouteGainLabelsKeepRouteMeaningInTipsAndValuesEditOnDoubleClick)
{
    for (const auto field : {NodeContentField::OutputVolume, NodeContentField::SendVolume}) {
        SCOPED_TRACE(static_cast<int>(field));
        const bool send = field == NodeContentField::SendVolume;
        NodeContentInfo info;
        if (send) info.sendVolume = 0.5f;
        else info.outputVolume = -6.0f;
        NodeContentEditor editor(info, field, NodeContentEditor::labelWidth(field));
        editor.setRouteGainMode(true);
        const auto labels = editor.findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly);
        ASSERT_EQ(labels.size(), 1);
        const QString suffix = send ? QStringLiteral("sendLevel") : QStringLiteral("outputBusVolume");
        auto* value = editor.findChild<QLabel*>(QStringLiteral("nodeValue_") + suffix);
        auto* input = editor.findChild<QDoubleSpinBox*>(QStringLiteral("nodeEditor_") + suffix);
        auto* slider = editor.findChild<ParameterControlHost*>(QStringLiteral("nodeSlider_") + suffix);
        ASSERT_NE(value, nullptr);
        ASSERT_NE(input, nullptr);
        ASSERT_NE(slider, nullptr);
        auto* stack = qobject_cast<QStackedWidget*>(value->parentWidget());
        ASSERT_NE(stack, nullptr);
        EXPECT_EQ(stack->currentWidget(), value);
        EXPECT_EQ(value->text(), QStringLiteral("-6.0"));
        EXPECT_EQ(value->toolTip(), QStringLiteral("-6.0 dB"));
        EXPECT_TRUE(input->suffix().isEmpty());
        auto* range = slider->findChild<QSlider*>();
        ASSERT_NE(range, nullptr);
        EXPECT_EQ(range->cursor().shape(), Qt::SizeHorCursor);
        QMouseEvent doubleClick(QEvent::MouseButtonDblClick, QPointF(2, 2), QPointF(2, 2),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(value, &doubleClick);
        EXPECT_EQ(stack->currentWidget(), input);
        EXPECT_TRUE(editor.isEditing());
        editor.finishEditing(false);
        EXPECT_EQ(stack->currentWidget(), value);
        EXPECT_FALSE(editor.isEditing());
        editor.setRouteGainMode(false);
        EXPECT_EQ(labels.front()->text(), labels.front()->toolTip());
    }
}

TEST_F(BlueprintNodeGraphVisualTest, SendVolumeConvertsDisplayDecibelsToLinearAuthoringValues)
{
    NodeContentEditor editor({.sendVolume = 1.0f}, NodeContentField::SendVolume,
        NodeContentEditor::labelWidth(NodeContentField::SendVolume));
    auto* binding = editor.binding(NodeContentField::SendVolume);
    ASSERT_NE(binding, nullptr);
    EXPECT_DOUBLE_EQ(binding->value(), 0.0);
    QVariant preview;
    QVariant committed;
    bool editCommitted = false;
    QObject::connect(&editor, &NodeContentEditor::editPreviewed, &editor,
        [&](NodeContentField field, const QVariant& value) {
            EXPECT_EQ(field, NodeContentField::SendVolume);
            preview = value;
        });
    QObject::connect(&editor, &NodeContentEditor::valueCommitted, &editor,
        [&](NodeContentField field, const QVariant& value) {
            EXPECT_EQ(field, NodeContentField::SendVolume);
            committed = value;
        });
    QObject::connect(&editor, &NodeContentEditor::editEnded, &editor,
        [&](NodeContentField field, bool success) {
            EXPECT_EQ(field, NodeContentField::SendVolume);
            editCommitted = success;
        });
    binding->beginEdit();
    binding->previewEdit(-12.0);
    ASSERT_TRUE(preview.isValid());
    EXPECT_NEAR(preview.toDouble(), std::pow(10.0, -12.0 / 20.0), 0.000001);
    binding->commitEdit();
    EXPECT_TRUE(editCommitted);
    binding->commitValue(-3.0);
    ASSERT_TRUE(committed.isValid());
    EXPECT_NEAR(committed.toDouble(), std::pow(10.0, -3.0 / 20.0), 0.000001);
    editor.setValues({.sendVolume = 0.0f});
    EXPECT_DOUBLE_EQ(binding->value(), -96.0);
    EXPECT_EQ(editor.findChild<QLabel*>(QStringLiteral("nodeValue_sendLevel"))->text(), QStringLiteral("-inf"));

    NodeContentEditor output({.outputVolume = 0.0f}, NodeContentField::OutputVolume,
        NodeContentEditor::labelWidth(NodeContentField::OutputVolume));
    QObject::connect(&output, &NodeContentEditor::valueCommitted, &output,
        [&](NodeContentField field, const QVariant& value) {
            EXPECT_EQ(field, NodeContentField::OutputVolume);
            committed = value;
        });
    output.binding(NodeContentField::OutputVolume)->commitValue(-3.0);
    EXPECT_DOUBLE_EQ(committed.toDouble(), -3.0);
}

TEST_F(BlueprintNodeGraphVisualTest, RouteGainRendersSeparateHardStateBordersWithoutObjectHeaderOrGlow)
{
    NodeItem node(QStringLiteral("Hidden gain header"));
    node.setRouteGainMode(true);
    node.setSelected(true);
    node.setPlaybackActive(true);
    const auto render = [&]() {
        QImage image(180, 52, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.translate(10, 10);
        QStyleOptionGraphicsItem option;
        node.paint(&painter, &option, nullptr);
        return image;
    };
    node.setItemType(ProjectItem::Type::Sound);
    const auto sound = render();
    node.setItemType(ProjectItem::Type::Bus);
    const auto bus = render();
    EXPECT_EQ(sound, bus);
    EXPECT_EQ(bus.pixelColor(90, 10), ThemeManager::instance().colors().accent);
    EXPECT_EQ(bus.pixelColor(90, 13), ThemeManager::instance().colors().success);
    EXPECT_EQ(bus.pixelColor(90, 7).alpha(), 0);
    EXPECT_GT(bus.pixelColor(11, 11).alpha(), 0);
}

}
