#ifndef HARKEN_RUNTIME_REAL_DEVICE_INIT_STAGE_H
#define HARKEN_RUNTIME_REAL_DEVICE_INIT_STAGE_H

namespace harken {

// Not stored on the engine. A failed Vn_Init resets the engine before
// PackPlay reads this, so the word has to outlive that reset.
void ClearRealDeviceInitStage() noexcept;
void SetRealDeviceInitStage(const char* stage) noexcept;
void SetRealDeviceInitMaCode(int code) noexcept;
void SetRealDeviceInitOpenPlan(
    bool enumerateFailed, int triedEnumeratedCount,
    int openedEnumeratedIndex) noexcept;
const char* RealDeviceInitStage() noexcept;
bool RealDeviceInitStageHasCode() noexcept;
int RealDeviceInitStageCode() noexcept;
bool RealDeviceInitStageHasOpenPlan() noexcept;
bool RealDeviceInitEnumerateFailed() noexcept;
int RealDeviceInitTriedEnumeratedCount() noexcept;
int RealDeviceInitOpenedEnumeratedIndex() noexcept;

}  // namespace harken

#endif
