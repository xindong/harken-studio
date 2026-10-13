
#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <memory>

#ifdef Q_OS_WIN
#include <QSet>
struct HWND__;
typedef HWND__* HWND;
#endif

class QWidget;
class QMimeData;
class QEvent;

namespace VNX {

class DragDropOverlay;

class ExternalDropDispatcher : public QObject
{
    Q_OBJECT

public:
    explicit ExternalDropDispatcher(QObject* parent = nullptr);
    ~ExternalDropDispatcher() override;

    void install(QWidget* topLevel);

    void refresh();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool dispatchMove(const QStringList& paths, bool layeredFeedback);
    void dispatchDrop(const QStringList& paths);
    void dispatchLeave();
    void armStaleOverlayGuard();

    QWidget* dropTargetAt(const QPoint& globalPos) const;
    void rebuildMime(const QStringList& paths);

#ifdef Q_OS_WIN
    void installOn(HWND hwnd);
    QSet<HWND> installed_;
#endif

    QWidget* topLevel_ = nullptr;
    DragDropOverlay* overlay_ = nullptr;
    QPointer<QWidget> currentTarget_;
    std::unique_ptr<QMimeData> mime_;
    QTimer staleOverlayTimer_;
};

}
