#include "UI/HubIcons.h"

#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

namespace VNH::HubIcons {

namespace {

qreal normalizedDevicePixelRatio(qreal value)
{
    return qMax(qreal(1), value);
}

QPixmap renderSettings(qreal devicePixelRatio)
{
    const qreal dpr = normalizedDevicePixelRatio(devicePixelRatio);
    const QSize logicalSize(20, 20);
    QPixmap pixmap(logicalSize * dpr);
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(dpr);
    QPainter painter(&pixmap);
    QSvgRenderer renderer(QStringLiteral(":/Resources/icons/Settings.svg"));
    renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(logicalSize)));
    return pixmap;
}

QPixmap renderMore(qreal devicePixelRatio)
{
    const qreal dpr = normalizedDevicePixelRatio(devicePixelRatio);
    const QSize logicalSize(16, 16);
    QPixmap pixmap(logicalSize * dpr);
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(dpr);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0xAA, 0xB0, 0xBA));
    for (const qreal x : {4.0, 8.0, 12.0})
        painter.drawEllipse(QPointF(x, 8.0), 1.25, 1.25);
    return pixmap;
}

QIcon iconWithAllStates(const QPixmap& pixmap)
{
    QIcon icon;
    icon.addPixmap(pixmap, QIcon::Normal);
    icon.addPixmap(pixmap, QIcon::Disabled);
    icon.addPixmap(pixmap, QIcon::Active);
    icon.addPixmap(pixmap, QIcon::Selected);
    return icon;
}

}

QIcon settingsIcon(qreal devicePixelRatio)
{
    return iconWithAllStates(renderSettings(devicePixelRatio));
}

QIcon moreIcon(qreal devicePixelRatio)
{
    return iconWithAllStates(renderMore(devicePixelRatio));
}

}
