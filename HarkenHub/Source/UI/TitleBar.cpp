#include "UI/TitleBar.h"

#include <QHBoxLayout>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionButton>

namespace VNH {

TitleBarButton::TitleBarButton(TitleBarButtonKind kind, QWidget* parent)
    : QPushButton(parent)
    , kind_(kind)
{
    setText(QString());
    setFixedSize(46, 36);
    setFlat(true);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::ArrowCursor);
    setAttribute(Qt::WA_Hover, true);
}

void TitleBarButton::setButtonKind(TitleBarButtonKind kind)
{
    if (kind_ == kind)
        return;
    kind_ = kind;
    update();
}

QColor TitleBarButton::iconColor() const
{
    if (!isEnabled())
        return QColor(0x55, 0x5B, 0x66);
    if (underMouse() || isDown())
        return QColor(0xFF, 0xFF, 0xFF);
    return QColor(0xAA, 0xB0, 0xBA);
}

void TitleBarButton::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QStyleOptionButton option;
    initStyleOption(&option);
    option.text.clear();
    option.icon = QIcon();

    QPainter painter(this);
    style()->drawControl(QStyle::CE_PushButton, &option, &painter, this);

    QPen pen(iconColor());
    pen.setWidthF(1.0);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const QRectF glyph(width() / 2.0 - 5.0, height() / 2.0 - 5.0, 10.0, 10.0);
    switch (kind_) {
    case TitleBarButtonKind::Minimize:
        painter.drawLine(QPointF(glyph.left(), glyph.center().y()),
                         QPointF(glyph.right(), glyph.center().y()));
        break;
    case TitleBarButtonKind::Maximize:
        painter.drawRect(glyph.adjusted(0.5, 0.5, -0.5, -0.5));
        break;
    case TitleBarButtonKind::Restore:
        painter.drawRect(QRectF(glyph.left() + 0.5, glyph.top() + 2.5, 7.0, 7.0));
        painter.drawLine(QPointF(glyph.left() + 2.5, glyph.top() + 2.5),
                         QPointF(glyph.left() + 2.5, glyph.top() + 0.5));
        painter.drawLine(QPointF(glyph.left() + 2.5, glyph.top() + 0.5),
                         QPointF(glyph.right() - 0.5, glyph.top() + 0.5));
        painter.drawLine(QPointF(glyph.right() - 0.5, glyph.top() + 0.5),
                         QPointF(glyph.right() - 0.5, glyph.bottom() - 2.5));
        painter.drawLine(QPointF(glyph.right() - 0.5, glyph.bottom() - 2.5),
                         QPointF(glyph.right() - 2.5, glyph.bottom() - 2.5));
        break;
    case TitleBarButtonKind::Close:
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.drawLine(glyph.topLeft(), glyph.bottomRight());
        painter.drawLine(glyph.topRight(), glyph.bottomLeft());
        break;
    }
}

TitleBar::TitleBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("TitleBarWidget"));
    setFixedHeight(36);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addStretch();

    btnMinimize_ = new TitleBarButton(TitleBarButtonKind::Minimize, this);
    btnMinimize_->setObjectName(QStringLiteral("titleBarMin"));
    btnMaximize_ = new TitleBarButton(TitleBarButtonKind::Maximize, this);
    btnMaximize_->setObjectName(QStringLiteral("titleBarMax"));
    btnClose_ = new TitleBarButton(TitleBarButtonKind::Close, this);
    btnClose_->setObjectName(QStringLiteral("titleBarClose"));

    layout->addWidget(btnMinimize_);
    layout->addWidget(btnMaximize_);
    layout->addWidget(btnClose_);
}

void TitleBar::setMaximized(bool maximized)
{
    btnMaximize_->setButtonKind(maximized ? TitleBarButtonKind::Restore
                                          : TitleBarButtonKind::Maximize);
}

}
