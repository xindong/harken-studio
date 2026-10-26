
#pragma once

#include <QGraphicsItem>
#include <QList>
#include <QSizeF>
#include <QString>
#include <functional>
#include <optional>
#include <vector>

#include "../Common/WaveformRenderer.h"
#include "NodeContentInfo.h"
#include "NodeContentEditor.h"
#include "Core/AssetPipeline/WaveformCache.h"
#include "Core/ProjectTypes.h"
#include "Widgets/Shared/InlineRenameEdit.h"

class QGraphicsProxyWidget;
class QGraphicsRectItem;
class QLineEdit;

namespace VNX {

class ConnectionPath;

enum class CanvasFlowDirection : uint8_t {
    LR = 0,
    RL = 1,
    TB = 2,
    BT = 3
};

enum class NodeRole {
    Source,
    Self,
    Parent,
    Bus,
    ReceiveBus
};

class NodePin : public QGraphicsItem
{
public:
    enum Type { Input, Output };

    enum class Domain : uint8_t { Audio, Control, Value };
    enum class ConnectState : uint8_t { None, Ok, Bad };

    NodePin(Type type, QGraphicsItem* parent = nullptr);
    ~NodePin() override;

    Type pinType() const { return type_; }
    void addConnection(ConnectionPath* conn);
    void removeConnection(ConnectionPath* conn);
    QPointF scenePos() const;

    void    setDomain(Domain domain);
    Domain  domain() const { return domain_; }

    void         setConnectState(ConnectState state);
    ConnectState connectState() const { return connectState_; }
    void setChainVisibility(std::optional<Qt::CheckState> state, bool toggleEnabled = true);
    std::optional<Qt::CheckState> chainVisibility() const { return chainVisibility_; }
    void setConnectionDragging(bool dragging);
    void setPlaybackActive(bool active) { if (playbackActive_ != active) { playbackActive_ = active; update(); } }
    bool playbackActive() const { return playbackActive_; }

    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;

protected:
    void hoverEnterEvent(QGraphicsSceneHoverEvent* event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override;

private:
    Type type_;
    QList<ConnectionPath*> connections_;
    Domain       domain_       = Domain::Audio;
    ConnectState connectState_ = ConnectState::None;
    bool         hovered_      = false;
    bool playbackActive_ = false;
    std::optional<Qt::CheckState> chainVisibility_;
    bool chainToggleEnabled_ = true;
    bool connectionDragging_ = false;

    void updateToolTip_();
};

class NodeItem : public QGraphicsItem
{
public:
    static constexpr float kDefaultWidth = 160.0f;
    enum class DropFeedback { None, Valid, Invalid };

    using PositionChangedCallback = std::function<void(const QPointF&)>;
    using DragCompleteCallback = std::function<void(const QPointF& oldPos, const QPointF& newPos)>;
    using TitleEditFinishedCallback = std::function<void(const QString&, bool)>;

    explicit NodeItem(const QString& title, QGraphicsItem* parent = nullptr);
    ~NodeItem() override;

    void addInputPin();
    void addOutputPin();
    
    NodePin* inputPin(int index = 0) const { return (index >= 0 && index < inputs_.size()) ? inputs_[index] : nullptr; }
    NodePin* outputPin(int index = 0) const { return (index >= 0 && index < outputs_.size()) ? outputs_[index] : nullptr; }
    int inputCount() const { return inputs_.size(); }
    int outputCount() const { return outputs_.size(); }
    
    QString title() const { return title_; }
    void setTitle(const QString& title) { title_ = title; update(); }
    void setTitleDirty(bool dirty) { titleDirty_ = dirty; update(); }
    bool titleDirty() const { return titleDirty_; }
    void setTitleColor(const QColor& color) { titleColor_ = color; update(); }
    QColor titleColor() const { return titleColor_; }
    QString displayTitle() const { return titleDirty_ ? title_ + QStringLiteral(" *") : title_; }
    QRectF titleRect() const;
    void beginTitleEditing();
    void finishTitleEditing(bool committed);
    bool isTitleEditing() const { return titleEditing_; }
    QLineEdit* titleEditor() const;
    void setTitleEditFinishedCallback(TitleEditFinishedCallback callback);
    void setTitleRenameHandler(InlineRenameEdit::RenameHandler handler);
    void setPositionChangedCallback(PositionChangedCallback callback);

    NodeRole role() const { return role_; }
    void setRole(NodeRole r);
    void setRouteGainMode(bool enabled);
    bool routeGainMode() const { return routeGainMode_; }

    uint32_t objectID() const { return objectID_; }
    void setObjectID(uint32_t id) { objectID_ = id; }

    void setSandboxBypassed(bool bypassed) { sandboxBypassed_ = bypassed; update(); }
    bool isSandboxBypassed() const { return sandboxBypassed_; }

    void setOverrideBadge(bool on)        { overrideBadge_ = on;  update(); }
    void setReceiveBadge(int count)       { receiveBadge_ = count; update(); }
    bool overrideBadge() const            { return overrideBadge_; }
    int  receiveBadge() const             { return receiveBadge_; }

    void setDropFeedback(DropFeedback feedback);
    DropFeedback dropFeedback() const { return dropFeedback_; }
    void setPlaybackActive(bool active) { if (playbackActive_ != active) { playbackActive_ = active; update(); } }
    bool playbackActive() const { return playbackActive_; }
    void setFocusDimmed(bool dimmed) { setOpacity(dimmed ? 0.35 : 1.0); }

    void           setSubtitle(const QString& s);
    const QString& subtitle() const { return subtitle_; }

    void setItemType(std::optional<ProjectItem::Type> t) { itemType_ = t; update(); }
    std::optional<ProjectItem::Type> itemType() const { return itemType_; }

    void setBlueprintWaveformMode(bool on);
    bool blueprintWaveformMode() const     { return blueprintWaveformMode_; }

    void setBlueprintGridSnap(bool on, double cellPx = 20.0)
    {
        blueprintGridSnap_ = on;
        blueprintGridCell_ = cellPx;
    }

    void setDragCompleteCallback(DragCompleteCallback cb);


    void setContentInfo(const NodeContentInfo& info);
    const NodeContentInfo& contentInfo() const { return contentInfo_; }
    void setContentWidget(QWidget* widget);
    QWidget* contentWidget() const;
    QGraphicsProxyWidget* contentProxy() const { return contentProxy_; }
    void setContentFactory(std::function<QWidget*()> factory,
                           std::function<NodeContentPresentation()> presentation);
    QWidget* ensureContentWidget();
    bool releaseContentWidget();
    void refreshContentPresentation();

    void setContentFields(NodeContentFields fields);
    NodeContentFields contentFields() const { return contentFields_; }

    std::optional<NodeContentField> contentFieldAt(const QPointF& localPos) const;

    void setWaveformPeaks(const std::vector<float>& peaks);

    void setWaveformPeakData(const WaveformPeakData& data,
                             std::optional<WaveformFrameRange> range = {});

    float contentHeight() const { return height_; }

    static float heightForContent(const NodeContentInfo& info, bool hasSubtitle,
                                  bool hasWaveform, bool blueprintWaveformMode,
                                  int portPairCount = 1, NodeContentFields fields = kAllNodeContentFields);

    static QPointF pinCenter(NodePin::Type type, int index, int portPairCount,
                             const QSizeF& size, CanvasFlowDirection direction,
                             bool routeGainMode = false);

    void                setCanvasFlowDirection(CanvasFlowDirection dir);
    CanvasFlowDirection canvasFlowDirection() const { return flowDir_; }

    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;
    void hoverEnterEvent(QGraphicsSceneHoverEvent* event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override;

private:
    QString title_;
    bool titleDirty_ = false;
    QColor titleColor_;
    std::optional<ProjectItem::Type> itemType_;
    bool    hovered_ = false;
    bool    routeGainMode_ = false;
    QList<NodePin*> inputs_;
    QList<NodePin*> outputs_;
    PositionChangedCallback positionChangedCallback_;
    DragCompleteCallback    dragCompleteCallback_;
    TitleEditFinishedCallback titleEditFinishedCallback_;
    InlineRenameEdit::RenameHandler titleRenameHandler_;
    bool titleEditing_ = false;
    QGraphicsProxyWidget* titleProxy_ = nullptr;
    float width_ = kDefaultWidth;
    float height_ = 100.0f;
    bool sandboxBypassed_ = false;
    QPointF pressPos_;
    bool    blueprintGridSnap_ = false;
    double  blueprintGridCell_ = 20.0;
    bool    blueprintWaveformMode_ = false;
    QString subtitle_;
    DropFeedback dropFeedback_ = DropFeedback::None;
    QGraphicsRectItem* dropOverlay_ = nullptr;
    bool playbackActive_ = false;
    bool overrideBadge_ = false;
    int  receiveBadge_ = 0;
    NodeRole role_ = NodeRole::Self;
    uint32_t objectID_ = 0;

    NodeContentInfo contentInfo_;
    NodeContentFields contentFields_ = kAllNodeContentFields;
    QGraphicsProxyWidget* contentProxy_ = nullptr;
    std::function<QWidget*()> contentFactory_;
    std::function<NodeContentPresentation()> contentPresentationFactory_;
    std::optional<NodeContentPresentation> contentPresentation_;

    std::vector<float> waveformPeaks_;

    WaveformPeakData waveformPeakData_;
    std::optional<WaveformFrameRange> waveformRange_;

    void updateContentHeight();
    QRectF titleEditorRect() const;

    void paintContentBody(QPainter* painter, float bodyTop) const;
    QPainterPath routeGainShape(qreal inset) const;
    void paintWaveform(QPainter* painter, const QRectF& rect) const;

    CanvasFlowDirection flowDir_ = CanvasFlowDirection::LR;

    void repositionAllPins_();
};

}
