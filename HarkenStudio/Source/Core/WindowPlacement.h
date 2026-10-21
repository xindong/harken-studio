#pragma once

#include <QGuiApplication>
#include <QRect>
#include <QScreen>
#include <QSettings>
#include <QWidget>

namespace VNX {

inline constexpr QSize kDefaultStudioWindowSize{1800, 1100};

struct WindowPresentation
{
    QSize normalSize = kDefaultStudioWindowSize;
    Qt::WindowState state = Qt::WindowNoState;
};

inline Qt::WindowState persistentWindowState(Qt::WindowStates state)
{
    if (state.testFlag(Qt::WindowFullScreen))
        return Qt::WindowFullScreen;
    if (state.testFlag(Qt::WindowMaximized))
        return Qt::WindowMaximized;
    return Qt::WindowNoState;
}

inline WindowPresentation readWindowPresentation(QSettings& settings)
{
    WindowPresentation result;
    if (settings.contains("normalSize") || settings.contains("presentationState")) {
        const QVariant size = settings.value("normalSize");
        if (size.metaType().id() == QMetaType::QSize && size.toSize().isValid()
            && !size.toSize().isEmpty())
            result.normalSize = size.toSize();
        bool validState = false;
        const int state = settings.value("presentationState").toInt(&validState);
        if (validState && (state == Qt::WindowMaximized || state == Qt::WindowFullScreen))
            result.state = static_cast<Qt::WindowState>(state);
        return result;
    }

    const QByteArray geometry = settings.value("geometry").toByteArray();
    if (geometry.isEmpty())
        return result;
    QWidget legacy;
    if (!legacy.restoreGeometry(geometry))
        return result;
    const QSize normalSize = legacy.normalGeometry().size();
    if (normalSize.isValid() && !normalSize.isEmpty())
        result.normalSize = normalSize;
    result.state = persistentWindowState(legacy.windowState());
    return result;
}

inline void saveWindowPresentation(QSettings& settings, const QWidget& window,
                                   QSize normalSize = {})
{
    if (!normalSize.isValid() || normalSize.isEmpty())
        normalSize = window.normalGeometry().size();
    if (!normalSize.isValid() || normalSize.isEmpty())
        normalSize = window.size();
    settings.setValue("normalSize", normalSize);
    settings.setValue("presentationState", int(persistentWindowState(window.windowState())));
    settings.sync();
    if (settings.status() == QSettings::NoError)
        settings.remove("geometry");
}

inline QRect centeredWindowFrame(const QSize& requestedSize, const QRect& available)
{
    if (!requestedSize.isValid() || requestedSize.isEmpty() || !available.isValid())
        return {};
    const QSize size = requestedSize.boundedTo(available.size());
    const QPoint offset((available.width() - size.width()) / 2,
                        (available.height() - size.height()) / 2);
    return {available.topLeft() + offset, size};
}

inline QScreen* startupTargetScreen(QScreen* preferred)
{
    const auto screens = QGuiApplication::screens();
    if (preferred && screens.contains(preferred))
        return preferred;
    return QGuiApplication::primaryScreen();
}

inline QSize centerWindowOnScreen(QWidget* window, QScreen* preferred)
{
    if (!window || !window->isWindow() || window->isMaximized()
        || window->isFullScreen() || window->isMinimized())
        return {};
    QScreen* screen = startupTargetScreen(preferred);
    if (!screen || !screen->availableGeometry().isValid())
        return {};
    if (window->screen() != screen)
        window->setScreen(screen);

    const QRect available = screen->availableGeometry();
    const QRect frame = window->frameGeometry();
    const QRect placed = centeredWindowFrame(frame.size(), available);
    if (!placed.isValid())
        return {};
    const QSize frameMargins = frame.size() - window->size();
    const QSize clientSize = placed.size() - frameMargins;
    window->resize(qMax(1, clientSize.width()), qMax(1, clientSize.height()));
    const QRect actual = window->frameGeometry();
    const QPoint offset(qMax(0, (available.width() - actual.width()) / 2),
                        qMax(0, (available.height() - actual.height()) / 2));
    window->move(window->pos() + available.topLeft() + offset - actual.topLeft());
    const QSize normalSize = window->size();
    if (actual.width() > available.width() || actual.height() > available.height())
        window->setWindowState(Qt::WindowMaximized);
    return normalSize;
}

}
