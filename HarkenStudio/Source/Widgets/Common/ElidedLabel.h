#pragma once

#include <QLabel>
#include <QPainter>
#include <QStyle>

namespace VNX {

class ElidedLabel : public QLabel
{
public:
    explicit ElidedLabel(QWidget* parent = nullptr) : QLabel(parent)
    {
        setTextFormat(Qt::PlainText);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

    ElidedLabel(const QString& text, QWidget* parent = nullptr) : ElidedLabel(parent)
    {
        setText(text);
    }

    QString displayText() const
    {
        return fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width());
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QFrame::paintEvent(event);
        QPainter painter(this);
        style()->drawItemText(&painter, contentsRect(), alignment(), palette(), isEnabled(),
            displayText(), QPalette::WindowText);
    }
};

}
