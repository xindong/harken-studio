#pragma once

#include "Utils/ObjectId.h"

// RuntimeLink -- bridge-private connection to the new harken_core C API.
//
// This file is intentionally not part of the public bridge surface. It keeps
// all in-process harken_core C API calls behind the bridge-private boundary.

#include "DiagnosticsBridge.h"
#include "PlaybackBridge.h"

#include <harken/Harken.h>

#include <QString>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace HarkenEditor {
namespace Bridge {
namespace Detail {

bool runtimeInit(const EngineSessionConfig& config);
void runtimeTerm();
bool runtimeIsActive();
bool runtimeCaptureSnapshot(VnSnapshot& outSnapshot);

void runtimeSetLogHandler(std::function<void(int level, const QString& msg)> handler);

EngineResult runtimeStartOutput();
void runtimeStopOutput();

void runtimeUpdate();
int runtimeActiveVoiceCount();
void runtimeDrainEndedPlaybacks(std::vector<uint64_t>& out);
void runtimeDrainStudioNotifications(std::vector<EngineNotification>& out);
void runtimeInjectNotificationForTest(uint32_t type,
                                      uint64_t voiceHandle,
                                      uint64_t blockTimestamp);
void runtimeInvalidateNotificationsForTest();
void runtimeMasterMeterPeak(float& l, float& r);
float runtimeAudioCpuPercent();
double runtimeOutputLatency();
int runtimeActualSampleRate();
int32_t runtimeBufferSize();
EngineResult runtimeReconfigureBufferSize(int32_t newBlockSize);

void runtimeSetMasterFade(float volume, int fadeMs);

void runtimeRegisterSoundName(const QString& name, harkenstudio::ObjectId objectId,
                              harkenstudio::ObjectId soundId, uint32_t sourceId,
                              uint32_t loopCount, bool spatialized3D,
                              harkenstudio::ObjectId attenuationId);
bool runtimePrepareMedia(uint32_t sourceId);
uint64_t runtimeRegisteredSoundRevision(harkenstudio::ObjectId soundId);
VnResult runtimeRegisterSoundPcm(harkenstudio::ObjectId soundId, const std::vector<float>& samples,
                                  uint32_t channels, uint32_t sampleRate,
                                  uint64_t loopStartFrame, uint64_t loopEndFrame);
void runtimeUnregisterSoundObject(harkenstudio::ObjectId objectId);
void runtimeSetObjectOutputBus(harkenstudio::ObjectId objectId, harkenstudio::ObjectId busId);
void runtimeClearSoundNames();
void runtimeRegisterActionName(const QString& name, harkenstudio::ObjectId actionId);
void runtimeUnregisterActionName(harkenstudio::ObjectId actionId);
void runtimeClearActionNames();

bool runtimeLoadPack(const QString& path);
void runtimeUnloadPack(const QString& path);
uint32_t runtimeLoadedPackCount();

harkenstudio::ObjectId runtimeMintRawPcmSoundId();
EngineResult runtimeRegisterRawPcmSlice(harkenstudio::ObjectId soundId,
                                        const QString& absolutePath,
                                        uint64_t startFrame,
                                        uint64_t frameCount,
                                        bool loops = false,
                                        uint32_t* outChannels = nullptr,
                                        uint32_t* outSampleRate = nullptr,
                                        uint64_t* outWindowStartFrame = nullptr,
                                        uint64_t* outWindowFrameCount = nullptr);
EngineResult runtimeRelocateRawPcmSlice(harkenstudio::ObjectId soundId,
                                        uint64_t startFrame,
                                        uint64_t* outStartFrame = nullptr,
                                        uint64_t* outFrameCount = nullptr,
                                        uint32_t* outChannels = nullptr,
                                        uint32_t* outSampleRate = nullptr,
                                        uint64_t* outWindowStartFrame = nullptr,
                                        uint64_t* outWindowFrameCount = nullptr,
                                        bool* outLoops = nullptr);
EngineResult runtimeUnregisterRawPcm(harkenstudio::ObjectId soundId);
uint64_t runtimePlayRawPcm(harkenstudio::ObjectId soundId, bool loops,
                           EngineResult* outResult = nullptr);
bool runtimeStopRawPcm(uint64_t playbackHandle, uint32_t fadeMs = 0);

bool runtimeHasActionProgram(harkenstudio::ObjectId actionId);
uint64_t runtimePlayByName(const QString& name, bool spatialized,
                           float x, float y, float z,
                           EngineResult* outResult = nullptr,
                           uint64_t borrowedActionEmitter = 0);
ActionSeekStartResult runtimeSeekAction(uint64_t playbackHandle,
                                        const QString& expectedActionName,
                                        double positionMs, bool relative);
EngineResult runtimeQueryActionSeek(uint64_t operationId,
                                    ActionSeekInfo& outInfo);
EngineResult runtimeReleaseActionSeek(uint64_t operationId);
bool runtimeStopPlayback(uint64_t handle, uint32_t fadeMs);
void runtimeStopAll(uint32_t fadeMs);
bool runtimeHasCurrentWorldPlayableConsumers();
EngineResult runtimeRefreshPreviewVoiceBindings(uint64_t voiceInstance);
EngineResult runtimeSetPreviewVoiceProperty(uint64_t voiceInstance, uint8_t property,
    float value, harkenstudio::ObjectId sendBusId = {});
void runtimePumpNullMixer(std::uint32_t blockCount);
void runtimeSetVoicePitchByName(const QString& name, float cents);
void runtimeSetVoiceVolumeByName(const QString& name, float volumeDb);
void runtimeSetVoiceOutputVolumeByName(const QString& name, float volumeDb);
void runtimeSetVoiceSpatMixByName(const QString& name, float mix);
void runtimeSetVoiceDistanceScaleByName(const QString& name, float scale);
void runtimeSetVoiceSendsByName(const QString& name,
                                const std::vector<std::pair<harkenstudio::ObjectId, float>>& sends);
bool runtimeSetPlaybackPosition(uint64_t handle, float x, float y, float z);
bool runtimeOwnsPlayback(uint64_t handle);

bool runtimeCurrentWorldBusAdmissionIsOpen(uint64_t worldGeneration);
EngineResult runtimeSetBusMute(harkenstudio::ObjectId busId, bool mute, uint32_t fadeMs);
EngineResult runtimeSetBusSolo(harkenstudio::ObjectId busId, bool solo, uint32_t fadeMs);
EngineResult runtimeSetBusOutputVolume(harkenstudio::ObjectId busId, float db, uint32_t fadeMs);

} // namespace Detail
} // namespace Bridge
} // namespace HarkenEditor
