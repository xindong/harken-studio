#pragma once

#include "Monitor/MonitorTypes.h"

namespace HarkenEditor::Bridge::Monitoring {

int32_t openSession(const VNX::Monitor::MonitorSessionDesc* desc, uint64_t* handle);
int32_t openEventSession(const VNX::Monitor::MonitorSessionDesc* desc, uint64_t* handle);
int32_t closeSession(uint64_t handle);
int32_t sealEventSession(uint64_t handle);
int32_t getEventSessionStatus(uint64_t handle, VNX::Monitor::MonitorEventSessionStatus* out);
int32_t getCapabilities(VNX::Monitor::MonitorCapabilities* out);
int32_t poll(uint64_t handle, const VNX::Monitor::MonitorPollDesc* desc,
             VNX::Monitor::MonitorPollResult* out);
int32_t getSubscriptionStatus(uint64_t handle, VNX::Monitor::MonitorSubscriptionStatus* out);
int32_t getConsumerStatus(uint64_t handle, VNX::Monitor::MonitorDropSummary* out,
                          uint32_t capacity, uint32_t* required);
int32_t getCaptureControlStatus(uint64_t handle, VNX::Monitor::MonitorCaptureControlStatus* out);
int32_t acquireCaptureControl(uint64_t handle, VNX::Monitor::MonitorCaptureControlStatus* out);
int32_t releaseCaptureControl(uint64_t handle, uint64_t lease);
int32_t validateCapturePlan(uint64_t handle, uint64_t lease,
                            const VNX::Monitor::MonitorCapturePlan* plan,
                            VNX::Monitor::MonitorEffectiveCapturePlan* out);
int32_t applyCapturePlan(uint64_t handle, uint64_t lease,
                         const VNX::Monitor::MonitorCapturePlan* plan,
                         VNX::Monitor::MonitorEffectiveCapturePlan* out);
int32_t stopCapture(uint64_t handle, uint64_t lease, VNX::Monitor::MonitorEffectiveCapturePlan* out);
int32_t getEffectiveCapturePlan(uint64_t handle, uint64_t generation,
                                VNX::Monitor::MonitorEffectiveCapturePlan* out);
int32_t startRecording(uint64_t handle, uint64_t lease, const VNX::Monitor::MonitorRecordingDesc* desc);
int32_t stopRecording(uint64_t handle, uint64_t lease);
int32_t getRecordingStatus(uint64_t handle, VNX::Monitor::MonitorRecordingStatus* out);
int32_t compareSessions(const VNX::Monitor::MonitorSessionCompareDesc* desc,
                        VNX::Monitor::MonitorCompareResult* out);

}
