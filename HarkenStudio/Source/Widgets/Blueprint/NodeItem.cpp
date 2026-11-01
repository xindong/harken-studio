
#include "NodeItem.h"
#include "NodeContentEditor.h"
#include "ConnectionPath.h"
#include "Core/FontManager.h"
#include "Core/Components/VMixNode.h"
#include "Core/ItemTypeIcons.h"
#include "Core/NumericFormat.h"
#include "Core/ThemeManager.h"
#include <QObject>
#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsProxyWidget>
#include <QGraphicsSceneHoverEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <algorithm>
#include <bit>
#include <cmath>

namespace VNX {

static constexpr float kHeaderHeight = 34.0f;
static constexpr float kBodyPadTop = 10.0f;
static constexpr float kBodyPadBot = 12.0f;
static constexpr float kBodyPadX = 12.0f;
static constexpr float kRowHeight = 21.0f;
static constexpr float kWaveformHeight = 22.0f;
static constexpr float kSubtitleRowHeight = 14.0f;
static constexpr float kBlueprintWaveformHeight = 26.0f;
static constexpr float kWaveformGap = 8.0f;
static constexpr float kRouteGainPadY = 6.0f;
static constexpr float kRouteGainMinimumHeight = 32.0f;

static constexpr float kPinHeadRowY = kHeaderHeight * 0.5f;
static constexpr float kPinColumnPad = 29.0f;
static constexpr float kPortPairSpacing = 32.0f;

namespace {

class NodeContentProxy final : public QGraphicsProxyWidget
{
public:
    explicit NodeContentProxy(NodeItem* node) : QGraphicsProxyWidget(node), node_(node) {}

protected:
    void focusInEvent(QFocusEvent* event) override
    {
        node_->ensureContentWidget();
        QGraphicsProxyWidget::focusInEvent(event);
    }

private:
    NodeItem* node_;
};

class NodeDropOverlay final : public QGraphicsRectItem
{
public:
    explicit NodeDropOverlay(NodeItem* node)
        : QGraphicsRectItem(node), node_(node)
    {
        setAcceptedMouseButtons(Qt::NoButton);
        setAcceptHoverEvents(false);
        setFlag(ItemIgnoresParentOpacity);
        setZValue(2.5);
        hide();
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override
    {
        const auto theme = ThemeManager::instance().snapshot();
        const QColor accent = node_->dropFeedback() == NodeItem::DropFeedback::Invalid
            ? theme->ui.colors.error : theme->ui.colors.accent;
        QColor veil = theme->domains.nodeGraphNode.bodyNormal;
        veil.setAlphaF(veil.alphaF() * 230 / 255.0);
        painter->fillRect(rect(), veil);
        QColor tint = accent;
        tint.setAlphaF(tint.alphaF() * 20 / 255.0);
        painter->setBrush(tint);
        painter->setPen(QPen(accent, 1.0, Qt::DashLine, Qt::SquareCap, Qt::MiterJoin));
        const auto zone = rect().adjusted(2.0, 2.0, -2.0, -2.0);
        painter->drawRect(zone);
    }

private:
    NodeItem* node_;
};

}

NodePin::NodePin(Type type, QGraphicsItem* parent)
    : QGraphicsItem(parent)
    , type_(type)
{
    setFlag(ItemSendsScenePositionChanges);
    setAcceptHoverEvents(true);
    setZValue(3.0);
    setDomain(Domain::Audio);
}

NodePin::~NodePin()
{
    for (ConnectionPath* conn : connections_) {
        if (conn) conn->notifyPinDestroyed(this);
    }
    connections_.clear();
}

void NodePin::addConnection(ConnectionPath* conn)
{
    connections_.append(conn);
    update();
}

void NodePin::removeConnection(ConnectionPath* conn)
{
    connections_.removeAll(conn);
    update();
}

QPointF NodePin::scenePos() const
{
    return mapToScene(QPointF(6.0, 6.0));
}

namespace {
QColor pinDomainColor(NodePin::Domain d)
{
    const auto theme = ThemeManager::instance().snapshot();
    const auto& pd = theme->domains.pinDomain;
    switch (d) {
    case NodePin::Domain::Audio:   return pd.audio;
    case NodePin::Domain::Control: return pd.control;
    case NodePin::Domain::Value:   return pd.value;
    }
    return pd.audio;
}
}

void NodePin::setDomain(Domain domain)
{
    domain_ = domain;
    updateToolTip_();
    update();
}

void NodePin::updateToolTip_()
{
    const QString dir = (type_ == Input) ? QObject::tr("Input") : QObject::tr("Output");
    QString label;
    switch (domain_) {
    case Domain::Audio:   label = QObject::tr("Audio · Sample-level"); break;
    case Domain::Control: label = QObject::tr("Control flow"); break;
    case Domain::Value:   label = QObject::tr("Value · Variable-level"); break;
    }
    QString text = QStringLiteral("%1 · %2").arg(label, dir);
    if (chainVisibility_ && chainToggleEnabled_) {
        text += QLatin1Char('\n');
        text += *chainVisibility_ == Qt::Checked ? QObject::tr("Click to collapse inputs; drag to connect")
                                                : QObject::tr("Click to expand inputs; drag to connect");
        text += QLatin1Char('\n');
        text += QObject::tr("Hold Ctrl to apply recursively to all upstream inputs");
    }
    setToolTip(text);
}

void NodePin::setChainVisibility(std::optional<Qt::CheckState> state, bool toggleEnabled)
{
    if (chainVisibility_ == state && chainToggleEnabled_ == toggleEnabled) return;
    chainVisibility_ = state;
    chainToggleEnabled_ = toggleEnabled;
    updateToolTip_();
    update();
}

void NodePin::setConnectionDragging(bool dragging)
{
    if (connectionDragging_ == dragging) return;
    connectionDragging_ = dragging;
    update();
}

void NodePin::setConnectState(ConnectState state)
{
    if (connectState_ == state) return;
    connectState_ = state;
    setFlag(ItemIgnoresParentOpacity, state != ConnectState::None);
    update();
}

void NodePin::hoverEnterEvent(QGraphicsSceneHoverEvent* event)
{
    hovered_ = true;
    update();
    QGraphicsItem::hoverEnterEvent(event);
}

void NodePin::hoverLeaveEvent(QGraphicsSceneHoverEvent* event)
{
    hovered_ = false;
    update();
    QGraphicsItem::hoverLeaveEvent(event);
}

QRectF NodePin::boundingRect() const
{
    return QRectF(-8, -8, 28, 28);
}

QPainterPath NodePin::shape() const
{
    QPainterPath p;
    p.addRect(QRectF(-2.0, -2.0, 16.0, 16.0));
    return p;
}

void NodePin::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget)
{
    Q_UNUSED(option)
    Q_UNUSED(widget)

    painter->setRenderHint(QPainter::Antialiasing, true);

    const auto theme = ThemeManager::instance().snapshot();
    const auto&   pd    = theme->domains.pinDomain;
    const QColor  fill  = playbackActive_ ? theme->ui.colors.success : pinDomainColor(domain_);

    QColor outline = fill;
    if (connectState_ == ConnectState::Ok) {
        outline = theme->ui.colors.accent;
    } else if (connectState_ == ConnectState::Bad) {
        outline = pd.targetBad;
    }

    qreal angle = type_ == Input ? 180.0 : 0.0;
    if (const auto* node = dynamic_cast<const NodeItem*>(parentItem())) {
        switch (node->canvasFlowDirection()) {
        case CanvasFlowDirection::LR: break;
        case CanvasFlowDirection::RL: angle += 180.0; break;
        case CanvasFlowDirection::TB: angle += 90.0; break;
        case CanvasFlowDirection::BT: angle -= 90.0; break;
        }
    }

    painter->save();
    painter->translate(6.0, 6.0);
    painter->rotate(angle);
    painter->translate(-2.0, 0.0);
    if (chainVisibility_ && (*chainVisibility_ != Qt::Checked || (hovered_ && chainToggleEnabled_))
        && !connectionDragging_ && connectState_ == ConnectState::None) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(pinDomainColor(domain_));
        painter->drawRect(QRectF(-4.0, -4.0, 8.0, 8.0));
        painter->rotate(-angle);
        painter->setPen(QPen(pd.ring, 1.25, Qt::SolidLine, Qt::SquareCap));
        if (*chainVisibility_ == Qt::PartiallyChecked) {
            painter->setBrush(Qt::NoBrush);
            painter->drawEllipse(QPointF(0.0, 0.0), 2.0, 2.0);
        } else {
            painter->drawLine(QPointF(-2.0, 0.0), QPointF(2.0, 0.0));
            if (*chainVisibility_ == Qt::Unchecked)
                painter->drawLine(QPointF(0.0, -2.0), QPointF(0.0, 2.0));
        }
        if (playbackActive_) {
            painter->setBrush(Qt::NoBrush);
            painter->setPen(QPen(fill, 1.0));
            painter->drawRect(QRectF(-4.5, -4.5, 9.0, 9.0));
        }
        painter->restore();
        return;
    }
    painter->fillRect(QRectF(-4.0, -4.0, 8.0, 8.0), pd.ring);
    QPainterPath socket;
    socket.moveTo(3.25, -3.25);
    socket.lineTo(-3.25, -3.25);
    socket.lineTo(-3.25, 3.25);
    socket.lineTo(3.25, 3.25);
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(outline, hovered_ || connectState_ != ConnectState::None ? 2.0 : 1.5,
        Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    painter->drawPath(socket);
    if (connectState_ == ConnectState::Ok) {
        painter->fillRect(QRectF(-1.5, -1.5, 5.5, 3.0), ConnectionPath::defaultColor());
    } else if (!connections_.isEmpty()) {
        const ConnectionPath* highlighted = connections_.front();
        for (const auto* connection : connections_) {
            if (connection->playbackActive()) {
                highlighted = connection;
                break;
            }
            if (connection->isSelected()) highlighted = connection;
        }
        painter->fillRect(QRectF(-1.5, -1.5, 5.5, 3.0), highlighted->displayColor());
    }
    if (playbackActive_) {
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(fill, 1.0));
        painter->drawRect(QRectF(-4.5, -4.5, 9.0, 9.0));
    }
    if (connectState_ != ConnectState::None) {
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(outline, 1.5, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        painter->drawRect(QRectF(-6.5, -6.5, 13.0, 13.0));
    }
    painter->restore();
}

QVariant NodePin::itemChange(GraphicsItemChange change, const QVariant& value)
{
    if (change == ItemScenePositionHasChanged)
    {
        for (ConnectionPath* conn : connections_)
        {
            conn->updatePosition();
        }
    }
    return QGraphicsItem::itemChange(change, value);
}

NodeItem::NodeItem(const QString& title, QGraphicsItem* parent)
    : QGraphicsItem(parent)
    , title_(title)
{
    setFlag(ItemIsMovable);
    setFlag(ItemIsSelectable);
    setFlag(ItemSendsGeometryChanges);
    setAcceptHoverEvents(true);
    dropOverlay_ = new NodeDropOverlay(this);
    updateContentHeight();
}

NodeItem::~NodeItem()
{
    if (auto* editor = qobject_cast<InlineRenameEdit*>(titleEditor())) {
        editor->blockSignals(true);
        editor->finish(false);
    }
}

QRectF NodeItem::titleRect() const
{
    if (routeGainMode_) return {};
    QFont font = FontManager::instance().getFont(FontRole::Caption, 10, QFont::Normal);
    font.setItalic(titleDirty_);
    const QFontMetricsF metrics(font);
    const qreal x = kBodyPadX + 20.0 + 9.0;
    const qreal available = width_ - x - kBodyPadX;
    const auto text = metrics.elidedText(displayTitle(), Qt::ElideMiddle, available);
    return {x, (kHeaderHeight - metrics.height()) * 0.5,
        metrics.horizontalAdvance(text), metrics.height()};
}

QRectF NodeItem::titleEditorRect() const
{
    const qreal x = kBodyPadX + 20.0 + 9.0;
    return {x - 3.0, 3.0, width_ - x - kBodyPadX + 6.0, kHeaderHeight - 6.0};
}

QLineEdit* NodeItem::titleEditor() const
{
    return titleProxy_ ? qobject_cast<QLineEdit*>(titleProxy_->widget()) : nullptr;
}

void NodeItem::setTitleEditFinishedCallback(TitleEditFinishedCallback callback)
{
    titleEditFinishedCallback_ = std::move(callback);
}

void NodeItem::setTitleRenameHandler(InlineRenameEdit::RenameHandler handler)
{
    titleRenameHandler_ = std::move(handler);
}

void NodeItem::beginTitleEditing()
{
    if (routeGainMode_ || titleEditing_ || !titleRenameHandler_) return;
    if (!titleProxy_) {
        auto* editor = new InlineRenameEdit(title_, titleRenameHandler_);
        editor->setObjectName(QStringLiteral("nodeTitleEditor"));
        editor->setFont(FontManager::instance().getFont(FontRole::Caption, 10, QFont::Normal));
        editor->setProperty("fieldRole", QStringLiteral("inspectorValue"));
        QObject::connect(editor, &InlineRenameEdit::finished, editor, [this, editor](bool committed) {
            titleEditing_ = false;
            titleProxy_->hide();
            update();
            if (titleEditFinishedCallback_) titleEditFinishedCallback_(editor->text(), committed);
        });
        titleProxy_ = new QGraphicsProxyWidget(this);
        titleProxy_->setWidget(editor);
        titleProxy_->setZValue(2.0);
    }
    titleEditing_ = true;
    titleProxy_->setGeometry(titleEditorRect());
    auto* editor = qobject_cast<InlineRenameEdit*>(titleEditor());
    titleProxy_->show();
    titleProxy_->setFocus(Qt::MouseFocusReason);
    editor->begin(title_);
    update();
}

void NodeItem::finishTitleEditing(bool committed)
{
    if (!titleEditing_) return;
    qobject_cast<InlineRenameEdit*>(titleEditor())->finish(committed);
}

void NodeItem::setRole(NodeRole r)
{
    if (role_ == r) return;
    role_ = r;
    updateContentHeight();
    repositionAllPins_();
    update();
}

void NodeItem::setDropFeedback(DropFeedback feedback)
{
    if (dropFeedback_ == feedback) return;
    dropFeedback_ = feedback;
    dropOverlay_->setVisible(feedback != DropFeedback::None);
    dropOverlay_->update();
}

void NodeItem::setRouteGainMode(bool enabled)
{
    if (routeGainMode_ == enabled) return;
    routeGainMode_ = enabled;
    if (auto* editor = qobject_cast<NodeContentEditor*>(contentWidget()))
        editor->setRouteGainMode(enabled);
    updateContentHeight();
    repositionAllPins_();
    update();
}

void NodeItem::addInputPin()
{
    NodePin* pin = new NodePin(NodePin::Input, this);
    inputs_.append(pin);
    updateContentHeight();
    repositionAllPins_();
}

void NodeItem::addOutputPin()
{
    NodePin* pin = new NodePin(NodePin::Output, this);
    outputs_.append(pin);
    updateContentHeight();
    repositionAllPins_();
}

QPointF NodeItem::pinCenter(NodePin::Type type, int index, int portPairCount,
                            const QSizeF& size, CanvasFlowDirection direction,
                            bool routeGainMode)
{
    const bool isInput = (type == NodePin::Input);
    const qreal rowY = routeGainMode ? size.height() * 0.5
        : kPinHeadRowY + index * kPortPairSpacing;
    const qreal columnX = (size.width() - (portPairCount - 1) * kPortPairSpacing) / 2.0
        + index * kPortPairSpacing;
    switch (direction) {
    case CanvasFlowDirection::LR:
        return QPointF(isInput ? 0.0 : size.width(), rowY);
    case CanvasFlowDirection::RL:
        return QPointF(isInput ? size.width() : 0.0, rowY);
    case CanvasFlowDirection::TB:
        return QPointF(columnX, isInput ? 0.0 : size.height());
    case CanvasFlowDirection::BT:
        return QPointF(columnX, isInput ? size.height() : 0.0);
    }
    return {};
}

void NodeItem::repositionAllPins_()
{
    if (titleProxy_) titleProxy_->setGeometry(titleEditorRect());
    const int count = std::max(inputs_.size(), outputs_.size());
    const QSizeF size(width_, height_);
    for (int index = 0; index < inputs_.size(); ++index)
        inputs_[index]->setPos(pinCenter(NodePin::Input, index, count, size, flowDir_, routeGainMode_) - QPointF(6, 6));
    for (int index = 0; index < outputs_.size(); ++index)
        outputs_[index]->setPos(pinCenter(NodePin::Output, index, count, size, flowDir_, routeGainMode_) - QPointF(6, 6));
}

void NodeItem::setCanvasFlowDirection(CanvasFlowDirection dir)
{
    if (flowDir_ == dir) return;
    flowDir_ = dir;
    updateContentHeight();
    repositionAllPins_();
    update();
}


void NodeItem::setContentInfo(const NodeContentInfo& info)
{
    if (contentInfo_ == info) return;
    contentInfo_ = info;
    if (auto* editor = qobject_cast<NodeContentEditor*>(contentWidget()))
        editor->setValues(info);
    else if (contentPresentationFactory_)
        contentPresentation_ = contentPresentationFactory_();
    updateContentHeight();
    repositionAllPins_();
    update();
}

void NodeItem::setContentWidget(QWidget* widget)
{
    if (contentProxy_) {
        auto* previous = contentProxy_->widget();
        contentProxy_->setWidget(nullptr);
        contentProxy_->setMinimumSize(0, 0);
        contentProxy_->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        delete previous;
    }
    if (widget) {
        if (!contentProxy_) contentProxy_ = new NodeContentProxy(this);
        contentProxy_->setWidget(widget);
        if (auto* editor = qobject_cast<NodeContentEditor*>(widget)) {
            editor->setRouteGainMode(routeGainMode_);
            QObject::connect(editor, &NodeContentEditor::contentSizeChanged, contentProxy_, [this] {
                updateContentHeight();
                repositionAllPins_();
                update();
            });
        }
    }
    updateContentHeight();
    repositionAllPins_();
    update();
}

QWidget* NodeItem::contentWidget() const
{
    return contentProxy_ ? contentProxy_->widget() : nullptr;
}

void NodeItem::setContentFactory(std::function<QWidget*()> factory,
    std::function<NodeContentPresentation()> presentation)
{
    contentFactory_ = std::move(factory);
    contentPresentationFactory_ = std::move(presentation);
    if (!contentProxy_) contentProxy_ = new NodeContentProxy(this);
    refreshContentPresentation();
}

QWidget* NodeItem::ensureContentWidget()
{
    if (!contentWidget() && contentFactory_) setContentWidget(contentFactory_());
    return contentWidget();
}

bool NodeItem::releaseContentWidget()
{
    auto* widget = contentWidget();
    if (!widget || !contentFactory_) return false;
    if (const auto* editor = qobject_cast<NodeContentEditor*>(widget); editor && editor->isEditing())
        return false;
    const auto owns = [widget](const QWidget* candidate) {
        return candidate && (candidate == widget || widget->isAncestorOf(candidate));
    };
    if (owns(QApplication::focusWidget()) || owns(QWidget::mouseGrabber())) return false;
    if (scene()) {
        auto* focused = scene()->focusItem();
        if (focused && (focused == contentProxy_ || contentProxy_->isAncestorOf(focused))) return false;
        auto* grabber = scene()->mouseGrabberItem();
        if (grabber && (grabber == contentProxy_ || contentProxy_->isAncestorOf(grabber))) return false;
    }
    contentPresentation_ = contentPresentationFactory_();
    setContentWidget(nullptr);
    return true;
}

void NodeItem::refreshContentPresentation()
{
    if (contentPresentationFactory_) contentPresentation_ = contentPresentationFactory_();
    updateContentHeight();
    repositionAllPins_();
    update();
}

void NodeItem::refreshContentAppearance()
{
    if (contentPresentationFactory_) contentPresentation_ = contentPresentationFactory_();
    update();
}

void NodeItem::setWaveformPeaks(const std::vector<float>& peaks)
{
    waveformPeaks_ = peaks;
    waveformPeakData_.reset();
    waveformRange_.reset();
    updateContentHeight();
    repositionAllPins_();
    update();
}

void NodeItem::setContentFields(NodeContentFields fields)
{
    if (contentFields_ == fields) return;
    contentFields_ = fields;
    if (auto* editor = qobject_cast<NodeContentEditor*>(contentWidget()))
        editor->setFields(fields);
    else if (contentPresentationFactory_)
        contentPresentation_ = contentPresentationFactory_();
    updateContentHeight();
    repositionAllPins_();
    update();
}

void NodeItem::setWaveformPeakData(std::shared_ptr<const WaveformPeakData> data, std::optional<WaveformFrameRange> range)
{
    if (waveformPeakData_ == data && waveformRange_ == range && waveformPeaks_.empty()) return;
    waveformPeakData_ = std::move(data);
    waveformRange_ = range;
    waveformPeaks_.clear();
    updateContentHeight();
    repositionAllPins_();
    update();
}

void NodeItem::updateContentHeight()
{
    prepareGeometryChange();
    if (routeGainMode_) {
        const auto* widget = contentWidget();
        const float contentWidth = widget ? widget->width() : contentPresentation_
            ? contentPresentation_->size.width() : kDefaultWidth - 2 * kBodyPadX;
        const float contentHeight = widget ? widget->height() : contentPresentation_
            ? contentPresentation_->size.height() : 20.0f;
        width_ = contentWidth + 2 * kBodyPadX;
        height_ = std::max(kRouteGainMinimumHeight, contentHeight + 2 * kRouteGainPadY);
        if (contentProxy_) {
            contentProxy_->setVisible(contentHeight > 0);
            contentProxy_->setPos(kBodyPadX, (height_ - contentHeight) * 0.5f);
            if (!widget) contentProxy_->resize(contentWidth, contentHeight);
        }
        dropOverlay_->setRect(QRectF(6.0, 6.0, width_ - 12.0, height_ - 12.0));
        return;
    }
    const int count = std::max(inputs_.size(), outputs_.size());
    const bool vertical = flowDir_ == CanvasFlowDirection::TB || flowDir_ == CanvasFlowDirection::BT;
    width_ = vertical ? std::max(kDefaultWidth, 2 * kPinColumnPad + (count - 1) * kPortPairSpacing) : kDefaultWidth;
    height_ = heightForContent(contentInfo_, !subtitle_.isEmpty(),
                               blueprintWaveformMode_ || (waveformPeakData_ && waveformPeakData_->isValid()) || !waveformPeaks_.empty(),
                               blueprintWaveformMode_, count, contentFields_);
    if (contentWidget() || contentPresentation_) {
        const QSize size = contentWidget() ? contentWidget()->size() : contentPresentation_->size;
        contentProxy_->setVisible(size.height() > 0);
        if (size.height() > 0)
            width_ = std::max(width_, static_cast<float>(size.width()) + 2 * kBodyPadX);
        const auto rows = std::popcount(static_cast<unsigned int>(
            (contentInfo_.fields() & contentFields_).toInt()));
        height_ += size.height() - rows * kRowHeight;
        contentProxy_->setPos(kBodyPadX, kHeaderHeight + kBodyPadTop
            + (subtitle_.isEmpty() ? 0.0f : kSubtitleRowHeight));
        if (!contentWidget()) contentProxy_->resize(size);
    }
    dropOverlay_->setRect(QRectF(6.0, 6.0, width_ - 12.0, height_ - 12.0));
}

float NodeItem::heightForContent(const NodeContentInfo& info, bool hasSubtitle,
                                 bool hasWaveform, bool blueprintWaveformMode, int portPairCount,
                                 NodeContentFields fields)
{
    const float subtitleRow = hasSubtitle ? kSubtitleRowHeight : 0.0f;
    const float portRows = std::max(0, portPairCount - 1) * kPortPairSpacing;
    float body = kBodyPadTop + subtitleRow
        + std::popcount(static_cast<unsigned int>((info.fields() & fields).toInt())) * kRowHeight;
    if (hasWaveform && fields.testFlag(NodeContentField::Waveform)) {
        const float waveRow = blueprintWaveformMode ? kBlueprintWaveformHeight : kWaveformHeight;
        body += waveRow + kWaveformGap;
    }
    body += kBodyPadBot;
    return std::max(60.0f + subtitleRow, kHeaderHeight + body) + portRows;
}

void NodeItem::setBlueprintWaveformMode(bool on)
{
    if (blueprintWaveformMode_ == on) return;
    blueprintWaveformMode_ = on;
    updateContentHeight();
    repositionAllPins_();
    update();
}

void NodeItem::setPositionChangedCallback(PositionChangedCallback callback)
{
    positionChangedCallback_ = std::move(callback);
}

void NodeItem::setDragCompleteCallback(DragCompleteCallback callback)
{
    dragCompleteCallback_ = std::move(callback);
}

void NodeItem::setSubtitle(const QString& s)
{
    if (subtitle_ == s) return;
    prepareGeometryChange();
    subtitle_ = s;
    updateContentHeight();
    repositionAllPins_();
    update();
}

QRectF NodeItem::boundingRect() const
{
    constexpr float kBadgeHalo = 13.0f;
    return QRectF(-kBadgeHalo, -kBadgeHalo, width_ + 2 * kBadgeHalo, height_ + 2 * kBadgeHalo);
}

QPainterPath NodeItem::shape() const
{
    if (routeGainMode_) return routeGainShape(0.0);
    QPainterPath path;
    path.addRect(QRectF(0.0, 0.0, width_, height_));
    return path;
}

QPainterPath NodeItem::routeGainShape(qreal inset) const
{
    QPainterPath path;
    path.addRect(QRectF(inset, inset, width_ - 2.0 * inset, height_ - 2.0 * inset));
    return path;
}

static QColor blendColors(const QColor& base, const QColor& tint, float ratio)
{
    return QColor(
        base.red()   + int((tint.red()   - base.red())   * ratio),
        base.green() + int((tint.green() - base.green()) * ratio),
        base.blue()  + int((tint.blue()  - base.blue())  * ratio),
        base.alpha() + int((tint.alpha() - base.alpha()) * ratio));
}

void NodeItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget)
{
    Q_UNUSED(widget)

    const auto theme = ThemeManager::instance().snapshot();
    const auto& ngn = theme->domains.nodeGraphNode;

    if (routeGainMode_) {
        QColor borderColor = ngn.borderNormal;
        if (isSelected()) borderColor = theme->ui.colors.accent;
        else if (hovered_) borderColor = blendColors(ngn.borderNormal, ngn.borderHoverMix, 0.35f);
        painter->setBrush(isSelected() ? ngn.bodySelected : ngn.bodyNormal);
        painter->setPen(QPen(borderColor, isSelected() ? 2.0 : 1.0,
            Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        painter->drawPath(routeGainShape(0.0));
        if (playbackActive_) {
            painter->setBrush(Qt::NoBrush);
            painter->setPen(QPen(theme->ui.colors.success, 2.0,
                Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
            painter->drawPath(routeGainShape(3.0));
        }
        paintContentBody(painter, kRouteGainPadY - kBodyPadTop);
        return;
    }

    const QColor shadowBase = ngn.shadow;
    for (int i = 3; i >= 1; --i) {
        QColor layer = shadowBase;
        layer.setAlpha(shadowBase.alpha() / (i + 1));
        const qreal pad = i * 2.0;
        painter->setPen(Qt::NoPen);
        painter->setBrush(layer);
        painter->drawRect(QRectF(2.0 - pad, 3.0 - pad,
                                 width_ + pad * 2.0, height_ + pad * 2.0));
    }

    QColor bodyColor = isSelected() ? ngn.bodySelected : ngn.bodyNormal;
    painter->setBrush(bodyColor);

    QColor borderColor = ngn.borderNormal;
    qreal  borderWidth = 1.0;
    if (isSelected()) {
        borderColor = theme->ui.colors.accent;
        borderWidth = 2.0;
    } else if (hovered_) {
        borderColor = blendColors(ngn.borderNormal, ngn.borderHoverMix, 0.35f);
    }
    QPen borderPen(borderColor);
    borderPen.setWidthF(borderWidth);
    borderPen.setJoinStyle(Qt::MiterJoin);
    painter->setPen(borderPen);
    painter->drawRect(QRectF(0, 0, width_, height_));

    painter->setBrush(theme->ui.colors.headerBackground);
    painter->setPen(Qt::NoPen);
    painter->drawRect(QRectF(1, 1, width_ - 2, kHeaderHeight));
    if (playbackActive_) {
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(theme->ui.colors.success, 2.0, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        painter->drawRect(QRectF(3, 3, width_ - 6, height_ - 6));
    }

    const float headPad  = kBodyPadX;
    const float iconSize = 20.0f;
    const QColor titleColor = titleColor_.isValid() ? titleColor_ : ngn.titleText;
    if (itemType_.has_value()) {
        const int iconY = static_cast<int>((kHeaderHeight - iconSize) * 0.5f) + 1;
        const QRect iconRect(static_cast<int>(headPad), iconY,
                             static_cast<int>(iconSize), static_cast<int>(iconSize));
        const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        const qreal effectiveScale = painter->worldTransform().m11() * dpr;
        const QPixmap iconPx = iconForItemType(*itemType_, titleColor)
                                   .pixmap(iconRect.size(), effectiveScale);
        if (!iconPx.isNull()) {
            const bool smooth = painter->testRenderHint(QPainter::SmoothPixmapTransform);
            painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter->drawPixmap(QRectF(iconRect), iconPx, QRectF(iconPx.rect()));
            painter->setRenderHint(QPainter::SmoothPixmapTransform, smooth);
        }
    }

    const float textX = headPad + iconSize + 9.0f;
    const float textW = width_ - textX - headPad;

    painter->setPen(titleColor);
    QFont nameFont = FontManager::instance().getFont(FontRole::Caption, 10, QFont::Normal);
    nameFont.setItalic(titleDirty_);
    painter->setFont(nameFont);
    QFontMetricsF nameFm(nameFont);
    const QString elidedName = nameFm.elidedText(displayTitle(), Qt::ElideMiddle, textW);
    if (!titleEditing_)
        painter->drawText(QRectF(textX, 0, textW, kHeaderHeight),
                          Qt::AlignVCenter | Qt::AlignLeft, elidedName);

    float bodyTop = kHeaderHeight;
    if (!subtitle_.isEmpty()) {
        const float padX = 8.0f;
        QFont subFont = FontManager::instance().getFont(FontRole::Caption, 7);
        painter->setFont(subFont);
        QFontMetricsF subFm(subFont);
        QString elided = subFm.elidedText(subtitle_, Qt::ElideMiddle, width_ - padX * 2);
        painter->setPen(theme->ui.colors.textSecondary);
        painter->drawText(QRectF(padX, bodyTop, width_ - padX * 2, kSubtitleRowHeight),
                          Qt::AlignVCenter | Qt::AlignLeft, elided);
        bodyTop += kSubtitleRowHeight;
    }

    paintContentBody(painter, bodyTop);

    if (sandboxBypassed_) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(ngn.sandboxBadgeBg);
        painter->drawEllipse(QPointF(width_ - 12, 12), 6, 6);

        painter->setPen(ngn.sandboxBadgeText);
        painter->setFont(FontManager::instance().getFont(FontRole::Caption, 7, QFont::Bold));
        painter->drawText(QRectF(width_ - 18, 6, 12, 12), Qt::AlignCenter, QStringLiteral("!"));
    }

    {
        QFont badgeFont = FontManager::instance().getFont(FontRole::Caption, 9, QFont::Bold);
        const QColor badgeText = ngn.sandboxBadgeText;
        const QPen badgeRim(theme->domains.blueprint.canvasBackground, 2.0);

        if (overrideBadge_) {
            const QRectF rect(width_ - 11.0, -11.0, 22.0, 22.0);
            painter->setPen(badgeRim);
            painter->setBrush(ngn.badgeOverride);
            painter->drawEllipse(rect);
            painter->setPen(badgeText);
            painter->setFont(badgeFont);
            painter->drawText(rect, Qt::AlignCenter, QString(QChar(0x26A1)));
        }

        const bool hasReceives = (receiveBadge_    > 0);

        if (hasReceives) {
            const QRectF rect(-11.0, -11.0, 22.0, 22.0);
            painter->setPen(badgeRim);
            painter->setBrush(ngn.badgeReceive);
            painter->drawEllipse(rect);
            painter->setPen(badgeText);
            painter->setFont(badgeFont);
            painter->drawText(rect, Qt::AlignCenter, QString::number(receiveBadge_));
        }
    }
}


std::optional<NodeContentField> NodeItem::contentFieldAt(const QPointF& localPos) const
{
    if (contentWidget()) {
        auto* editor = qobject_cast<NodeContentEditor*>(contentProxy_->widget());
        if (!editor)
            return std::nullopt;
        return editor->fieldAt(contentProxy_->mapFromItem(this, localPos).toPoint());
    }
    if (contentPresentation_) {
        const auto point = contentProxy_->mapFromItem(this, localPos).toPoint();
        for (const auto& [field, rect] : contentPresentation_->fields)
            if (rect.contains(point)) return field;
        return std::nullopt;
    }

    float bodyTop = routeGainMode_ ? (kRouteGainPadY - kBodyPadTop) : kHeaderHeight;
    if (!routeGainMode_ && !subtitle_.isEmpty())
        bodyTop += kSubtitleRowHeight;
    float y = bodyTop + kBodyPadTop;
    const auto fields = contentFields_ & contentInfo_.fields();
    const NodeContentField order[] = {
        NodeContentField::Volume,
        NodeContentField::Pitch,
        NodeContentField::OffsetOfStart,
        NodeContentField::Loop,
        NodeContentField::OutputVolume,
        NodeContentField::SendVolume,
    };
    for (const NodeContentField field : order) {
        if (!fields.testFlag(field))
            continue;
        if (localPos.y() >= y && localPos.y() < y + kRowHeight)
            return field;
        y += kRowHeight;
    }
    return std::nullopt;
}

void NodeItem::paintContentBody(QPainter* painter, float bodyTop) const
{
    const float padLeft = kBodyPadX;
    const float padRight = kBodyPadX;
    const float valueX = width_ - padRight;
    float y = bodyTop + kBodyPadTop;

    QFont labelFont = FontManager::instance().getFont(FontRole::Label, 9);
    QFont valueFont = FontManager::instance().getFont(FontRole::Label, 9, QFont::DemiBold);
    const auto theme = ThemeManager::instance().snapshot();
    const auto& ngnContent = theme->domains.nodeGraphNode;
    const QColor labelColor = ngnContent.contentLabel;
    const QColor valueColor = ngnContent.contentValue;

    auto drawParamRow = [&](const QString& label, const QString& value) {
        const qreal valueWidth = std::min(QFontMetricsF(valueFont).horizontalAdvance(value),
            static_cast<qreal>(width_ - padLeft - padRight));
        const qreal labelWidth = std::max(0.0, valueX - valueWidth - padLeft - 8.0);
        painter->setFont(labelFont);
        painter->setPen(labelColor);
        painter->drawText(QRectF(padLeft, y, labelWidth, kRowHeight),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetricsF(labelFont).elidedText(label, Qt::ElideRight, labelWidth));

        painter->setFont(valueFont);
        painter->setPen(valueColor);
        painter->drawText(QRectF(valueX - valueWidth, y, valueWidth, kRowHeight),
                          Qt::AlignRight | Qt::AlignVCenter,
                          QFontMetricsF(valueFont).elidedText(value, Qt::ElideRight, valueWidth));
        y += kRowHeight;
    };

    const auto fields = contentFields_ & contentInfo_.fields();
    if (contentWidget()) {
        y += contentProxy_->widget()->height();
    } else if (contentPresentation_) {
        painter->save();
        painter->drawPicture(contentProxy_->pos(), contentPresentation_->drawing);
        painter->restore();
        y += contentPresentation_->size.height();
    } else {
        const auto decibels = [](float value) {
            return value <= -96.0f ? QStringLiteral("-inf") : formatDecimal(value, 1);
        };
        if (fields.testFlag(NodeContentField::Volume))
            drawParamRow(QObject::tr("Volume"), decibels(*contentInfo_.volume));
        if (fields.testFlag(NodeContentField::Pitch))
            drawParamRow(QObject::tr("Pitch"), formatDecimal(*contentInfo_.pitch, 0));
        if (fields.testFlag(NodeContentField::OffsetOfStart))
            drawParamRow(QObject::tr("Offset of Start"), formatDecimal(*contentInfo_.offsetOfStart, 3));
        if (fields.testFlag(NodeContentField::Loop)) {
            const auto count = *contentInfo_.loopCount;
            drawParamRow(QObject::tr("Loop"), count == 0 ? QString(QChar(0x221E)) : QString::number(count));
        }
        if (fields.testFlag(NodeContentField::OutputVolume))
            drawParamRow(routeGainMode_ ? QObject::tr("Volume") : QObject::tr("Output Volume"),
                decibels(*contentInfo_.outputVolume));
        if (fields.testFlag(NodeContentField::SendVolume))
            drawParamRow(routeGainMode_ ? QObject::tr("Volume") : QObject::tr("Send Volume"),
                decibels(VMixNode::linearToDb(*contentInfo_.sendVolume)));
    }

    if (contentFields_.testFlag(NodeContentField::Waveform)
        && (blueprintWaveformMode_ || (waveformPeakData_ && waveformPeakData_->isValid()) || !waveformPeaks_.empty())) {
        y += kWaveformGap;
        const float waveH = blueprintWaveformMode_ ? kBlueprintWaveformHeight : kWaveformHeight;
        QRectF waveRect(padLeft, y, width_ - padLeft - padRight, waveH);
        paintWaveform(painter, waveRect);
    }
}

void NodeItem::paintWaveform(QPainter* painter, const QRectF& rect) const
{
    const auto theme = ThemeManager::instance().snapshot();
    const auto& ngnWf = theme->domains.nodeGraphNode;
    painter->setPen(Qt::NoPen);
    painter->setBrush(ngnWf.waveformBg);
    painter->drawRoundedRect(rect, 2, 2);
    QColor waveColor = ngnWf.blueprintWaveform;
    if (!blueprintWaveformMode_)
        waveColor.setAlphaF(waveColor.alphaF() * (isSelected() ? 1.0 : 140 / 255.0));

    if (waveformPeakData_ && waveformPeakData_->isValid()) {
        QRect intRect(static_cast<int>(rect.x()), static_cast<int>(rect.y()),
                      static_cast<int>(rect.width()), static_cast<int>(rect.height()));
        WaveformRenderOptions options{WaveColorMode::Static, WaveChannelSource::Mono, waveColor};
        if (waveformRange_) {
            options.startFrame = static_cast<double>(waveformRange_->startFrame);
            options.frameCount = static_cast<double>(waveformRange_->endFrame - waveformRange_->startFrame);
        }
        WaveformRenderer::render(*painter, intRect, *waveformPeakData_, theme->domains.waveform, options);

        float midY = rect.y() + rect.height() / 2.0f;
        painter->setPen(QPen(ngnWf.waveformCenter, 0.5f));
        painter->drawLine(QPointF(rect.x(), midY), QPointF(rect.x() + rect.width(), midY));
        return;
    }

    if (waveformPeaks_.empty()) return;

    const int numPeaks = static_cast<int>(waveformPeaks_.size()) / 2;
    if (numPeaks == 0) return;

    WaveformRenderer::renderRawPeaks(
        *painter, rect, waveformPeaks_.data(), numPeaks,
        theme->domains.waveform, waveColor);
}

QVariant NodeItem::itemChange(GraphicsItemChange change, const QVariant& value)
{
    if (change == ItemPositionHasChanged && positionChangedCallback_) {
        positionChangedCallback_(value.toPointF());
    }
    return QGraphicsItem::itemChange(change, value);
}

void NodeItem::hoverEnterEvent(QGraphicsSceneHoverEvent* event)
{
    hovered_ = true;
    update();
    QGraphicsItem::hoverEnterEvent(event);
}

void NodeItem::hoverLeaveEvent(QGraphicsSceneHoverEvent* event)
{
    hovered_ = false;
    update();
    QGraphicsItem::hoverLeaveEvent(event);
}

void NodeItem::mousePressEvent(QGraphicsSceneMouseEvent* event)
{
    pressPos_ = pos();
    update();
    QGraphicsItem::mousePressEvent(event);
}

void NodeItem::mouseReleaseEvent(QGraphicsSceneMouseEvent* event)
{
    QGraphicsItem::mouseReleaseEvent(event);

    QPointF endPos = pos();
    if (pressPos_ != endPos && blueprintGridSnap_ && blueprintGridCell_ > 0.0) {
        const double cell = blueprintGridCell_;
        QPointF snapped(std::round(endPos.x() / cell) * cell,
                        std::round(endPos.y() / cell) * cell);
        if (snapped != endPos) {
            setPos(snapped);
            endPos = snapped;
        }
    }
    if (dragCompleteCallback_ && pressPos_ != endPos) {
        dragCompleteCallback_(pressPos_, endPos);
    }
}

}
