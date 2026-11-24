
#include "AudioEngine.h"
#include "AssetPipeline/MediaPool.h"
#include "AssetPipeline/AssetRegistry.h"
#include "BindingRuntimeLowering.h"
#include "LanguageContextService.h"
#include "PlatformContextService.h"
#include "ProjectManager.h"
#include "Tooling/Harkenizer/Core/CurrentWorldProjectAdapter.h"
#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapterPrivate.h"
#include "RuntimeSyncCoordinator.h"
#include "HarkenMath.h"
#include "RuntimeBridge/CurrentWorldSyncBridge.h"
#include "Core/Build/CurrentWorldCompile.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"
#include "RuntimeBridge/ProjectSyncBridge.h"
#include "RuntimeBridge/PreviewBridge.h"
#include "Core/Build/PackagingBridge.h"
#include <harken_studio_build/PackImage.h>
#include "VariableRehearsal.h"


#include <cstring>
#include <utility>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QCoreApplication>
#include <QThread>
#include <algorithm>

namespace harken::studio {


struct CurrentWorldCompilation {
    struct Job {
        bridge::CurrentWorldSyncToken token;
        build::CurrentWorldSyncRequestPtr request;
        std::vector<build::CurrentWorldScalarUpdate> updates;
    };
    struct Result {
        bridge::CurrentWorldSyncToken token;
        build::CurrentWorldSyncRequestPtr request;
        build::CurrentWorldCompileResult compiled;
    };

    CurrentWorldCompilation() : worker([this] { run(); }) {}
    ~CurrentWorldCompilation() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            pending.reset();
        }
        ready.notify_one();
        worker.join();
    }

    void submit(Job job) {
        {
            std::lock_guard lock(mutex);
            pending = std::move(job);
        }
        ready.notify_one();
    }

    void invalidate() {
        {
            std::lock_guard lock(mutex);
            pending.reset();
            completed.reset();
            published.reset();
            resetRequested = true;
        }
        ready.notify_one();
    }

    void acknowledgePublication(bridge::CurrentWorldSyncToken token) {
        {
            std::lock_guard lock(mutex);
            published = token;
        }
        ready.notify_one();
    }

    std::optional<Result> takeResult() {
        std::lock_guard lock(mutex);
        auto result = std::move(completed);
        completed.reset();
        return result;
    }

    void run() {
        build::CurrentWorldRequestCache cache;
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [this] {
                    return stopping || resetRequested || published.has_value()
                        || pending.has_value();
                });
                if (stopping) return;
                if (resetRequested) {
                    resetRequested = false;
                    lock.unlock();
                    cache = {};
                    continue;
                }
                if (published) {
                    const auto token = *published;
                    published.reset();
                    lock.unlock();
                    try {
                        cache.acknowledgePublication(token);
                    } catch (...) {
                    }
                    continue;
                }
                job = std::move(*pending);
                pending.reset();
            }
            Result result;
            result.token = job.token;
            try {
                result.request = cache.update(std::move(job.request), job.updates, job.token);
                if (result.request)
                    result.compiled = build::compileCurrentWorldRequest(*result.request, true);
            } catch (...) {
                result.compiled.status = bridge::CurrentWorldSyncStatus::CompileFailed;
            }
            {
                std::lock_guard lock(mutex);
                if (stopping) return;
                if (!resetRequested)
                    completed = std::move(result);
            }
        }
    }

    std::mutex mutex;
    std::condition_variable ready;
    std::optional<Job> pending;
    std::optional<Result> completed;
    std::optional<bridge::CurrentWorldSyncToken> published;
    bool stopping = false;
    bool resetRequested = false;
    std::thread worker;
};

static_assert(static_cast<uint8_t>(AudioEngine::ExecErrorDomain::Unclassified)
                  == bridge::kExecErrorDomainUnclassified,
              "EngineNotification::domain Unclassified must match AudioEngine");

namespace {

bool preparedSoundsAreCurrent(const build::CurrentWorldSyncRequestPtr& request)
{
    if (!request || !request->soundPreparation) return true;
    const auto& preparation = *request->soundPreparation;
    for (const auto& source : preparation.sources) {
        const auto* asset = AssetRegistry::instance().getAsset(source.sourceId);
        if ((asset != nullptr) != source.assetAvailable
            || (asset && (asset->absolutePath != source.path
                || asset->contentHash != source.contentHash)))
            return false;
    }
    for (const auto& sound : preparation.sounds) {
        const auto revision = preparation.contentRevisions.find(sound.binding.soundId);
        if (sound.samples.empty() && !sound.media && revision != preparation.contentRevisions.end()
            && bridge::registeredSoundRevision(sound.binding.soundId) != revision->second)
            return false;
    }
    return true;
}

int runtimeServiceIntervalMs(const bridge::EngineSession& session)
{
    constexpr int kMaximumIntervalMs = 5;
    const double blockDurationMs = session.outputLatency() * 1000.0;
    return std::clamp(static_cast<int>(blockDurationMs), 1, kMaximumIntervalMs);
}

bridge::EngineResult currentWorldPlaybackGate(
    const RuntimeSyncCoordinator::RevisionBarrierSnapshot& barrier)
{
    if (barrier.state == RuntimeSyncCoordinator::RevisionBarrierState::Ready)
        return bridge::kEngineOk;
    if (barrier.state == RuntimeSyncCoordinator::RevisionBarrierState::Pending)
        return bridge::kEngineBackpressure;
    return bridge::kEngineNotInit;
}

bool sameBindingProgram(const BindingRuntimeProgram& a, const BindingRuntimeProgram& b)
{
    return a.bindingId == b.bindingId && a.variableId == b.variableId
        && a.targetScopeKind == b.targetScopeKind && a.targetId == b.targetId
        && a.transitionMs == b.transitionMs
        && a.points.size() == b.points.size() && a.handles.size() == b.handles.size()
        && std::equal(a.points.begin(), a.points.end(), b.points.begin(),
            [](const auto& x, const auto& y) { return x.x == y.x && x.value == y.value; })
        && std::equal(a.handles.begin(), a.handles.end(), b.handles.begin(),
            [](const auto& x, const auto& y) {
                return x.h1x == y.h1x && x.h1y == y.h1y && x.h2x == y.h2x && x.h2y == y.h2y;
            });
}

}

ProjectItem* findItemByPathInProject(ProjectData* project, const QString& path);

ProjectItem* findItemByShortIDInProject(ProjectData* project, uint32_t shortID);

AudioEngine* AudioEngine::instance_ = nullptr;

AudioEngine& AudioEngine::instance()
{
    if (!instance_)
    {
        instance_ = new AudioEngine();
    }
    return *instance_;
}

AudioEngine::AudioEngine(QObject* parent)
    : QObject(parent)
{
    instance_ = this;
    bridge::setMediaServices(bridge::makeBuildMediaServices());
    currentWorldCompilation_ = std::make_unique<CurrentWorldCompilation>();
    runtimeSyncCoordinator_ = std::make_unique<RuntimeSyncCoordinator>(
        [this](bridge::CurrentWorldSyncToken token) {
            while (const auto completion = bridge::takeCurrentWorldSyncCompletion()) {
                if (completion->ok())
                    currentWorldCompilation_->acknowledgePublication(completion->token);
                runtimeSyncCoordinator_->acceptCurrentWorldSyncResult(*completion);
            }
            if (const auto superseded = bridge::discardPendingCurrentWorld(
                    bridge::PendingCurrentWorldDisposition::Superseded)) {
                runtimeSyncCoordinator_->acceptCurrentWorldSyncResult(*superseded);
            }
            auto* project = ProjectManager::instance().currentProject();
            if (project == nullptr) {
                currentWorldCompilation_->invalidate();
                currentWorldPreviewMutations_.clear();
                currentWorldRequestSession_ = 0;
                currentWorldRequestEpoch_ = 0;
                return build::clearCurrentWorld(token);
            }
            bindCurrentWorldPlatformContext();
            QString error;
            const auto scope = currentWorldPlatformScope(
                PlatformContextService::instance().context());
            const auto& mutations = runtimeSyncCoordinator_->submittedMutations();
            CurrentWorldCompilation::Job job;
            job.token = token;
            bool incremental = !mutations.full
                && currentWorldRequestSession_ == token.projectSessionId
                && currentWorldRequestEpoch_ == token.runtimeEpoch;
            if (incremental) {
                auto properties = std::move(currentWorldPreviewMutations_);
                currentWorldPreviewMutations_.clear();
                for (const auto& mutation : mutations.properties) {
                    const auto key = std::make_pair(mutation.stableId, mutation.propertyId);
                    if (std::find(properties.begin(), properties.end(), key) == properties.end())
                        properties.push_back(key);
                }
                for (const auto& [stableId, propertyId] : properties) {
                    auto* item = ProjectManager::instance().findItemByRuntimeStableId(stableId);
                    build::CurrentWorldScalarUpdate update;
                    if (!item || !readCurrentWorldScalarUpdate(
                            *item, scope, propertyId, &update)) {
                        incremental = false;
                        job.updates.clear();
                        break;
                    }
                    job.updates.push_back(update);
                }
            }
            if (incremental) {
                currentWorldCompilation_->submit(std::move(job));
                bridge::CurrentWorldSyncResult result;
                result.token = token;
                result.status = bridge::CurrentWorldSyncStatus::Pending;
                return result;
            }
            currentWorldRequestSession_ = 0;
            currentWorldRequestEpoch_ = 0;
            currentWorldPreviewMutations_.clear();
            const auto sourceIndex = bridge::canonicalRuntimeProjectSourceIndex(*project);
            const auto fieldGroups = buildFieldGroupEffectiveSnapshot(
                *project, toFieldGroupScope(scope), *sourceIndex);
            auto sounds = std::make_shared<bridge::CurrentWorldSoundPreparation>();
            if (!ProjectManager::prepareRuntimeSounds(fieldGroups, *sounds, &error)) {
                qWarning().noquote()
                    << "[CurrentWorldSync] Sound preparation rejected:" << error;
                bridge::CurrentWorldSyncResult result;
                result.status = bridge::CurrentWorldSyncStatus::InvalidProject;
                result.token = token;
                return result;
            }
            CurrentWorldBuildOptions options;
            options.emitEmptySignalTables = true;
            options.isolateUnavailable = true;
            options.soundContentRevisions = &sounds->contentRevisions;
            options.sourceIndex = sourceIndex;
            options.fieldGroups = &fieldGroups;
            auto request = buildCurrentWorldSyncRequest(
                *project, scope,
                &error, nullptr, &options, token.authoringRevision);
            if (!request) {
                qWarning().noquote()
                    << "[CurrentWorldSync] authoring rejected:" << error;
                bridge::CurrentWorldSyncResult result;
                result.status = bridge::CurrentWorldSyncStatus::InvalidProject;
                result.token = token;
                return result;
            }
            request->soundPreparation = std::move(sounds);
            job.request = std::move(request);
            currentWorldRequestSession_ = token.projectSessionId;
            currentWorldRequestEpoch_ = token.runtimeEpoch;
            currentWorldCompilation_->submit(std::move(job));
            bridge::CurrentWorldSyncResult result;
            result.token = token;
            result.status = bridge::CurrentWorldSyncStatus::Pending;
            return result;
        });

    runtimeServiceTimer_ = new QTimer(this);
    runtimeServiceTimer_->setObjectName(QStringLiteral("runtimeServiceTimer"));
    runtimeServiceTimer_->setTimerType(Qt::PreciseTimer);
    connect(runtimeServiceTimer_, &QTimer::timeout,
            this, &AudioEngine::onRuntimeServiceTimer);

    meterTimer_ = new QTimer(this);
    meterTimer_->setObjectName(QStringLiteral("meterTimer"));
    meterTimer_->setTimerType(Qt::PreciseTimer);
    connect(meterTimer_, &QTimer::timeout, this, &AudioEngine::onMeterTimer);

    monitorTimer_ = new QTimer(this);
    monitorTimer_->setObjectName(QStringLiteral("monitorTimer"));
    connect(monitorTimer_, &QTimer::timeout, this, &AudioEngine::onMonitorTimer);
    connect(runtimeSyncCoordinator_.get(), &RuntimeSyncCoordinator::barrierChanged,
            this, &AudioEngine::refreshPlaybackPaths);
}

void AudioEngine::bindVariableRehearsal(VariableRehearsal& rehearsal)
{
    if (variableRehearsal_ == &rehearsal)
        return;

    QObject::disconnect(variableRehearsalConnection_);
    variableRehearsal_ = &rehearsal;
    variableRehearsalConnection_ = connect(
        variableRehearsal_, &VariableRehearsal::rehearsalChanged, this,
        [this](const QUuid& variableId) {
            capturePreviewVariableOverride(variableId);
        });
}

AudioEngine::~AudioEngine()
{
    shutdown();
    currentWorldCompilation_.reset();

    if (instance_ == this)
    {
        instance_ = nullptr;
    }
}

bool AudioEngine::initialize(int sampleRate, int bufferSize, int maxVoices)
{
    bridge::EngineSessionConfig config;
    config.sampleRate = sampleRate;
    config.bufferSize = bufferSize;
    config.maxVoices = maxVoices;

    return initialize(config);
}

void AudioEngine::bindCurrentWorldPlatformContext()
{
    if (currentWorldPlatformBound_)
        return;
    currentWorldPlatformBound_ = true;
    const auto trackProperty = [this](ProjectItem* item, const QString& property) {
        if (property == QLatin1String("dirty") || property == QLatin1String("sourceName")) return;
        const auto* field = PlayablePropertyAuthoringV1::fieldByName(property);
        using Property = harkenplayableproperties::PlayablePropertyIdV1;
        if (item && item->type != ProjectItem::Type::Bus && field
            && (field->id == static_cast<std::uint8_t>(Property::Volume)
                || field->id == static_cast<std::uint8_t>(Property::Pitch))) {
            const auto key = std::make_pair(item->runtimeId(), field->id);
            if (std::find(currentWorldPreviewMutations_.begin(),
                          currentWorldPreviewMutations_.end(), key)
                == currentWorldPreviewMutations_.end())
                currentWorldPreviewMutations_.push_back(key);
            return;
        }
        currentWorldRequestSession_ = 0;
        currentWorldRequestEpoch_ = 0;
    };
    connect(&ProjectManager::instance(), &ProjectManager::itemPropertyPreviewed,
            this, trackProperty);
    connect(&ProjectManager::instance(), &ProjectManager::itemPropertyChanged,
            this, trackProperty);
    connect(&MediaPool::instance(), &MediaPool::mediaHotSwapped, this,
            &ProjectManager::resyncRefreshedSourceToRuntime, Qt::QueuedConnection);
    lastCurrentWorldPrimary_ =
        PlatformContextService::instance().context().primary;
    connect(&PlatformContextService::instance(),
            &PlatformContextService::contextChanged, this,
            [this](const PlatformContext& context) {
                if (context.primary == lastCurrentWorldPrimary_)
                    return;
                lastCurrentWorldPrimary_ = context.primary;
                resetLivePropertyPreview();
                if (const auto* project = ProjectManager::instance().currentProject()) {
                    std::vector<ProjectItem*> sounds;
                    for (auto* item : ProjectPlatformSemantics::referenceOwners(*project)) {
                        if (item->comp<VSound>())
                            sounds.push_back(item);
                    }
                    ProjectManager::resyncItemsToRuntime(sounds);
                }
                runtimeSyncCoordinator_->markCurrentWorldMutation();
            });
    connect(&LanguageContextService::instance(),
            &LanguageContextService::languageChanged, this,
            [](const QString& language) {
                auto* project = ProjectManager::instance().currentProject();
                if (!project)
                    return;
                ProjectManager::instance().refreshSoundSourceProjections(
                    PlatformContextService::instance().context().primary, language);
                std::vector<ProjectItem*> sounds;
                const auto refresh = [&](ProjectItem* item, const auto& self) -> void {
                    if (!item)
                        return;
                    const auto* sound = item->type == ProjectItem::Type::Sound
                        ? item->comp<VSound>() : nullptr;
                    if (sound && sound->distinguishLanguage)
                        sounds.push_back(item);
                    for (const auto& child : item->children)
                        self(child.get(), self);
                };
                for (auto* binding : project->allFileBindings())
                    if (binding && binding->rootItem)
                        refresh(binding->rootItem.get(), refresh);
                ProjectManager::resyncItemsToRuntime(sounds);
            });
}

bool AudioEngine::initialize(const bridge::EngineSessionConfig& config)
{
    bindCurrentWorldPlatformContext();
    if (initialized_)
    {
        qWarning() << "AudioEngine already initialized";
        return true;
    }

    session_ = bridge::EngineSession::create(config);
    if (!session_)
    {
        // Engine unavailable: the editor keeps running in simulation mode
        // (initialized, no live session), exactly as before.
        qWarning() << "Harken engine unavailable - running in simulation mode";
        initialized_ = true;
        emit initializedChanged(true);
        voiceCountKnown_ = false;
        emit voiceCountUnavailable();
        return true;
    }

    if (runtimeSyncCoordinator_->runtimeStarted()
        == RuntimeSyncCoordinator::AllocationState::IdentityExhausted) {
        session_.reset();
        voiceCountKnown_ = false;
        emit voiceCountUnavailable();
        return false;
    }
    runtimeSessionActive_ = true;

    // The engine log callback fires on arbitrary runtime threads; hop to the
    // UI thread before emitting.
    session_->setLogHandler([this](int level, const QString& msg) {
        if (QThread::currentThread() == this->thread())
        {
            emit logMessage(level, msg);
            switch (level)
            {
                case 0: qInfo() << "[Harken]" << msg; break;
                case 1: qWarning() << "[Harken]" << msg; break;
                default: qCritical() << "[Harken]" << msg; break;
            }
        }
        else
        {
            QMetaObject::invokeMethod(this, [this, level, msg]() {
                emit logMessage(level, msg);
                switch (level)
                {
                    case 0: qInfo() << "[Harken]" << msg; break;
                    case 1: qWarning() << "[Harken]" << msg; break;
                    default: qCritical() << "[Harken]" << msg; break;
                }
            }, Qt::QueuedConnection);
        }
    });

    initialized_ = true;
    runtimeProjectRestoreRequired_ = false;
    emit initializedChanged(true);

    updateRuntimeServiceInterval();
    runtimeServiceTimer_->start();
    meterTimer_->start(33);
    monitorTimer_->start(100);
    onMonitorTimer();

    if (auto* project = ProjectManager::instance().currentProject())
        ProjectManager::syncProjectToRuntime(project);

    qDebug() << "AudioEngine initialized successfully";
    return true;
}

void AudioEngine::shutdown()
{
    if (!initialized_)
        return;

    runtimeServiceTimer_->stop();
    meterTimer_->stop();
    monitorTimer_->stop();

    stopAll(0.0f);

    if (playing_)
    {
        stopOutput();
    }

    releasePreviewEmitter();

    if (session_)
        stopRuntimeSynchronization();
    session_.reset();
    consumeRuntimeNotifications();
    runtimeProjectRestoreRequired_ = false;

    initialized_ = false;
    refreshPlaybackPaths();
    voiceCountKnown_ = false;
    emit initializedChanged(false);
    emit voiceCountUnavailable();

    qDebug() << "AudioEngine shutdown complete";
}

bool AudioEngine::startOutput()
{
    if (!initialized_ || !session_)
    {
        playing_ = true;
        emit playingChanged(true);
        return true;
    }

    if (session_->startOutput() != bridge::kEngineOk)
    {
        emit errorOccurred(tr("Failed to start audio output"));
        return false;
    }


    if (!runtimeSessionActive_) {
        if (runtimeSyncCoordinator_->runtimeStarted()
            == RuntimeSyncCoordinator::AllocationState::IdentityExhausted) {
            session_->stopOutput();
            emit errorOccurred(tr("Runtime synchronization identity exhausted"));
            return false;
        }
        runtimeSessionActive_ = true;
    }

    restoreCurrentProjectAfterRuntimeRestart();

    playing_ = true;
    emit playingChanged(true);
    return true;
}

void AudioEngine::stopOutput()
{
    const bool wasPlaying = playing_;
    if (!wasPlaying && !runtimeSessionActive_)
        return;

    if (session_)
    {
        releasePreviewEmitter();
        stopRuntimeSynchronization();
        session_->stopOutput();
        runtimeProjectRestoreRequired_ = true;
    }

    playing_ = false;
    consumeRuntimeNotifications();
    masterMeterPeakL_ = 0.0f;
    masterMeterPeakR_ = 0.0f;
    emit meterLevelsUpdated();
    refreshPlaybackPaths();
    if (wasPlaying)
        emit playingChanged(false);
}

void AudioEngine::restoreCurrentProjectAfterRuntimeRestart()
{
    if (!runtimeProjectRestoreRequired_)
        return;

    runtimeProjectRestoreRequired_ = false;
    if (auto* project = ProjectManager::instance().currentProject())
        ProjectManager::syncProjectToRuntime(project);
}

void AudioEngine::stopRuntimeSynchronization()
{
    if (!runtimeSessionActive_)
        return;

    const auto cancelled = bridge::discardPendingCurrentWorld();
    if (cancelled)
        runtimeSyncCoordinator_->acceptCurrentWorldSyncResult(*cancelled);
    runtimeSyncCoordinator_->runtimeStopping();
    currentWorldCompilation_->invalidate();
    currentWorldRequestSession_ = 0;
    currentWorldRequestEpoch_ = 0;
    currentWorldPreviewMutations_.clear();
    overlayMutePendingIds_.clear();
    overlaySoloPendingIds_.clear();
    overlayOutputVolumePendingValues_.clear();
    ProjectManager::instance().auditionOverlay().clearAll();

    // Project close owns Runtime-side caches until its empty-world barrier is
    // terminal. Settle that ownership while the Runtime session is still live.
    ProjectManager::instance().finishRuntimeCloseIfReady();
    runtimeSessionActive_ = false;
    runtimeBindingAttachments_.clear();
    runtimeBindingIds_.clear();
    runtimeBindingPrograms_.clear();
    runtimeBindingSyncIdentity_ = {};
    runtimeBindingsPending_ = true;
    resetLivePropertyPreview();
}

bool AudioEngine::setMasterVolume(float volume, int fadeMs)
{
    const float nextVolume = qBound(0.0f, volume, 2.0f);
    if (session_ && initialized_ && !session_->setMasterFade(nextVolume, fadeMs))
        return false;
    masterVolume_ = nextVolume;
    return true;
}

harken::studio::types::ObjectId AudioEngine::mintRawPcmSoundId()
{
    return session_ ? session_->mintRawPcmSoundId() : harken::studio::types::ObjectId{};
}

bridge::EngineSession::RawPcmSliceRegistration AudioEngine::registerRawPcmSlice(
    harken::studio::types::ObjectId soundId, const QString& absolutePath,
    uint64_t startFrame, uint64_t frameCount, bool loops)
{
    if (!session_) {
        bridge::EngineSession::RawPcmSliceRegistration registration;
        registration.soundId = soundId;
        registration.startFrame = startFrame;
        registration.frameCount = frameCount;
        registration.loops = loops;
        registration.result = bridge::kEngineNotInit;
        return registration;
    }
    return session_->registerRawPcmSlice(soundId, absolutePath, startFrame, frameCount, loops);
}

bridge::EngineSession::RawPcmSliceRegistration AudioEngine::registerRawPcmSlice(
    harken::studio::types::ObjectId soundId, std::vector<float> interleaved,
    int channels, int sampleRate, uint64_t startFrame, uint64_t frameCount, bool loops)
{
    if (!session_) {
        bridge::EngineSession::RawPcmSliceRegistration registration;
        registration.soundId = soundId;
        registration.startFrame = startFrame;
        registration.frameCount = frameCount;
        registration.loops = loops;
        registration.result = bridge::kEngineNotInit;
        return registration;
    }
    return session_->registerRawPcmSlice(soundId, std::move(interleaved), channels,
                                        sampleRate, startFrame, frameCount, loops);
}

bridge::EngineSession::RawPcmSliceRegistration AudioEngine::relocateRawPcmSlice(
    harken::studio::types::ObjectId soundId, uint64_t startFrame)
{
    if (!session_) {
        bridge::EngineSession::RawPcmSliceRegistration registration;
        registration.soundId = soundId;
        registration.startFrame = startFrame;
        registration.result = bridge::kEngineNotInit;
        return registration;
    }
    return session_->relocateRawPcmSlice(soundId, startFrame);
}

bridge::EngineResult AudioEngine::unregisterRawPcm(harken::studio::types::ObjectId soundId)
{
    return session_ ? session_->unregisterRawPcm(soundId) : bridge::kEngineNotInit;
}

uint64_t AudioEngine::playRawPcm(harken::studio::types::ObjectId soundId, bool loops)
{
    if (!session_)
        return 0;
    startOutput();
    return session_->playRawPcm(soundId, loops);
}

bool AudioEngine::stopRawPcm(uint64_t playbackHandle, uint32_t fadeMs)
{
    return session_ ? session_->stopRawPcm(playbackHandle, fadeMs) : false;
}

AudioEngine::PlaybackStartResult AudioEngine::playByNameDetailed(const QString& name)
{
    return playByNameInternal(name, false);
}

AudioEngine::PlaybackStartResult AudioEngine::playByNameWithPreviewContext(const QString& name)
{
    return playByNameInternal(name, true);
}

AudioEngine::PlaybackStartResult AudioEngine::playByNameInternal(
    const QString& name, bool usePreviewContext)
{
    PlaybackStartResult result;
    if (!session_)
    {
        result.engineResult = harken::studio::bridge::kEngineNotInit;
        result.failure = harken::studio::bridge::PlaybackFailure::RuntimeRefused;
        emit playbackStarted(0, name);
        return result;
    }

    const bridge::EngineResult gate = currentWorldPlaybackGate(
        currentWorldPlaybackBarrier());
    if (gate != bridge::kEngineOk)
    {
        result.engineResult = gate;
        result.failure = bridge::PlaybackFailure::RuntimeRefused;
        emit playbackStarted(0, name);
        return result;
    }

    quint64 emitter = 0;
    if (usePreviewContext) {
        emitter = ensurePreviewEmitter(&result.engineResult);
        if (emitter == 0) {
            result.failure = bridge::PlaybackFailure::RuntimeRefused;
            emit playbackStarted(0, name);
            return result;
        }
        applyPreviewVariableOverrides();
        if (hasPendingPreviewVariableOverrides()) {
            result.previewVariablesPending = true;
            result.engineResult = bridge::kEngineBackpressure;
            emit playbackStarted(0, name);
            return result;
        }
    }
    const harken::studio::bridge::PlaybackStartResult bridgeResult =
        session_->playByNameDetailed(name, emitter);
    result.playbackHandle = static_cast<quintptr>(bridgeResult.handle);
    result.hasSound = bridgeResult.handle != 0;
    result.engineResult = bridgeResult.result;
    result.failure = bridgeResult.failure;
    result.topologyFailure = bridgeResult.topologyFailure;

    emit playbackStarted(result.playbackHandle, name);
    return result;
}

quintptr AudioEngine::playByName(const QString& name)
{
    return playByNameDetailed(name).playbackHandle;
}

quintptr AudioEngine::playByName3D(const QString& name, const harken::studio::Vec3& position)
{
    return playByName3DDetailed(name, position).playbackHandle;
}

AudioEngine::PlaybackStartResult AudioEngine::playByName3DDetailed(
    const QString& name, const harken::studio::Vec3& position)
{
    PlaybackStartResult result;
    if (!session_)
    {
        result.engineResult = bridge::kEngineNotInit;
        result.failure = bridge::PlaybackFailure::RuntimeRefused;
        emit playbackStarted(0, name);
        return result;
    }
    result.engineResult = currentWorldPlaybackGate(currentWorldPlaybackBarrier());
    if (result.engineResult != bridge::kEngineOk)
    {
        result.failure = bridge::PlaybackFailure::RuntimeRefused;
        emit playbackStarted(0, name);
        return result;
    }

    const auto started = session_->playByName3DDetailed(
        name, position.x, position.y, position.z);
    result.playbackHandle = static_cast<quintptr>(started.handle);
    result.hasSound = started.handle != 0;
    result.engineResult = started.result;
    result.failure = started.failure;
    result.topologyFailure = started.topologyFailure;
    emit playbackStarted(result.playbackHandle, name);
    return result;
}

bridge::EngineResult AudioEngine::stopPlayback(quintptr playbackHandle, float fadeMs)
{
    if (!session_)
        return bridge::kEngineNotInit;
    return session_->stopPlayback(static_cast<uint64_t>(playbackHandle), fadeMs);
}

bool AudioEngine::stopAll(float fadeMs)
{
    if (!session_)
        return false;

    return session_->stopAll(fadeMs);
}

bool AudioEngine::hasInstalledCurrentWorld() const
{
    return runtimeSyncCoordinator().hasInstalledWorld();
}

RuntimeSyncCoordinator::RevisionBarrierSnapshot AudioEngine::currentWorldPlaybackBarrier() const noexcept
{
    const auto& coordinator = runtimeSyncCoordinator();
    return coordinator.barrier(coordinator.currentAuthoringRevision());
}

bool AudioEngine::hasActionProgram(harken::studio::types::ObjectId actionId) const
{
    return session_ != nullptr && session_->hasActionProgram(actionId);
}

bool AudioEngine::hasCurrentWorldPlayableConsumers() const
{
    return session_ != nullptr && session_->hasCurrentWorldPlayableConsumers();
}

void AudioEngine::cancelCurrentWorldPlayableConsumers()
{
    if (!session_)
        return;
    session_->stopAll(0);
    session_->update();
    if (!session_->hasCurrentWorldPlayableConsumers())
        return;
    session_->pumpNullMixer(1);
    session_->update();
}

void AudioEngine::resetListenerTransformCache()
{
    if (!initialized_)
        return;

    listenerPosition_ = harken::studio::Vec3(0.0f, 0.0f, 0.0f);
    listenerForward_ = harken::studio::Vec3(0.0f, 0.0f, -1.0f);
    listenerUp_ = harken::studio::Vec3(0.0f, 1.0f, 0.0f);
}

bool AudioEngine::setPlaybackPosition(quintptr playbackHandle, const harken::studio::Vec3& position)
{
    if (!playbackHandle || !session_)
        return false;

    return session_->setPlaybackPosition(static_cast<uint64_t>(playbackHandle),
                                position.x, position.y, position.z);
}

void AudioEngine::setListenerPosition(const harken::studio::Vec3& position)
{
    setListenerTransform(position, listenerForward_, listenerUp_);
}

void AudioEngine::setListenerOrientation(const harken::studio::Vec3& forward, const harken::studio::Vec3& up)
{
    setListenerTransform(listenerPosition_, forward.normalized(), up.normalized());
}

bool AudioEngine::setListenerTransform(const harken::studio::Vec3& position, const harken::studio::Vec3& forward, const harken::studio::Vec3& up)
{
    if (!session_ || !session_->setListenerTransform(position.x, position.y, position.z,
            forward.x, forward.y, forward.z, up.x, up.y, up.z))
        return false;
    listenerPosition_ = position;
    listenerForward_ = forward;
    listenerUp_ = up;
    return true;
}

bool AudioEngine::loadPack(const QString& path, bridge::EngineResult* result)
{
    if (!session_) {
        if (result) *result = bridge::kEngineNotInit;
        return false;
    }

    if (result) *result = bridge::kEngineInvalidArg;
    QFile file(QFileInfo(path).absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = file.readAll();
    harken::studio::build::PackCanonicalImage image;
    if (file.error() != QFileDevice::NoError
        || harken::studio::build::ParsePackCanonicalImage(
            reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size(), &image)
            != harken::studio::build::kPackOk)
        return false;
    return session_->loadCanonicalPack(path, bytes, image.packStableId, image.packKind, result);
}

bool AudioEngine::unloadPack(const QString& path, bridge::EngineResult* result)
{
    if (!session_) {
        if (result) *result = bridge::kEngineNotInit;
        return false;
    }

    return session_->unloadPack(path, result);
}

uint32_t AudioEngine::getLoadedPackCount() const
{
    if (!session_)
        return 0;

    return session_->loadedPackCount();
}

QStringList AudioEngine::getLoadedPackPaths() const
{
    return session_ ? session_->loadedPackPaths() : QStringList{};
}

double AudioEngine::outputLatency() const
{
    if (!session_)
        return 0.0;

    return session_->outputLatency();
}

int AudioEngine::actualSampleRate() const
{
    if (!session_)
        return 48000;

    return session_->actualSampleRate();
}

harken::studio::bridge::ActionSeekStartResult AudioEngine::seekAction(
    quintptr actionHandle, const QString& expectedActionName,
    double positionMs, bool isRelative)
{
    if (!session_)
        return {0, bridge::kEngineNotInit};
    return session_->seekAction(
        static_cast<uint64_t>(actionHandle), expectedActionName,
        positionMs, isRelative);
}

harken::studio::bridge::EngineResult AudioEngine::queryActionSeek(
    uint64_t operationId, bridge::ActionSeekInfo& outInfo) const
{
    if (!session_)
        return bridge::kEngineNotInit;
    return session_->queryActionSeek(operationId, outInfo);
}

harken::studio::bridge::EngineResult AudioEngine::releaseActionSeek(
    uint64_t operationId)
{
    if (!session_)
        return bridge::kEngineNotInit;
    return session_->releaseActionSeek(operationId);
}

harken::studio::bridge::EngineResult AudioEngine::setGameObjectVolume(uint64_t gameObjectId, float volume)
{
    Q_UNUSED(gameObjectId);
    Q_UNUSED(volume);
    return bridge::kEngineNotInit;
}

harken::studio::bridge::EngineResult AudioEngine::setGameObjectMute(uint64_t gameObjectId, bool mute)
{
    Q_UNUSED(gameObjectId);
    Q_UNUSED(mute);
    return bridge::kEngineNotInit;
}

void AudioEngine::syncRuntimeBindings()
{
    if (!session_ || !runtimeSessionActive_)
        return;
    const auto barrier = runtimeSyncCoordinator_->barrier(0);
    const std::array<uint64_t, 3> identity{
        barrier.projectSessionId, runtimeSyncCoordinator_->currentAuthoringRevision(),
        barrier.runtimeEpoch};
    if (!runtimeBindingsPending_ && runtimeBindingSyncIdentity_ == identity)
        return;
    runtimeBindingSyncIdentity_ = identity;
    runtimeBindingsPending_ = false;
    auto* project = ProjectManager::instance().currentProject();
    if (!project) {
        for (const auto& attachment : runtimeBindingAttachments_) {
            harken::studio::bridge::detachPreviewBinding(attachment.bindingId, attachment.scopeKind, attachment.objectId);
        }
        for (harken::studio::types::ObjectId bindingId : runtimeBindingIds_)
            harken::studio::bridge::removePreviewBinding(bindingId);
        runtimeBindingAttachments_.clear();
        runtimeBindingIds_.clear();
        runtimeBindingPrograms_.clear();
        return;
    }

    const BindingRuntimeLowering lowering =
        BindingRuntimeLoweringSemantics::lowerProject(*project);
    if (lowering.failed)
        return;

    auto fillDesc = [](const BindingRuntimeProgram& program, harken::studio::bridge::PreviewBindingDefinition* desc) {
        *desc = {};
        desc->variableId = program.variableId;
        desc->targetScopeKind = program.targetScopeKind;
        desc->targetId = program.targetId;
        desc->transitionMs = program.transitionMs;
        const uint32_t count = std::min(
            static_cast<uint32_t>(program.points.size()),
            static_cast<uint32_t>(harken::studio::types::kCurveMaxPoints));
        desc->pointCount = count;
        for (uint32_t i = 0; i < count; ++i) {
            desc->points[i].x = program.points[static_cast<int>(i)].x;
            desc->points[i].y = program.points[static_cast<int>(i)].value;
        }
        const uint32_t handleCount = count > 0 ? count - 1 : 0;
        for (uint32_t i = 0; i < handleCount
             && i < static_cast<uint32_t>(program.handles.size()); ++i) {
            const CurveHandlePairDef& handle = program.handles[static_cast<int>(i)];
            desc->handles[i].h1x = handle.h1x;
            desc->handles[i].h1y = handle.h1y;
            desc->handles[i].h2x = handle.h2x;
            desc->handles[i].h2y = handle.h2y;
        }
    };

    std::vector<harken::studio::types::ObjectId> registeredOk;
    registeredOk.reserve(lowering.programs.size());
    for (const BindingRuntimeProgram& authored : lowering.programs) {
        const auto preview = runtimeBindingPreviews_.constFind(authored.bindingId);
        const auto& program = preview == runtimeBindingPreviews_.cend() ? authored : preview->back().second;
        const auto previousProgram = runtimeBindingPrograms_.constFind(program.bindingId);
        if (previousProgram != runtimeBindingPrograms_.cend()
            && sameBindingProgram(*previousProgram, program)) {
            registeredOk.push_back(program.bindingId);
            continue;
        }
        harken::studio::bridge::PreviewBindingDefinition desc{};
        fillDesc(program, &desc);
        const auto result = harken::studio::bridge::setPreviewBinding(program.bindingId, desc);
        if (result == harken::studio::bridge::kEngineOk) {
            registeredOk.push_back(program.bindingId);
            runtimeBindingPrograms_[program.bindingId] = program;
        } else if (result == harken::studio::bridge::kEngineBackpressure) {
            runtimeBindingsPending_ = true;
        }
    }

    std::vector<BindingRuntimeAttachment> attachedOk;
    attachedOk.reserve(lowering.attachments.size());
    for (const BindingRuntimeAttachment& attachment : lowering.attachments) {
        if (std::any_of(runtimeBindingAttachments_.begin(), runtimeBindingAttachments_.end(),
                [&attachment](const auto& previous) {
                    return BindingRuntimeLoweringSemantics::sameAttachment(previous, attachment);
                })) {
            attachedOk.push_back(attachment);
            continue;
        }
        const auto result = harken::studio::bridge::attachPreviewBinding(
            attachment.bindingId, attachment.scopeKind, attachment.objectId);
        if (result == harken::studio::bridge::kEngineOk || result == harken::studio::bridge::kEngineInUse)
            attachedOk.push_back(attachment);
        else if (result == harken::studio::bridge::kEngineBackpressure)
            runtimeBindingsPending_ = true;
    }

    const PreviewBindingSyncState previous{runtimeBindingIds_,
                                           runtimeBindingAttachments_};
    const PreviewBindingSyncState next = BindingRuntimeLoweringSemantics::reconcilePreviewSync(
        lowering, previous, registeredOk, attachedOk);

    for (const auto& previousAttachment : runtimeBindingAttachments_) {
        bool keep = false;
        for (const auto& nextAttachment : next.attachments) {
            if (BindingRuntimeLoweringSemantics::sameAttachment(
                    nextAttachment, previousAttachment)) {
                keep = true;
                break;
            }
        }
        if (keep) continue;
        harken::studio::bridge::detachPreviewBinding(previousAttachment.bindingId,
            previousAttachment.scopeKind, previousAttachment.objectId);
    }
    for (harken::studio::types::ObjectId previousId : runtimeBindingIds_) {
        bool keep = false;
        for (harken::studio::types::ObjectId nextId : next.ids) {
            if (nextId == previousId) {
                keep = true;
                break;
            }
        }
        if (keep) continue;
        harken::studio::bridge::removePreviewBinding(previousId);
        runtimeBindingPrograms_.remove(previousId);
    }

    runtimeBindingIds_ = next.ids;
    runtimeBindingAttachments_ = next.attachments;
}

void AudioEngine::onRuntimeServiceTimer()
{
    if (session_) {
        if (auto compiled = currentWorldCompilation_->takeResult()) {
            bridge::CurrentWorldSyncResult result;
            result.token = compiled->token;
            if (!runtimeSyncCoordinator_->isCurrentSubmission(compiled->token)) {
                result.status = bridge::CurrentWorldSyncStatus::Superseded;
            } else if (compiled->compiled.requiresFullRefresh
                || !preparedSoundsAreCurrent(compiled->request)) {
                currentWorldRequestSession_ = 0;
                currentWorldRequestEpoch_ = 0;
                runtimeSyncCoordinator_->markCurrentWorldMutation();
                result.status = bridge::CurrentWorldSyncStatus::Superseded;
            } else {
                if (!compiled->compiled.ok()) {
                    currentWorldRequestSession_ = 0;
                    currentWorldRequestEpoch_ = 0;
                }
                result = build::installCompiledCurrentWorld(
                    std::move(compiled->request), std::move(compiled->compiled), compiled->token);
            }
            if (result.ok())
                currentWorldCompilation_->acknowledgePublication(result.token);
            runtimeSyncCoordinator_->acceptCurrentWorldSyncResult(result);
        }
        session_->update();
        while (const auto completion = bridge::takeCurrentWorldSyncCompletion()) {
            if (completion->ok())
                currentWorldCompilation_->acknowledgePublication(completion->token);
            runtimeSyncCoordinator_->acceptCurrentWorldSyncResult(*completion);
            if (completion->ok()) {
                invalidatePreviewVariableAcceptance();
                runtimeBindingsPending_ = true;
                applyPendingAuditionOverlay();
            }
        }
        syncRuntimeBindings();
        applyPreviewVariableOverrides();
        applyLivePropertyPreview();
        retryBackpressuredAuditionOverlay();
    }
}

void AudioEngine::serviceRuntime()
{
    onRuntimeServiceTimer();
    onMeterTimer();
    onMonitorTimer();
}

void AudioEngine::onMeterTimer()
{
    if (!session_)
        return;
    session_->publishMeterPeaks();
    if (playing_)
        session_->masterMeterPeak(masterMeterPeakL_, masterMeterPeakR_);
    else {
        masterMeterPeakL_ = 0.0f;
        masterMeterPeakR_ = 0.0f;
    }
    emit meterLevelsUpdated();
}

RuntimeSyncCoordinator& AudioEngine::runtimeSyncCoordinator() noexcept
{
    return *runtimeSyncCoordinator_;
}

const RuntimeSyncCoordinator& AudioEngine::runtimeSyncCoordinator() const noexcept
{
    return *runtimeSyncCoordinator_;
}

void AudioEngine::updateRuntimeServiceInterval()
{
    if (session_)
        runtimeServiceTimer_->setInterval(runtimeServiceIntervalMs(*session_));
}

void AudioEngine::consumeRuntimeNotifications()
{
    std::vector<bridge::EngineNotification> notifications;
    bridge::pollEngineNotifications(notifications);
    if (notifications.empty())
        return;

    auto* project = ProjectManager::instance().currentProject();
    auto resolveName = [project](uint32_t shortID) -> QString {
        if (project && shortID != 0)
            if (auto* item = findItemByShortIDInProject(project, shortID))
                return item->name;
        return QString();
    };

    using Kind = bridge::EngineNotification::Kind;
    for (const auto& n : notifications)
    {
        switch (n.kind)
        {
        case Kind::ExecError:
            addExecError(static_cast<ExecErrorDomain>(n.domain), n.severity,
                         n.objectId, n.playingId, resolveName(n.objectId),
                         n.errorType, n.message);
            break;
        case Kind::PlaybackEnded:
            emit playbackStopped(static_cast<quintptr>(n.playingId));
            break;
        case Kind::VoiceStolen:
        case Kind::VoiceParked:
        case Kind::VoiceReturned:
            break;
        }
    }
}

void AudioEngine::onMonitorTimer()
{
    if (!session_)
        return;

    consumeRuntimeNotifications();
    refreshPlaybackPaths();

    int count = session_->activeVoiceCount();
    if (!voiceCountKnown_ || count != activeVoiceCount_)
    {
        activeVoiceCount_ = count;
        voiceCountKnown_ = true;
        emit voiceCountChanged(activeVoiceCount_);
    }

    cpuUsage_ = session_->audioCpuPercent();
}

void AudioEngine::refreshPlaybackPaths()
{
    std::vector<bridge::PlaybackPathObservation> paths;
    if (session_ && playing_ && runtimeSyncCoordinator_->barrier(
            runtimeSyncCoordinator_->currentAuthoringRevision()).state
            == RuntimeSyncCoordinator::RevisionBarrierState::Ready)
        bridge::capturePlaybackPaths(paths);
    if (paths == playbackPaths_) return;
    playbackPaths_ = std::move(paths);
    emit playbackPathsChanged();
}

void AudioEngine::getMasterMeterPeak(float& peakL, float& peakR) const
{
    peakL = masterMeterPeakL_;
    peakR = masterMeterPeakR_;
}


void AudioEngine::addExecError(ExecErrorDomain domain, uint8_t severity,
                               uint32_t objectID, quint64 playingID,
                               const QString& objectName,
                               const QString& errorType, const QString& message)
{
    Q_ASSERT(QThread::currentThread() == qApp->thread());

    static const char* const kDomainNames[] =
        { "Music", "Stop", "Lifecycle", "Dispatch", "Gate", "Unclassified" };

    ExecError err;
    err.domain = domain;
    err.severity = severity;
    err.objectID = objectID;
    err.playingID = playingID;
    err.objectName = objectName;
    err.errorType = errorType;
    err.message = message;
    err.timestampMs = QDateTime::currentMSecsSinceEpoch();

    if (execErrors_.size() >= kMaxExecErrors)
        execErrors_.removeFirst();
    execErrors_.append(err);

    qWarning() << "[ExecError]" << kDomainNames[static_cast<int>(domain)]
               << errorType << ":" << message
               << "(object:" << objectName << "id:" << objectID << ")";

    emit execErrorOccurred(static_cast<int>(domain), errorType, message);
}

QVector<AudioEngine::ExecError> AudioEngine::recentExecErrors(
    int maxCount, int domainFilter) const
{
    QVector<ExecError> out;
    for (const auto& err : execErrors_)
    {
        if (domainFilter >= 0 && static_cast<int>(err.domain) != domainFilter)
            continue;
        out.append(err);
    }
    if (maxCount >= 0 && out.size() > maxCount)
        out = out.mid(out.size() - maxCount);
    return out;
}

void AudioEngine::clearExecErrors(int domainFilter)
{
    if (domainFilter < 0)
    {
        execErrors_.clear();
        return;
    }
    execErrors_.erase(
        std::remove_if(execErrors_.begin(), execErrors_.end(),
                       [domainFilter](const ExecError& err) {
                           return static_cast<int>(err.domain) == domainFilter;
                       }),
        execErrors_.end());
}


int AudioEngine::getActiveVoiceInfoList(std::vector<VoiceInfoData>& outInfos) const
{
    return bridge::getActiveVoiceInfoList(outInfos);
}


bool AudioEngine::getVoiceStateBreakdown(VoiceStateBreakdown& out) const
{
    return bridge::getVoiceStateBreakdown(out);
}


void AudioEngine::setVoicePitchByName(const QString& soundName, float cents)
{
    if (!session_) return;
    session_->setVoicePitchByName(soundName, cents);
}

void AudioEngine::setVoiceVolumeByName(const QString& soundName, float volumeDb)
{
    if (!session_) return;
    session_->setVoiceVolumeByName(soundName, volumeDb);
}

void AudioEngine::setVoiceOutputVolumeByName(const QString& soundName, float volumeDb)
{
    if (!session_) return;
    session_->setVoiceOutputVolumeByName(soundName, volumeDb);
}

void AudioEngine::setVoiceSpatMixByName(const QString& soundName, float mix)
{
    if (!session_) return;
    session_->setVoiceSpatMixByName(soundName, mix);
}

void AudioEngine::setVoiceDistanceScaleByName(const QString& soundName, float scale)
{
    if (!session_) return;
    session_->setVoiceDistanceScaleByName(soundName, scale);
}

void AudioEngine::setVoiceSendsByName(const QString& soundName,
                                      const std::vector<std::pair<harken::studio::types::ObjectId, float>>& sends)
{
    if (!session_) return;
    session_->setVoiceSendsByName(soundName, sends);
}


void AudioEngine::removeRuntimeObject(harken::studio::types::ObjectId objectID)
{
    bridge::removeRuntimeObject(objectID);
}

void AudioEngine::clearRuntimeObjects()
{
    bridge::clearRuntimeObjects();
}

uint32_t AudioEngine::getRuntimeObjectCount() const
{
    return bridge::runtimeObjectCount();
}

void AudioEngine::registerRuntimeMedia(uint32_t mediaID, const std::string& path, uint64_t fileSize)
{
    bridge::mediaRegistryRegister(mediaID, QString::fromStdString(path), fileSize);
}

bool AudioEngine::prepareMedia(uint32_t mediaID, const std::string& path, uint64_t fileSize)
{
    bridge::mediaRegistryRegister(mediaID, QString::fromStdString(path), fileSize);
    if (!session_) return true;
    return session_->prepareMedia(mediaID, QString::fromStdString(path), fileSize);
}

bool AudioEngine::swapMedia(uint32_t sourceID, const std::string& newPath, uint64_t fileSize)
{
    bridge::mediaRegistryRegister(sourceID, QString::fromStdString(newPath), fileSize);
    if (!session_) return true;
    return session_->swapMedia(sourceID, QString::fromStdString(newPath), fileSize);
}

void AudioEngine::clearRuntimeMedia()
{
    bridge::mediaRegistryClear();
}

uint32_t AudioEngine::getRuntimeMediaCount() const
{
    return bridge::mediaRegistryCount();
}


void AudioEngine::setGameObjectPosition(uint64_t gameObjectId, float x, float y, float z)
{
    Q_UNUSED(gameObjectId);
    Q_UNUSED(x);
    Q_UNUSED(y);
    Q_UNUSED(z);
}

void AudioEngine::setGameObjectOrientation(uint64_t gameObjectId, float fwdX, float fwdY, float fwdZ)
{
    Q_UNUSED(gameObjectId);
    Q_UNUSED(fwdX);
    Q_UNUSED(fwdY);
    Q_UNUSED(fwdZ);
}


// -- system / debug control forwards ---------------------------------------

bool AudioEngine::setMonitorFilterMask(uint64_t mask)
{
    Q_UNUSED(mask);
    return false;
}

int32_t AudioEngine::watchParameter(uint32_t objectId, uint32_t paramId, uint8_t watchType)
{
    Q_UNUSED(objectId);
    Q_UNUSED(paramId);
    Q_UNUSED(watchType);
    return INT32_MIN;
}

void AudioEngine::unwatchParameter(uint32_t objectId, uint32_t paramId)
{
    Q_UNUSED(objectId);
    Q_UNUSED(paramId);
}

void AudioEngine::setMeteringInterval(uint32_t ms)
{
    Q_UNUSED(ms);
}

bool AudioEngine::setCpuBudgetConfig(float escalationPct, float recoveryPct)
{
    Q_UNUSED(escalationPct);
    Q_UNUSED(recoveryPct);
    return false;
}

harken::studio::bridge::EngineResult AudioEngine::setMemoryBudgetLimit(uint32_t category, uint64_t maxBytes)
{
    Q_UNUSED(category);
    Q_UNUSED(maxBytes);
    return bridge::kEngineNotInit;
}

harken::studio::bridge::EngineResult AudioEngine::setListenerOutputBus(uint32_t listenerId, harken::studio::types::ObjectId busId)
{
    Q_UNUSED(listenerId);
    Q_UNUSED(busId);
    return bridge::kEngineNotInit;
}

void AudioEngine::setRuntimeLanguage(const QString& code)
{
    Q_UNUSED(code);
}

bool AudioEngine::setPackRoutingPriority(const QString& packName, int priority)
{
    Q_UNUSED(packName);
    Q_UNUSED(priority);
    return false;
}

}
