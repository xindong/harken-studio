#pragma once

#include <QWidget>

class QPainter;

namespace VNX {

class PreviewField : public QWidget
{
    Q_OBJECT

public:
    explicit PreviewField(QWidget* parent = nullptr);
    void setDimmed(bool dimmed);

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    virtual void paintPreview(QPainter& painter, const QRectF& bounds) = 0;
};

}
