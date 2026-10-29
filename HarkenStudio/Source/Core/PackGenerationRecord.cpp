#include "PackGenerationRecord.h"
#include <algorithm>

namespace VNX {

QJsonObject PackGenerationRecord::toJson() const
{
    return {{QStringLiteral("packName"), packName},
            {QStringLiteral("objectID"), static_cast<qint64>(objectID)},
            {QStringLiteral("source"), source},
            {QStringLiteral("unitKey"), resolvedUnitKey()},
            {QStringLiteral("platform_id"), platform},
            {QStringLiteral("platform_name"), platformName},
            {QStringLiteral("base_platform"), ProjectPlatformSemantics::basePlatformName(basePlatform)},
            {QStringLiteral("language"), language},
            {QStringLiteral("success"), success},
            {QStringLiteral("canceled"), canceled},
            {QStringLiteral("status"), status},
            {QStringLiteral("outputPath"), outputPath},
            {QStringLiteral("warnings"), warnings},
            {QStringLiteral("errors"), errors},
            {QStringLiteral("bytes"), bytes},
            {QStringLiteral("elapsedMs"), elapsedMs}};
}

QString PackGenerationRecord::resolvedUnitKey() const
{
    if (!unitKey.isEmpty()) return unitKey;
    if (source == QLatin1String("Action") && objectID != 0)
        return QStringLiteral("a:") + QString::number(objectID);
    return source + QLatin1Char(':') + packName;
}

PackGenerationRecordStore& PackGenerationRecordStore::instance()
{
    static PackGenerationRecordStore store;
    return store;
}

void PackGenerationRecordStore::add(const PackGenerationRecord& record)
{
    for (auto& current : records_) {
        if (current.platform == record.platform && current.language == record.language
            && current.resolvedUnitKey() == record.resolvedUnitKey()) {
            current = record;
            emit changed();
            return;
        }
    }
    records_.push_back(record);
    emit changed();
}

void PackGenerationRecordStore::replace(const QVector<PackGenerationRecord>& records)
{
    records_.clear();
    for (const auto& record : records) {
        auto existing = std::find_if(records_.begin(), records_.end(), [&](const auto& current) {
            return current.platform == record.platform && current.language == record.language
                && current.resolvedUnitKey() == record.resolvedUnitKey();
        });
        if (existing == records_.end()) records_.push_back(record);
        else *existing = record;
    }
    emit changed();
}

void PackGenerationRecordStore::clear()
{
    const bool exporting = isExporting();
    cancelExport();
    finishPreflight(activeRunId_);
    activeRunId_ = 0;
    cancel_.reset();
    records_.clear();
    progress_.clear();
    globalObservation_.reset();
    emit globalObservationChanged();
    emit changed();
    if (exporting) emit exportActivityChanged(false);
}

const PackGenerationRecord* PackGenerationRecordStore::find(
    const QString& platform, const QString& language, const QString& unitKey) const
{
    const auto found = std::find_if(records_.cbegin(), records_.cend(), [&](const auto& record) {
        return record.platform == platform && record.language == language
            && record.resolvedUnitKey() == unitKey;
    });
    return found == records_.cend() ? nullptr : &*found;
}

QVector<PackGenerationRecord> PackGenerationRecordStore::records() const
{
    return records_;
}

quint64 PackGenerationRecordStore::beginProgress(const std::shared_ptr<std::atomic_bool>& cancel)
{
    if (isExporting()) return 0;
    activeRunId_ = ++nextRunId_;
    cancel_ = cancel;
    preflighting_ = true;
    emit exportActivityChanged(true);
    emit preflightActivityChanged(true);
    return activeRunId_;
}

void PackGenerationRecordStore::finishPreflight(quint64 runId)
{
    if (!isCurrentRun(runId) || !preflighting_) return;
    preflighting_ = false;
    emit preflightActivityChanged(false);
}

void PackGenerationRecordStore::updateProgress(quint64 runId, const PackGenerationProgress& progress)
{
    if (!isCurrentRun(runId)) return;
    auto existing = std::find_if(progress_.begin(), progress_.end(), [&](const auto& entry) {
        return entry.progress.platform == progress.platform
            && entry.progress.language == progress.language
            && entry.progress.unitKey == progress.unitKey;
    });
    PackGenerationProgress next = progress;
    next.percent = std::clamp(next.percent, 0, 100);
    const bool newRun = existing == progress_.end() || existing->runId != runId;
    if (existing == progress_.end()) {
        progress_.push_back({next, runId});
    } else {
        if (existing->runId == runId) {
            if (next.stage == PackGenerationStage::Queued
                || existing->progress.stage == PackGenerationStage::Complete
                || existing->progress.stage == PackGenerationStage::Skipped
                || existing->progress.stage == PackGenerationStage::Failed
                || existing->progress.stage == PackGenerationStage::Canceled)
                return;
            next.percent = std::max(next.percent, existing->progress.percent);
        }
        if (newRun || (next.stage != PackGenerationStage::Compiling
                       && next.stage != PackGenerationStage::Publishing))
            existing->observation.reset();
        existing->progress = next;
        existing->runId = runId;
    }
    if (newRun && next.stage == PackGenerationStage::Queued) {
        records_.erase(std::remove_if(records_.begin(), records_.end(), [&](const auto& record) {
            return record.platform == next.platform && record.language == next.language
                && record.resolvedUnitKey() == next.unitKey;
        }), records_.end());
    }
    emit progressChanged(next.platform, next.language, next.unitKey);
}

void PackGenerationRecordStore::updateObservation(quint64 runId, const PackBuildObservation& observation)
{
    if (!isCurrentRun(runId)) return;
    if (observation.unitKey.isEmpty()) {
        switch (observation.stage) {
        case PackBuildObservationStage::Preflight:
        case PackBuildObservationStage::PreparingMedia:
        case PackBuildObservationStage::CompilingProject:
        case PackBuildObservationStage::WritingPacks:
        case PackBuildObservationStage::Publishing:
        case PackBuildObservationStage::Finalizing:
            globalObservation_ = observation;
            emit globalObservationChanged();
            break;
        default:
            break;
        }
        return;
    }
    if (observation.stage != PackBuildObservationStage::Prepared
        && observation.stage != PackBuildObservationStage::ReadyToPublish)
        return;
    const auto existing = std::find_if(progress_.begin(), progress_.end(), [&](const auto& entry) {
        return entry.runId == runId && entry.progress.platform == observation.platform
            && entry.progress.language == observation.language
            && entry.progress.unitKey == observation.unitKey;
    });
    if (existing == progress_.end()
        || existing->progress.stage == PackGenerationStage::Complete
        || existing->progress.stage == PackGenerationStage::Skipped
        || existing->progress.stage == PackGenerationStage::Failed
        || existing->progress.stage == PackGenerationStage::Canceled)
        return;
    existing->observation = observation;
    emit progressChanged(observation.platform, observation.language, observation.unitKey);
}

void PackGenerationRecordStore::finishProgress(quint64 runId, bool canceled)
{
    if (!isCurrentRun(runId)) return;
    finishPreflight(runId);
    QVector<PackGenerationProgress> unfinished;
    for (auto& entry : progress_) {
        if (entry.runId == runId) entry.observation.reset();
        if (entry.runId != runId
            || entry.progress.stage == PackGenerationStage::Complete
            || entry.progress.stage == PackGenerationStage::Skipped
            || entry.progress.stage == PackGenerationStage::Failed
            || entry.progress.stage == PackGenerationStage::Canceled)
            continue;
        entry.progress.stage = canceled ? PackGenerationStage::Canceled : PackGenerationStage::Failed;
        unfinished.push_back(entry.progress);
    }
    activeRunId_ = 0;
    cancel_.reset();
    globalObservation_.reset();
    emit globalObservationChanged();
    for (const auto& progress : unfinished)
        emit progressChanged(progress.platform, progress.language, progress.unitKey);
    emit exportActivityChanged(false);
}

const PackGenerationProgress* PackGenerationRecordStore::findProgress(
    const QString& platform, const QString& language, const QString& unitKey) const
{
    const auto found = std::find_if(progress_.cbegin(), progress_.cend(), [&](const auto& entry) {
        return entry.progress.platform == platform && entry.progress.language == language
            && entry.progress.unitKey == unitKey;
    });
    return found == progress_.cend() ? nullptr : &found->progress;
}

const PackBuildObservation* PackGenerationRecordStore::globalObservation() const
{
    return globalObservation_ ? &*globalObservation_ : nullptr;
}

const PackBuildObservation* PackGenerationRecordStore::findObservation(
    const QString& platform, const QString& language, const QString& unitKey) const
{
    const auto found = std::find_if(progress_.cbegin(), progress_.cend(), [&](const auto& entry) {
        return entry.runId == activeRunId_ && entry.progress.platform == platform
            && entry.progress.language == language && entry.progress.unitKey == unitKey;
    });
    return found == progress_.cend() || !found->observation ? nullptr : &*found->observation;
}

bool PackGenerationRecordStore::isExporting() const
{
    return activeRunId_ != 0;
}

bool PackGenerationRecordStore::isCurrentRun(quint64 runId) const
{
    return runId != 0 && runId == activeRunId_;
}

void PackGenerationRecordStore::cancelExport()
{
    if (cancel_) cancel_->store(true, std::memory_order_relaxed);
}

}
