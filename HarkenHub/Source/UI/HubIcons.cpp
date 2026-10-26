#include "UI/HubIcons.h"

#include <QApplication>
#include <QImageReader>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

#ifdef Q_OS_WIN
#include "Platform/Windows/SystemTheme.h"
#include "Platform/Windows/TaskbarIcon.h"
#endif

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

QIcon applicationIcon(const QColor& color)
{
    QImageReader reader(QStringLiteral(":/Resources/icons/HubIcon.ico"));
    QIcon icon;
    for (int frame = 0; frame < reader.imageCount(); ++frame) {
        if (!reader.jumpToImage(frame))
            continue;
        QImage image = reader.read().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (image.isNull())
            continue;
        QPainter painter(&image);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(image.rect(), color);
        painter.end();
        icon.addPixmap(QPixmap::fromImage(image));
    }
    return icon;
}

ApplicationIcon::ApplicationIcon(QObject* parent)
    : QObject(parent)
{
    QApplication::setWindowIcon(applicationIcon(Qt::white));
#ifdef Q_OS_WIN
    taskbar_ = new TaskbarIcon(this);
    auto* theme = new SystemTheme(this);
    connect(theme, &SystemTheme::iconColorChanged, this, &ApplicationIcon::applyColor);
    applyColor(theme->iconColor());
#else
    applyColor(Qt::white);
#endif
}

QIcon ApplicationIcon::systemIcon() const
{
    return systemIcon_;
}

void ApplicationIcon::applyColor(const QColor& color)
{
    systemIcon_ = applicationIcon(color);
#ifdef Q_OS_WIN
    taskbar_->setIcon(systemIcon_);
#endif
    emit changed(systemIcon_);
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
