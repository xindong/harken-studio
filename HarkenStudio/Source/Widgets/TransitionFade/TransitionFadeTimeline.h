#pragma once

#include "TransitionRuleEditing.h"

#include "Core/AssetPipeline/WaveformCache.h"

#include <QHash>
#include <QPainterPath>
#include <QWidget>

namespace VNX {

class TransitionFadeTimeline final : public QWidget
{
    Q_OBJECT

public:
    explicit TransitionFadeTimeline(QWidget* parent = nullptr);

    void setPreview(const TransitionPreview& preview);
    void fitView();
    void setViewStart(double seconds);
    void zoomBy(double factor);
    double viewStart() const { return viewStart_; }
    double viewSpan() const { return viewSpan_; }
    double contentStart() const;
    double contentEnd() const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void viewChanged();
    void fadesPreviewed(const TransitionFadeDef& fadeOut, const TransitionFadeDef& fadeIn);
    void fadesCommitted(const TransitionFadeDef& fadeOut, const TransitionFadeDef& fadeIn);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    enum class Part { None, Start, End, Handle1, Handle2, Curve };
    struct Hit
    {
        int side = -1;
        Part part = Part::None;
        bool operator==(const Hit&) const = default;
    };

    TransitionFadeDef& fade(int side);
    const TransitionFadeDef& fade(int side) const;
    QRect rulerRect() const;
    QRect plotRect() const;
    QRect laneRect(int side) const;
    QRectF fadeRect(int side) const;
    double xAt(double seconds) const;
    double secondsAt(double x) const;
    QPointF fadePoint(int side, float x, float progress) const;
    QPointF targetPoint(Hit target) const;
    bool singleSegment(int side) const;
    QPainterPath fadePath(int side) const;
    Hit hitTest(const QPoint& position) const;
    double snapped(double seconds) const;
    void dragTo(const QPoint& position, Qt::KeyboardModifiers modifiers);
    void finishDrag(bool commit);
    void resetFade(int side);
    void resetCurve(int side);
    QString tooltipFor(Hit target) const;
    const WaveformPeakData* waveform(const QString& path) const;

    TransitionPreview preview_;
    TransitionPreview original_;
    double viewStart_ = -2.0;
    double viewSpan_ = 4.0;
    int selected_ = -1;
    Hit hovered_;
    Hit active_;
    QPoint dragOrigin_;
    double dragT_ = 0.5;
    double grabOffset_ = 0.0;
    bool moving_ = false;
    bool panning_ = false;
    double panOrigin_ = 0.0;
    mutable QHash<QString, WaveformPeakData> waveforms_;
};

}
