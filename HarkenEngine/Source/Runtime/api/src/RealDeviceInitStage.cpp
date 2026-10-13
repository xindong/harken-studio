#include "RealDeviceInitStage.h"

namespace harken {

thread_local const char* g_realDeviceInitStage = nullptr;
thread_local bool g_realDeviceInitHasMaCode = false;
thread_local int g_realDeviceInitMaCode = 0;
thread_local bool g_realDeviceInitHasOpenPlan = false;
thread_local bool g_realDeviceInitEnumerateFailed = false;
thread_local int g_realDeviceInitTriedEnumeratedCount = 0;
thread_local int g_realDeviceInitOpenedEnumeratedIndex = -1;

void ClearRealDeviceInitStage() noexcept {
    g_realDeviceInitStage = nullptr;
    g_realDeviceInitHasMaCode = false;
    g_realDeviceInitMaCode = 0;
    g_realDeviceInitHasOpenPlan = false;
    g_realDeviceInitEnumerateFailed = false;
    g_realDeviceInitTriedEnumeratedCount = 0;
    g_realDeviceInitOpenedEnumeratedIndex = -1;
}

void SetRealDeviceInitStage(const char* stage) noexcept {
    g_realDeviceInitStage = stage;
}

void SetRealDeviceInitMaCode(int code) noexcept {
    g_realDeviceInitHasMaCode = true;
    g_realDeviceInitMaCode = code;
}

void SetRealDeviceInitOpenPlan(
    bool enumerateFailed, int triedEnumeratedCount,
    int openedEnumeratedIndex) noexcept {
    g_realDeviceInitHasOpenPlan = true;
    g_realDeviceInitEnumerateFailed = enumerateFailed;
    g_realDeviceInitTriedEnumeratedCount = triedEnumeratedCount;
    g_realDeviceInitOpenedEnumeratedIndex = openedEnumeratedIndex;
}

const char* RealDeviceInitStage() noexcept {
    return g_realDeviceInitStage;
}

bool RealDeviceInitStageHasCode() noexcept {
    return g_realDeviceInitHasMaCode;
}

int RealDeviceInitStageCode() noexcept {
    return g_realDeviceInitMaCode;
}

bool RealDeviceInitStageHasOpenPlan() noexcept {
    return g_realDeviceInitHasOpenPlan;
}

bool RealDeviceInitEnumerateFailed() noexcept {
    return g_realDeviceInitEnumerateFailed;
}

int RealDeviceInitTriedEnumeratedCount() noexcept {
    return g_realDeviceInitTriedEnumeratedCount;
}

int RealDeviceInitOpenedEnumeratedIndex() noexcept {
    return g_realDeviceInitOpenedEnumeratedIndex;
}

}  // namespace harken
