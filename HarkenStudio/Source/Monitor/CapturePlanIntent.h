#pragma once

#include "MonitorCaptureControl.h"

#include "Monitor/MonitorTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace VNX {
namespace Monitor {

enum class CapturePlanFace
{
    Unsupported = 0,
    PermissionDenied,
    BudgetExceeded,
    BindingNotActive,
    EmptyMeasurement,
    UnpublishedMeasurement,
    Pending,
    Ready,
};

enum class CapturePlanSubmitKind
{
    None = 0,
    Validate,
    Apply,
    Stop,
    Acquire,
    Release,
};

struct CapturePlanIntent
{
    std::vector<uint64_t> measurementIds;
    uint32_t streamKindMask = kMonitorStreamOptionalAudio;
    uint64_t bytesPerSecondBudget = 0;
    uint64_t correlation = 0;
};

struct CaptureBudgetPreview
{
    CapturePlanFace face = CapturePlanFace::Unsupported;
    MonitorResult result = kMonitorUnsupported;
    uint64_t capturePlanGeneration = 0;
    uint64_t previousGeneration = 0;
    uint64_t effectiveFromExecuteFrame = 0;
    uint32_t activationState = kMonitorPlanPending;
    uint64_t acceptedBudget = 0;
    uint64_t hardCaptureByteRate = 0;
    uint64_t capabilityFingerprint = 0;
    uint64_t correlation = 0;
    bool chargesAudioChunk = false;
};

bool measurementIdAllowed(uint64_t measurementId);
CapturePlanIntent canonicalIntent(const CapturePlanIntent& raw);
CapturePlanIntent hideMeasurement(const CapturePlanIntent& current, uint64_t measurementId);
bool intentsEqual(const CapturePlanIntent& a, const CapturePlanIntent& b);
bool previewMatchesCorrelation(const CaptureBudgetPreview& preview, uint64_t correlation);

CapturePlanFace faceForResult(MonitorResult result, const CapturePlanIntent& intent);
CaptureBudgetPreview previewFromEffective(
    MonitorResult result,
    const MonitorEffectiveCapturePlan* accepted,
    uint64_t hardCaptureByteRate,
    uint64_t correlation,
    const CapturePlanIntent& intent);

CaptureBudgetPreview keepPreviousPreview(
    const CaptureBudgetPreview& previous,
    MonitorResult result,
    const CapturePlanIntent& intent);

bool shouldApply(const CaptureBudgetPreview& preview);
bool generationUnchanged(const CaptureBudgetPreview& before, const CaptureBudgetPreview& after);

std::string capturePlanFaceText(const CaptureBudgetPreview& preview);

} // namespace Monitor
} // namespace VNX
