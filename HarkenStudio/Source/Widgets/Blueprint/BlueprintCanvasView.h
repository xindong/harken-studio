
#pragma once

#include <QGraphicsView>
#include <QColor>
#include <QPixmap>
#include <QPoint>

class QPainter;
class QRectF;
class QWheelEvent;
class QMouseEvent;
class QContextMenuEvent;
class QMenu;

namespace VNX {

class BlueprintCanvasView : public QGraphicsView
{
    Q_OBJECT
public:
    enum class InteractionMode { Select, Pan };
    Q_ENUM(InteractionMode)

    explicit BlueprintCanvasView(QWidget* parent = nullptr);
    ~BlueprintCanvasView() override;

    static constexpr double kMinZoom = 0.1;
    static constexpr double kMaxZoom = 4.0;
    static constexpr double kGridSpacing = 20.0;

    double gridSpacing() const;

    void setInteractionMode(InteractionMode mode);
    InteractionMode interactionMode() const { return interactionMode_; }

    // Absolute zoom: sets the view scale to `factor` (1.0 == 100%) about the
    // current viewport centre and emits zoomChanged.
    void setZoomFactor(double factor);

signals:
    void zoomChanged(double factor);
    void interactionModeChanged(InteractionMode mode);

protected:
    bool viewportEvent(QEvent* event) override;
    void drawBackground(QPainter* painter, const QRectF& rect) override;

    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

    virtual void populateContextMenu(QMenu& menu, QContextMenuEvent* event);

private:
    void updateInteractionCursor_();

    QPixmap gridTile_;
    double gridTilePitch_ = 0.0;
    double gridTileDpr_ = 0.0;
    QColor gridTileBackground_;
    QColor gridTileDot_;

    InteractionMode interactionMode_ = InteractionMode::Select;
    Qt::MouseButton panButton_ = Qt::NoButton;
    QPoint panStartPos_;
    QPoint panLastPos_;
    bool   rightButtonDragOccurred_ = false;
};

}
