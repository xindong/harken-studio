
#include "AudioEngine.h"
#include "ProjectManager.h"
#include "RuntimeSyncCoordinator.h"
#include "RuntimeBridge/PreviewBridge.h"

namespace VNX {

namespace {

using HarkenEditor::Bridge::EngineResult;
using HarkenEditor::Bridge::kEngineBackpressure;
using HarkenEditor::Bridge::kEngineDeviceError;
using HarkenEditor::Bridge::kEngineNotInit;
using HarkenEditor::Bridge::kEngineOk;
using HarkenEditor::Bridge::kEngineUnknownId;

bool overlayGenerationIsOpen()
{
    auto& coordinator = AudioEngine::instance().runtimeSyncCoordinator();
    const auto snapshot = coordinator.barrier(coordinator.currentAuthoringRevision());
    if (snapshot.state != RuntimeSyncCoordinator::RevisionBarrierState::Ready
        || snapshot.worldGeneration == 0) {
        return false;
    }
    return HarkenEditor::Bridge::currentWorldBusAdmissionIsOpen(
        snapshot.worldGeneration);
}

bool overlayBusIsConfirmed(harkenstudio::ObjectId busId)
{
    if (harkenstudio::IsNull(busId))
        return false;
    const auto ids = AudioEngine::instance().getRuntimeBusIds();
    return ids.contains(busId);
}

} // namespace



void AudioEngine::setBusVolumeDbById(harkenstudio::ObjectId busId, float db)
{
    if (!session_) return;
    session_->setBusVolumeDbById(busId, db);
}

void AudioEngine::setBusOutputVolumeDbById(harkenstudio::ObjectId busId, float db)
{
    if (session_)
        session_->setBusOutputVolumeDbById(busId, db);
    (void)applyInstalledBusOutputVolumeOverlay(busId, db, 0);
}

float AudioEngine::getBusVolumeDbById(harkenstudio::ObjectId busId) const
{
    return session_ ? session_->busVolumeDbById(busId) : 0.0f;
}

void AudioEngine::setBusPanById(harkenstudio::ObjectId busId, float pan)
{
    if (!session_) return;
    session_->setBusPanById(busId, pan);
}

void AudioEngine::setBusMuteById(harkenstudio::ObjectId busId, bool mute)
{
    if (!session_) return;
    session_->setBusMuteById(busId, mute);
}

void AudioEngine::setBusSoloById(harkenstudio::ObjectId busId, bool solo)
{
    if (!session_) return;
    session_->setBusSoloById(busId, solo);
}

EngineResult AudioEngine::applyInstalledBusMuteOverlay(harkenstudio::ObjectId busId, bool mute, uint32_t fadeMs)
{
    overlayMutePendingIds_.remove(busId);
    if (!overlayGenerationIsOpen() || !overlayBusIsConfirmed(busId)) {
        overlayMutePendingIds_.insert(busId);
        return kEngineNotInit;
    }
    const EngineResult result =
        HarkenEditor::Bridge::setCurrentWorldBusMute(busId, mute, fadeMs);
    if (result == kEngineBackpressure || result == kEngineNotInit ||
        result == kEngineDeviceError)
        overlayMutePendingIds_.insert(busId);
    return result;
}

EngineResult AudioEngine::applyInstalledBusSoloOverlay(harkenstudio::ObjectId busId, bool solo, uint32_t fadeMs)
{
    overlaySoloPendingIds_.remove(busId);
    if (!overlayGenerationIsOpen() || !overlayBusIsConfirmed(busId)) {
        overlaySoloPendingIds_.insert(busId);
        return kEngineNotInit;
    }
    const EngineResult result =
        HarkenEditor::Bridge::setCurrentWorldBusSolo(busId, solo, fadeMs);
    if (result == kEngineBackpressure || result == kEngineNotInit ||
        result == kEngineDeviceError)
        overlaySoloPendingIds_.insert(busId);
    return result;
}

EngineResult AudioEngine::applyInstalledBusOutputVolumeOverlay(
    harkenstudio::ObjectId busId, float db, uint32_t fadeMs)
{
    overlayOutputVolumePendingValues_.remove(busId);
    if (!overlayGenerationIsOpen() || !overlayBusIsConfirmed(busId)) {
        overlayOutputVolumePendingValues_.insert(busId, db);
        return kEngineNotInit;
    }
    const EngineResult result =
        HarkenEditor::Bridge::setCurrentWorldBusOutputVolume(busId, db, fadeMs);
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
    for (harkenstudio::ObjectId id : overlay.mutedIds())
        (void)applyInstalledBusMuteOverlay(id, true, 0);
    for (harkenstudio::ObjectId id : overlay.soloedIds())
        (void)applyInstalledBusSoloOverlay(id, true, 0);
}

void AudioEngine::retryBackpressuredAuditionOverlay()
{
    auto& overlay = ProjectManager::instance().auditionOverlay();
    const QSet<harkenstudio::ObjectId> muteIds = overlay.mutedIds() + overlayMutePendingIds_;
    const QSet<harkenstudio::ObjectId> soloIds = overlay.soloedIds() + overlaySoloPendingIds_;
    const auto outputVolumeValues = overlayOutputVolumePendingValues_;
    overlayMutePendingIds_.clear();
    overlaySoloPendingIds_.clear();
    overlayOutputVolumePendingValues_.clear();
    for (harkenstudio::ObjectId id : muteIds)
        (void)applyInstalledBusMuteOverlay(id, overlay.isMuted(id), 0);
    for (harkenstudio::ObjectId id : soloIds)
        (void)applyInstalledBusSoloOverlay(id, overlay.isSoloed(id), 0);
    for (auto it = outputVolumeValues.cbegin(); it != outputVolumeValues.cend(); ++it) {
        const harkenstudio::ObjectId id = it.key();
        if (harkenstudio::IsNull(id))
            continue;
        (void)applyInstalledBusOutputVolumeOverlay(id, it.value(), 0);
    }
}

HarkenEditor::Bridge::IpcQueryStatus AudioEngine::getBusMeterPeakById(
    harkenstudio::ObjectId busId, float& peakL, float& peakR) const
{
    if (!session_)
        return HarkenEditor::Bridge::IpcQueryStatus::NoEngine;
    if (!session_->busMeterPeakById(busId, peakL, peakR))
        return HarkenEditor::Bridge::IpcQueryStatus::NotFound;
    return HarkenEditor::Bridge::IpcQueryStatus::Ok;
}



void AudioEngine::synchronizeBusTopology(
    const std::vector<HarkenEditor::Bridge::RuntimeBusDefinition>& buses,
    const std::vector<HarkenEditor::Bridge::RuntimeBusSendDefinition>& sends)
{
    if (!session_) return;
    session_->synchronizeBusTopology(buses, sends);
}

HarkenEditor::Bridge::BusTopologyState AudioEngine::busTopologyState() const
{
    return session_
        ? session_->busTopologyState()
        : HarkenEditor::Bridge::BusTopologyState::Idle;
}

HarkenEditor::Bridge::BusTopologyFailure AudioEngine::busTopologyFailure() const
{
    return session_
        ? session_->busTopologyFailure()
        : HarkenEditor::Bridge::BusTopologyFailure{};
}

void AudioEngine::renameBus(harkenstudio::ObjectId busId, const QString& newName)
{
    if (!session_) return;
    session_->renameBus(busId, newName);
}

QList<harkenstudio::ObjectId> AudioEngine::getRuntimeBusIds() const
{
    QList<harkenstudio::ObjectId> out;
    if (!session_) return out;
    for (harkenstudio::ObjectId id : session_->runtimeBusIds())
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




void AudioEngine::setMixerVolume(harkenstudio::ObjectId busId, float volumeDb)
{
    if (!session_) return;
    session_->setMixerVolume(busId, volumeDb);
}


}
