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

class IntegrationTaskProgress : public QWidget
{
public:
    explicit IntegrationTaskProgress(QWidget* parent) : QWidget(parent)
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        status_ = new QLabel(this);
        status_->setObjectName(QStringLiteral("integrationTaskStatus"));
        status_->setWordWrap(true);
        layout->addWidget(status_);
        auto* progress = new QProgressBar(this);
        progress->setRange(0, 0);
        progress->setTextVisible(false);
        layout->addWidget(progress);
        cancel_ = new QPushButton(QCoreApplication::translate("HarkenHub", "Stop Scan"), this);
        cancel_->setObjectName(QStringLiteral("cancelIntegrationInspection"));
        cancel_->setAutoDefault(false);
        layout->addWidget(cancel_, 0, Qt::AlignRight);
        connect(cancel_, &QPushButton::clicked, this, [this]() {
            if (!cancelled_) return;
            cancelled_->store(true, std::memory_order_relaxed);
            cancel_->setEnabled(false);
            cancel_->setText(QCoreApplication::translate("HarkenHub", "Stopping..."));
        });
    }

    void begin(std::atomic<bool>& cancelled, bool cancellable)
    {
        cancelled_ = &cancelled;
        cancel_->setVisible(cancellable);
        cancel_->setEnabled(true);
        cancel_->setText(QCoreApplication::translate("HarkenHub", "Stop Scan"));
        for (QWidget* owner = window(); owner; owner = owner->parentWidget() ? owner->parentWidget()->window() : nullptr)
            owner->installEventFilter(this);
        updateProgress(0);
        show();
    }

    void finish()
    {
        for (QWidget* owner = window(); owner; owner = owner->parentWidget() ? owner->parentWidget()->window() : nullptr)
            owner->removeEventFilter(this);
        cancelled_ = nullptr;
        hide();
    }

    void updateProgress(quint64 inspected)
    {
        status_->setText(inspected == 0
            ? QCoreApplication::translate("HarkenHub", "Processing SDK files...")
            : QCoreApplication::translate("HarkenHub", "Processing SDK files... Items inspected: %1").arg(inspected));
    }

protected:
    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (cancelled_ && event->type() == QEvent::Close) {
            event->ignore();
            return true;
        }
        return QWidget::eventFilter(object, event);
    }

private:
    QLabel* status_;
    QPushButton* cancel_;
    std::atomic<bool>* cancelled_ = nullptr;
};

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
        progress_ = new IntegrationTaskProgress(this);
        layout->addWidget(progress_);
        progress_->begin(cancelled, cancellable);
    }

    ~IntegrationTaskDialog() override { progress_->finish(); }
    void updateProgress(quint64 inspected) { progress_->updateProgress(inspected); }
    void done(int) override {}
    void accept() override {}
    void reject() override {}

protected:
    void closeEvent(QCloseEvent* event) override { event->ignore(); }

private:
    IntegrationTaskProgress* progress_;
};

template<class Task>
auto runIntegrationTask(QWidget* parent, Task task, bool cancellable = false,
                        IntegrationTaskProgress* inlineProgress = nullptr)
{
    QWidget* window = parent->window();
    const bool enabled = window->isEnabled();
    if (!inlineProgress) window->setEnabled(false);
    const auto restore = qScopeGuard([window, enabled]() { window->setEnabled(enabled); });
    std::atomic<bool> cancelled{false};
    std::unique_ptr<IntegrationTaskDialog> dialog;
    if (inlineProgress) inlineProgress->begin(cancelled, cancellable);
    else {
        dialog = std::make_unique<IntegrationTaskDialog>(window, cancelled, cancellable);
        dialog->setEnabled(true);
    }
    const auto finish = qScopeGuard([inlineProgress]() {
        if (inlineProgress) inlineProgress->finish();
    });
    std::atomic<quint64> inspected{0};
    std::optional<std::invoke_result_t<Task>> result;
    QEventLoop loop;
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer progress;
    QObject::connect(&progress, &QTimer::timeout, parent, [&]() {
        const quint64 count = inspected.load(std::memory_order_relaxed);
        if (inlineProgress) inlineProgress->updateProgress(count);
        else {
            dialog->updateProgress(count);
            if (elapsed.elapsed() >= 300 && !dialog->isVisible()) dialog->show();
        }
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
