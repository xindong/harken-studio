#pragma once

#include "Core/ThemeSnapshot.h"

#include <QSize>
#include <QSurfaceFormat>
#include <QThread>
#include <QWindow>

#include <QObject>

#include <atomic>

namespace VNX {

class StartupRenderer final : public QObject
{
    Q_OBJECT

public:
    explicit StartupRenderer(ThemeSnapshotPtr theme);
    ~StartupRenderer();

    QWindow* window() const { return window_; }
    void start();
    void stop();
    void setViewport(const QSize& size, qreal devicePixelRatio);
    void setExposed(bool exposed);

    bool isRunning() const { return running_.load(std::memory_order_acquire); }
    quintptr threadId() const { return threadId_.load(std::memory_order_acquire); }

signals:
    void initialFramePresented();
    void initializationFailed();

private:
    void renderLoop();

    const ThemeSnapshotPtr theme_;
    QWindow* window_ = nullptr;
    QSurfaceFormat format_;
    QThread* thread_ = nullptr;
    std::atomic_bool running_{false};
    std::atomic_bool exposed_{false};
    std::atomic_bool initialFramePresented_{false};
    std::atomic<quintptr> threadId_{0};
    std::atomic<int> pixelWidth_{1};
    std::atomic<int> pixelHeight_{1};
    std::atomic<qreal> devicePixelRatio_{1.0};
};

}
