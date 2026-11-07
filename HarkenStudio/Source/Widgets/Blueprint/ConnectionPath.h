
#pragma once

#include <QGraphicsPathItem>
#include <optional>

#include "BlueprintEdgeKind.h"
#include "BlueprintDetailLevel.h"
#include "NodeItem.h"
#include "Core/ThemeSnapshot.h"

namespace VNX {

class NodePin;

enum class ConnectionStyle {
    Solid,
    Dashed,
    Sidechain
};

class ConnectionPath : public QGraphicsPathItem
{
public:
    ConnectionPath(NodePin* startPin, NodePin* endPin, QGraphicsItem* parent = nullptr);
    ~ConnectionPath() override;

    void updatePosition();
    void refreshMetrics();

    NodePin* startPin() const { return startPin_; }
    NodePin* endPin() const { return endPin_; }

    void notifyPinDestroyed(NodePin* pin);

    ConnectionStyle style() const { return style_; }
    void setStyle(ConnectionStyle s) { style_ = s; update(); }

    static QColor defaultColor();
    QColor displayColor() const;
    void setPlaybackActive(bool active);
    bool playbackActive() const { return playbackActive_; }
    void setFocusDimmed(bool dimmed) { setOpacity(dimmed ? 0.35 : 1.0); }

    void setLabel(const QString& label);
    void setDetailLevel(BlueprintDetailLevel level);
    BlueprintDetailLevel detailLevel() const { return detailLevel_; }

    void setBlueprintEdgeKind(BlueprintEdgeKind k) { blueprintKind_ = k; update(); }
    void clearBlueprintEdgeKind()                  { blueprintKind_.reset(); update(); }
    std::optional<BlueprintEdgeKind> blueprintEdgeKind() const { return blueprintKind_; }

    void setBlueprintFlowDirection(CanvasFlowDirection dir);
    CanvasFlowDirection blueprintFlowDirection() const { return blueprintFlowDir_; }
    void setParallelOffset(qreal offset);
    qreal parallelOffset() const { return parallelOffset_; }

    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;

private:
    NodePin* startPin_;
    NodePin* endPin_;
    ConnectionStyle style_ = ConnectionStyle::Solid;
    bool playbackActive_ = false;
    QString label_;
    BlueprintDetailLevel detailLevel_ = BlueprintDetailLevel::Full;
    std::optional<BlueprintEdgeKind> blueprintKind_;
    CanvasFlowDirection blueprintFlowDir_ = CanvasFlowDirection::LR;
    qreal parallelOffset_ = 0.0;
    ThemeSnapshotPtr theme_;

    void updatePins();
    void updateLabelToolTip();
    QRectF labelRect() const;
};

}
