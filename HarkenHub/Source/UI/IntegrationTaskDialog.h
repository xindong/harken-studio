#pragma once

#include "Integrations/IntegrationProgress.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScopeGuard>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>
#include <optional>
#include <type_traits>

namespace VNH {

class IntegrationTaskDialog : public QDialog
{
public:
    IntegrationTaskDialog(QWidget* parent, std::atomic<bool>& cancelled, bool cancellable)
        : QDialog(parent)
    {
        setObjectName(QStringLiteral("integrationTaskProgress"));
        setWindowTitle(QCoreApplication::translate("HarkenHub", "SDK operation"));
        setWindowModality(Qt::ApplicationModal);
        setWindowFlag(Qt::WindowCloseButtonHint, false);
        setMinimumWidth(400);
        auto* layout = new QVBoxLayout(this);
        status_ = new QLabel(this);
        status_->setObjectName(QStringLiteral("integrationTaskStatus"));
        status_->setWordWrap(true);
        layout->addWidget(status_);
        auto* progress = new QProgressBar(this);
        progress->setRange(0, 0);
        progress->setTextVisible(false);
        layout->addWidget(progress);
        if (cancellable) {
            auto* cancel = new QPushButton(QCoreApplication::translate("HarkenHub", "Stop Scan"), this);
            cancel->setObjectName(QStringLiteral("cancelIntegrationInspection"));
            layout->addWidget(cancel, 0, Qt::AlignRight);
            connect(cancel, &QPushButton::clicked, this, [cancel, &cancelled]() {
                cancelled.store(true, std::memory_order_relaxed);
                cancel->setEnabled(false);
                cancel->setText(QCoreApplication::translate("HarkenHub", "Stopping..."));
            });
        }
        parent->installEventFilter(this);
    }

    void updateProgress(quint64 inspected)
    {
        status_->setText(inspected == 0
            ? QCoreApplication::translate("HarkenHub", "Processing SDK files...")
            : QCoreApplication::translate("HarkenHub", "Processing SDK files... Items inspected: %1").arg(inspected));
    }

    void done(int) override {}
    void accept() override {}
    void reject() override {}

protected:
    void closeEvent(QCloseEvent* event) override { event->ignore(); }
    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (event->type() == QEvent::Close) {
            event->ignore();
            return true;
        }
        return QDialog::eventFilter(object, event);
    }

private:
    QLabel* status_;
};

template<class Task>
auto runIntegrationTask(QWidget* parent, Task task, bool cancellable = false)
{
    QWidget* window = parent->window();
    const bool enabled = window->isEnabled();
    window->setEnabled(false);
    const auto restore = qScopeGuard([window, enabled]() { window->setEnabled(enabled); });
    std::atomic<bool> cancelled{false};
    IntegrationTaskDialog dialog(window, cancelled, cancellable);
    dialog.setEnabled(true);
    std::atomic<quint64> inspected{0};
    std::optional<std::invoke_result_t<Task>> result;
    QEventLoop loop;
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer progress;
    QObject::connect(&progress, &QTimer::timeout, &dialog, [&]() {
        dialog.updateProgress(inspected.load(std::memory_order_relaxed));
        if (elapsed.elapsed() >= 300 && !dialog.isVisible()) dialog.show();
    });
    progress.start(100);
    const std::unique_ptr<QThread> worker(QThread::create([&]() {
        IntegrationProgress reporting(inspected, cancelled);
        try {
            result = task();
            if (cancelled.load(std::memory_order_relaxed)) result.reset();
        } catch (const IntegrationScanStopped&) {
        }
    }));
    QObject::connect(worker.get(), &QThread::finished, &loop, &QEventLoop::quit);
    worker->start();
    loop.exec();
    worker->wait();
    return result;
}

}
