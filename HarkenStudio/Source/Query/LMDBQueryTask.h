
#pragma once

#include <QRunnable>
#include <QObject>
#include <QString>
#include <QElapsedTimer>
#include <vector>
#include <memory>
#include <atomic>
#include <functional>

#include "LMDBQueryPipeline.h"

namespace VNX {

class LMDBService;

class LMDBQueryTask : public QObject, public QRunnable {
    Q_OBJECT
public:
    using ResultCallback = std::function<void(const std::vector<SearchResult>&)>;

    LMDBQueryTask(LMDBService* service, const QString& query,
                  int maxResults = 100, int budgetMs = 16);
    ~LMDBQueryTask() override;

    void run() override;
    void cancel();
    bool isCancelled() const { return cancelled_.load(std::memory_order_relaxed); }

    void setResultCallback(ResultCallback cb) { callback_ = std::move(cb); }

signals:
    void searchCompleted(const std::vector<VNX::SearchResult>& results);
    void searchCancelled();
    void searchError(const QString& message);
    void progressChanged(int stage, const QString& stageName);

private:
    void executeWithBudget();
    void executeNoBudget();

    LMDBService* service_;
    QString query_;
    int maxResults_;
    int budgetMs_;
    std::atomic<bool> cancelled_{false};
    ResultCallback callback_;
    std::unique_ptr<LMDBQueryPipeline> pipeline_;
};

}
