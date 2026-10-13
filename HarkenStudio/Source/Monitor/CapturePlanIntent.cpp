#include "CapturePlanIntent.h"
#include "Monitor/MonitorTypes.h"

#include <algorithm>
#include <cstring>

namespace VNX {
namespace Monitor {

bool measurementIdAllowed(uint64_t)
{
    // P5-2 Waveform is unpublished. P5-3..5 are unopened. This pack only
    // ships the empty-intent control skeleton, so no measurement id is wired.
    return false;
}

CapturePlanIntent canonicalIntent(const CapturePlanIntent& raw)
{
    CapturePlanIntent next = raw;
    std::sort(next.measurementIds.begin(), next.measurementIds.end());
    next.measurementIds.erase(
        std::unique(next.measurementIds.begin(), next.measurementIds.end()),
        next.measurementIds.end());
    return next;
}

CapturePlanIntent hideMeasurement(const CapturePlanIntent& current, uint64_t measurementId)
{
    CapturePlanIntent next = current;
    next.measurementIds.erase(
        std::remove(next.measurementIds.begin(), next.measurementIds.end(), measurementId),
        next.measurementIds.end());
    return canonicalIntent(next);
}

bool intentsEqual(const CapturePlanIntent& a, const CapturePlanIntent& b)
{
    return a.measurementIds == b.measurementIds
        && a.streamKindMask == b.streamKindMask
        && a.bytesPerSecondBudget == b.bytesPerSecondBudget;
}

CapturePlanFace faceForResult(MonitorResult result, const CapturePlanIntent& intent)
{
    if (!intent.measurementIds.empty())
    {
        for (uint64_t id : intent.measurementIds)
        {
            if (!measurementIdAllowed(id))
                return CapturePlanFace::UnpublishedMeasurement;
        }
    }

    switch (result)
    {
    case kOk:
        if (intent.measurementIds.empty())
            return CapturePlanFace::EmptyMeasurement;
        return CapturePlanFace::Ready;
    case kMonitorBudgetExceeded:
        return CapturePlanFace::BudgetExceeded;
    case kMonitorPermissionDenied:
    case kMonitorStaleLease:
        return CapturePlanFace::PermissionDenied;
    case kMonitorStaleTap:
        return CapturePlanFace::BindingNotActive;
    case kMonitorUnsupported:
    case kNotInitialized:
        return CapturePlanFace::Unsupported;
    default:
        return CapturePlanFace::Unsupported;
    }
}

CaptureBudgetPreview previewFromEffective(
    MonitorResult result,
    const MonitorEffectiveCapturePlan* accepted,
    uint64_t hardCaptureByteRate,
    uint64_t correlation,
    const CapturePlanIntent& intent)
{
    CaptureBudgetPreview preview;
    preview.result = result;
    preview.face = faceForResult(result, intent);
    preview.hardCaptureByteRate = hardCaptureByteRate;
    preview.correlation = correlation;
    preview.chargesAudioChunk =
        (intent.streamKindMask & kMonitorStreamOptionalAudio) != 0;
    if (accepted != nullptr && result == kOk)
    {
        preview.capturePlanGeneration = accepted->capturePlanGeneration;
        preview.previousGeneration = accepted->previousGeneration;
        preview.effectiveFromExecuteFrame = accepted->effectiveFromExecuteFrame;
        preview.activationState = accepted->activationState;
        preview.acceptedBudget = accepted->accepted.bytesPerSecondBudget;
        if (accepted->activationState == kMonitorPlanPending
            && preview.face == CapturePlanFace::Ready)
            preview.face = CapturePlanFace::Pending;
    }
    return preview;
}

CaptureBudgetPreview keepPreviousPreview(
    const CaptureBudgetPreview& previous,
    MonitorResult result,
    const CapturePlanIntent& intent)
{
    CaptureBudgetPreview preview = previous;
    preview.result = result;
    preview.face = faceForResult(result, intent);
    preview.chargesAudioChunk =
        (intent.streamKindMask & kMonitorStreamOptionalAudio) != 0;
    return preview;
}

bool shouldApply(const CaptureBudgetPreview& preview)
{
    return preview.result == kOk
        && preview.face != CapturePlanFace::UnpublishedMeasurement
        && (preview.face == CapturePlanFace::Ready
            || preview.face == CapturePlanFace::EmptyMeasurement
            || preview.face == CapturePlanFace::Pending);
}

bool generationUnchanged(const CaptureBudgetPreview& before, const CaptureBudgetPreview& after)
{
    return before.capturePlanGeneration == after.capturePlanGeneration;
}

bool previewMatchesCorrelation(const CaptureBudgetPreview& preview, uint64_t correlation)
{
    return preview.correlation == correlation;
}

std::string capturePlanFaceText(const CaptureBudgetPreview& preview)
{
    switch (preview.face)
    {
    case CapturePlanFace::Unsupported:
        return "Unsupported";
    case CapturePlanFace::PermissionDenied:
        return "PermissionDenied";
    case CapturePlanFace::BudgetExceeded:
        return "BudgetExceeded";
    case CapturePlanFace::BindingNotActive:
        return "BindingNotActive";
    case CapturePlanFace::EmptyMeasurement:
        return preview.chargesAudioChunk
            ? "EmptyMeasurement audio-chunk billed cap="
                + std::to_string(preview.hardCaptureByteRate)
            : "EmptyMeasurement";
    case CapturePlanFace::UnpublishedMeasurement:
        return "UnpublishedMeasurement";
    case CapturePlanFace::Pending:
        return "Pending gen=" + std::to_string(preview.capturePlanGeneration)
            + " from=" + std::to_string(preview.effectiveFromExecuteFrame);
    case CapturePlanFace::Ready:
        return "Ready gen=" + std::to_string(preview.capturePlanGeneration)
            + " budget-cap=" + std::to_string(preview.acceptedBudget)
            + "/" + std::to_string(preview.hardCaptureByteRate);
    }
    return "Unsupported";
}

} // namespace Monitor
} // namespace VNX
