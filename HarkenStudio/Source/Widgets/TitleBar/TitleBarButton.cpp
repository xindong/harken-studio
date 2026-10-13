#include "TitleBarButton.h"

#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"

#include <QApplication>
#include <QPainter>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOptionButton>

namespace VNX {

namespace {

QString iconId(TitleBarButtonKind kind)
{
    switch (kind) {
    case TitleBarButtonKind::Minimize:
        return QString::fromLatin1(ThemeAssetIds::WindowMinimize);
    case TitleBarButtonKind::Maximize:
        return QString::fromLatin1(ThemeAssetIds::WindowMaximize);
    case TitleBarButtonKind::Restore:
        return QString::fromLatin1(ThemeAssetIds::WindowRestore);
    case TitleBarButtonKind::Close:
        return QString::fromLatin1(ThemeAssetIds::WindowClose);
    }
    return {};
}

}

TitleBarButton::TitleBarButton(TitleBarButtonKind kind, QWidget* parent)
    : QPushButton(parent)
    , kind_(kind)
{
    setText(QString());
    setProperty("flatIconButton", true);
    setFixedSize(46, 36);
    setFlat(true);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::ArrowCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
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
    const QPalette pal = qApp->palette();
    if (!isEnabled())
        return pal.color(QPalette::Disabled, QPalette::ButtonText);
    if (kind_ == TitleBarButtonKind::Close && isDown())
        return pal.color(QPalette::Active, QPalette::BrightText);
    return pal.color(QPalette::Active, QPalette::ButtonText);
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

    const QSize iconSize(20, 20);
    const QRect iconRect(QPoint((width() - iconSize.width()) / 2,
                                (height() - iconSize.height()) / 2),
                         iconSize);
    ThemeManager::instance().resolveIcon(
        iconId(kind_), ThemeAssetVariant{QSize(), 1.0, 0, iconColor()})
        .icon.paint(&painter, iconRect);
}

}
