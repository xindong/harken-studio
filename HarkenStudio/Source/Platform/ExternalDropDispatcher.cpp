
#include "ExternalDropDispatcher.h"

#include "Widgets/DragDropOverlay.h"

#include <QApplication>
#include <QWidget>
#include <QWindow>
#include <QMimeData>
#include <QUrl>
#include <QCursor>
#include <QTimer>
#include <QEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>

#ifdef Q_OS_WIN
#include "Platform/NativeDropTarget_Win.h"
#endif

namespace VNX {

ExternalDropDispatcher::ExternalDropDispatcher(QObject* parent)
    : QObject(parent)
{
    staleOverlayTimer_.setSingleShot(true);
    staleOverlayTimer_.setInterval(1500);
    connect(&staleOverlayTimer_, &QTimer::timeout, this, [this]() {
        dispatchLeave();
    });
}

ExternalDropDispatcher::~ExternalDropDispatcher() = default;

void ExternalDropDispatcher::install(QWidget* topLevel)
{
    topLevel_ = topLevel;
    overlay_ = new DragDropOverlay(topLevel);

#ifdef Q_OS_WIN
    qApp->installEventFilter(this);

    QTimer::singleShot(200, this, [this]() { refresh(); });
#endif
}

#ifdef Q_OS_WIN
void ExternalDropDispatcher::installOn(HWND hwnd)
{
    if (!hwnd)
        return;
    if (!GetPropW(hwnd, L"OleDropTargetInterface"))
        return;

    installNativeDropTarget(
        hwnd,
        [this](const NativeDropInfo& info) { dispatchDrop(info.paths); },
        [this](const QStringList& files, bool layeredFeedback) -> bool {
            return dispatchMove(files, layeredFeedback);
        },
        [this]() { dispatchLeave(); });
    installed_.insert(hwnd);
}

void ExternalDropDispatcher::refresh()
{
    const QWidgetList windows = qApp->topLevelWidgets();
    for (QWidget* w : windows) {
        if (!w || !w->isWindow())
            continue;
        if (!w->windowHandle())
            continue;
        installOn(reinterpret_cast<HWND>(w->winId()));
    }
}
#else
void ExternalDropDispatcher::refresh() {}
#endif

bool ExternalDropDispatcher::eventFilter(QObject* watched, QEvent* event)
{
#ifdef Q_OS_WIN
    const QEvent::Type t = event->type();
    if (t == QEvent::Show || t == QEvent::WinIdChange) {
        QWidget* w = qobject_cast<QWidget*>(watched);
        if (w && w->isWindow()
            && w->windowType() != Qt::Popup && w->windowType() != Qt::ToolTip) {
            QPointer<QWidget> wp(w);
            QTimer::singleShot(0, this, [this, wp]() {
                if (wp && wp->windowHandle())
                    installOn(reinterpret_cast<HWND>(wp->winId()));
            });
        }
    }
#else
    Q_UNUSED(watched);
    Q_UNUSED(event);
#endif
    return QObject::eventFilter(watched, event);
}

QWidget* ExternalDropDispatcher::dropTargetAt(const QPoint& globalPos) const
{
    QWidget* w = QApplication::widgetAt(globalPos);
    while (w && !w->acceptDrops())
        w = w->parentWidget();
    return w;
}

void ExternalDropDispatcher::rebuildMime(const QStringList& paths)
{
    if (!mime_)
        mime_ = std::make_unique<QMimeData>();
    QList<QUrl> urls;
    urls.reserve(paths.size());
    for (const QString& f : paths)
        urls.append(QUrl::fromLocalFile(f));
    mime_->setUrls(urls);
}

bool ExternalDropDispatcher::dispatchMove(const QStringList& paths, bool layeredFeedback)
{
    const QPoint gp = QCursor::pos();
    QWidget* target = paths.isEmpty() ? nullptr : dropTargetAt(gp);

    if (!paths.isEmpty())
        rebuildMime(paths);

    if (target != currentTarget_) {
        if (currentTarget_) {
            QDragLeaveEvent leave;
            QApplication::sendEvent(currentTarget_, &leave);
        }
        currentTarget_ = target;
        if (target) {
            QDragEnterEvent enter(target->mapFromGlobal(gp), Qt::CopyAction, mime_.get(),
                                  Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(target, &enter);
        }
    }

    bool accepted = false;
    if (target) {
        QDragMoveEvent move(target->mapFromGlobal(gp), Qt::CopyAction, mime_.get(),
                            Qt::LeftButton, Qt::NoModifier);
        move.setDropAction(Qt::CopyAction);
        QApplication::sendEvent(target, &move);
        accepted = move.isAccepted();
    }

    if (overlay_) {
        overlay_->showOverlay(paths, accepted ? Qt::CopyAction : Qt::IgnoreAction, layeredFeedback);
        armStaleOverlayGuard();
    }
    return accepted;
}

void ExternalDropDispatcher::dispatchDrop(const QStringList& paths)
{
    if (overlay_)
        overlay_->hideOverlay();
    staleOverlayTimer_.stop();

    const QPoint gp = QCursor::pos();
    QWidget* target = currentTarget_;
    if (!target && !paths.isEmpty())
        target = dropTargetAt(gp);

    if (target && !paths.isEmpty()) {
        rebuildMime(paths);
        QDropEvent drop(QPointF(target->mapFromGlobal(gp)), Qt::CopyAction, mime_.get(),
                        Qt::LeftButton, Qt::NoModifier);
        drop.setDropAction(Qt::CopyAction);
        QApplication::sendEvent(target, &drop);
    }

    currentTarget_ = nullptr;
    mime_.reset();
}

void ExternalDropDispatcher::dispatchLeave()
{
    if (currentTarget_) {
        QDragLeaveEvent leave;
        QApplication::sendEvent(currentTarget_, &leave);
    }
    if (overlay_)
        overlay_->hideOverlay();
    staleOverlayTimer_.stop();
    currentTarget_ = nullptr;
    mime_.reset();
}

void ExternalDropDispatcher::armStaleOverlayGuard()
{
    staleOverlayTimer_.start();
}

}
