#include "MonitorSessionCompare.h"

#include <cstdint>
#include <cstring>

// Public completeness pin: 112 functions / 84 structs after Compare.

namespace harken {
namespace monitor {
namespace {

void ClearResult(VnMonitorCompareResult* out) {
    if (out == nullptr) {
        return;
    }
    const uint32_t size = out->structSize;
    std::memset(out, 0, sizeof(*out));
    out->structSize = size;
}

bool ViewReady(const VnMonitorSessionCompareView* view) {
    return view != nullptr && view->structSize == sizeof(VnMonitorSessionCompareView);
}

bool ViewMissingData(const VnMonitorSessionCompareView* view) {
    if (view->status != static_cast<uint32_t>(VN_OK)) {
        return true;
    }
    if (view->hasCompleteFooter == 0 || view->recoveryNeeded != 0 ||
        view->droppedRequired != 0 || view->requiredUnknown != 0 ||
        view->crcFailedRequired != 0 || view->truncatedAfterCommit != 0 ||
        view->gapInvalidWarmup != 0) {
        return true;
    }
    return false;
}

bool IdentityPresent(const VnMonitorSessionCompareView* view) {
    return view->programSemanticFingerprint != 0 && view->tapDescriptorId != 0 &&
           view->sampleRate != 0 && view->algorithmId != 0 && view->unitId != 0;
}

bool IdentityMatches(const VnMonitorSessionCompareView* left,
                     const VnMonitorSessionCompareView* right) {
    return left->programSemanticFingerprint == right->programSemanticFingerprint &&
           left->runtimeLineageFingerprint == right->runtimeLineageFingerprint &&
           left->tapDescriptorId == right->tapDescriptorId &&
           left->measurementSegmentGeneration == right->measurementSegmentGeneration &&
           left->sampleRate == right->sampleRate &&
           left->algorithmId == right->algorithmId &&
           left->algorithmVersion == right->algorithmVersion &&
           left->unitId == right->unitId;
}

bool TargetStreamPresent(const VnMonitorSessionCompareView* view) {
    const uint32_t mask = view->targetStreamKindMask;
    if (mask == 0) {
        return view->presentStreamKindMask != 0;
    }
    return (view->presentStreamKindMask & mask) == mask;
}

bool RangeHasSamples(const VnMonitorSessionCompareView* view, uint64_t begin,
                     uint64_t end) {
    if (view->rangeEnd < view->rangeBegin) {
        return false;
    }
    if (end < begin) {
        return false;
    }
    const uint64_t lo = begin > view->rangeBegin ? begin : view->rangeBegin;
    const uint64_t hi = end < view->rangeEnd ? end : view->rangeEnd;
    return hi > lo;
}

void QueryRange(const VnMonitorSessionCompareDesc* desc, uint64_t* begin,
                uint64_t* end) {
    if (desc->executeFrameBegin == 0 && desc->executeFrameEnd == 0) {
        *begin = 0;
        *end = UINT64_MAX;
        return;
    }
    *begin = desc->executeFrameBegin;
    *end = desc->executeFrameEnd;
}

} // namespace

VnResult CompareSessions(const VnMonitorSessionCompareDesc* desc,
                         VnMonitorCompareResult* out) {
    if (out == nullptr) {
        return VN_INVALID_ARG;
    }
    if (out->structSize != sizeof(VnMonitorCompareResult)) {
        return VN_INVALID_ARG;
    }
    ClearResult(out);
    if (desc == nullptr || desc->structSize != sizeof(VnMonitorSessionCompareDesc)) {
        return VN_INVALID_ARG;
    }
    if (!ViewReady(desc->left) || !ViewReady(desc->right)) {
        return VN_INVALID_ARG;
    }

    uint64_t begin = 0;
    uint64_t end = 0;
    QueryRange(desc, &begin, &end);
    if (end < begin) {
        out->result = VN_MONITOR_COMPARE_MISSING_DATA;
        return VN_OK;
    }

    if (ViewMissingData(desc->left) || ViewMissingData(desc->right)) {
        out->result = VN_MONITOR_COMPARE_MISSING_DATA;
        return VN_OK;
    }
    if (!RangeHasSamples(desc->left, begin, end) ||
        !RangeHasSamples(desc->right, begin, end)) {
        out->result = VN_MONITOR_COMPARE_MISSING_DATA;
        return VN_OK;
    }
    if (!IdentityPresent(desc->left) || !IdentityPresent(desc->right) ||
        !IdentityMatches(desc->left, desc->right)) {
        out->result = VN_MONITOR_COMPARE_INCOMPARABLE;
        return VN_OK;
    }

    const bool leftTarget = TargetStreamPresent(desc->left);
    const bool rightTarget = TargetStreamPresent(desc->right);
    if (!leftTarget || !rightTarget) {
        out->result = VN_MONITOR_COMPARE_PARTIALLY_COMPARABLE;
        return VN_OK;
    }
    if (desc->left->hasOptionalSymbols != desc->right->hasOptionalSymbols ||
        desc->left->hasTraceCorrelation != desc->right->hasTraceCorrelation) {
        out->result = VN_MONITOR_COMPARE_PARTIALLY_COMPARABLE;
        return VN_OK;
    }

    out->result = VN_MONITOR_COMPARE_COMPARABLE;
    return VN_OK;
}

} // namespace monitor
} // namespace harken
