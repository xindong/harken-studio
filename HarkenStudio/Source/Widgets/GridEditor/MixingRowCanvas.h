#pragma once

#include "Widgets/CurveEditor/CurveCanvas.h"

namespace VNX {

struct MixingCurveClip
{
    QString title;
    QVector<CurvePointDef> points;
    QVector<CurveHandlePairDef> handles;
};

class MixingRowCanvas : public CurveCanvas
{
    Q_OBJECT

public:
    explicit MixingRowCanvas(QWidget* parent = nullptr);
    void setClips(const QVector<MixingCurveClip>& clips, int selected);
    QRectF clipTitleRect(int clip) const;
    void cancelDrag() override;

signals:
    void clipSelected(int clip);

protected:
    QRectF graphRect() const override;
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    int titleAt(const QPointF& position) const;
    int titleHeight() const;
    QVector<CurvePointDef> clipPoints(int clip) const;

    QVector<MixingCurveClip> clips_;
    QVector<int> overlayClips_;
    int selected_ = -1;
    bool moving_ = false;
    bool moved_ = false;
    bool updatingClip_ = false;
    QPointF moveOrigin_;
    QVector<CurvePointDef> movePoints_;
    QVector<CurveHandlePairDef> moveHandles_;
};

}
