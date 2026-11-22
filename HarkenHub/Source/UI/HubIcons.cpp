#include "UI/HubIcons.h"

#include <QApplication>
#include <QIconEngine>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSvgRenderer>

#ifdef Q_OS_WIN
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
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(pixmap.rect(), QApplication::palette().color(QPalette::ButtonText));
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
    painter.setBrush(QApplication::palette().color(QPalette::ButtonText));
    for (const qreal x : {4.0, 8.0, 12.0})
        painter.drawEllipse(QPointF(x, 8.0), 1.25, 1.25);
    return pixmap;
}

class ControlIconEngine final : public QIconEngine
{
public:
    explicit ControlIconEngine(bool settings) : settings_(settings) {}
    QIconEngine* clone() const override { return new ControlIconEngine(settings_); }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode, QIcon::State) override
    {
        const auto image = settings_ ? renderSettings(painter->device()->devicePixelRatioF())
                                     : renderMore(painter->device()->devicePixelRatioF());
        painter->drawPixmap(rect, image);
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap image(size);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        paint(&painter, image.rect(), mode, state);
        return image;
    }
private:
    bool settings_;
};

}

QIcon applicationIcon()
{
    return QIcon(QStringLiteral(":/Resources/icons/HubIcon.ico"));
}

void installApplicationIcon(QApplication& app)
{
    const QIcon icon = applicationIcon();
    app.setWindowIcon(icon);
#ifdef Q_OS_WIN
    auto* taskbar = new TaskbarIcon(&app);
    taskbar->setIcon(icon);
#endif
}

QIcon settingsIcon(qreal devicePixelRatio)
{
    Q_UNUSED(devicePixelRatio);
    return QIcon(new ControlIconEngine(true));
}

QIcon moreIcon(qreal devicePixelRatio)
{
    Q_UNUSED(devicePixelRatio);
    return QIcon(new ControlIconEngine(false));
}

}
