#include "RuntimeLink.h"
#include "NativeValues.h"
#include "RuntimeLinkInternal.h"

#include "DiagnosticsBridge.h"
#include "PackagingBridge.h"
#include "CurrentWorldSyncBridgeInternal.h"

#include <harken/Harken.h>
#include <harken_preview/PreviewAbi.h>
#include <harken_preview/PreviewProject.h>

#include <QCoreApplication>
#include <QFileInfo>
#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace HarkenEditor {
namespace Bridge {
namespace Detail {

static_assert(VN_OK == 0, "EngineResult success expects harken_core zero success");
static_assert(kEngineStaleHandle == static_cast<EngineResult>(VN_STALE_HANDLE));
static_assert(kEngineNoVoice == static_cast<EngineResult>(VN_NO_VOICE));
static_assert(kEngineCapabilityUnavailable ==
              static_cast<EngineResult>(VN_CAPABILITY_UNAVAILABLE));
static_assert(kEngineRuntimeRecreateRequired ==
              static_cast<EngineResult>(VN_RUNTIME_RECREATE_REQUIRED));
static_assert(VN_BUS_MASTER == toNative(kEngineMasterBusId));
static_assert(sizeof(VnVoiceHandle) == sizeof(uint64_t), "Voice handles must fit the bridge handle");

namespace {

void runtimeTrace(const QString& message)
{
    if (!qEnvironmentVariableIsSet("HARKEN_STARTUP_TRACE"))
        return;
    QDir().mkpath(QStringLiteral(".temp"));
    QFile f(QStringLiteral(".temp/editor_startup_trace.log"));
    if (!f.open(QIODevice::Append | QIODevice::Text))
        return;
    const QString line = QStringLiteral("%1 %2\n")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs), message);
    f.write(line.toUtf8());
}

struct SoundBinding {
    harkenstudio::ObjectId soundId = {};
    uint32_t sourceId = 0;
    uint32_t loopCount = 1;
    bool spatialized3D = false;
    harkenstudio::ObjectId objectId = {};
    harkenstudio::ObjectId attenuationId = {};
};

enum class RuntimePlaybackKind : uint8_t {
    Preview,
    Action,
    RawPcm
};

struct RuntimePlayback {
    RuntimePlaybackKind kind = RuntimePlaybackKind::Preview;
    uint64_t nativeHandle = 0;
    VnEmitterHandle emitter = 0;
    QString actionName;
};

struct RuntimeActionSeek {
    VnActionSeekTicket nativeTicket = 0;
    ActionSeekInfo info;
};

struct RuntimeState {
    bool initialized = false;
    bool outputRunning = false;
    EngineSessionConfig requestedConfig;
    int sampleRate = 48000;
    int blockFrames = 256;
    VnSnapshot snapshot{};
    bool snapshotValid = false;
    VnPreviewChannelPeaksV1 meterPeaks{};
    bool meterPeaksValid = false;
    std::function<void(int level, const QString& msg)> logHandler;
    std::unordered_map<harkenstudio::ObjectId, SoundBinding> soundsById;
    std::unordered_map<harkenstudio::ObjectId, QString> soundNameByObjectId;
    std::unordered_map<harkenstudio::ObjectId, harkenstudio::ObjectId> soundIdByObjectId;
    std::unordered_map<QString, harkenstudio::ObjectId> actionsByName;
    std::unordered_map<harkenstudio::ObjectId, QString> actionNameById;
    struct RawPcmDecodeCache {
        QString path;
        std::vector<float> interleaved;
        int channels = 0;
        int sampleRate = 0;
        uint64_t windowStartFrame = 0;
        uint64_t windowFrameCount = 0;
        uint64_t registeredStartFrame = 0;
        uint64_t registeredFrameCount = 0;
        bool loops = false;
        uint64_t voiceHandle = 0;
        uint64_t playbackHandle = 0;
    };

    std::unordered_set<harkenstudio::ObjectId> registeredSounds;
    std::unordered_map<harkenstudio::ObjectId, uint64_t> registeredSoundRevisions;
    std::unordered_set<harkenstudio::ObjectId> preparedSounds;
    std::unordered_set<harkenstudio::ObjectId> registeredRawPcm;
    std::unordered_map<harkenstudio::ObjectId, RawPcmDecodeCache> rawPcmDecodes;
    std::unordered_map<QString, VnPackHandle> loadedPacksByPath;
    std::unordered_set<uint64_t> liveVoices;
    std::unordered_map<uint64_t, QString> voiceNameByHandle;
    std::unordered_map<uint64_t, RuntimePlayback> playbacks;
    std::unordered_map<uint64_t, uint64_t> playbackByPreview;
    std::unordered_map<uint64_t, uint64_t> playbackByAction;
    std::unordered_map<uint64_t, uint64_t> playbackByRawVoice;
    std::unordered_map<uint64_t, RuntimeActionSeek> actionSeeks;
    std::vector<uint64_t> endedPlaybackHandles;
    std::vector<EngineNotification> pendingNotifications;
    uint64_t notificationGeneration = 0;
    QMutex notificationMutex;
    uint64_t nextPlaybackHandle = 1;
    uint64_t nextActionSeekOperationId = 1;
    std::unordered_map<QString, float> voicePitchCentsByName;
    std::unordered_map<QString, float> voiceVolumeDbByName;
    std::unordered_map<QString, float> voiceSpatMixByName;
    std::unordered_map<QString, float> voiceDistanceScaleByName;
    std::unordered_map<QString, std::vector<std::pair<harkenstudio::ObjectId, float>>> voiceSendsByName;
    std::unordered_map<harkenstudio::ObjectId, harkenstudio::ObjectId> objectBusById;
};

RuntimeState g_runtime;

uint64_t allocatePlaybackHandle()
{
    for (;;)
    {
        const uint64_t handle = g_runtime.nextPlaybackHandle++;
        if (g_runtime.nextPlaybackHandle == 0)
            g_runtime.nextPlaybackHandle = 1;
        if (handle != 0 && g_runtime.playbacks.find(handle) == g_runtime.playbacks.end())
            return handle;
    }
}

uint64_t registerPlayback(RuntimePlaybackKind kind, uint64_t nativeHandle,
                          VnEmitterHandle emitter = 0,
                          const QString& actionName = {})
{
    if (nativeHandle == 0)
        return 0;
    const uint64_t playbackHandle = allocatePlaybackHandle();
    g_runtime.playbacks.emplace(
        playbackHandle, RuntimePlayback{kind, nativeHandle, emitter, actionName});
    if (kind == RuntimePlaybackKind::Preview)
        g_runtime.playbackByPreview[nativeHandle] = playbackHandle;
    else if (kind == RuntimePlaybackKind::Action)
        g_runtime.playbackByAction[nativeHandle] = playbackHandle;
    else
        g_runtime.playbackByRawVoice[nativeHandle] = playbackHandle;
    return playbackHandle;
}

uint64_t allocateActionSeekOperationId()
{
    for (;;)
    {
        const uint64_t id = g_runtime.nextActionSeekOperationId++;
        if (g_runtime.nextActionSeekOperationId == 0)
            g_runtime.nextActionSeekOperationId = 1;
        if (id != 0 && g_runtime.actionSeeks.find(id) == g_runtime.actionSeeks.end())
            return id;
    }
}

bool hasAcceptedActionSeek()
{
    return std::any_of(
        g_runtime.actionSeeks.cbegin(), g_runtime.actionSeeks.cend(),
        [](const auto& entry) {
            return entry.second.info.state == ActionSeekState::Accepted;
        });
}

void serviceActionSeeks()
{
    for (auto& [operationId, operation] : g_runtime.actionSeeks)
    {
        Q_UNUSED(operationId);
        if (operation.info.state == ActionSeekState::Accepted)
        {
            VnActionSeekInfoV1 native{};
            native.structSize = sizeof(native);
            const VnResult queryResult =
                Vn_QueryActionSeek(operation.nativeTicket, &native);
            operation.info.serviceResult =
                static_cast<EngineResult>(queryResult);
            if (queryResult != VN_OK)
            {
                operation.info.result =
                    static_cast<EngineResult>(queryResult);
                operation.info.state = ActionSeekState::Cancelled;
            }
            else
            {
                operation.info.result =
                    static_cast<EngineResult>(native.result);
                operation.info.targetCount = native.targetCount;
                operation.info.appliedCount = native.appliedCount;
                operation.info.cancelledCount = native.cancelledCount;
                operation.info.acceptedLogicSequence =
                    native.acceptedLogicSequence;
                operation.info.appliedAudioBlockSequence =
                    native.appliedAudioBlockSequence;
                operation.info.minimumAppliedSourceFrame =
                    native.minimumAppliedSourceFrame;
                operation.info.maximumAppliedSourceFrame =
                    native.maximumAppliedSourceFrame;
                if (native.state == VN_ACTION_SEEK_APPLIED)
                    operation.info.state = ActionSeekState::Applied;
                else if (native.state == VN_ACTION_SEEK_CANCELLED)
                    operation.info.state = ActionSeekState::Cancelled;
            }
        }

        if (operation.info.state == ActionSeekState::Accepted
            || operation.info.nativeReleased) {
            continue;
        }
        const VnResult releaseResult =
            Vn_ReleaseActionSeek(operation.nativeTicket);
        operation.info.nativeReleaseResult =
            static_cast<EngineResult>(releaseResult);
        if (releaseResult == VN_OK || releaseResult == VN_STALE_HANDLE)
        {
            operation.info.nativeReleased = true;
            operation.nativeTicket = 0;
        }
    }
}

void releasePlayback(uint64_t playbackHandle, bool reportEnded)
{
    const auto it = g_runtime.playbacks.find(playbackHandle);
    if (it == g_runtime.playbacks.end())
        return;
    const RuntimePlayback playback = it->second;
    if (playback.kind == RuntimePlaybackKind::Preview)
        g_runtime.playbackByPreview.erase(playback.nativeHandle);
    else if (playback.kind == RuntimePlaybackKind::Action)
        g_runtime.playbackByAction.erase(playback.nativeHandle);
    else
        g_runtime.playbackByRawVoice.erase(playback.nativeHandle);
    if (playback.emitter != 0 && g_runtime.initialized)
        Vn_DestroyEmitter(playback.emitter);
    g_runtime.playbacks.erase(it);
    if (reportEnded)
        g_runtime.endedPlaybackHandles.push_back(playbackHandle);
}

void collectCompletedPlaybacks()
{
    std::vector<uint64_t> completed;
    completed.reserve(g_runtime.playbackByPreview.size() +
                      g_runtime.playbackByAction.size());
    for (const auto& [nativeHandle, playbackHandle] :
         g_runtime.playbackByPreview)
    {
        VnPreviewPlayableStateV2 state{};
        state.structSize = sizeof(state);
        const VnPreviewResult result = VnPreview_QueryPlayable(
            static_cast<VnPreviewPlayableHandle>(nativeHandle), &state);
        if (result == VN_PREVIEW_STALE_HANDLE)
            completed.push_back(playbackHandle);
    }
    for (const auto& [nativeHandle, playbackHandle] : g_runtime.playbackByAction)
    {
        VnActionFiringInfo info{};
        info.structSize = sizeof(info);
        const auto firing = static_cast<VnActionFiringHandle>(nativeHandle);
        const VnResult result = Vn_QueryActionFiring(firing, &info);
        if (result == VN_STALE_HANDLE)
            completed.push_back(playbackHandle);
        else if (result == VN_OK &&
                 info.state == VN_ACTION_FIRING_TERMINAL_RETAINED)
        {
            const VnResult releaseResult = Vn_ReleaseActionFiring(firing);
            if (releaseResult == VN_OK || releaseResult == VN_STALE_HANDLE)
                completed.push_back(playbackHandle);
        }
    }
    for (uint64_t playbackHandle : completed)
        releasePlayback(playbackHandle, true);
}

uint32_t positiveOrDefault(int value, uint32_t fallback)
{
    return value > 0 ? static_cast<uint32_t>(value) : fallback;
}

float linearToDb(float linear)
{
    if (!(linear > 0.0f))
        return -96.0f;
    return 20.0f * std::log10(linear);
}

// Vn_SetVoicePitch accepts ratios in [1/64, 64], i.e. +/-7200 cents.
float pitchRatioFromCents(float cents)
{
    return std::pow(2.0f, qBound(-7200.0f, cents, 7200.0f) / 1200.0f);
}

void logCallback(void* userData, uint32_t level, uint32_t, const char* text)
{
    Q_UNUSED(userData);
    if (!g_runtime.logHandler)
        return;
    g_runtime.logHandler(static_cast<int>(level),
                         QString::fromUtf8(text ? text : ""));
}

void enqueueStudioNotification(uint32_t type,
                               uint64_t voiceHandle,
                               uint64_t blockTimestamp)
{
    EngineNotification n;
    n.voiceHandle = voiceHandle;
    n.playingId = voiceHandle;
    n.blockTimestamp = blockTimestamp;
    n.domain = kExecErrorDomainUnclassified;
    switch (type)
    {
    case VN_NOTIFY_DEGRADATION_OCCURRED:
        n.kind = EngineNotification::Kind::ExecError;
        n.errorType = QStringLiteral("DEGRADATION_OCCURRED");
        n.severity = 2;
        break;
    case VN_NOTIFY_VOICE_STARVED:
        n.kind = EngineNotification::Kind::ExecError;
        n.errorType = QStringLiteral("VOICE_STARVED");
        n.severity = 2;
        break;
    case VN_NOTIFY_VOICE_STOLEN:
        n.kind = EngineNotification::Kind::VoiceStolen;
        break;
    case VN_NOTIFY_VOICE_PARKED:
        n.kind = EngineNotification::Kind::VoiceParked;
        break;
    case VN_NOTIFY_VOICE_RETURNED:
        n.kind = EngineNotification::Kind::VoiceReturned;
        break;
    default:
        return;
    }

    uint64_t generation = 0;
    {
        QMutexLocker lock(&g_runtime.notificationMutex);
        generation = g_runtime.notificationGeneration;
    }

    const auto deliver = [n, generation]() {
        QMutexLocker lock(&g_runtime.notificationMutex);
        if (generation != g_runtime.notificationGeneration)
            return;
        g_runtime.pendingNotifications.push_back(n);
    };

    if (QCoreApplication::instance() == nullptr
        || QThread::currentThread() == QCoreApplication::instance()->thread())
    {
        deliver();
        return;
    }
    QMetaObject::invokeMethod(
        QCoreApplication::instance(), deliver, Qt::QueuedConnection);
}

void notificationCallback(const VnNotification* notification, void*)
{
    if (!notification)
        return;

    const uint32_t type = notification->type;
    const uint64_t voiceHandle = notification->voiceHandle;
    const uint64_t blockTimestamp = notification->blockTimestamp;

    if (voiceHandle != 0
        && (type == VN_NOTIFY_VOICE_ENDED
            || type == VN_NOTIFY_VOICE_STOPPED_BY_FADE
            || type == VN_NOTIFY_VOICE_STOLEN))
    {
        g_runtime.liveVoices.erase(voiceHandle);
        g_runtime.voiceNameByHandle.erase(voiceHandle);
        const auto rawIt = g_runtime.playbackByRawVoice.find(voiceHandle);
        if (rawIt != g_runtime.playbackByRawVoice.end()) {
            const uint64_t playbackHandle = rawIt->second;
            const auto deliver = [playbackHandle]() {
                releasePlayback(playbackHandle, true);
            };
            if (QCoreApplication::instance() == nullptr
                || QThread::currentThread() == QCoreApplication::instance()->thread())
            {
                deliver();
            } else {
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(), deliver, Qt::QueuedConnection);
            }
        }
    }

    enqueueStudioNotification(type, voiceHandle, blockTimestamp);
}

VnConfig makeConfig(const EngineSessionConfig& config)
{
    VnConfig c{};
    c.structSize = sizeof(VnConfig);
    c.sampleRate = positiveOrDefault(config.sampleRate, 48000);
    c.blockFrames = positiveOrDefault(config.bufferSize, 256);
    c.maxVoices = positiveOrDefault(config.maxVoices, 64);
    c.voiceSendSlots = 8;
    c.deviceMode = config.deviceMode == EngineDeviceMode::Null
        ? VN_DEVICE_NULL
        : VN_DEVICE_REAL;
    c.mixChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    c.deviceChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    c.onNotification = &notificationCallback;
    c.logCallback = &logCallback;
    return c;
}

bool ensureRuntimeStarted()
{
    if (g_runtime.initialized)
        return true;

    VnConfig c = makeConfig(g_runtime.requestedConfig);
    qInfo() << "RuntimeLink: before Vn_Init"
            << "sampleRate=" << c.sampleRate
            << "blockFrames=" << c.blockFrames
            << "deviceLayout=" << c.deviceChannelLayout;
    runtimeTrace(QStringLiteral("RuntimeLink: before Vn_Init"));
    const VnResult result = Vn_Init(&c);
    runtimeTrace(QStringLiteral("RuntimeLink: after Vn_Init result=%1")
                 .arg(static_cast<int>(result)));
    qInfo() << "RuntimeLink: after Vn_Init result=" << static_cast<int>(result);
    if (result != VN_OK)
        return false;

    const VnResult suspendResult = Vn_Suspend();
    if (suspendResult != VN_OK)
    {
        Vn_Term();
        return false;
    }

    if (VnPreview_EnableChannelPeakMetering() != VN_OK)
    {
        Vn_Term();
        return false;
    }

    g_runtime.initialized = true;
    g_runtime.outputRunning = false;
    g_runtime.sampleRate = static_cast<int>(c.sampleRate);
    g_runtime.blockFrames = static_cast<int>(c.blockFrames);
    g_runtime.snapshot = VnSnapshot{};
    g_runtime.snapshot.structSize = sizeof(VnSnapshot);
    g_runtime.snapshotValid = false;
    g_runtime.meterPeaks = {};
    g_runtime.meterPeaksValid = false;
    g_runtime.liveVoices.clear();
    g_runtime.voiceNameByHandle.clear();
    g_runtime.playbacks.clear();
    g_runtime.playbackByPreview.clear();
    g_runtime.playbackByAction.clear();
    g_runtime.playbackByRawVoice.clear();
    g_runtime.actionSeeks.clear();
    g_runtime.endedPlaybackHandles.clear();
    g_runtime.registeredSounds.clear();
    g_runtime.registeredSoundRevisions.clear();
    g_runtime.preparedSounds.clear();
    g_runtime.registeredRawPcm.clear();
    g_runtime.rawPcmDecodes.clear();
    g_runtime.loadedPacksByPath.clear();
    return true;
}

VnResult registerDecodedSound(harkenstudio::ObjectId soundId, const std::vector<float>& samples,
                                uint32_t channels, uint32_t sampleRate,
                                uint64_t loopStartFrame, uint64_t loopEndFrame)
{
    uint64_t revision = soundPcmContentRevision(samples, channels, sampleRate,
                                                loopStartFrame, loopEndFrame);
    if (harkenstudio::IsNull(soundId) || revision == 0)
        return VN_INVALID_ARG;
    const auto cached = g_runtime.registeredSoundRevisions.find(soundId);
    if (cached != g_runtime.registeredSoundRevisions.end()
        && cached->second == revision)
        return VN_OK;

    VnPCMDesc desc{};
    desc.structSize = sizeof(desc);
    desc.sampleRate = sampleRate;
    desc.channels = channels;
    desc.layout = VN_PCM_LAYOUT_INTERLEAVED;
    desc.policy = VN_PCM_POLICY_COPY;
    desc.frames = samples.size() / channels;
    desc.samplesInterleaved = samples.data();
    const auto result = VnPreview_RegisterPcm(
        toNative(soundId), &desc, loopStartFrame, loopEndFrame, &revision);
    if (result != VN_OK) {
        if (result != VN_BACKPRESSURE)
            qWarning() << "[PlaybackRoute] PCM registration failed:"
                       << soundId << "result=" << result;
        return result;
    }
    g_runtime.registeredSounds.insert(soundId);
    g_runtime.registeredSoundRevisions[soundId] = revision;
    return VN_OK;
}

bool registerPcmForBinding(const SoundBinding& binding)
{
    if (binding.sourceId == 0)
        return false;

    const harkenstudio::ObjectId soundId = binding.soundId;
    if (g_runtime.preparedSounds.count(soundId) != 0)
        return g_runtime.registeredSoundRevisions.count(soundId) != 0;
    const QString path = mediaRegistryFindPath(binding.sourceId);
    if (path.isEmpty() || !QFileInfo::exists(path))
    {
        qWarning() << "[PlaybackRoute] runtime pcm register failed:"
                   << "sourceId=" << binding.sourceId
                   << "path missing";
        return false;
    }

    std::vector<float> interleaved;
    int channels = 0;
    int sampleRate = 0;
    if (!decodeAudioFileInterleaved(path, interleaved, channels, sampleRate))
    {
        qWarning() << "[PlaybackRoute] runtime pcm register failed:"
                   << "sourceId=" << binding.sourceId
                   << "decode failed" << path;
        return false;
    }
    if (channels <= 0 || sampleRate <= 0 || interleaved.empty())
    {
        qWarning() << "[PlaybackRoute] runtime pcm register failed:"
                   << "sourceId=" << binding.sourceId
                   << "empty decode" << path;
        return false;
    }

    return registerDecodedSound(soundId, interleaved,
                                  static_cast<uint32_t>(channels),
                                  static_cast<uint32_t>(sampleRate), 0, 0) == VN_OK;
}

void forgetRawPcmPlayback(harkenstudio::ObjectId soundId);

EngineResult registerDecodedPcmSlice(harkenstudio::ObjectId soundId,
                                     const std::vector<float>& interleaved,
                                     int channels,
                                     int sampleRate,
                                     uint64_t startFrame,
                                     uint64_t frameCount)
{
    if (harkenstudio::IsNull(soundId) || channels <= 0 || sampleRate <= 0 || interleaved.empty())
        return kEngineInvalidArg;

    const uint64_t decodedFrames =
        interleaved.size() / static_cast<size_t>(channels);
    if (decodedFrames == 0
        || interleaved.size() % static_cast<size_t>(channels) != 0)
        return kEngineInvalidArg;
    if (frameCount == 0 || startFrame >= decodedFrames)
        return kEngineInvalidArg;
    if (frameCount > decodedFrames - startFrame)
        return kEngineInvalidArg;

    VnPCMDesc desc{};
    desc.structSize = sizeof(VnPCMDesc);
    desc.sampleRate = static_cast<uint32_t>(sampleRate);
    desc.channels = static_cast<uint32_t>(channels);
    desc.layout = VN_PCM_LAYOUT_INTERLEAVED;
    desc.policy = VN_PCM_POLICY_COPY;
    desc.frames = frameCount;
    desc.samplesInterleaved =
        interleaved.data() + startFrame * static_cast<uint64_t>(channels);

    const bool alreadyRegistered =
        g_runtime.registeredSounds.find(soundId) != g_runtime.registeredSounds.end()
        || g_runtime.registeredRawPcm.find(soundId) != g_runtime.registeredRawPcm.end();
    if (alreadyRegistered)
        forgetRawPcmPlayback(soundId);
    VnResult result = alreadyRegistered
        ? Vn_ReplacePCM(toNative(soundId), &desc)
        : Vn_RegisterPCM(toNative(soundId), &desc);
    if (result != VN_OK)
        return static_cast<EngineResult>(result);

    g_runtime.registeredRawPcm.insert(soundId);
    return kEngineOk;
}

EngineResult registerRotatedPcmWindow(harkenstudio::ObjectId soundId,
                                      const std::vector<float>& interleaved,
                                      int channels,
                                      int sampleRate,
                                      uint64_t windowStartFrame,
                                      uint64_t windowFrameCount,
                                      uint64_t startFrame)
{
    if (harkenstudio::IsNull(soundId) || channels <= 0 || sampleRate <= 0 || interleaved.empty())
        return kEngineInvalidArg;
    if (windowFrameCount == 0)
        return kEngineInvalidArg;

    const uint64_t decodedFrames =
        interleaved.size() / static_cast<size_t>(channels);
    if (decodedFrames == 0
        || interleaved.size() % static_cast<size_t>(channels) != 0)
        return kEngineInvalidArg;
    if (windowStartFrame >= decodedFrames
        || windowFrameCount > decodedFrames - windowStartFrame)
        return kEngineInvalidArg;
    if (startFrame < windowStartFrame
        || startFrame >= windowStartFrame + windowFrameCount)
        return kEngineInvalidArg;

    const uint64_t offset = startFrame - windowStartFrame;
    if (offset == 0) {
        return registerDecodedPcmSlice(soundId, interleaved, channels, sampleRate,
                                       windowStartFrame, windowFrameCount);
    }

    const size_t channelCount = static_cast<size_t>(channels);
    const size_t windowSamples = static_cast<size_t>(windowFrameCount) * channelCount;
    const size_t headSamples = static_cast<size_t>(offset) * channelCount;
    std::vector<float> rotated(windowSamples);
    const float* window = interleaved.data()
        + static_cast<size_t>(windowStartFrame) * channelCount;
    std::copy(window + headSamples, window + windowSamples, rotated.begin());
    std::copy(window, window + headSamples, rotated.begin() + (windowSamples - headSamples));
    return registerDecodedPcmSlice(soundId, rotated, channels, sampleRate, 0, windowFrameCount);
}

void rememberRawPcmDecode(harkenstudio::ObjectId soundId,
                          const QString& path,
                          std::vector<float> interleaved,
                          int channels,
                          int sampleRate,
                          uint64_t windowStartFrame,
                          uint64_t windowFrameCount,
                          uint64_t registeredStartFrame,
                          uint64_t registeredFrameCount,
                          bool loops)
{
    RuntimeState::RawPcmDecodeCache cache;
    cache.path = path;
    cache.interleaved = std::move(interleaved);
    cache.channels = channels;
    cache.sampleRate = sampleRate;
    cache.windowStartFrame = windowStartFrame;
    cache.windowFrameCount = windowFrameCount;
    cache.registeredStartFrame = registeredStartFrame;
    cache.registeredFrameCount = registeredFrameCount;
    cache.loops = loops;
    g_runtime.rawPcmDecodes[soundId] = std::move(cache);
}

void forgetRawPcmPlayback(harkenstudio::ObjectId soundId)
{
    const auto cachedIt = g_runtime.rawPcmDecodes.find(soundId);
    if (cachedIt == g_runtime.rawPcmDecodes.end() || cachedIt->second.voiceHandle == 0)
        return;
    const uint64_t voiceHandle = cachedIt->second.voiceHandle;
    const uint64_t playbackHandle = cachedIt->second.playbackHandle;
    Vn_Stop(static_cast<VnVoiceHandle>(voiceHandle), 0);
    if (playbackHandle != 0)
        releasePlayback(playbackHandle, false);
    cachedIt->second.voiceHandle = 0;
    cachedIt->second.playbackHandle = 0;
}

} // namespace

bool runtimeInit(const EngineSessionConfig& config)
{
    g_runtime.requestedConfig = config;
    return ensureRuntimeStarted();
}

void runtimeTerm()
{
    const auto cancelled = discardPendingCurrentWorld();
    if (cancelled)
        qDebug() << "[CurrentWorldSync] cancelled pending world during teardown";
    if (g_runtime.initialized)
        Vn_Term();
    g_runtime.initialized = false;
    g_runtime.outputRunning = false;
    g_runtime.snapshotValid = false;
    g_runtime.meterPeaks = {};
    g_runtime.meterPeaksValid = false;
    g_runtime.registeredSounds.clear();
    g_runtime.registeredSoundRevisions.clear();
    g_runtime.preparedSounds.clear();
    g_runtime.registeredRawPcm.clear();
    g_runtime.rawPcmDecodes.clear();
    g_runtime.loadedPacksByPath.clear();
    g_runtime.liveVoices.clear();
    g_runtime.voiceNameByHandle.clear();
    g_runtime.playbacks.clear();
    g_runtime.playbackByPreview.clear();
    g_runtime.playbackByAction.clear();
    g_runtime.playbackByRawVoice.clear();
    g_runtime.actionSeeks.clear();
    g_runtime.endedPlaybackHandles.clear();
    {
        QMutexLocker lock(&g_runtime.notificationMutex);
        ++g_runtime.notificationGeneration;
        g_runtime.pendingNotifications.clear();
    }
    g_runtime.voicePitchCentsByName.clear();
    g_runtime.voiceVolumeDbByName.clear();
    g_runtime.voiceSpatMixByName.clear();
    g_runtime.voiceDistanceScaleByName.clear();
    g_runtime.voiceSendsByName.clear();
    g_runtime.objectBusById.clear();
}

bool runtimeIsActive()
{
    return g_runtime.initialized;
}

bool runtimeCaptureSnapshot(VnSnapshot& outSnapshot)
{
    outSnapshot = {};
    outSnapshot.structSize = sizeof(VnSnapshot);
    if (!g_runtime.initialized)
        return false;
    if (Vn_CaptureSnapshot(&outSnapshot) != VN_OK)
        return false;
    g_runtime.snapshot = outSnapshot;
    g_runtime.snapshotValid = true;
    return true;
}

void runtimeSetLogHandler(std::function<void(int, const QString&)> handler)
{
    g_runtime.logHandler = std::move(handler);
}

EngineResult runtimeStartOutput()
{
    if (!ensureRuntimeStarted())
        return static_cast<EngineResult>(VN_DEVICE_ERROR);
    if (g_runtime.outputRunning)
        return kEngineOk;
    const VnResult result = Vn_Resume();
    if (result == VN_OK)
        g_runtime.outputRunning = true;
    return static_cast<EngineResult>(result);
}

void runtimeStopOutput()
{
    runtimeStopAll(0);
    runtimeUpdate();
    runtimeTerm();
}

void runtimeUpdate()
{
    if (!g_runtime.initialized)
        return;
    if (g_runtime.requestedConfig.deviceMode == EngineDeviceMode::Null
        && hasAcceptedActionSeek()) {
        VnPreview_PumpNullMixer(1);
    }
    Vn_Update();
    runtimeDrainProjectGenerationAcks();
    servicePendingCurrentWorld();
    serviceActionSeeks();
    collectCompletedPlaybacks();
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    if (Vn_CaptureSnapshot(&snapshot) == VN_OK)
    {
        g_runtime.snapshot = snapshot;
        g_runtime.snapshotValid = true;
    }
}

int runtimeActiveVoiceCount()
{
    return g_runtime.snapshotValid ? static_cast<int>(g_runtime.snapshot.activeVoiceCount) : 0;
}

void runtimeDrainEndedPlaybacks(std::vector<uint64_t>& out)
{
    out = std::move(g_runtime.endedPlaybackHandles);
    g_runtime.endedPlaybackHandles.clear();
}

void runtimeDrainStudioNotifications(std::vector<EngineNotification>& out)
{
    QMutexLocker lock(&g_runtime.notificationMutex);
    out = std::move(g_runtime.pendingNotifications);
    g_runtime.pendingNotifications.clear();
}

void runtimeInjectNotificationForTest(uint32_t type,
                                      uint64_t voiceHandle,
                                      uint64_t blockTimestamp)
{
    enqueueStudioNotification(type, voiceHandle, blockTimestamp);
}

void runtimeInvalidateNotificationsForTest()
{
    QMutexLocker lock(&g_runtime.notificationMutex);
    ++g_runtime.notificationGeneration;
    g_runtime.pendingNotifications.clear();
}

void runtimePublishMeterPeaks()
{
    VnPreviewChannelPeaksV1 peaks{};
    peaks.structSize = sizeof(peaks);
    g_runtime.meterPeaksValid = g_runtime.initialized
        && VnPreview_TakeChannelPeaks(&peaks) == VN_OK;
    g_runtime.meterPeaks = peaks;
}

bool runtimeBusMeterPeak(harkenstudio::ObjectId busId, float& l, float& r)
{
    l = r = 0.0f;
    if (!g_runtime.meterPeaksValid || harkenstudio::IsNull(busId))
        return false;
    const auto& peaks = g_runtime.meterPeaks;
    for (uint32_t bus = 0; bus < peaks.busCount; ++bus)
    {
        if (peaks.busIds[bus] != toNative(busId))
            continue;
        l = peaks.channelPeaks[bus * VN_MAX_CHANNELS];
        r = peaks.channelPeaks[bus * VN_MAX_CHANNELS + (peaks.mixChannelCount == 1 ? 0 : 1)];
        return true;
    }
    return false;
}

void runtimeMasterMeterPeak(float& l, float& r)
{
    runtimeBusMeterPeak(kEngineMasterBusId, l, r);
}

float runtimeAudioCpuPercent()
{
    if (!g_runtime.snapshotValid || g_runtime.sampleRate <= 0 || g_runtime.blockFrames <= 0)
        return 0.0f;
    const double blockUs =
        (static_cast<double>(g_runtime.blockFrames) * 1000000.0) /
        static_cast<double>(g_runtime.sampleRate);
    if (blockUs <= 0.0)
        return 0.0f;
    return static_cast<float>(
        (static_cast<double>(g_runtime.snapshot.audioCpuLastUs) / blockUs) * 100.0);
}

double runtimeOutputLatency()
{
    if (g_runtime.sampleRate <= 0)
        return 0.0;
    return static_cast<double>(g_runtime.blockFrames) / static_cast<double>(g_runtime.sampleRate);
}

int runtimeActualSampleRate()
{
    return g_runtime.sampleRate;
}

int32_t runtimeBufferSize()
{
    return g_runtime.blockFrames;
}

EngineResult runtimeReconfigureBufferSize(int32_t newBlockSize)
{
    if (newBlockSize <= 0)
        return static_cast<EngineResult>(VN_INVALID_ARG);
    const bool wasInitialized = g_runtime.initialized;
    const bool wasOutputRunning = g_runtime.outputRunning;
    if (wasInitialized)
        runtimeTerm();
    g_runtime.requestedConfig.bufferSize = newBlockSize;
    if (!ensureRuntimeStarted())
        return static_cast<EngineResult>(VN_DEVICE_ERROR);
    if (!wasOutputRunning)
        return kEngineOk;
    const VnResult result = Vn_Resume();
    if (result == VN_OK)
        g_runtime.outputRunning = true;
    return static_cast<EngineResult>(result);
}

void runtimeSetMasterFade(float volume, int fadeMs)
{
    if (!g_runtime.initialized)
        return;
    Vn_SetBusVolume(VN_BUS_MASTER, linearToDb(volume),
                    fadeMs > 0 ? static_cast<uint32_t>(fadeMs) : 0u);
}

void runtimeRegisterSoundName(const QString& name, harkenstudio::ObjectId objectId,
                              harkenstudio::ObjectId soundId, uint32_t sourceId,
                              uint32_t loopCount, bool spatialized3D,
                              harkenstudio::ObjectId attenuationId)
{
    if (name.isEmpty() || harkenstudio::IsNull(soundId) || sourceId == 0)
        return;
    const SoundBinding binding{ soundId, sourceId, loopCount, spatialized3D,
                                objectId, attenuationId };
    g_runtime.soundsById[soundId] = binding;
    if (!harkenstudio::IsNull(objectId))
    {
        g_runtime.soundNameByObjectId[objectId] = name;
        g_runtime.soundIdByObjectId[objectId] = soundId;
    }
}

bool runtimePrepareMedia(uint32_t sourceId)
{
    bool prepared = true;
    for (const auto& [soundId, binding] : g_runtime.soundsById)
    {
        Q_UNUSED(soundId);
        if (binding.sourceId == sourceId)
            prepared = registerPcmForBinding(binding) && prepared;
    }
    return prepared;
}

uint64_t runtimeRegisteredSoundRevision(harkenstudio::ObjectId soundId)
{
    const auto found = g_runtime.registeredSoundRevisions.find(soundId);
    return found == g_runtime.registeredSoundRevisions.end() ? 0 : found->second;
}

VnResult runtimeRegisterSoundPcm(harkenstudio::ObjectId soundId, const std::vector<float>& samples,
                                  uint32_t channels, uint32_t sampleRate,
                                  uint64_t loopStartFrame, uint64_t loopEndFrame)
{
    const auto result = registerDecodedSound(soundId, samples, channels, sampleRate,
                                              loopStartFrame, loopEndFrame);
    if (result == VN_OK)
        g_runtime.preparedSounds.insert(soundId);
    return result;
}

void runtimeUnregisterSoundObject(harkenstudio::ObjectId objectId)
{
    g_runtime.soundNameByObjectId.erase(objectId);

    const auto idIt = g_runtime.soundIdByObjectId.find(objectId);
    if (idIt != g_runtime.soundIdByObjectId.end())
    {
        g_runtime.soundsById.erase(idIt->second);
        g_runtime.soundIdByObjectId.erase(idIt);
    }
}

void runtimeSetObjectOutputBus(harkenstudio::ObjectId objectId, harkenstudio::ObjectId busId)
{
    if (harkenstudio::IsNull(objectId))
        return;
    if (harkenstudio::IsNull(busId))
        g_runtime.objectBusById.erase(objectId);
    else
        g_runtime.objectBusById[objectId] = busId;
}

void runtimeRegisterActionName(const QString& name, harkenstudio::ObjectId actionId)
{
    if (name.isEmpty() || harkenstudio::IsNull(actionId))
        return;
    g_runtime.actionsByName[name] = actionId;
    g_runtime.actionNameById[actionId] = name;
}

void runtimeUnregisterActionName(harkenstudio::ObjectId actionId)
{
    const auto it = g_runtime.actionNameById.find(actionId);
    if (it == g_runtime.actionNameById.end())
        return;
    g_runtime.actionsByName.erase(it->second);
    g_runtime.actionNameById.erase(it);
}

void runtimeClearActionNames()
{
    g_runtime.actionsByName.clear();
    g_runtime.actionNameById.clear();
}

void runtimeClearSoundNames()
{
    runtimeStopAll(0);
    runtimeUpdate();
    if (g_runtime.initialized)
    {
        std::unordered_set<harkenstudio::ObjectId> sounds = g_runtime.registeredSounds;
        sounds.insert(g_runtime.registeredRawPcm.begin(),
                      g_runtime.registeredRawPcm.end());
        for (harkenstudio::ObjectId soundId : sounds)
            Vn_UnregisterPCM(toNative(soundId));
    }
    g_runtime.soundsById.clear();
    g_runtime.soundNameByObjectId.clear();
    g_runtime.soundIdByObjectId.clear();
    runtimeClearActionNames();
    g_runtime.registeredSounds.clear();
    g_runtime.registeredSoundRevisions.clear();
    g_runtime.preparedSounds.clear();
    g_runtime.registeredRawPcm.clear();
    g_runtime.rawPcmDecodes.clear();
    g_runtime.loadedPacksByPath.clear();
    g_runtime.liveVoices.clear();
    g_runtime.voiceNameByHandle.clear();
    g_runtime.playbacks.clear();
    g_runtime.playbackByPreview.clear();
    g_runtime.playbackByAction.clear();
    g_runtime.playbackByRawVoice.clear();
    g_runtime.endedPlaybackHandles.clear();
    g_runtime.voicePitchCentsByName.clear();
    g_runtime.voiceVolumeDbByName.clear();
    g_runtime.voiceSpatMixByName.clear();
    g_runtime.voiceDistanceScaleByName.clear();
    g_runtime.voiceSendsByName.clear();
    g_runtime.objectBusById.clear();
}

harkenstudio::ObjectId runtimeMintRawPcmSoundId()
{
    if (!g_runtime.initialized) return {};
    for (;;) {
        const auto id = VNX::createObjectId();
        if (!g_runtime.registeredSounds.contains(id) && !g_runtime.registeredRawPcm.contains(id))
            return id;
    }
}

EngineResult runtimeRegisterRawPcmSlice(harkenstudio::ObjectId soundId,
                                        const QString& absolutePath,
                                        uint64_t startFrame,
                                        uint64_t frameCount,
                                        bool loops,
                                        uint32_t* outChannels,
                                        uint32_t* outSampleRate,
                                        uint64_t* outWindowStartFrame,
                                        uint64_t* outWindowFrameCount)
{
    if (outChannels)
        *outChannels = 0;
    if (outSampleRate)
        *outSampleRate = 0;
    if (outWindowStartFrame)
        *outWindowStartFrame = 0;
    if (outWindowFrameCount)
        *outWindowFrameCount = 0;
    if (harkenstudio::IsNull(soundId) || absolutePath.isEmpty() || frameCount == 0)
        return kEngineInvalidArg;
    if (!ensureRuntimeStarted())
        return kEngineNotInit;
    if (g_runtime.registeredSounds.find(soundId) != g_runtime.registeredSounds.end()
        && g_runtime.registeredRawPcm.find(soundId) == g_runtime.registeredRawPcm.end())
        return kEngineInUse;

    const QString normalized = QFileInfo(absolutePath).absoluteFilePath();
    auto cachedIt = g_runtime.rawPcmDecodes.find(soundId);
    const bool reuseDecode = cachedIt != g_runtime.rawPcmDecodes.end()
        && cachedIt->second.path == normalized;

    std::vector<float> decoded;
    int channels = 0;
    int sampleRate = 0;
    const std::vector<float>* interleaved = nullptr;
    if (reuseDecode) {
        interleaved = &cachedIt->second.interleaved;
        channels = cachedIt->second.channels;
        sampleRate = cachedIt->second.sampleRate;
    } else {
        if (!decodeAudioFileInterleaved(normalized, decoded, channels, sampleRate))
            return kEngineInvalidArg;
        interleaved = &decoded;
    }

    const EngineResult result = registerDecodedPcmSlice(
        soundId, *interleaved, channels, sampleRate, startFrame, frameCount);
    if (result != kEngineOk)
        return result;
    if (reuseDecode) {
        cachedIt->second.windowStartFrame = startFrame;
        cachedIt->second.windowFrameCount = frameCount;
        cachedIt->second.registeredStartFrame = startFrame;
        cachedIt->second.registeredFrameCount = frameCount;
        cachedIt->second.loops = loops;
    } else {
        rememberRawPcmDecode(soundId, normalized, std::move(decoded),
                             channels, sampleRate, startFrame, frameCount,
                             startFrame, frameCount, loops);
    }
    if (outChannels)
        *outChannels = static_cast<uint32_t>(channels);
    if (outSampleRate)
        *outSampleRate = static_cast<uint32_t>(sampleRate);
    if (outWindowStartFrame)
        *outWindowStartFrame = startFrame;
    if (outWindowFrameCount)
        *outWindowFrameCount = frameCount;
    return kEngineOk;
}

EngineResult runtimeRelocateRawPcmSlice(harkenstudio::ObjectId soundId,
                                        uint64_t startFrame,
                                        uint64_t* outStartFrame,
                                        uint64_t* outFrameCount,
                                        uint32_t* outChannels,
                                        uint32_t* outSampleRate,
                                        uint64_t* outWindowStartFrame,
                                        uint64_t* outWindowFrameCount,
                                        bool* outLoops)
{
    if (outStartFrame)
        *outStartFrame = 0;
    if (outFrameCount)
        *outFrameCount = 0;
    if (outChannels)
        *outChannels = 0;
    if (outSampleRate)
        *outSampleRate = 0;
    if (outWindowStartFrame)
        *outWindowStartFrame = 0;
    if (outWindowFrameCount)
        *outWindowFrameCount = 0;
    if (outLoops)
        *outLoops = false;
    if (harkenstudio::IsNull(soundId))
        return kEngineInvalidArg;
    if (!g_runtime.initialized)
        return kEngineNotInit;

    const auto cachedIt = g_runtime.rawPcmDecodes.find(soundId);
    if (cachedIt == g_runtime.rawPcmDecodes.end()
        || g_runtime.registeredRawPcm.find(soundId) == g_runtime.registeredRawPcm.end())
        return kEngineUnknownId;

    RuntimeState::RawPcmDecodeCache& cached = cachedIt->second;
    const uint64_t windowEnd = cached.windowStartFrame + cached.windowFrameCount;
    if (startFrame < cached.windowStartFrame || startFrame >= windowEnd)
        return kEngineInvalidArg;

    const uint64_t frameCount = cached.loops
        ? cached.windowFrameCount
        : (windowEnd - startFrame);
    if (cached.registeredStartFrame == startFrame
        && cached.registeredFrameCount == frameCount) {
        if (outStartFrame)
            *outStartFrame = startFrame;
        if (outFrameCount)
            *outFrameCount = frameCount;
        if (outChannels)
            *outChannels = static_cast<uint32_t>(cached.channels);
        if (outSampleRate)
            *outSampleRate = static_cast<uint32_t>(cached.sampleRate);
        if (outWindowStartFrame)
            *outWindowStartFrame = cached.windowStartFrame;
        if (outWindowFrameCount)
            *outWindowFrameCount = cached.windowFrameCount;
        if (outLoops)
            *outLoops = cached.loops;
        return kEngineOk;
    }

    const EngineResult result = cached.loops
        ? registerRotatedPcmWindow(soundId, cached.interleaved, cached.channels,
                                   cached.sampleRate, cached.windowStartFrame,
                                   cached.windowFrameCount, startFrame)
        : registerDecodedPcmSlice(soundId, cached.interleaved, cached.channels,
                                  cached.sampleRate, startFrame, frameCount);
    if (result != kEngineOk)
        return result;

    cached.registeredStartFrame = startFrame;
    cached.registeredFrameCount = frameCount;
    if (outStartFrame)
        *outStartFrame = startFrame;
    if (outFrameCount)
        *outFrameCount = frameCount;
    if (outChannels)
        *outChannels = static_cast<uint32_t>(cached.channels);
    if (outSampleRate)
        *outSampleRate = static_cast<uint32_t>(cached.sampleRate);
    if (outWindowStartFrame)
        *outWindowStartFrame = cached.windowStartFrame;
    if (outWindowFrameCount)
        *outWindowFrameCount = cached.windowFrameCount;
    if (outLoops)
        *outLoops = cached.loops;
    return kEngineOk;
}

EngineResult runtimeUnregisterRawPcm(harkenstudio::ObjectId soundId)
{
    if (harkenstudio::IsNull(soundId))
        return kEngineInvalidArg;
    if (!g_runtime.initialized)
        return kEngineNotInit;
    if (g_runtime.registeredRawPcm.find(soundId) == g_runtime.registeredRawPcm.end())
        return kEngineUnknownId;

    forgetRawPcmPlayback(soundId);
    runtimeUpdate();
    const VnResult result = Vn_UnregisterPCM(toNative(soundId));
    if (result != VN_OK)
        return static_cast<EngineResult>(result);
    g_runtime.registeredRawPcm.erase(soundId);
    g_runtime.rawPcmDecodes.erase(soundId);
    return kEngineOk;
}

uint64_t runtimePlayRawPcm(harkenstudio::ObjectId soundId, bool loops, EngineResult* outResult)
{
    if (outResult)
        *outResult = kEngineOk;
    if (harkenstudio::IsNull(soundId)) {
        if (outResult)
            *outResult = kEngineInvalidArg;
        return 0;
    }
    if (!ensureRuntimeStarted()) {
        if (outResult)
            *outResult = kEngineNotInit;
        return 0;
    }
    const auto cachedIt = g_runtime.rawPcmDecodes.find(soundId);
    if (cachedIt == g_runtime.rawPcmDecodes.end()
        || g_runtime.registeredRawPcm.find(soundId) == g_runtime.registeredRawPcm.end()) {
        if (outResult)
            *outResult = kEngineUnknownId;
        return 0;
    }

    if (cachedIt->second.playbackHandle != 0)
        runtimeStopRawPcm(cachedIt->second.playbackHandle, 0);

    VnPlayDesc play{};
    play.structSize = sizeof(VnPlayDesc);
    play.soundId = toNative(soundId);
    play.busId = VN_BUS_MASTER;
    play.loopCount = loops ? 0u : 1u;
    VnVoiceHandle voiceHandle = 0;
    const VnResult result = VnPreview_PlayVoiceRaw(&play, &voiceHandle);
    if (outResult)
        *outResult = static_cast<EngineResult>(result);
    if (result != VN_OK || voiceHandle == 0)
        return 0;

    const uint64_t playbackHandle =
        registerPlayback(RuntimePlaybackKind::RawPcm, voiceHandle);
    g_runtime.liveVoices.insert(voiceHandle);
    cachedIt->second.loops = loops;
    cachedIt->second.voiceHandle = voiceHandle;
    cachedIt->second.playbackHandle = playbackHandle;
    return playbackHandle;
}

bool runtimeStopRawPcm(uint64_t playbackHandle, uint32_t fadeMs)
{
    if (!g_runtime.initialized || playbackHandle == 0)
        return false;
    const auto playback = g_runtime.playbacks.find(playbackHandle);
    if (playback == g_runtime.playbacks.end()
        || playback->second.kind != RuntimePlaybackKind::RawPcm)
        return false;

    const RuntimePlayback record = playback->second;
    const VnResult result =
        Vn_Stop(static_cast<VnVoiceHandle>(record.nativeHandle), fadeMs);
    const bool stale = result == VN_STALE_HANDLE;
    if (result == VN_OK || stale) {
        for (auto& [soundId, cache] : g_runtime.rawPcmDecodes) {
            Q_UNUSED(soundId);
            if (cache.playbackHandle == playbackHandle) {
                cache.voiceHandle = 0;
                cache.playbackHandle = 0;
                break;
            }
        }
        releasePlayback(playbackHandle, stale);
        if (result == VN_OK)
            runtimeUpdate();
        return true;
    }
    return false;
}

bool runtimeLoadPack(const QString& path)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return false;
    if (!ensureRuntimeStarted())
        return false;

    const QString normalized = QFileInfo(path).absoluteFilePath();
    if (g_runtime.loadedPacksByPath.find(normalized) != g_runtime.loadedPacksByPath.end())
        return true;

    const QByteArray utf8 = normalized.toUtf8();
    VnPackDesc desc{};
    desc.structSize = sizeof(VnPackDesc);
    desc.utf8Path = utf8.constData();
    VnPackHandle handle = 0;
    const VnResult result = Vn_LoadPack(&desc, &handle);
    if (result != VN_OK || handle == 0)
    {
        qWarning() << "[PlaybackRoute] runtime pack load failed:"
                   << normalized << "result=" << static_cast<int>(result);
        return false;
    }
    g_runtime.loadedPacksByPath[normalized] = handle;
    return true;
}

void runtimeUnloadPack(const QString& path)
{
    if (!g_runtime.initialized)
        return;
    const QString normalized = QFileInfo(path).absoluteFilePath();
    const auto it = g_runtime.loadedPacksByPath.find(normalized);
    if (it == g_runtime.loadedPacksByPath.end())
        return;
    runtimeStopAll(0);
    runtimeUpdate();
    const VnResult result = Vn_UnloadPack(it->second);
    if (result == VN_OK || result == VN_STALE_HANDLE)
        g_runtime.loadedPacksByPath.erase(it);
}

uint32_t runtimeLoadedPackCount()
{
    return static_cast<uint32_t>(g_runtime.loadedPacksByPath.size());
}

bool runtimeHasActionProgram(harkenstudio::ObjectId actionId)
{
    if (!g_runtime.initialized || harkenstudio::IsNull(actionId))
        return false;
    return VnPreview_HasAction(toNative(actionId)) != 0;
}

uint64_t runtimePlayByName(const QString& name, bool spatialized,
                           float x, float y, float z,
                           EngineResult* outResult,
                           uint64_t borrowedActionEmitter)
{
    if (outResult != nullptr)
        *outResult = kEngineOk;
    if (!ensureRuntimeStarted())
    {
        if (outResult != nullptr)
            *outResult = static_cast<EngineResult>(VN_NOT_INITIALIZED);
        qWarning() << "[PlaybackRoute] runtime play unavailable:" << name;
        return 0;
    }

    const auto actionIt = g_runtime.actionsByName.find(name);
    if (actionIt != g_runtime.actionsByName.end())
    {
        VnEmitterHandle emitter = 0;
        if (spatialized)
        {
            VnResult emitterResult = Vn_CreateEmitter(&emitter);
            if (emitterResult == VN_OK)
            {
                const VnVec3 position{x, y, z};
                emitterResult = Vn_SetEmitterPosition(emitter, position);
            }
            if (emitterResult != VN_OK)
            {
                if (outResult != nullptr)
                    *outResult = static_cast<EngineResult>(emitterResult);
                if (emitter != 0)
                    Vn_DestroyEmitter(emitter);
                return 0;
            }
        }

        VnPostActionDesc desc{};
        desc.structSize = sizeof(desc);
        desc.actionId = toNative(actionIt->second);
        // Only the locally created spatial emitter belongs to this firing.
        // The shared Studio preview emitter survives individual retirements.
        desc.emitter = emitter != 0 ? emitter : borrowedActionEmitter;
        VnActionFiringHandle actionHandle = 0;
        const VnResult result = Vn_PostAction(&desc, &actionHandle);
        if (outResult != nullptr)
            *outResult = static_cast<EngineResult>(result);
        if (result == VN_OK && actionHandle != 0)
        {
            const uint64_t playbackHandle = registerPlayback(
                RuntimePlaybackKind::Action, actionHandle, emitter, name);
            qInfo() << "[PlaybackRoute] trigger runtime action:" << name
                    << "instance=" << static_cast<qulonglong>(actionHandle)
                    << "playback=" << static_cast<qulonglong>(playbackHandle);
            return playbackHandle;
        }
        if (emitter != 0)
            Vn_DestroyEmitter(emitter);
        qWarning() << "[PlaybackRoute] runtime action trigger failed, fallback eligible:"
                   << name << "result=" << static_cast<int>(result);
        return 0;
    }

    const SoundBinding* binding = nullptr;
    for (const auto& [objectId, soundName] : g_runtime.soundNameByObjectId) {
        if (soundName != name) continue;
        if (binding) {
            if (outResult) *outResult = static_cast<EngineResult>(VN_INVALID_ARG);
            qWarning() << "[PlaybackRoute] ambiguous sound name; select by GUID:" << name;
            return 0;
        }
        const auto soundId = g_runtime.soundIdByObjectId.find(objectId);
        if (soundId == g_runtime.soundIdByObjectId.end()) continue;
        const auto sound = g_runtime.soundsById.find(soundId->second);
        if (sound != g_runtime.soundsById.end()) binding = &sound->second;
    }
    if (!binding)
    {
        if (outResult != nullptr)
            *outResult = static_cast<EngineResult>(VN_UNKNOWN_ID);
        qWarning() << "[PlaybackRoute] runtime play miss, fallback eligible:" << name;
        return 0;
    }

    VnEmitterHandle emitter = 0;
    if (spatialized || binding->spatialized3D)
    {
        VnResult emitterResult = Vn_CreateEmitter(&emitter);
        if (emitterResult == VN_OK)
        {
            const VnVec3 position{x, y, z};
            emitterResult = Vn_SetEmitterPosition(emitter, position);
        }
        if (emitterResult != VN_OK)
        {
            if (outResult != nullptr)
                *outResult = static_cast<EngineResult>(emitterResult);
            if (emitter != 0)
                Vn_DestroyEmitter(emitter);
            return 0;
        }
    }

    auto tryPlay = [&]() -> std::pair<VnPreviewResult,
                                      VnPreviewPlayableHandle> {
        VnPreviewPlayPlayableDescV2 play{};
        play.structSize = sizeof(play);
        play.playableKind = VN_PREVIEW_PLAYABLE_KIND_SOUND;
        play.stableId = toNative(binding->objectId);
        play.emitterHandle = emitter;
        VnPreviewPlayableHandle h = 0;
        const VnPreviewResult r = VnPreview_PlayPlayable(&play, &h);
        return {r, h};
    };

    auto [playResult, handle] = tryPlay();
    if (playResult == VN_PREVIEW_UNKNOWN_ID)
    {
        if (!registerPcmForBinding(*binding))
        {
            if (outResult != nullptr)
                *outResult = static_cast<EngineResult>(VN_UNKNOWN_ID);
            if (emitter != 0)
                Vn_DestroyEmitter(emitter);
            qWarning() << "[PlaybackRoute] runtime play media miss, fallback eligible:"
                       << name << "sourceId=" << binding->sourceId;
            return 0;
        }
        std::tie(playResult, handle) = tryPlay();
    }
    if (outResult != nullptr)
        *outResult = static_cast<EngineResult>(playResult);

    if (playResult != VN_PREVIEW_OK || handle == 0)
    {
        if (emitter != 0)
            Vn_DestroyEmitter(emitter);
        qWarning() << "[PlaybackRoute] runtime play failed, fallback eligible:" << name;
        return 0;
    }
    const uint64_t playbackHandle =
        registerPlayback(RuntimePlaybackKind::Preview, handle, emitter);
    qInfo() << "[PlaybackRoute] play runtime:" << name
            << "playback=" << static_cast<qulonglong>(playbackHandle);
    return playbackHandle;
}

ActionSeekStartResult runtimeSeekAction(uint64_t playbackHandle,
                                        const QString& expectedActionName,
                                        double positionMs, bool relative)
{
    ActionSeekStartResult result;
    if (!g_runtime.initialized)
    {
        result.result = static_cast<EngineResult>(VN_NOT_INITIALIZED);
        return result;
    }
    if (g_runtime.actionSeeks.size() >= 256u)
    {
        result.result = static_cast<EngineResult>(VN_BACKPRESSURE);
        return result;
    }
    const auto playback = g_runtime.playbacks.find(playbackHandle);
    if (playback == g_runtime.playbacks.end())
    {
        result.result = static_cast<EngineResult>(VN_STALE_HANDLE);
        return result;
    }
    if (playback->second.kind != RuntimePlaybackKind::Action)
    {
        result.result = static_cast<EngineResult>(VN_INVALID_ARG);
        return result;
    }
    if (!expectedActionName.isEmpty()
        && playback->second.actionName != expectedActionName)
    {
        result.result = static_cast<EngineResult>(VN_INVALID_ARG);
        return result;
    }

    VnActionSeekDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.firing = static_cast<VnActionFiringHandle>(
        playback->second.nativeHandle);
    desc.positionMs = positionMs;
    desc.mode = relative ? VN_ACTION_SEEK_RELATIVE : VN_ACTION_SEEK_ABSOLUTE;
    VnActionSeekTicket ticket = 0;
    const VnResult nativeResult = Vn_SeekActionFiring(&desc, &ticket);
    result.result = static_cast<EngineResult>(nativeResult);
    if (nativeResult != VN_OK)
        return result;

    result.operationId = allocateActionSeekOperationId();
    RuntimeActionSeek operation;
    operation.nativeTicket = ticket;
    operation.info.playbackHandle = playbackHandle;
    operation.info.actionName = playback->second.actionName;
    g_runtime.actionSeeks.emplace(result.operationId, std::move(operation));
    return result;
}

EngineResult runtimeQueryActionSeek(uint64_t operationId,
                                    ActionSeekInfo& outInfo)
{
    outInfo = {};
    if (!g_runtime.initialized)
        return static_cast<EngineResult>(VN_NOT_INITIALIZED);
    const auto operation = g_runtime.actionSeeks.find(operationId);
    if (operation == g_runtime.actionSeeks.end())
        return static_cast<EngineResult>(VN_STALE_HANDLE);
    outInfo = operation->second.info;
    return kEngineOk;
}

EngineResult runtimeReleaseActionSeek(uint64_t operationId)
{
    if (!g_runtime.initialized)
        return static_cast<EngineResult>(VN_NOT_INITIALIZED);
    const auto operation = g_runtime.actionSeeks.find(operationId);
    if (operation == g_runtime.actionSeeks.end())
        return static_cast<EngineResult>(VN_STALE_HANDLE);
    if (operation->second.info.serviceResult != kEngineOk)
    {
        const EngineResult serviceResult =
            operation->second.info.serviceResult;
        g_runtime.actionSeeks.erase(operation);
        return serviceResult;
    }
    if (operation->second.info.state == ActionSeekState::Accepted
        || !operation->second.info.nativeReleased)
    {
        return static_cast<EngineResult>(VN_IN_USE);
    }
    g_runtime.actionSeeks.erase(operation);
    return kEngineOk;
}

bool runtimeStopPlayback(uint64_t handle, uint32_t fadeMs)
{
    if (!g_runtime.initialized || handle == 0)
        return false;
    const auto playback = g_runtime.playbacks.find(handle);
    if (playback == g_runtime.playbacks.end())
        return false;
    const RuntimePlayback record = playback->second;
    const int32_t result =
        record.kind == RuntimePlaybackKind::Preview
            ? VnPreview_StopPlayable(
                  static_cast<VnPreviewPlayableHandle>(record.nativeHandle),
                  fadeMs)
            : record.kind == RuntimePlaybackKind::Action
                ? Vn_StopActionFiring(
                      static_cast<VnActionFiringHandle>(record.nativeHandle),
                      fadeMs)
                : Vn_Stop(static_cast<VnVoiceHandle>(record.nativeHandle), fadeMs);
    const bool stale = record.kind == RuntimePlaybackKind::Preview
        ? result == VN_PREVIEW_STALE_HANDLE
        : result == VN_STALE_HANDLE;
    if (stale)
        releasePlayback(handle, false);
    if (result == 0 || stale)
    {
        qInfo() << "[PlaybackRoute] stop runtime:"
                << "playback=" << static_cast<qulonglong>(handle)
                << "native=" << static_cast<qulonglong>(record.nativeHandle)
                << "result=" << static_cast<int>(result);
        return true;
    }
    return false;
}

void runtimeStopAll(uint32_t fadeMs)
{
    if (!g_runtime.initialized)
        return;
    const VnResult result = Vn_StopAll(fadeMs);
    if (result != VN_OK)
        qWarning() << "[PlaybackRoute] stop all failed result="
                   << static_cast<int>(result);
}

bool runtimeHasCurrentWorldPlayableConsumers()
{
    return runtimeIsActive() && VnPreview_HasPlayableConsumers() != 0;
}

EngineResult runtimeRefreshPreviewVoiceBindings(uint64_t voiceInstance)
{
    return VnPreview_RefreshVoiceBindings(voiceInstance);
}

EngineResult runtimeSetPreviewVoiceProperty(uint64_t voiceInstance, uint8_t property,
    float value, harkenstudio::ObjectId sendBusId)
{
    return VnPreview_SetVoiceProperty(voiceInstance, property, value, toNative(sendBusId));
}

void runtimeSetVoicePitchByName(const QString& name, float cents)
{
    if (name.isEmpty())
        return;
    if (cents == 0.0f)
        g_runtime.voicePitchCentsByName.erase(name);
    else
        g_runtime.voicePitchCentsByName[name] = cents;
    if (!g_runtime.initialized)
        return;
    const float ratio = pitchRatioFromCents(cents);
    for (const auto& [handle, voiceName] : g_runtime.voiceNameByHandle)
    {
        if (voiceName != name)
            continue;
        if (g_runtime.liveVoices.find(handle) == g_runtime.liveVoices.end())
            continue;
        Vn_SetVoicePitch(static_cast<VnVoiceHandle>(handle), ratio);
    }
}

void runtimeSetVoiceVolumeByName(const QString& name, float volumeDb)
{
    if (name.isEmpty())
        return;
    if (volumeDb == 0.0f)
        g_runtime.voiceVolumeDbByName.erase(name);
    else
        g_runtime.voiceVolumeDbByName[name] = volumeDb;
    if (!g_runtime.initialized)
        return;
    for (const auto& [handle, voiceName] : g_runtime.voiceNameByHandle)
    {
        if (voiceName != name)
            continue;
        if (g_runtime.liveVoices.find(handle) == g_runtime.liveVoices.end())
            continue;
        Vn_SetVoiceVolume(static_cast<VnVoiceHandle>(handle), volumeDb, 20);
    }
}

void runtimeSetVoiceOutputVolumeByName(const QString& name, float volumeDb)
{
    if (name.isEmpty())
        return;
    if (!g_runtime.initialized)
        return;
    for (const auto& [handle, voiceName] : g_runtime.voiceNameByHandle)
    {
        if (voiceName != name)
            continue;
        if (g_runtime.liveVoices.find(handle) == g_runtime.liveVoices.end())
            continue;
        Vn_SetVoiceOutputVolume(static_cast<VnVoiceHandle>(handle), volumeDb, 20);
    }
}

void runtimeSetVoiceSpatMixByName(const QString& name, float mix)
{
    if (name.isEmpty())
        return;
    if (mix == 1.0f)
        g_runtime.voiceSpatMixByName.erase(name);
    else
        g_runtime.voiceSpatMixByName[name] = mix;
    if (!g_runtime.initialized)
        return;
    for (const auto& [handle, voiceName] : g_runtime.voiceNameByHandle)
    {
        if (voiceName != name)
            continue;
        if (g_runtime.liveVoices.find(handle) == g_runtime.liveVoices.end())
            continue;
        Vn_SetVoiceSpatializationMix(static_cast<VnVoiceHandle>(handle), mix);
    }
}

void runtimeSetVoiceDistanceScaleByName(const QString& name, float scale)
{
    if (name.isEmpty())
        return;
    if (scale == 1.0f)
        g_runtime.voiceDistanceScaleByName.erase(name);
    else
        g_runtime.voiceDistanceScaleByName[name] = scale;
    if (!g_runtime.initialized)
        return;
    for (const auto& [handle, voiceName] : g_runtime.voiceNameByHandle)
    {
        if (voiceName != name)
            continue;
        if (g_runtime.liveVoices.find(handle) == g_runtime.liveVoices.end())
            continue;
        Vn_SetVoiceDistanceScale(static_cast<VnVoiceHandle>(handle), scale);
    }
}

void runtimeSetVoiceSendsByName(const QString& name,
                                const std::vector<std::pair<harkenstudio::ObjectId, float>>& sends)
{
    if (name.isEmpty())
        return;
    uint32_t prevCount = 0;
    if (const auto it = g_runtime.voiceSendsByName.find(name);
        it != g_runtime.voiceSendsByName.end())
        prevCount = static_cast<uint32_t>(it->second.size());
    if (sends.empty())
        g_runtime.voiceSendsByName.erase(name);
    else
        g_runtime.voiceSendsByName[name] = sends;
    if (!g_runtime.initialized)
        return;
    for (const auto& [handle, voiceName] : g_runtime.voiceNameByHandle)
    {
        if (voiceName != name)
            continue;
        if (g_runtime.liveVoices.find(handle) == g_runtime.liveVoices.end())
            continue;
        uint32_t slot = 0;
        for (const auto& [busId, levelDb] : sends)
            Vn_SetVoiceSend(static_cast<VnVoiceHandle>(handle), slot++,
                            toNative(busId), levelDb);
        for (; slot < prevCount; ++slot)
            Vn_ClearVoiceSend(static_cast<VnVoiceHandle>(handle), slot);
    }
}

bool runtimeSetPlaybackPosition(uint64_t handle, float x, float y, float z)
{
    if (!g_runtime.initialized || handle == 0)
        return false;
    const auto playback = g_runtime.playbacks.find(handle);
    if (playback == g_runtime.playbacks.end())
        return false;
    const RuntimePlayback& record = playback->second;
    const VnResult result = record.emitter != 0
        ? Vn_SetEmitterPosition(record.emitter, VnVec3{x, y, z})
        : VN_INVALID_ARG;
    return result == VN_OK || result == VN_STALE_HANDLE;
}

bool runtimeOwnsPlayback(uint64_t handle)
{
    return g_runtime.playbacks.find(handle) != g_runtime.playbacks.end();
}

RuntimeLiveOwnerRevision runtimeLiveOwnerRevision(std::uint64_t ownerId)
{
    RuntimeLiveOwnerRevision result;
    result.engineActive = runtimeIsActive();
    if (result.engineActive && ownerId != 0)
        result.found = VnPreview_QueryOwnerRevision(ownerId, &result.currentRevision) != 0;
    return result;
}

RuntimeProjectGenerationPrepareStatus runtimePrepareProjectGeneration(
    const VnPreviewProjectDescV1& view, RuntimePreparedProject& out)
{
    if (out.Valid())
        return RuntimeProjectGenerationPrepareStatus::InvalidView;
    if (!runtimeIsActive())
        return RuntimeProjectGenerationPrepareStatus::RuntimeUnavailable;
    return static_cast<RuntimeProjectGenerationPrepareStatus>(
        VnPreview_PrepareProject(&view, &out.handle));
}

RuntimeProjectGenerationPrepareStatus runtimePrepareProjectGeneration(
    const VnPreviewProjectDescV2& view, RuntimePreparedProject& out)
{
    if (out.Valid())
        return RuntimeProjectGenerationPrepareStatus::InvalidView;
    if (!runtimeIsActive())
        return RuntimeProjectGenerationPrepareStatus::RuntimeUnavailable;
    return static_cast<RuntimeProjectGenerationPrepareStatus>(
        VnPreview_PrepareProjectV2(&view, &out.handle));
}

RuntimeProjectGenerationPrepareStatus runtimePrepareProjectGeneration(
    const VnPreviewProjectDescV3& view, RuntimePreparedProject& out)
{
    if (out.Valid())
        return RuntimeProjectGenerationPrepareStatus::InvalidView;
    if (!runtimeIsActive())
        return RuntimeProjectGenerationPrepareStatus::RuntimeUnavailable;
    return static_cast<RuntimeProjectGenerationPrepareStatus>(
        VnPreview_PrepareProjectV3(&view, &out.handle));
}

RuntimeProjectGenerationCommitStatus runtimeCommitPreparedProjectGeneration(
    RuntimePreparedProject& prepared, std::uint64_t* outInstalledWorldGeneration)
{
    if (!runtimeIsActive())
        return RuntimeProjectGenerationCommitStatus::RuntimeUnavailable;
    const auto status = VnPreview_CommitProject(prepared.handle, outInstalledWorldGeneration);
    if (status == VN_PREVIEW_COMMIT_OK)
        prepared.handle = 0;
    return static_cast<RuntimeProjectGenerationCommitStatus>(status);
}

void runtimeCancelPreparedProjectGeneration(RuntimePreparedProject&& prepared)
{
    VnPreview_CancelProject(std::exchange(prepared.handle, 0));
}

std::uint32_t runtimeDrainProjectGenerationAcks()
{
    return runtimeIsActive() ? VnPreview_DrainProjectAcks() : 0;
}

bool runtimeCurrentWorldBusAdmissionIsOpen(std::uint64_t worldGeneration)
{
    return runtimeIsActive() && VnPreview_ProjectIsOpen(worldGeneration) != 0;
}

void runtimePumpNullMixer(std::uint32_t blockCount)
{
    if (runtimeIsActive())
        VnPreview_PumpNullMixer(blockCount);
}

void runtimeConfirmBusTopologyWithoutApply(
    const std::vector<RuntimeBusDefinition>& buses,
    const std::vector<RuntimeBusSendDefinition>& sends)
{
    EngineSession::confirmActiveBusTopologyWithoutApply(buses, sends);
}

EngineResult runtimeSetBusMute(harkenstudio::ObjectId busId, bool mute, std::uint32_t fadeMs)
{
    if (!runtimeIsActive())
        return kEngineNotInit;
    return static_cast<EngineResult>(
        Vn_SetBusMute(toNative(busId), mute ? 1u : 0u, fadeMs));
}

EngineResult runtimeSetBusSolo(harkenstudio::ObjectId busId, bool solo, std::uint32_t fadeMs)
{
    if (!runtimeIsActive())
        return kEngineNotInit;
    return static_cast<EngineResult>(
        Vn_SetBusSolo(toNative(busId), solo ? 1u : 0u, fadeMs));
}

EngineResult runtimeSetBusOutputVolume(harkenstudio::ObjectId busId, float db,
                                       std::uint32_t fadeMs)
{
    if (!runtimeIsActive())
        return kEngineNotInit;
    return static_cast<EngineResult>(
        Vn_SetBusOutputVolume(toNative(busId), db, fadeMs));
}

} // namespace Detail
} // namespace Bridge
} // namespace HarkenEditor
