#include "Tooling/Harkenizer/Core/PackMediaPreparation.h"

#include "SoundSourceSemantics.h"
#include "Utils/ObjectId.h"

#include <QMutex>
#include <QMutexLocker>
#include <QScopeGuard>
#include <QThread>
#include <QThreadPool>
#include <QWaitCondition>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <exception>
#include <future>
#include <map>
#include <utility>
#include <vector>

namespace VNX {

struct PackMediaPreparation::Impl
{
    struct Work
    {
        harkenstudio::ObjectId soundId;
        SoundSourceDef source;
        double scheduleOffsetSec;
        std::promise<Result> result;
    };

    struct Group
    {
        uint32_t sourceId;
        std::vector<Work> items;
    };

    using Key = std::pair<harkenstudio::ObjectId, QUuid>;

    MediaBuildSnapshot snapshot;
    std::shared_ptr<SoundSourceDerivative::MaterializationCache> cache;
    ProjectPlatformId platform;
    QString derivativeDir;
    QThreadPool pool;
    std::vector<std::shared_ptr<Group>> groups;
    std::map<uint32_t, size_t> groupIndices;
    std::map<Key, std::deque<std::future<Result>>> results;
    std::atomic<bool> stopped{false};
    QMutex budgetMutex;
    QWaitCondition budgetChanged;
    uint64_t activeBytes = 0;
    static constexpr uint64_t byteLimit = 64u * 1024u * 1024u;
    bool started = false;

    Impl(const MediaBuildSnapshot& source,
         std::shared_ptr<SoundSourceDerivative::MaterializationCache> materializations,
         ProjectPlatformId platformId, QString directory)
        : snapshot(source), cache(std::move(materializations)), platform(platformId),
          derivativeDir(std::move(directory))
    {
        pool.setMaxThreadCount(std::clamp(QThread::idealThreadCount(), 1, 8));
    }

    uint64_t estimateBytes(const Work& work, const Result& result) const
    {
        const auto& metadata = result.conversion->asset.metadata;
        const auto& settings = result.conversion->settings;
        if (metadata.sampleRate == 0 || metadata.channels == 0
            || !std::isfinite(work.scheduleOffsetSec) || work.scheduleOffsetSec < 0.0
            || (settings.performResample && settings.targetSampleRate == 0))
            return byteLimit + 1;
        const uint64_t start = std::min(result.source.trimStartFrame, metadata.totalSamples);
        const uint64_t end = std::clamp(result.source.trimEndFrame.value_or(metadata.totalSamples),
            start, metadata.totalSamples);
        const long double frameBytes = static_cast<long double>(metadata.channels) * sizeof(float);
        const long double decodedBytes = static_cast<long double>(metadata.totalSamples) * frameBytes;
        const long double trimmedFrames = static_cast<long double>(end - start);
        const long double outputRate = settings.performResample
            ? std::max(metadata.sampleRate, settings.targetSampleRate) : metadata.sampleRate;
        const long double outputFrames = std::ceil(trimmedFrames * outputRate / metadata.sampleRate)
            + std::ceil(static_cast<long double>(work.scheduleOffsetSec) * outputRate);
        const long double bytes = 3.0L * (decodedBytes + trimmedFrames * frameBytes
            + outputFrames * frameBytes);
        return !std::isfinite(bytes) || bytes > byteLimit
            ? byteLimit + 1 : static_cast<uint64_t>(std::ceil(bytes));
    }

    bool acquire(uint64_t bytes)
    {
        QMutexLocker lock(&budgetMutex);
        while (!stopped.load(std::memory_order_acquire)) {
            if (activeBytes == 0
                || (bytes <= byteLimit && activeBytes <= byteLimit - bytes)) {
                activeBytes += bytes;
                return true;
            }
            budgetChanged.wait(&budgetMutex);
        }
        return false;
    }

    void release(uint64_t bytes)
    {
        QMutexLocker lock(&budgetMutex);
        activeBytes -= bytes;
        budgetChanged.wakeAll();
    }

    void requestStop()
    {
        QMutexLocker lock(&budgetMutex);
        stopped.store(true, std::memory_order_release);
        budgetChanged.wakeAll();
    }

    Result prepare(const Work& work, const std::optional<MediaConversionRequest>& conversion,
                   const QString& captureError)
    {
        Result result;
        result.source = work.source;
        if (stopped.load(std::memory_order_acquire)) {
            result.error = QStringLiteral("Export canceled");
            return result;
        }
        result.conversion = conversion;
        if (!conversion || !conversion->sourceMatches()) {
            result.error = conversion
                ? QStringLiteral("Source file changed or is unavailable for Sound %1")
                    .arg(VNX::objectIdString(work.soundId))
                : captureError;
            return result;
        }
        result.source = SoundSourceSemantics::authoredView(work.source, &conversion->asset);
        const uint64_t bytes = estimateBytes(work, result);
        if (!acquire(bytes)) {
            result.error = QStringLiteral("Export canceled");
            return result;
        }
        const auto releaseBudget = qScopeGuard([&] { release(bytes); });
        QString derivativeError;
        result.artifact = SoundSourceDerivative::materialize(
            conversion->inputPath(), result.source, work.scheduleOffsetSec,
            platform, derivativeDir, conversion->settings, &derivativeError,
            cache.get(), conversion->sourceInput.get());
        if (stopped.load(std::memory_order_acquire)) {
            result.error = QStringLiteral("Export canceled");
            return result;
        }
        if (!conversion->sourceMatches()) {
            result.error = QStringLiteral("Source file changed during Sound conversion");
            return result;
        }
        result.conversionError = derivativeError;
        if (!result.artifact) {
            result.artifact = SoundSourceDerivative::materialize(
                conversion->inputPath(), result.source, work.scheduleOffsetSec,
                platform, derivativeDir, ConversionSettings{}, &derivativeError,
                cache.get(), conversion->sourceInput.get());
            if (stopped.load(std::memory_order_acquire)) {
                result.error = QStringLiteral("Export canceled");
                return result;
            }
            result.fallback = result.artifact.has_value();
        }
        if (!result.artifact || !conversion->sourceMatches())
            result.error = QStringLiteral("Derivative media failed for Sound %1: %2")
                .arg(VNX::objectIdString(work.soundId)).arg(derivativeError);
        return result;
    }

    void run(Group& group)
    {
        std::optional<MediaConversionRequest> conversion;
        QString captureError;
        std::exception_ptr captureException;
        try {
            if (!stopped.load(std::memory_order_acquire))
                conversion = snapshot.request(group.sourceId, platform, &captureError);
        } catch (...) {
            captureException = std::current_exception();
        }
        for (auto& work : group.items) {
            try {
                if (captureException) work.result.set_exception(captureException);
                else work.result.set_value(prepare(work, conversion, captureError));
            } catch (...) {
                work.result.set_exception(std::current_exception());
            }
        }
    }
};

PackMediaPreparation::PackMediaPreparation(
    const MediaBuildSnapshot& snapshot,
    std::shared_ptr<SoundSourceDerivative::MaterializationCache> cache,
    ProjectPlatformId platform, QString derivativeDir)
    : impl_(std::make_unique<Impl>(snapshot, std::move(cache), platform, std::move(derivativeDir)))
{
}

PackMediaPreparation::~PackMediaPreparation()
{
    stop();
}

void PackMediaPreparation::add(harkenstudio::ObjectId soundId, const SoundSourceDef& source,
                              double scheduleOffsetSec)
{
    auto [found, inserted] = impl_->groupIndices.emplace(source.sourceId, impl_->groups.size());
    if (inserted)
        impl_->groups.push_back(std::make_shared<Impl::Group>(Impl::Group{source.sourceId, {}}));
    Impl::Work work{soundId, source, scheduleOffsetSec, {}};
    impl_->results[{soundId, source.entryId}].push_back(work.result.get_future());
    impl_->groups[found->second]->items.push_back(std::move(work));
}

void PackMediaPreparation::start()
{
    if (impl_->started) return;
    impl_->started = true;
    for (const auto& group : impl_->groups)
        impl_->pool.start([impl = impl_.get(), group] { impl->run(*group); });
}

PackMediaPreparation::Result PackMediaPreparation::take(
    harkenstudio::ObjectId soundId, const QUuid& entryId,
    const std::function<bool()>& cancelCheck)
{
    const auto canceled = [&] { return cancelCheck && cancelCheck(); };
    const auto cancel = [&] {
        impl_->requestStop();
        Result result;
        result.error = QStringLiteral("Export canceled");
        return result;
    };
    if (canceled()) return cancel();
    const auto found = impl_->results.find({soundId, entryId});
    if (found == impl_->results.end() || found->second.empty()) {
        Result result;
        result.error = QStringLiteral("Media preparation is missing for Sound %1")
            .arg(VNX::objectIdString(soundId));
        return result;
    }
    auto& future = found->second.front();
    while (future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready)
        if (canceled()) return cancel();
    auto result = future.get();
    found->second.pop_front();
    if (canceled()) return cancel();
    if (result.error.isEmpty() && result.conversion && !result.conversion->sourceMatches())
        result.error = QStringLiteral("Source file changed during Sound conversion");
    return result;
}

void PackMediaPreparation::stop()
{
    impl_->requestStop();
    impl_->pool.waitForDone();
    impl_->results.clear();
    impl_->groups.clear();
    impl_->groupIndices.clear();
}

}
