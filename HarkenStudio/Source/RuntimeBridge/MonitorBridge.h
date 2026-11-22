#pragma once

#include "Monitor/MonitorTypes.h"

namespace harken::studio::bridge::Monitoring {

int32_t openSession(const harken::studio::Monitor::MonitorSessionDesc* desc, uint64_t* handle);
int32_t openEventSession(const harken::studio::Monitor::MonitorSessionDesc* desc, uint64_t* handle);
int32_t closeSession(uint64_t handle);
int32_t sealEventSession(uint64_t handle);
int32_t getEventSessionStatus(uint64_t handle, harken::studio::Monitor::MonitorEventSessionStatus* out);
int32_t getCapabilities(harken::studio::Monitor::MonitorCapabilities* out);
int32_t poll(uint64_t handle, const harken::studio::Monitor::MonitorPollDesc* desc,
             harken::studio::Monitor::MonitorPollResult* out);
int32_t getSubscriptionStatus(uint64_t handle, harken::studio::Monitor::MonitorSubscriptionStatus* out);
int32_t getConsumerStatus(uint64_t handle, harken::studio::Monitor::MonitorDropSummary* out,
                          uint32_t capacity, uint32_t* required);
int32_t getCaptureControlStatus(uint64_t handle, harken::studio::Monitor::MonitorCaptureControlStatus* out);
int32_t acquireCaptureControl(uint64_t handle, harken::studio::Monitor::MonitorCaptureControlStatus* out);
int32_t releaseCaptureControl(uint64_t handle, uint64_t lease);
int32_t validateCapturePlan(uint64_t handle, uint64_t lease,
                            const harken::studio::Monitor::MonitorCapturePlan* plan,
                            harken::studio::Monitor::MonitorEffectiveCapturePlan* out);
int32_t applyCapturePlan(uint64_t handle, uint64_t lease,
                         const harken::studio::Monitor::MonitorCapturePlan* plan,
                         harken::studio::Monitor::MonitorEffectiveCapturePlan* out);
int32_t stopCapture(uint64_t handle, uint64_t lease, harken::studio::Monitor::MonitorEffectiveCapturePlan* out);
int32_t getEffectiveCapturePlan(uint64_t handle, uint64_t generation,
                                harken::studio::Monitor::MonitorEffectiveCapturePlan* out);

}
