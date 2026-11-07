
#include "ConnectionPath.h"
#include "NodeItem.h"
#include "Core/FontManager.h"
#include "Core/ThemeManager.h"
#include <QPainter>
#include <QPainterPathStroker>
#include <QFontMetricsF>
#include <QStyleOptionGraphicsItem>
#include <QTextDocument>
#include <algorithm>
#include <cmath>

namespace VNX {

ConnectionPath::ConnectionPath(NodePin* startPin, NodePin* endPin, QGraphicsItem* parent)
    : QGraphicsPathItem(parent)
    , startPin_(startPin)
    , endPin_(endPin)
    , theme_(ThemeManager::instance().snapshot())
{
    setZValue(-1);
    setFlag(ItemIsSelectable);
    if (startPin) startPin->addConnection(this);
    if (endPin) endPin->addConnection(this);
    updatePosition();
}

ConnectionPath::~ConnectionPath()
{
    if (startPin_) startPin_->removeConnection(this);
    if (endPin_)   endPin_->removeConnection(this);
}

void ConnectionPath::notifyPinDestroyed(NodePin* pin)
{
    if (startPin_ == pin) startPin_ = nullptr;
    if (endPin_   == pin) endPin_   = nullptr;
}

QColor ConnectionPath::defaultColor()
{
    return ThemeManager::instance().snapshot()->domains.blueprint.containment;
}

QColor ConnectionPath::displayColor() const
{
    const auto& colors = ThemeManager::instance().colors();
    if (playbackActive_) return colors.success;
    if (isSelected()) return colors.accent;
    return defaultColor();
}

void ConnectionPath::setPlaybackActive(bool active)
{
    if (playbackActive_ == active) return;
    playbackActive_ = active;
    update();
    updatePins();
}

void ConnectionPath::setLabel(const QString& label)
{
    if (label_ == label) return;
    prepareGeometryChange();
    label_ = label;
    updateLabelToolTip();
    update();
}

void ConnectionPath::setDetailLevel(BlueprintDetailLevel level)
{
    if (detailLevel_ == level) return;
    detailLevel_ = level;
    updateLabelToolTip();
    update();
}

void ConnectionPath::updateLabelToolTip()
{
    setToolTip(detailLevel_ == BlueprintDetailLevel::Full || label_.isEmpty()
        ? QString() : Qt::convertFromPlainText(label_));
}

void ConnectionPath::updatePins()
{
    if (startPin_) startPin_->update();
    if (endPin_) endPin_->update();
}

QVariant ConnectionPath::itemChange(GraphicsItemChange change, const QVariant& value)
{
    if (change == ItemSelectedHasChanged) updatePins();
    return QGraphicsPathItem::itemChange(change, value);
}

void ConnectionPath::updatePosition()
{
    refreshMetrics();
    if (!startPin_ || !endPin_) return;

    const QPointF p1 = startPin_->scenePos();
    const QPointF p2 = endPin_->scenePos();
    QPainterPath path;
    path.moveTo(p1);

    const float dx = p2.x() - p1.x();
    const float dy = p2.y() - p1.y();
    QPointF c1, c2;
    switch (blueprintFlowDir_) {
    case CanvasFlowDirection::LR: {
        const float ctrlX = qAbs(dx) * 0.5f;
        c1 = p1 + QPointF(ctrlX, 0.0f);
        c2 = p2 + QPointF(-ctrlX, 0.0f);
        break;
    }
    case CanvasFlowDirection::RL: {
        const float ctrlX = qAbs(dx) * 0.5f;
        c1 = p1 + QPointF(-ctrlX, 0.0f);
        c2 = p2 + QPointF(ctrlX, 0.0f);
        break;
    }
    case CanvasFlowDirection::TB: {
        const float ctrlY = qAbs(dy) * 0.5f;
        c1 = p1 + QPointF(0.0f, ctrlY);
        c2 = p2 + QPointF(0.0f, -ctrlY);
        break;
    }
    case CanvasFlowDirection::BT: {
        const float ctrlY = qAbs(dy) * 0.5f;
        c1 = p1 + QPointF(0.0f, -ctrlY);
        c2 = p2 + QPointF(0.0f, ctrlY);
        break;
    }
    }

    const bool vertical = blueprintFlowDir_ == CanvasFlowDirection::TB
        || blueprintFlowDir_ == CanvasFlowDirection::BT;
    const qreal controlOffset = parallelOffset_ * 4.0 / 3.0;
    const QPointF shift = vertical ? QPointF(controlOffset, 0.0) : QPointF(0.0, controlOffset);
    path.cubicTo(c1 + shift, c2 + shift, p2);
    setPath(path);
}

void ConnectionPath::setParallelOffset(qreal offset)
{
    if (parallelOffset_ == offset) return;
    parallelOffset_ = offset;
    updatePosition();
}

void ConnectionPath::setBlueprintFlowDirection(CanvasFlowDirection dir)
{
    if (blueprintFlowDir_ == dir) return;
    blueprintFlowDir_ = dir;
    updatePosition();
    update();
}

QRectF ConnectionPath::boundingRect() const
{
    const auto& metrics = theme_->ui.metrics;
    const qreal halo = std::max({metrics.connection.hitWidth, metrics.connection.maximumOverviewWidth,
        metrics.connection.lineWidth + 2 * metrics.borderWidth}) / 2;
    QRectF base = QGraphicsPathItem::boundingRect().adjusted(-halo, -halo, halo, halo);
    if (!label_.isEmpty()) base = base.united(labelRect().adjusted(-halo, -halo, halo, halo));
    return base;
}

QRectF ConnectionPath::labelRect() const
{
    const QPointF mid = path().pointAtPercent(0.5);
    const QFontMetricsF metrics(FontManager::instance().getFont(theme_, FontRole::Caption));
    const qreal width = metrics.horizontalAdvance(label_);
    const auto& geometry = theme_->ui.metrics;
    return QRectF(mid.x() - width / 2 - geometry.spacingXSmall, mid.y() - metrics.height() - geometry.spacingCompact,
                  width + geometry.spacingXSmall * 2, metrics.height() + geometry.spacingCompact * 2);
}

void ConnectionPath::refreshMetrics()
{
    const auto current = ThemeManager::instance().snapshot();
    if (theme_ == current) return;
    prepareGeometryChange();
    theme_ = current;
    update();
}

QPainterPath ConnectionPath::shape() const
{
    QPainterPathStroker stroker;
    stroker.setWidth(theme_->ui.metrics.connection.hitWidth);
    auto hit = stroker.createStroke(path());
    if (detailLevel_ == BlueprintDetailLevel::Full && !label_.isEmpty()) hit.addRect(labelRect());
    return hit;
}

void ConnectionPath::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget)
{
    Q_UNUSED(option)
    Q_UNUSED(widget)

    const auto& geometry = theme_->ui.metrics;
    qreal width = geometry.connection.lineWidth
        + (playbackActive_ ? geometry.borderWidth : 0) + (isSelected() ? geometry.borderWidth : 0);
    if (detailLevel_ != BlueprintDetailLevel::Full) {
        const auto transform = painter->worldTransform();
        const qreal scale = std::hypot(transform.m11(), transform.m12());
        const qreal minimumWidth = geometry.connection.lineWidth * (playbackActive_ || isSelected() ? 0.6 : 0.4);
        if (scale > 0.0) width = std::max(width, std::min(geometry.connection.maximumOverviewWidth, minimumWidth / scale));
    }
    if (blueprintKind_.has_value()) {
        Qt::PenStyle penStyle = Qt::SolidLine;
        QVector<qreal> dashPat;

        switch (*blueprintKind_) {
        case BlueprintEdgeKind::Through:
        case BlueprintEdgeKind::Containment:
        case BlueprintEdgeKind::BusRoute:
        case BlueprintEdgeKind::Placement:
            break;
        case BlueprintEdgeKind::Send:
        case BlueprintEdgeKind::Override:
            penStyle = Qt::DashLine;
            dashPat = {6, 3};
            break;
        case BlueprintEdgeKind::Organization:
            penStyle = Qt::DashLine;
            dashPat = {8, 4};
            break;
        }

        painter->setBrush(Qt::NoBrush);

        QPen linePen(displayColor(), width);
        linePen.setStyle(penStyle);
        if (penStyle == Qt::DashLine && !dashPat.isEmpty()) {
            linePen.setDashPattern(dashPat);
        }
        painter->setPen(linePen);
        painter->drawPath(path());

    } else {
    QPen pen(displayColor(), width);

    switch (style_) {
    case ConnectionStyle::Dashed:
        pen.setStyle(Qt::DashLine);
        pen.setDashPattern({5, 3});
        break;
    case ConnectionStyle::Sidechain:
        pen.setStyle(Qt::DashLine);
        pen.setDashPattern({4, 3});
        break;
    case ConnectionStyle::Solid:
    default:
        break;
    }

    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(path());
    }

    if (detailLevel_ == BlueprintDetailLevel::Full && !label_.isEmpty()) {
        QFont labelFont = FontManager::instance().getFont(FontRole::Caption);
        painter->setFont(labelFont);
        const QRectF bg = labelRect();
        const auto theme = ThemeManager::instance().snapshot();
        const auto& bp = theme->domains.blueprint;
        painter->setPen(Qt::NoPen);
        painter->setBrush(bp.edgeLabelBg);
        painter->drawRoundedRect(bg, geometry.radiusSmall, geometry.radiusSmall);
        painter->setPen(bp.edgeLabelText);
        painter->drawText(bg, Qt::AlignCenter, label_);
    }
}

}
