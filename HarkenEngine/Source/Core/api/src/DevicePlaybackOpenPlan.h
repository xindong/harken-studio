#ifndef HARKEN_API_DEVICE_PLAYBACK_OPEN_PLAN_H
#define HARKEN_API_DEVICE_PLAYBACK_OPEN_PLAN_H

#include <cstddef>

namespace harken {

// Decision table for a real-device open. The first attempt always uses the
// default endpoint (NULL playback id). A failed default must keep the
// miniaudio context so enumeration can run. Opening parameters are owned by
// DeviceBackend and are not part of this table.
struct PlaybackOpenPlan {
    bool defaultAttempted = false;
    bool defaultSucceeded = false;
    bool defaultFailed = false;
    bool enumerateAttempted = false;
    bool enumerateFailed = false;
    int triedEnumeratedCount = 0;
    int openedEnumeratedIndex = -1;
    int maCode = 0;
    bool keepContext = false;
    bool resetImpl = false;
    bool writeDeviceInitStage = false;
    bool abandonFailedDevice = false;
};

inline PlaybackOpenPlan AfterDefaultPlaybackAttempt(int maResult) {
    PlaybackOpenPlan plan;
    plan.defaultAttempted = true;
    if (maResult == 0) {
        plan.defaultSucceeded = true;
        return plan;
    }
    plan.defaultFailed = true;
    plan.maCode = maResult;
    plan.keepContext = true;
    plan.abandonFailedDevice = true;
    return plan;
}

inline PlaybackOpenPlan AfterPlaybackEnumerateFailure(PlaybackOpenPlan plan) {
    plan.enumerateAttempted = true;
    plan.enumerateFailed = true;
    plan.keepContext = false;
    plan.resetImpl = true;
    plan.writeDeviceInitStage = true;
    return plan;
}

inline PlaybackOpenPlan AfterEnumeratedPlaybackAttempt(
    PlaybackOpenPlan plan, int index, int maResult, int playbackCount) {
    plan.enumerateAttempted = true;
    plan.triedEnumeratedCount = index + 1;
    if (maResult == 0) {
        plan.openedEnumeratedIndex = index;
        plan.keepContext = true;
        plan.resetImpl = false;
        plan.writeDeviceInitStage = false;
        plan.abandonFailedDevice = false;
        return plan;
    }
    plan.maCode = maResult;
    plan.abandonFailedDevice = true;
    const bool last = playbackCount <= 0 || index + 1 >= playbackCount;
    if (last) {
        plan.keepContext = false;
        plan.resetImpl = true;
        plan.writeDeviceInitStage = true;
    } else {
        plan.keepContext = true;
        plan.resetImpl = false;
        plan.writeDeviceInitStage = false;
    }
    return plan;
}

}  // namespace harken

#endif
