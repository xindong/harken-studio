
#include "LMDBQueryTask.h"
#include "LMDBQueryPipeline.h"
#include "LMDBService.h"
#include <QThread>

namespace VNX {


LMDBQueryTask::LMDBQueryTask(LMDBService* service, const QString& query,
                             int maxResults, int budgetMs)
    : service_(service)
    , query_(query)
    , maxResults_(maxResults)
    , budgetMs_(budgetMs)
{
    setAutoDelete(false);
}

LMDBQueryTask::~LMDBQueryTask() = default;


void LMDBQueryTask::run()
{
    if (cancelled_.load(std::memory_order_relaxed)) {
        emit searchCancelled();
        return;
    }

    if (!service_ || !service_->isOpen()) {
        emit searchError(QStringLiteral("LMDBService not available"));
        return;
    }

    pipeline_ = std::make_unique<LMDBQueryPipeline>(service_);
    pipeline_->setQuery(query_);
    pipeline_->setMaxResults(maxResults_);

    if (budgetMs_ > 0)
        executeWithBudget();
    else
        executeNoBudget();

    pipeline_.reset();
}


void LMDBQueryTask::executeWithBudget()
{
    while (!pipeline_->isFinished() && !cancelled_.load(std::memory_order_relaxed)) {
        QElapsedTimer stepTimer;
        stepTimer.start();

        PipelineStatus status = pipeline_->step(32);

        emit progressChanged(pipeline_->currentStage(), pipeline_->stageName());

        if (status == PipelineStatus::Error) {
            emit searchError(QStringLiteral("Pipeline error at stage: ") +
                             pipeline_->stageName());
            return;
        }

        if (stepTimer.elapsed() > budgetMs_)
            QThread::yieldCurrentThread();
    }

    if (cancelled_.load(std::memory_order_relaxed)) {
        emit searchCancelled();
        return;
    }

    const auto& results = pipeline_->results();
    if (callback_)
        callback_(results);
    emit searchCompleted(results);
}


void LMDBQueryTask::executeNoBudget()
{
    while (!pipeline_->isFinished() && !cancelled_.load(std::memory_order_relaxed)) {
        PipelineStatus status = pipeline_->step(32);

        emit progressChanged(pipeline_->currentStage(), pipeline_->stageName());

        if (status == PipelineStatus::Error) {
            emit searchError(QStringLiteral("Pipeline error at stage: ") +
                             pipeline_->stageName());
            return;
        }
    }

    if (cancelled_.load(std::memory_order_relaxed)) {
        emit searchCancelled();
        return;
    }

    const auto& results = pipeline_->results();
    if (callback_)
        callback_(results);
    emit searchCompleted(results);
}


void LMDBQueryTask::cancel()
{
    cancelled_.store(true, std::memory_order_relaxed);
}

}
