
#include "AudioEngine.h"
#include "ProjectManager.h"
#include "RuntimeSyncCoordinator.h"
#include "RuntimeBridge/PreviewBridge.h"

namespace harken::studio {

namespace {

using harken::studio::bridge::EngineResult;
using harken::studio::bridge::kEngineBackpressure;
using harken::studio::bridge::kEngineDeviceError;
using harken::studio::bridge::kEngineNotInit;
using harken::studio::bridge::kEngineOk;
using harken::studio::bridge::kEngineUnknownId;

bool overlayGenerationIsOpen()
{
    auto& coordinator = AudioEngine::instance().runtimeSyncCoordinator();
    const auto snapshot = coordinator.barrier(coordinator.currentAuthoringRevision());
    if (snapshot.state != RuntimeSyncCoordinator::RevisionBarrierState::Ready
        || snapshot.worldGeneration == 0) {
        return false;
    }
    return harken::studio::bridge::currentWorldBusAdmissionIsOpen(
        snapshot.worldGeneration);
}

bool overlayBusIsConfirmed(harken::studio::types::ObjectId busId)
{
    if (harken::studio::types::IsNull(busId))
        return false;
    const auto ids = AudioEngine::instance().getRuntimeBusIds();
    return ids.contains(busId);
}

} // namespace



void AudioEngine::setBusVolumeDbById(harken::studio::types::ObjectId busId, float db)
{
    if (!session_) return;
    session_->setBusVolumeDbById(busId, db);
}

void AudioEngine::setBusOutputVolumeDbById(harken::studio::types::ObjectId busId, float db)
{
    if (session_)
        session_->setBusOutputVolumeDbById(busId, db);
    (void)applyInstalledBusOutputVolumeOverlay(busId, db, 0);
}

float AudioEngine::getBusVolumeDbById(harken::studio::types::ObjectId busId) const
{
    return session_ ? session_->busVolumeDbById(busId) : 0.0f;
}

void AudioEngine::setBusPanById(harken::studio::types::ObjectId busId, float pan)
{
    if (!session_) return;
    session_->setBusPanById(busId, pan);
}

void AudioEngine::setBusMuteById(harken::studio::types::ObjectId busId, bool mute)
{
    if (!session_) return;
    session_->setBusMuteById(busId, mute);
}

void AudioEngine::setBusSoloById(harken::studio::types::ObjectId busId, bool solo)
{
    if (!session_) return;
    session_->setBusSoloById(busId, solo);
}

EngineResult AudioEngine::applyInstalledBusMuteOverlay(harken::studio::types::ObjectId busId, bool mute, uint32_t fadeMs)
{
    overlayMutePendingIds_.remove(busId);
    if (!overlayGenerationIsOpen() || !overlayBusIsConfirmed(busId)) {
        overlayMutePendingIds_.insert(busId);
        return kEngineNotInit;
    }
    const EngineResult result =
        harken::studio::bridge::setCurrentWorldBusMute(busId, mute, fadeMs);
    if (result == kEngineBackpressure || result == kEngineNotInit ||
        result == kEngineDeviceError)
        overlayMutePendingIds_.insert(busId);
    return result;
}

EngineResult AudioEngine::applyInstalledBusSoloOverlay(harken::studio::types::ObjectId busId, bool solo, uint32_t fadeMs)
{
    overlaySoloPendingIds_.remove(busId);
    if (!overlayGenerationIsOpen() || !overlayBusIsConfirmed(busId)) {
        overlaySoloPendingIds_.insert(busId);
        return kEngineNotInit;
    }
    const EngineResult result =
        harken::studio::bridge::setCurrentWorldBusSolo(busId, solo, fadeMs);
    if (result == kEngineBackpressure || result == kEngineNotInit ||
        result == kEngineDeviceError)
        overlaySoloPendingIds_.insert(busId);
    return result;
}

EngineResult AudioEngine::applyInstalledBusOutputVolumeOverlay(
    harken::studio::types::ObjectId busId, float db, uint32_t fadeMs)
{
    overlayOutputVolumePendingValues_.remove(busId);
    if (!overlayGenerationIsOpen() || !overlayBusIsConfirmed(busId)) {
        overlayOutputVolumePendingValues_.insert(busId, db);
        return kEngineNotInit;
    }
    const EngineResult result =
        harken::studio::bridge::setCurrentWorldBusOutputVolume(busId, db, fadeMs);
    if (result == kEngineBackpressure || result == kEngineNotInit ||
        result == kEngineDeviceError)
        overlayOutputVolumePendingValues_.insert(busId, db);
    return result;
}

void AudioEngine::applyPendingAuditionOverlay()
{
    overlayMutePendingIds_.clear();
    overlaySoloPendingIds_.clear();
    overlayOutputVolumePendingValues_.clear();
    auto& overlay = ProjectManager::instance().auditionOverlay();
    for (harken::studio::types::ObjectId id : overlay.mutedIds())
        (void)applyInstalledBusMuteOverlay(id, true, 0);
    for (harken::studio::types::ObjectId id : overlay.soloedIds())
        (void)applyInstalledBusSoloOverlay(id, true, 0);
}

void AudioEngine::retryBackpressuredAuditionOverlay()
{
    auto& overlay = ProjectManager::instance().auditionOverlay();
    const QSet<harken::studio::types::ObjectId> muteIds = overlay.mutedIds() + overlayMutePendingIds_;
    const QSet<harken::studio::types::ObjectId> soloIds = overlay.soloedIds() + overlaySoloPendingIds_;
    const auto outputVolumeValues = overlayOutputVolumePendingValues_;
    overlayMutePendingIds_.clear();
    overlaySoloPendingIds_.clear();
    overlayOutputVolumePendingValues_.clear();
    for (harken::studio::types::ObjectId id : muteIds)
        (void)applyInstalledBusMuteOverlay(id, overlay.isMuted(id), 0);
    for (harken::studio::types::ObjectId id : soloIds)
        (void)applyInstalledBusSoloOverlay(id, overlay.isSoloed(id), 0);
    for (auto it = outputVolumeValues.cbegin(); it != outputVolumeValues.cend(); ++it) {
        const harken::studio::types::ObjectId id = it.key();
        if (harken::studio::types::IsNull(id))
            continue;
        (void)applyInstalledBusOutputVolumeOverlay(id, it.value(), 0);
    }
}

harken::studio::bridge::IpcQueryStatus AudioEngine::getBusMeterPeakById(
    harken::studio::types::ObjectId busId, float& peakL, float& peakR) const
{
    if (!session_)
        return harken::studio::bridge::IpcQueryStatus::NoEngine;
    if (!session_->busMeterPeakById(busId, peakL, peakR))
        return harken::studio::bridge::IpcQueryStatus::NotFound;
    return harken::studio::bridge::IpcQueryStatus::Ok;
}



void AudioEngine::synchronizeBusTopology(
    const std::vector<harken::studio::bridge::RuntimeBusDefinition>& buses,
    const std::vector<harken::studio::bridge::RuntimeBusSendDefinition>& sends)
{
    if (!session_) return;
    session_->synchronizeBusTopology(buses, sends);
}

harken::studio::bridge::BusTopologyState AudioEngine::busTopologyState() const
{
    return session_
        ? session_->busTopologyState()
        : harken::studio::bridge::BusTopologyState::Idle;
}

harken::studio::bridge::BusTopologyFailure AudioEngine::busTopologyFailure() const
{
    return session_
        ? session_->busTopologyFailure()
        : harken::studio::bridge::BusTopologyFailure{};
}

void AudioEngine::renameBus(harken::studio::types::ObjectId busId, const QString& newName)
{
    if (!session_) return;
    session_->renameBus(busId, newName);
}

QList<harken::studio::types::ObjectId> AudioEngine::getRuntimeBusIds() const
{
    QList<harken::studio::types::ObjectId> out;
    if (!session_) return out;
    for (harken::studio::types::ObjectId id : session_->runtimeBusIds())
        out.append(id);
    return out;
}

void AudioEngine::setBusSendFilter(const QString& srcBusName, int32_t slot,
                                   float lpfHz, float hpfHz)
{
    Q_UNUSED(srcBusName);
    Q_UNUSED(slot);
    Q_UNUSED(lpfHz);
    Q_UNUSED(hpfHz);
}

bool AudioEngine::busSendFilter(const QString& srcBusName, int32_t slot,
                                float& lpfHz, float& hpfHz) const
{
    Q_UNUSED(srcBusName);
    Q_UNUSED(slot);
    lpfHz = 0.0f;
    hpfHz = 0.0f;
    return false;
}




void AudioEngine::setMixerVolume(harken::studio::types::ObjectId busId, float volumeDb)
{
    if (!session_) return;
    session_->setMixerVolume(busId, volumeDb);
}


}
