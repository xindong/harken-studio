#include "PlaybackBridge.h"
#include "NativeValues.h"

#include "BusTopologySync.h"
#include "DiagnosticsBridge.h"
#include "PackagingBridge.h"
#include "RuntimeLink.h"

#include <harken/Harken.h>

#include <QByteArray>
#include <QDebug>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

namespace HarkenEditor {
namespace Bridge {

static_assert(kEngineBackpressure == static_cast<EngineResult>(VN_BACKPRESSURE),
              "kEngineBackpressure must mirror the harken_core result code");
static_assert(toNative(kEngineMasterBusId) == VN_BUS_MASTER,
              "kEngineMasterBusId must mirror the harken_core master bus id");
static_assert(kEngineInUse == static_cast<EngineResult>(VN_IN_USE),
              "kEngineInUse must mirror the harken_core result code");
static_assert(kEngineUnknownId == static_cast<EngineResult>(VN_UNKNOWN_ID),
              "kEngineUnknownId must mirror the harken_core result code");
static_assert(kEngineNotInit ==
                  static_cast<EngineResult>(VN_NOT_INITIALIZED),
              "kEngineNotInit must mirror the harken_core result code");
static_assert(kEngineSendCycle == static_cast<EngineResult>(VN_SEND_CYCLE),
              "kEngineSendCycle must mirror the harken_core result code");

namespace {

constexpr uint32_t kPrimaryListenerId = 1;

struct BusState {
    harkenstudio::ObjectId parentId = kEngineMasterBusId;
    QString name;
    float volumeDb = 0.0f;
    float outputVolumeDb = 0.0f;
    bool mute = false;
    bool solo = false;
};

using BusCache = std::unordered_map<harkenstudio::ObjectId, BusState>;
using BusSendCache =
    std::map<std::pair<harkenstudio::ObjectId, harkenstudio::ObjectId>, RuntimeBusSendDefinition>;

BusCache g_buses;
BusSendCache g_busSends;
struct PendingBusCache {
    BusCache buses;
    BusSendCache sends;
};
std::optional<PendingBusCache> g_pendingBusCache;
struct PendingConfirmedPropertyEdit {
    std::optional<float> volumeDb;
    std::optional<float> outputVolumeDb;
    std::optional<bool> mute;
    std::optional<bool> solo;
};
std::unordered_map<harkenstudio::ObjectId, PendingConfirmedPropertyEdit>
    g_pendingConfirmedPropertyEdits;
std::unique_ptr<Detail::BusTopologySync> g_busTopologySync;

BusCache& desiredBusCache()
{
    return g_pendingBusCache
        ? g_pendingBusCache->buses
        : g_buses;
}

const BusSendCache& desiredBusSendCache()
{
    return g_pendingBusCache
        ? g_pendingBusCache->sends
        : g_busSends;
}

void rememberBus(BusCache& buses, harkenstudio::ObjectId busId, harkenstudio::ObjectId parentBusId,
                 const QString& name)
{
    if (harkenstudio::IsNull(busId))
        return;
    BusState& state = buses[busId];
    state.parentId = busId == kEngineMasterBusId
        ? harkenstudio::ObjectId{} : parentBusId;
    state.name = name;
}

Detail::BusTopologyBackend makeBusTopologyBackend()
{
    Detail::BusTopologyBackend backend;
    backend.applySnapshot =
        [](const std::vector<RuntimeBusDefinition>& buses,
           const std::vector<RuntimeBusSendDefinition>& sends,
           uint64_t baseRevision, uint64_t revision) {
            std::vector<VnBusTopologyBus> busRows;
            busRows.reserve(buses.size());
            for (const RuntimeBusDefinition& bus : buses) {
                VnBusTopologyBus row{};
                row.structSize = sizeof(row);
                row.busId = toNative(bus.id);
                row.parentId = toNative(bus.parentId);
                busRows.push_back(row);
            }
            std::vector<VnBusTopologySend> sendRows;
            sendRows.reserve(sends.size());
            for (const RuntimeBusSendDefinition& send : sends) {
                VnBusTopologySend row{};
                row.structSize = sizeof(row);
                row.fromBusId = toNative(send.sourceId);
                row.toBusId = toNative(send.targetId);
                row.levelDb = send.levelDb;
                sendRows.push_back(row);
            }
            VnBusTopologyDesc desc{};
            desc.structSize = sizeof(desc);
            desc.ownerId = 0x53545544494F4255ull;
            desc.baseRevision = baseRevision;
            desc.revision = revision;
            desc.busCount = static_cast<uint32_t>(busRows.size());
            desc.busStructSize = sizeof(VnBusTopologyBus);
            desc.buses = busRows.empty() ? nullptr : busRows.data();
            desc.sendCount = static_cast<uint32_t>(sendRows.size());
            desc.sendStructSize = sizeof(VnBusTopologySend);
            desc.sends = sendRows.empty() ? nullptr : sendRows.data();
            return static_cast<EngineResult>(
                Vn_ApplyBusTopology(&desc));
        };
    backend.setVolume = [](harkenstudio::ObjectId busId, float volumeDb,
                           uint32_t fadeMs) {
        return static_cast<EngineResult>(
            Vn_SetBusVolume(toNative(busId), volumeDb,
                            fadeMs));
    };
    backend.setOutputVolume = [](harkenstudio::ObjectId busId, float volumeDb,
                                 uint32_t fadeMs) {
        return static_cast<EngineResult>(
            Vn_SetBusOutputVolume(toNative(busId), volumeDb,
                                  fadeMs));
    };
    backend.setMute = [](harkenstudio::ObjectId busId, bool mute, uint32_t fadeMs) {
        return static_cast<EngineResult>(
            Vn_SetBusMute(toNative(busId),
                          mute ? 1u : 0u, fadeMs));
    };
    backend.setSolo = [](harkenstudio::ObjectId busId, bool solo, uint32_t fadeMs) {
        return static_cast<EngineResult>(
            Vn_SetBusSolo(toNative(busId),
                          solo ? 1u : 0u, fadeMs));
    };
    return backend;
}

void settlePendingBusCache();

void submitDesiredBusTopology(uint32_t propertyFadeMs = 0)
{
    if (!g_busTopologySync)
        return;

    const BusCache& desiredBuses = desiredBusCache();
    std::vector<RuntimeBusDefinition> buses;
    buses.reserve(desiredBuses.size());
    for (const auto& entry : desiredBuses) {
        RuntimeBusDefinition bus;
        bus.id = entry.first;
        bus.parentId = entry.second.parentId;
        bus.name = entry.second.name;
        bus.volumeDb = entry.second.volumeDb;
        bus.outputVolumeDb = entry.second.outputVolumeDb;
        bus.mute = entry.second.mute;
        bus.solo = entry.second.solo;
        buses.push_back(std::move(bus));
    }

    const BusSendCache& desiredSends = desiredBusSendCache();
    std::vector<RuntimeBusSendDefinition> sends;
    sends.reserve(desiredSends.size());
    for (const auto& entry : desiredSends)
        sends.push_back(entry.second);

    g_busTopologySync->ConfirmWithoutApply(buses, sends);
    settlePendingBusCache();
}

void settlePendingBusCache()
{
    if (!g_pendingBusCache || !g_busTopologySync)
        return;

    if (g_busTopologySync->State() == BusTopologyState::Ready) {
        g_buses = std::move(g_pendingBusCache->buses);
        g_busSends = std::move(g_pendingBusCache->sends);
        g_pendingBusCache.reset();
        g_pendingConfirmedPropertyEdits.clear();
    } else if (g_busTopologySync->State() == BusTopologyState::Failed) {
        std::vector<Detail::ConfirmedBusPropertyPatch> replayPatches;
        if (g_busTopologySync->Failure().operation ==
            BusTopologyOperation::ApplySnapshot) {
            replayPatches.reserve(g_pendingConfirmedPropertyEdits.size());
            for (const auto& entry : g_pendingConfirmedPropertyEdits) {
                const auto confirmed = g_buses.find(entry.first);
                if (confirmed == g_buses.end())
                    continue;
                auto& confirmedBus = confirmed->second;
                if (entry.second.volumeDb)
                    confirmedBus.volumeDb = *entry.second.volumeDb;
                if (entry.second.outputVolumeDb)
                    confirmedBus.outputVolumeDb = *entry.second.outputVolumeDb;
                if (entry.second.mute)
                    confirmedBus.mute = *entry.second.mute;
                if (entry.second.solo)
                    confirmedBus.solo = *entry.second.solo;
                Detail::ConfirmedBusPropertyPatch patch;
                patch.busId = confirmed->first;
                patch.busName = confirmedBus.name;
                patch.volumeDb = entry.second.volumeDb;
                patch.outputVolumeDb = entry.second.outputVolumeDb;
                patch.mute = entry.second.mute;
                patch.solo = entry.second.solo;
                replayPatches.push_back(std::move(patch));
            }
        }
        g_pendingBusCache.reset();
        g_pendingConfirmedPropertyEdits.clear();
        if (!replayPatches.empty()) {
            g_busTopologySync->BeginConfirmedPropertyReplay(
                replayPatches);
            g_busTopologySync->PumpConfirmedPropertyReplay();
        }
    }
}

} // namespace

//==============================================================================
// EngineSession
//==============================================================================

struct EngineSession::Impl {
    std::function<void(int level, const QString& msg)> logHandler;
};

EngineSession::EngineSession() : impl_(new Impl) {}

std::unique_ptr<EngineSession> EngineSession::create(const EngineSessionConfig& config)
{
    if (!Detail::runtimeInit(config))
        return nullptr;
    g_buses.clear();
    g_busSends.clear();
    g_pendingBusCache.reset();
    g_pendingConfirmedPropertyEdits.clear();
    rememberBus(g_buses, kEngineMasterBusId, {},
                QStringLiteral("Master"));
    g_busTopologySync = std::make_unique<Detail::BusTopologySync>(
        makeBusTopologyBackend(), kEngineMasterBusId);
    return std::unique_ptr<EngineSession>(new EngineSession());
}

EngineSession::~EngineSession()
{
    g_pendingBusCache.reset();
    g_pendingConfirmedPropertyEdits.clear();
    g_busTopologySync.reset();
    Detail::runtimeTerm();
}

void EngineSession::setLogHandler(std::function<void(int, const QString&)> handler)
{
    impl_->logHandler = std::move(handler);
    Detail::runtimeSetLogHandler(impl_->logHandler);
}

void EngineSession::update()
{
    Detail::runtimeUpdate();
    if (g_busTopologySync) {
        g_busTopologySync->Pump();
        if (g_busTopologySync->State() == BusTopologyState::Failed)
            g_busTopologySync->PumpConfirmedPropertyReplay();
        settlePendingBusCache();
    }
}
void EngineSession::publishMeterPeaks() { Detail::runtimePublishMeterPeaks(); }
int EngineSession::activeVoiceCount() const { return Detail::runtimeActiveVoiceCount(); }
void EngineSession::masterMeterPeak(float& l, float& r) const { Detail::runtimeMasterMeterPeak(l, r); }
float EngineSession::audioCpuPercent() const { return Detail::runtimeAudioCpuPercent(); }

EngineResult EngineSession::startOutput() { return Detail::runtimeStartOutput(); }
void EngineSession::stopOutput() { Detail::runtimeStopOutput(); }
void EngineSession::setMasterFade(float volume, int fadeMs) { Detail::runtimeSetMasterFade(volume, fadeMs); }
double EngineSession::outputLatency() const { return Detail::runtimeOutputLatency(); }
int EngineSession::actualSampleRate() const { return Detail::runtimeActualSampleRate(); }
EngineResult EngineSession::reconfigureBufferSize(int32_t newBlockSize)
{
    return Detail::runtimeReconfigureBufferSize(newBlockSize);
}
int32_t EngineSession::bufferSize() const { return Detail::runtimeBufferSize(); }

PlaybackStartResult EngineSession::playByNameDetailed(const QString& name,
                                                     uint64_t borrowedEmitter)
{
    PlaybackStartResult result;
    if (g_busTopologySync &&
        g_busTopologySync->State() == BusTopologyState::Syncing) {
        result.result = kEngineBackpressure;
        result.failure = PlaybackFailure::TopologySyncPending;
        return result;
    }
    if (g_busTopologySync &&
        g_busTopologySync->State() == BusTopologyState::Failed) {
        result.topologyFailure = g_busTopologySync->Failure();
        result.result = result.topologyFailure.result;
        result.failure = PlaybackFailure::TopologySyncFailed;
        return result;
    }

    result.handle = Detail::runtimePlayByName(
        name, false, 0.0f, 0.0f, 0.0f, &result.result, borrowedEmitter);
    if (result.handle != 0)
        return result;
    result.failure = result.result == kEngineOk
        ? PlaybackFailure::NoVoiceProduced
        : PlaybackFailure::RuntimeRefused;
    return result;
}

PlaybackStartResult EngineSession::playByName3DDetailed(
    const QString& name, float x, float y, float z)
{
    PlaybackStartResult result;
    if (g_busTopologySync &&
        g_busTopologySync->State() == BusTopologyState::Syncing) {
        result.result = kEngineBackpressure;
        result.failure = PlaybackFailure::TopologySyncPending;
        return result;
    }
    if (g_busTopologySync &&
        g_busTopologySync->State() == BusTopologyState::Failed) {
        result.topologyFailure = g_busTopologySync->Failure();
        result.result = result.topologyFailure.result;
        result.failure = PlaybackFailure::TopologySyncFailed;
        return result;
    }

    result.handle = Detail::runtimePlayByName(
        name, true, x, y, z, &result.result);
    if (result.handle != 0)
        return result;
    result.failure = result.result == kEngineOk
        ? PlaybackFailure::NoVoiceProduced
        : PlaybackFailure::RuntimeRefused;
    return result;
}

uint64_t EngineSession::playByName(const QString& name)
{
    return playByNameDetailed(name).handle;
}

uint64_t EngineSession::playByName3D(const QString& name,
                                     float x, float y, float z)
{
    return playByName3DDetailed(name, x, y, z).handle;
}

ActionSeekStartResult EngineSession::seekAction(
    uint64_t playbackHandle, const QString& expectedActionName,
    double positionMs, bool relative)
{
    return Detail::runtimeSeekAction(
        playbackHandle, expectedActionName, positionMs, relative);
}

EngineResult EngineSession::queryActionSeek(
    uint64_t operationId, ActionSeekInfo& outInfo) const
{
    return Detail::runtimeQueryActionSeek(operationId, outInfo);
}

EngineResult EngineSession::releaseActionSeek(uint64_t operationId)
{
    return Detail::runtimeReleaseActionSeek(operationId);
}

bool EngineSession::hasActionProgram(harkenstudio::ObjectId actionId) const
{
    return Detail::runtimeHasActionProgram(actionId);
}

void EngineSession::stopPlayback(uint64_t handle, float fadeMs)
{
    Detail::runtimeStopPlayback(handle, fadeMs > 0.0f ? static_cast<uint32_t>(fadeMs) : 0u);
}

void EngineSession::stopAll(float fadeMs)
{
    Detail::runtimeStopAll(fadeMs > 0.0f ? static_cast<uint32_t>(fadeMs) : 0u);
}

bool EngineSession::hasCurrentWorldPlayableConsumers() const
{
    return Detail::runtimeHasCurrentWorldPlayableConsumers();
}

void EngineSession::pumpNullMixer(std::uint32_t blockCount)
{
    Detail::runtimePumpNullMixer(blockCount);
}

void EngineSession::setPlaybackPosition(uint64_t handle, float x, float y, float z)
{
    Detail::runtimeSetPlaybackPosition(handle, x, y, z);
}

void EngineSession::setListenerTransform(float px, float py, float pz,
                                         float fx, float fy, float fz,
                                         float ux, float uy, float uz)
{
    Vn_SetListener(VnVec3{px, py, pz}, VnVec3{fx, fy, fz}, VnVec3{ux, uy, uz});
}

bool EngineSession::loadPack(const QString& path) { return Detail::runtimeLoadPack(path); }
void EngineSession::unloadPack(const QString& path) { Detail::runtimeUnloadPack(path); }
uint32_t EngineSession::loadedPackCount() const { return Detail::runtimeLoadedPackCount(); }

bool EngineSession::prepareMedia(uint32_t sourceId, const QString& absolutePath, uint64_t fileSize)
{
    mediaRegistryRegister(sourceId, absolutePath, fileSize);
    return Detail::runtimePrepareMedia(sourceId);
}

bool EngineSession::swapMedia(uint32_t sourceId, const QString& newPath, uint64_t fileSize)
{
    mediaRegistryRegister(sourceId, newPath, fileSize);
    return Detail::runtimePrepareMedia(sourceId);
}

void EngineSession::clearPreparedMedia() { mediaRegistryClear(); }

harkenstudio::ObjectId EngineSession::mintRawPcmSoundId()
{
    return Detail::runtimeMintRawPcmSoundId();
}

EngineSession::RawPcmSliceRegistration EngineSession::registerRawPcmSlice(
    harkenstudio::ObjectId soundId, const QString& absolutePath,
    uint64_t startFrame, uint64_t frameCount, bool loops)
{
    RawPcmSliceRegistration registration;
    registration.soundId = soundId;
    registration.startFrame = startFrame;
    registration.frameCount = frameCount;
    registration.windowStartFrame = startFrame;
    registration.windowFrameCount = frameCount;
    registration.loops = loops;
    uint32_t channels = 0;
    uint32_t sampleRate = 0;
    uint64_t windowStartFrame = 0;
    uint64_t windowFrameCount = 0;
    registration.result = Detail::runtimeRegisterRawPcmSlice(
        soundId, absolutePath, startFrame, frameCount, loops,
        &channels, &sampleRate, &windowStartFrame, &windowFrameCount);
    if (registration.result == kEngineOk) {
        registration.channels = channels;
        registration.sampleRate = sampleRate;
        registration.windowStartFrame = windowStartFrame;
        registration.windowFrameCount = windowFrameCount;
    }
    return registration;
}

EngineSession::RawPcmSliceRegistration EngineSession::relocateRawPcmSlice(
    harkenstudio::ObjectId soundId, uint64_t startFrame)
{
    RawPcmSliceRegistration registration;
    registration.soundId = soundId;
    uint64_t relocatedStart = 0;
    uint64_t frameCount = 0;
    uint32_t channels = 0;
    uint32_t sampleRate = 0;
    uint64_t windowStartFrame = 0;
    uint64_t windowFrameCount = 0;
    bool loops = false;
    registration.result = Detail::runtimeRelocateRawPcmSlice(
        soundId, startFrame, &relocatedStart, &frameCount, &channels, &sampleRate,
        &windowStartFrame, &windowFrameCount, &loops);
    if (registration.result == kEngineOk) {
        registration.startFrame = relocatedStart;
        registration.frameCount = frameCount;
        registration.channels = channels;
        registration.sampleRate = sampleRate;
        registration.windowStartFrame = windowStartFrame;
        registration.windowFrameCount = windowFrameCount;
        registration.loops = loops;
    } else {
        registration.startFrame = startFrame;
    }
    return registration;
}

EngineResult EngineSession::unregisterRawPcm(harkenstudio::ObjectId soundId)
{
    return Detail::runtimeUnregisterRawPcm(soundId);
}

uint64_t EngineSession::playRawPcm(harkenstudio::ObjectId soundId, bool loops, EngineResult* outResult)
{
    return Detail::runtimePlayRawPcm(soundId, loops, outResult);
}

bool EngineSession::stopRawPcm(uint64_t playbackHandle, uint32_t fadeMs)
{
    return Detail::runtimeStopRawPcm(playbackHandle, fadeMs);
}

void EngineSession::setBusMuteById(harkenstudio::ObjectId busId, bool mute)
{
    BusCache& buses = desiredBusCache();
    const auto bus = buses.find(busId);
    if (harkenstudio::IsNull(busId) || bus == buses.end()) return;
    if (g_pendingBusCache && g_buses.find(busId) != g_buses.end())
        g_pendingConfirmedPropertyEdits[busId].mute = mute;
    bus->second.mute = mute;
    submitDesiredBusTopology();
}

void EngineSession::setBusSoloById(harkenstudio::ObjectId busId, bool solo)
{
    BusCache& buses = desiredBusCache();
    const auto bus = buses.find(busId);
    if (harkenstudio::IsNull(busId) || bus == buses.end()) return;
    if (g_pendingBusCache && g_buses.find(busId) != g_buses.end())
        g_pendingConfirmedPropertyEdits[busId].solo = solo;
    bus->second.solo = solo;
    submitDesiredBusTopology();
}

bool EngineSession::busMeterPeakById(harkenstudio::ObjectId busId, float& l, float& r) const
{
    return Detail::runtimeBusMeterPeak(busId, l, r);
}

void EngineSession::setVoicePitchByName(const QString& soundName, float cents)
{
    Detail::runtimeSetVoicePitchByName(soundName, cents);
}

void EngineSession::setVoiceVolumeByName(const QString& soundName, float volumeDb)
{
    Detail::runtimeSetVoiceVolumeByName(soundName, volumeDb);
}

void EngineSession::setVoiceOutputVolumeByName(const QString& soundName, float volumeDb)
{
    Detail::runtimeSetVoiceOutputVolumeByName(soundName, volumeDb);
}

void EngineSession::setVoiceSpatMixByName(const QString& soundName, float mix)
{
    Detail::runtimeSetVoiceSpatMixByName(soundName, mix);
}

void EngineSession::setVoiceDistanceScaleByName(const QString& soundName, float scale)
{
    Detail::runtimeSetVoiceDistanceScaleByName(soundName, scale);
}

void EngineSession::setVoiceSendsByName(const QString& soundName,
                                        const std::vector<std::pair<harkenstudio::ObjectId, float>>& sends)
{
    Detail::runtimeSetVoiceSendsByName(soundName, sends);
}

void EngineSession::setBusVolumeDbById(harkenstudio::ObjectId busId, float db)
{
    BusCache& buses = desiredBusCache();
    const auto bus = buses.find(busId);
    if (harkenstudio::IsNull(busId) || bus == buses.end()) return;
    if (g_pendingBusCache && g_buses.find(busId) != g_buses.end())
        g_pendingConfirmedPropertyEdits[busId].volumeDb = db;
    bus->second.volumeDb = db;
    submitDesiredBusTopology();
}

void EngineSession::setBusOutputVolumeDbById(harkenstudio::ObjectId busId, float db)
{
    BusCache& buses = desiredBusCache();
    const auto bus = buses.find(busId);
    if (harkenstudio::IsNull(busId) || bus == buses.end()) return;
    if (g_pendingBusCache && g_buses.find(busId) != g_buses.end())
        g_pendingConfirmedPropertyEdits[busId].outputVolumeDb = db;
    bus->second.outputVolumeDb = db;
    submitDesiredBusTopology();
}

float EngineSession::busVolumeDbById(harkenstudio::ObjectId busId) const
{
    const auto it = g_buses.find(busId);
    return it == g_buses.end() ? 0.0f : it->second.volumeDb;
}

void EngineSession::setBusPanById(harkenstudio::ObjectId busId, float pan) { Q_UNUSED(busId); Q_UNUSED(pan); }

void EngineSession::synchronizeBusTopology(
    const std::vector<RuntimeBusDefinition>& buses,
    const std::vector<RuntimeBusSendDefinition>& sends)
{
    Q_UNUSED(buses);
    Q_UNUSED(sends);
}

void EngineSession::confirmActiveBusTopologyWithoutApply(
    const std::vector<RuntimeBusDefinition>& buses,
    const std::vector<RuntimeBusSendDefinition>& sends)
{
    if (!g_busTopologySync)
        return;
    g_pendingBusCache.reset();
    g_pendingConfirmedPropertyEdits.clear();
    BusCache candidateBuses;
    BusSendCache candidateSends;
    rememberBus(candidateBuses, kEngineMasterBusId, {},
                QStringLiteral("Master"));
    for (const RuntimeBusDefinition& bus : buses) {
        if (harkenstudio::IsNull(bus.id))
            continue;
        rememberBus(candidateBuses, bus.id, bus.parentId, bus.name);
        BusState& state = candidateBuses[bus.id];
        state.volumeDb = bus.volumeDb;
        state.outputVolumeDb = bus.outputVolumeDb;
        state.mute = bus.mute;
        state.solo = bus.solo;
    }
    for (const RuntimeBusSendDefinition& send : sends)
        candidateSends[{send.sourceId, send.targetId}] = send;
    g_busTopologySync->ConfirmWithoutApply(buses, sends);
    g_pendingBusCache.emplace(PendingBusCache{
        std::move(candidateBuses), std::move(candidateSends)});
    settlePendingBusCache();
}

void EngineSession::confirmBusTopologyWithoutApply(
    const std::vector<RuntimeBusDefinition>& buses,
    const std::vector<RuntimeBusSendDefinition>& sends)
{
    confirmActiveBusTopologyWithoutApply(buses, sends);
}

BusTopologyState EngineSession::busTopologyState() const
{
    return g_busTopologySync
        ? g_busTopologySync->State()
        : BusTopologyState::Idle;
}

BusTopologyFailure EngineSession::busTopologyFailure() const
{
    return g_busTopologySync
        ? g_busTopologySync->Failure()
        : BusTopologyFailure{};
}

void EngineSession::renameBus(harkenstudio::ObjectId busId, const QString& newName)
{
    BusCache& buses = desiredBusCache();
    const auto bus = buses.find(busId);
    if (bus == buses.end()) return;
    bus->second.name = newName;
}

std::vector<harkenstudio::ObjectId> EngineSession::runtimeBusIds() const
{
    return g_busTopologySync
        ? g_busTopologySync->ConfirmedBusIds()
        : std::vector<harkenstudio::ObjectId>{};
}

void EngineSession::setMixerVolume(harkenstudio::ObjectId busId, float volumeDb) { setBusVolumeDbById(busId, volumeDb); }

uint32_t EngineSession::addListener() { return kPrimaryListenerId; }
void EngineSession::removeListener(uint32_t listenerId) { Q_UNUSED(listenerId); }
bool EngineSession::isListenerActive(uint32_t listenerId) const { return listenerId == kPrimaryListenerId; }
void EngineSession::setListenerPositionById(uint32_t listenerId,
                                            float x, float y, float z,
                                            float fwdX, float fwdY, float fwdZ,
                                            float upX, float upY, float upZ)
{
    if (listenerId == kPrimaryListenerId)
        setListenerTransform(x, y, z, fwdX, fwdY, fwdZ, upX, upY, upZ);
}
void EngineSession::setGameObjectListeners(uint64_t gameObjectId, const uint32_t* listenerIds, uint32_t count)
{
    Q_UNUSED(gameObjectId); Q_UNUSED(listenerIds); Q_UNUSED(count);
}
void EngineSession::setListenerSpatialization(uint32_t listenerId, bool spatialized, uint8_t panningRule, uint8_t speakerConfigType)
{
    Q_UNUSED(listenerId); Q_UNUSED(spatialized); Q_UNUSED(panningRule); Q_UNUSED(speakerConfigType);
}
bool EngineSession::listenerInfo(uint32_t listenerId,
                                 float* outX, float* outY, float* outZ,
                                 float* outFwdX, float* outFwdY, float* outFwdZ,
                                 int* outSpatialized, uint32_t* outPanningRule)
{
    Q_UNUSED(listenerId);
    if (outX) *outX = 0.0f; if (outY) *outY = 0.0f; if (outZ) *outZ = 0.0f;
    if (outFwdX) *outFwdX = 0.0f; if (outFwdY) *outFwdY = 0.0f; if (outFwdZ) *outFwdZ = 1.0f;
    if (outSpatialized) *outSpatialized = 0;
    if (outPanningRule) *outPanningRule = 0;
    return true;
}
void EngineSession::setMultiPosition(uint64_t gameObjectId, const float* positions, uint32_t count, uint8_t type)
{
    Q_UNUSED(gameObjectId); Q_UNUSED(positions); Q_UNUSED(count); Q_UNUSED(type);
}
void EngineSession::clearMultiPosition(uint64_t gameObjectId) { Q_UNUSED(gameObjectId); }
void EngineSession::setAutomationPath(uint64_t gameObjectId, const float* points, uint32_t count, uint8_t interp, uint8_t loop)
{
    Q_UNUSED(gameObjectId); Q_UNUSED(points); Q_UNUSED(count); Q_UNUSED(interp); Q_UNUSED(loop);
}
void EngineSession::startAutomation(uint64_t gameObjectId) { Q_UNUSED(gameObjectId); }
void EngineSession::stopAutomation(uint64_t gameObjectId) { Q_UNUSED(gameObjectId); }

//==============================================================================
// Free functions.
//==============================================================================

void pollEngineNotifications(std::vector<EngineNotification>& out)
{
    out.clear();

    // Playback completion is collected by Vn_Update on the pumping thread,
    // either from a Voice callback or Action-state polling, so no lock is needed.
    // The opaque playback handle matches AuditionInfo.playbackHandle.
    std::vector<uint64_t> ended;
    Detail::runtimeDrainEndedPlaybacks(ended);

    std::vector<EngineNotification> queued;
    Detail::runtimeDrainStudioNotifications(queued);

    out.reserve(ended.size() + queued.size());
    for (uint64_t handle : ended)
    {
        EngineNotification n;
        n.kind = EngineNotification::Kind::PlaybackEnded;
        n.playingId = handle;
        out.push_back(n);
    }
    for (auto& item : queued)
        out.push_back(std::move(item));
}

void injectRuntimeNotificationForTest(uint32_t type,
                                      uint64_t voiceHandle,
                                      uint64_t blockTimestamp)
{
    Detail::runtimeInjectNotificationForTest(type, voiceHandle, blockTimestamp);
}

void invalidateRuntimeNotificationsForTest()
{
    Detail::runtimeInvalidateNotificationsForTest();
}

bool findModulatorPreset(const QString& name, ModulatorPresetDesc& out)
{
    Q_UNUSED(name);
    out = {};
    return false;
}

bool saveModulatorUserPreset(const ModulatorPresetDesc& desc)
{
    Q_UNUSED(desc);
    return false;
}

bool deleteModulatorUserPreset(uint8_t modulatorType, const QString& name)
{
    Q_UNUSED(modulatorType); Q_UNUSED(name); return false;
}

std::vector<ModulatorPresetDesc> listModulatorPresets(int typeFilter)
{
    Q_UNUSED(typeFilter);
    return {};
}

uint32_t modulatorPresetTotalCount()
{
    return 0;
}

} // namespace Bridge
} // namespace HarkenEditor
