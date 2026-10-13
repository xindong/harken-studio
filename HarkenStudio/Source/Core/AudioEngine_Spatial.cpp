
#include "AudioEngine.h"
#include "RuntimeSyncCoordinator.h"
#include "RuntimeBridge/CurrentWorldSyncBridge.h"

namespace VNX {



uint32_t AudioEngine::addListener()
{
    if (!session_) return UINT32_MAX;
    return session_->addListener();
}

void AudioEngine::removeListener(uint32_t listenerId)
{
    if (!session_) return;
    session_->removeListener(listenerId);
}

bool AudioEngine::isListenerActive(uint32_t listenerId) const
{
    if (!session_) return false;
    return session_->isListenerActive(listenerId);
}

void AudioEngine::setListenerPositionById(uint32_t listenerId,
                                          float x, float y, float z,
                                          float fwdX, float fwdY, float fwdZ,
                                          float upX, float upY, float upZ)
{
    if (!session_) return;
    session_->setListenerPositionById(listenerId, x, y, z, fwdX, fwdY, fwdZ,
                                      upX, upY, upZ);
}

void AudioEngine::setGameObjectListeners(uint64_t gameObjectId,
                                         const uint32_t* listenerIds, uint32_t count)
{
    if (!session_) return;
    session_->setGameObjectListeners(gameObjectId, listenerIds, count);
}

void AudioEngine::setListenerSpatialization(uint32_t listenerId, bool spatialized,
                                             uint8_t panningRule, uint8_t speakerConfigType)
{
    if (!session_) return;
    session_->setListenerSpatialization(listenerId, spatialized, panningRule,
                                        speakerConfigType);
}

bool AudioEngine::getListenerInfo(uint32_t listenerId,
                                   float* outX, float* outY, float* outZ,
                                   float* outFwdX, float* outFwdY, float* outFwdZ,
                                   int* outSpatialized, uint32_t* outPanningRule)
{
    if (!session_) return false;
    return session_->listenerInfo(listenerId, outX, outY, outZ,
                                  outFwdX, outFwdY, outFwdZ,
                                  outSpatialized, outPanningRule);
}

void AudioEngine::setMultiPosition(uint64_t gameObjectId,
                                   const float* positions, uint32_t count, uint8_t type)
{
    if (!session_) return;
    session_->setMultiPosition(gameObjectId, positions, count, type);
}

void AudioEngine::clearMultiPosition(uint64_t gameObjectId)
{
    if (!session_) return;
    session_->clearMultiPosition(gameObjectId);
}

void AudioEngine::setAutomationPath(uint64_t gameObjectId,
                                    const float* points, uint32_t count,
                                    uint8_t interp, uint8_t loop)
{
    if (!session_) return;
    session_->setAutomationPath(gameObjectId, points, count, interp, loop);
}

void AudioEngine::startAutomation(uint64_t gameObjectId)
{
    if (!session_) return;
    session_->startAutomation(gameObjectId);
}

void AudioEngine::stopAutomation(uint64_t gameObjectId)
{
    if (!session_) return;
    session_->stopAutomation(gameObjectId);
}



bool AudioEngine::initSpatialAudio(uint32_t maxPropDepth, uint32_t numRays,
                                    uint32_t maxReflOrder, uint32_t maxDiffOrder)
{
    Q_UNUSED(maxPropDepth);
    Q_UNUSED(numRays);
    Q_UNUSED(maxReflOrder);
    Q_UNUSED(maxDiffOrder);
    return false;
}

void AudioEngine::setImageSource(uint32_t srcID, uint32_t sourceID, float posX, float posY, float posZ,
                                  float distScale, float level, float diffraction, float occlusion, uint32_t textureID)
{
    Q_UNUSED(srcID);
    Q_UNUSED(sourceID);
    Q_UNUSED(posX);
    Q_UNUSED(posY);
    Q_UNUSED(posZ);
    Q_UNUSED(distScale);
    Q_UNUSED(level);
    Q_UNUSED(diffraction);
    Q_UNUSED(occlusion);
    Q_UNUSED(textureID);
}

void AudioEngine::removeImageSource(uint32_t srcID)
{
    Q_UNUSED(srcID);
}

void AudioEngine::clearImageSources()
{
}

void AudioEngine::setDiffractionOrder(uint32_t order, bool updatePaths)
{
    Q_UNUSED(order);
    Q_UNUSED(updatePaths);
}

void AudioEngine::setLoadBalancingSpread(uint32_t numFrames)
{
    Q_UNUSED(numFrames);
}

void AudioEngine::setSmoothingConstant(float smoothMs, uint64_t gameObjectID)
{
    Q_UNUSED(smoothMs);
    Q_UNUSED(gameObjectID);
}

void AudioEngine::setTransmissionOperation(uint8_t operation)
{
    Q_UNUSED(operation);
}

bool AudioEngine::loadHRTFDatabase(const uint8_t* data, uint32_t dataSize)
{
    Q_UNUSED(data);
    Q_UNUSED(dataSize);
    return false;
}

void AudioEngine::unloadHRTFDatabase()
{
}

void AudioEngine::setHRTFEnabled(bool enabled)
{
    Q_UNUSED(enabled);
}

void AudioEngine::setGameObjectRadius(uint64_t gameObjectID, float outerRadius, float innerRadius)
{
    Q_UNUSED(gameObjectID);
    Q_UNUSED(outerRadius);
    Q_UNUSED(innerRadius);
}



AudioEngine::BinauralStatus AudioEngine::getBinauralStatus() const
{
    return BinauralStatus();
}

bool AudioEngine::reconfigureBufferSize(int32_t newBlockSize)
{
    if (!session_) return false;
    runtimeProjectRestoreRequired_ = true;
    releasePreviewEmitter();
    if (runtimeSessionActive_) {
        stopRuntimeSynchronization();
    }
    if (session_->reconfigureBufferSize(newBlockSize)
        != HarkenEditor::Bridge::kEngineOk) {
        return false;
    }
    if (runtimeSyncCoordinator_->runtimeStarted()
        == RuntimeSyncCoordinator::AllocationState::IdentityExhausted) {
        session_->stopOutput();
        emit errorOccurred(tr("Runtime synchronization identity exhausted"));
        return false;
    }
    runtimeSessionActive_ = true;
    updateRuntimeServiceInterval();
    restoreCurrentProjectAfterRuntimeRestart();
    return true;
}

int32_t AudioEngine::getBufferSize() const
{
    if (!session_) return 0;
    return session_->bufferSize();
}

}
