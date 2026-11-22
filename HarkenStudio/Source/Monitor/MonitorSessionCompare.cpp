#include "MonitorSessionCompare.h"

#include <algorithm>
#include <cstdint>

namespace harken::studio::Monitor {

namespace {

bool missingData(const MonitorSessionCompareView& view)
{
    return view.status != static_cast<uint32_t>(kOk) || view.hasCompleteFooter == 0 ||
           view.recoveryNeeded != 0 || view.droppedRequired != 0 || view.requiredUnknown != 0 ||
           view.crcFailedRequired != 0 || view.truncatedAfterCommit != 0 || view.gapInvalidWarmup != 0;
}

bool identityPresent(const MonitorSessionCompareView& view)
{
    return view.programSemanticFingerprint != 0 && view.tapDescriptorId != 0 && view.sampleRate != 0 &&
           view.algorithmId != 0 && view.unitId != 0;
}

bool identityMatches(const MonitorSessionCompareView& left, const MonitorSessionCompareView& right)
{
    return left.programSemanticFingerprint == right.programSemanticFingerprint &&
           left.runtimeLineageFingerprint == right.runtimeLineageFingerprint &&
           left.tapDescriptorId == right.tapDescriptorId &&
           left.measurementSegmentGeneration == right.measurementSegmentGeneration &&
           left.sampleRate == right.sampleRate && left.algorithmId == right.algorithmId &&
           left.algorithmVersion == right.algorithmVersion && left.unitId == right.unitId;
}

bool targetStreamPresent(const MonitorSessionCompareView& view)
{
    if (view.targetStreamKindMask == 0)
        return view.presentStreamKindMask != 0;
    return (view.presentStreamKindMask & view.targetStreamKindMask) == view.targetStreamKindMask;
}

bool rangeHasSamples(const MonitorSessionCompareView& view, uint64_t begin, uint64_t end)
{
    if (view.rangeEnd < view.rangeBegin || end < begin)
        return false;
    return std::min(end, view.rangeEnd) > std::max(begin, view.rangeBegin);
}

}

MonitorCompareResult compareSessions(const MonitorSessionCompareDesc& desc)
{
    MonitorCompareResult out {};
    const bool unbounded = desc.executeFrameBegin == 0 && desc.executeFrameEnd == 0;
    const uint64_t begin = unbounded ? 0 : desc.executeFrameBegin;
    const uint64_t end = unbounded ? UINT64_MAX : desc.executeFrameEnd;
    const auto& left = *desc.left;
    const auto& right = *desc.right;
    if (end < begin || missingData(left) || missingData(right) ||
        !rangeHasSamples(left, begin, end) || !rangeHasSamples(right, begin, end))
        out.result = kMonitorCompareMissingData;
    else if (!identityPresent(left) || !identityPresent(right) || !identityMatches(left, right))
        out.result = kMonitorCompareIncomparable;
    else if (!targetStreamPresent(left) || !targetStreamPresent(right) ||
             left.hasOptionalSymbols != right.hasOptionalSymbols ||
             left.hasTraceCorrelation != right.hasTraceCorrelation)
        out.result = kMonitorComparePartiallyComparable;
    else
        out.result = kMonitorCompareComparable;
    return out;
}

}
