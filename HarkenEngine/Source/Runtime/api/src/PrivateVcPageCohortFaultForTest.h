#pragma once

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
namespace harken { struct Engine; struct EnginePreparedTimelineVoiceStart; }
namespace harken::detail {
void ForcePrivateVcSecondSideConsumeFailureOnceForTest() noexcept;
bool ConsumePrivateVcSecondSideFailureForTest() noexcept;
void SetTimelineVoiceOwnerRollbackRefusalHookForTest(
    void (*hook)(Engine&, EnginePreparedTimelineVoiceStart&, void*), void* context) noexcept;
void ObserveTimelineVoiceOwnerRollbackRefusalForTest(
    Engine& engine, EnginePreparedTimelineVoiceStart& start);

} // namespace harken::detail
#endif
