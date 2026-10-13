#pragma once

#include <QtGlobal>
#include <QCryptographicHash>
#include <QFile>

#include <atomic>
#include <utility>

namespace VNH {

struct IntegrationScanStopped {};

class IntegrationProgress
{
public:
    IntegrationProgress(std::atomic<quint64>& inspected, std::atomic<bool>& cancelled)
        : inspected_(inspected), cancelled_(cancelled), previous_(std::exchange(current_, this)) {}
    ~IntegrationProgress() { current_ = previous_; }
    IntegrationProgress(const IntegrationProgress&) = delete;
    IntegrationProgress& operator=(const IntegrationProgress&) = delete;

    static void inspectedEntry()
    {
        checkpoint();
        if (current_) current_->inspected_.fetch_add(1, std::memory_order_relaxed);
    }

    static bool cancelled() { return current_ && current_->cancelled_.load(std::memory_order_relaxed); }
    static void checkpoint() { if (cancelled()) throw IntegrationScanStopped{}; }
    static bool addFileData(QFile& file, QCryptographicHash& hash)
    {
        while (!file.atEnd()) {
            checkpoint();
            const QByteArray block = file.read(1024 * 1024);
            if (file.error() != QFileDevice::NoError) return false;
            hash.addData(block);
        }
        return true;
    }

private:
    inline static thread_local IntegrationProgress* current_ = nullptr;
    std::atomic<quint64>& inspected_;
    std::atomic<bool>& cancelled_;
    IntegrationProgress* previous_;
};

}
