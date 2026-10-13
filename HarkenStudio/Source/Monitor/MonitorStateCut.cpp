#include "MonitorStateCut.h"
#include "Monitor/MonitorTypes.h"

#include "MonitorDrop.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace VNX {
namespace Monitor {
namespace {

template <typename T>
bool readPayload(const MonitorStoreQueryHit& hit, T* out)
{
    if (hit.payload.size() < sizeof(T))
        return false;
    std::memcpy(out, hit.payload.data(), sizeof(T));
    return out->structSize >= sizeof(T);
}

std::vector<MonitorStoreQueryHit> stateRecords(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot)
{
    if (!snapshot)
        return {};
    auto hits = snapshot->findByRecordKind(kMonitorRecordStateCutBegin);
    auto upsert = snapshot->findByRecordKind(kMonitorRecordStateUpsert);
    auto tomb = snapshot->findByRecordKind(kMonitorRecordStateTombstone);
    auto end = snapshot->findByRecordKind(kMonitorRecordStateCutEnd);
    hits.insert(hits.end(), upsert.begin(), upsert.end());
    hits.insert(hits.end(), tomb.begin(), tomb.end());
    hits.insert(hits.end(), end.begin(), end.end());
    hits.erase(std::remove_if(hits.begin(), hits.end(),
                              [](const MonitorStoreQueryHit& hit) {
                                  return hit.header.streamKind != kMonitorStreamState;
                              }),
               hits.end());
    std::stable_sort(hits.begin(), hits.end(),
                     [](const MonitorStoreQueryHit& a, const MonitorStoreQueryHit& b) {
                         if (a.header.monitorSessionEpoch != b.header.monitorSessionEpoch)
                             return a.header.monitorSessionEpoch < b.header.monitorSessionEpoch;
                         if (a.header.streamId != b.header.streamId)
                             return a.header.streamId < b.header.streamId;
                         return a.header.sourceSequence < b.header.sourceSequence;
                     });
    return hits;
}

MonitorCutView emptyView(MonitorCutViewStatus status, const char* reason)
{
    MonitorCutView view;
    view.status = status;
    view.reason = reason;
    return view;
}

bool applyObject(MonitorCutView* view,
                 const MonitorStoreQueryHit& hit,
                 uint64_t expectedCutId,
                 bool tombstone)
{
    MonitorObjectKey key;
    uint32_t entityKind = 0;
    harkenstudio::ObjectId stableId = {};
    uint64_t stateCutId = 0;
    if (tombstone)
    {
        MonitorStateTombstone payload {};
        if (!readPayload(hit, &payload))
            return false;
        if (payload.instanceHandle != hit.header.instanceHandle ||
            payload.instanceGeneration != hit.header.instanceGeneration ||
            payload.stateCutId != expectedCutId)
            return false;
        key = {payload.instanceHandle, payload.instanceGeneration, hit.header.monitorSessionEpoch};
        entityKind = payload.entityKind;
        stableId = payload.stableId;
        stateCutId = payload.stateCutId;
    }
    else
    {
        MonitorStateUpsert payload {};
        if (!readPayload(hit, &payload))
            return false;
        if (payload.instanceHandle != hit.header.instanceHandle ||
            payload.instanceGeneration != hit.header.instanceGeneration ||
            payload.stateCutId != expectedCutId)
            return false;
        key = {payload.instanceHandle, payload.instanceGeneration, hit.header.monitorSessionEpoch};
        entityKind = payload.entityKind;
        stableId = payload.stableId;
        stateCutId = payload.stateCutId;
    }

    for (auto& object : view->objects)
    {
        if (object.key == key)
        {
            object.tombstoned = tombstone;
            object.entityKind = entityKind;
            object.stableId = stableId;
            object.stateCutId = stateCutId;
            return true;
        }
    }
    MonitorWorldObject object;
    object.key = key;
    object.entityKind = entityKind;
    object.stableId = stableId;
    object.stateCutId = stateCutId;
    object.tombstoned = tombstone;
    view->objects.push_back(object);
    return true;
}

void applyMergedObject(MonitorCutView* view, const MonitorWorldObject& object)
{
    for (auto& existing : view->objects)
    {
        if (existing.key == object.key)
        {
            existing = object;
            return;
        }
    }
    view->objects.push_back(object);
}

bool sourceFanoutHitsOpen(const MonitorDropProjection& drops,
                          uint64_t epoch,
                          uint64_t streamId,
                          uint64_t beginSequence)
{
    for (const auto& reason : drops.reasons)
    {
        if (reason.kind != MonitorDropReasonKind::StageGap)
            continue;
        if (reason.stage != kMonitorDropSourceProducer &&
            reason.stage != kMonitorDropHubFanout)
            continue;
        if (reason.epoch != epoch || reason.streamId != streamId)
            continue;
        if (reason.lastSourceSequence < beginSequence)
            continue;
        return true;
    }
    return false;
}

} // namespace

MonitorStateCutProjection projectStateCut(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot)
{
    MonitorStateCutProjection out;
    if (!snapshot)
    {
        out.current = emptyView(MonitorCutViewStatus::None, "empty-snapshot");
        return out;
    }

    const auto hits = stateRecords(snapshot);
    if (hits.empty())
    {
        out.current = emptyView(MonitorCutViewStatus::None, "no-cut-records");
        return out;
    }

    const auto drops = projectDrop(snapshot);

    bool open = false;
    bool sourceFanoutHit = false;
    uint64_t openStream = 0;
    uint64_t openEpoch = 0;
    uint64_t openWorld = 0;
    uint64_t openBeginSeq = 0;
    MonitorCutView pending = emptyView(MonitorCutViewStatus::Incomplete, "cut-open");

    auto keepPreviousCurrent = [&]() {
        if (out.current.status != MonitorCutViewStatus::Complete)
            out.current = out.previousComplete;
        if (out.current.status != MonitorCutViewStatus::Complete)
            out.current = emptyView(out.pending.status, out.pending.reason);
    };

    auto mergeCompleteIntoCurrent = [&](const MonitorCutView& completed) {
        if (out.current.status != MonitorCutViewStatus::Complete)
        {
            out.current = completed;
            out.previousComplete = completed;
            return;
        }
        for (const auto& object : completed.objects)
            applyMergedObject(&out.current, object);
        out.previousComplete = out.current;
    };

    auto invalidate = [&](const char* reason) {
        out.pending = emptyView(MonitorCutViewStatus::Invalid, reason);
        keepPreviousCurrent();
        if (out.current.status != MonitorCutViewStatus::Complete)
            out.current = emptyView(MonitorCutViewStatus::Invalid, reason);
        open = false;
        sourceFanoutHit = false;
    };

    auto closeOpenAsMissingEnd = [&]() {
        if (!open)
            return;
        pending.status = MonitorCutViewStatus::Truncated;
        pending.cutResult = kMonitorStateCutTruncated;
        pending.reason = sourceFanoutHit ? "source-or-fanout-drop" : "missing-end";
        out.pending = pending;
        keepPreviousCurrent();
        if (out.current.status != MonitorCutViewStatus::Complete)
            out.current = emptyView(MonitorCutViewStatus::Truncated, pending.reason);
        open = false;
        sourceFanoutHit = false;
    };

    auto matchesOpen = [&](const MonitorStoreQueryHit& hit) {
        return hit.header.monitorSessionEpoch == openEpoch &&
               hit.header.worldGeneration == openWorld &&
               hit.header.streamId == openStream;
    };

    for (const auto& hit : hits)
    {
        if (open && hit.header.monitorSessionEpoch == openEpoch &&
            hit.header.streamId != openStream)
            closeOpenAsMissingEnd();

        if (open && (hit.header.monitorSessionEpoch != openEpoch ||
                     hit.header.worldGeneration != openWorld))
        {
            pending.status = MonitorCutViewStatus::Truncated;
            pending.cutResult = kMonitorStateCutTruncated;
            pending.reason = "epoch-or-world-changed";
            out.pending = pending;
            keepPreviousCurrent();
            out.mandatoryResync = true;
            open = false;
            sourceFanoutHit = false;
        }
        else if (!open && !hits.empty() &&
                 (out.previousComplete.epoch != 0 ||
                  out.previousComplete.worldGeneration != 0) &&
                 (hit.header.monitorSessionEpoch != out.previousComplete.epoch ||
                  hit.header.worldGeneration != out.previousComplete.worldGeneration))
        {
            out.mandatoryResync = true;
            out.current = emptyView(MonitorCutViewStatus::None, "mandatory-resync");
        }

        switch (hit.header.recordKind)
        {
        case kMonitorRecordStateCutBegin: {
            if (open)
            {
                invalidate("nested-begin");
                break;
            }
            MonitorStateCutBegin begin {};
            if (!readPayload(hit, &begin) ||
                begin.worldGeneration != hit.header.worldGeneration)
            {
                invalidate("begin-identity-mismatch");
                break;
            }
            pending = emptyView(MonitorCutViewStatus::Incomplete, "cut-open");
            pending.stateCutId = begin.stateCutId;
            pending.worldGeneration = begin.worldGeneration;
            pending.epoch = hit.header.monitorSessionEpoch;
            pending.streamId = hit.header.streamId;
            open = true;
            openStream = hit.header.streamId;
            openEpoch = hit.header.monitorSessionEpoch;
            openWorld = hit.header.worldGeneration;
            openBeginSeq = hit.header.sourceSequence;
            sourceFanoutHit = sourceFanoutHitsOpen(drops, openEpoch, openStream, openBeginSeq);
            break;
        }
        case kMonitorRecordStateUpsert:
            if (!open)
            {
                invalidate("upsert-without-begin");
                break;
            }
            if (!matchesOpen(hit) || !applyObject(&pending, hit, pending.stateCutId, false))
                invalidate("upsert-identity-mismatch");
            break;
        case kMonitorRecordStateTombstone:
            if (!open)
            {
                invalidate("tombstone-without-begin");
                break;
            }
            if (!matchesOpen(hit) || !applyObject(&pending, hit, pending.stateCutId, true))
                invalidate("tombstone-identity-mismatch");
            break;
        case kMonitorRecordStateCutEnd: {
            if (!open)
            {
                invalidate("end-without-begin");
                break;
            }
            MonitorStateCutEnd end {};
            if (!readPayload(hit, &end) || !matchesOpen(hit) ||
                end.stateCutId != pending.stateCutId)
            {
                invalidate("end-identity-mismatch");
                break;
            }
            pending.cutResult = end.result;
            if (end.result == kMonitorStateCutAborted)
            {
                pending.status = MonitorCutViewStatus::Aborted;
                pending.reason = "aborted";
                out.pending = pending;
                keepPreviousCurrent();
                if (out.current.status != MonitorCutViewStatus::Complete)
                    out.current = emptyView(MonitorCutViewStatus::Aborted, "aborted-without-prior-world");
            }
            else if (sourceFanoutHit || end.result != kMonitorStateCutComplete)
            {
                pending.status = MonitorCutViewStatus::Truncated;
                pending.cutResult = kMonitorStateCutTruncated;
                pending.reason = sourceFanoutHit ? "source-or-fanout-drop" : "truncated";
                out.pending = pending;
                keepPreviousCurrent();
                if (out.current.status != MonitorCutViewStatus::Complete)
                    out.current = emptyView(MonitorCutViewStatus::Truncated, pending.reason);
            }
            else
            {
                pending.status = MonitorCutViewStatus::Complete;
                pending.reason = "complete";
                mergeCompleteIntoCurrent(pending);
                if (out.pending.status != MonitorCutViewStatus::Truncated &&
                    out.pending.status != MonitorCutViewStatus::Aborted &&
                    out.pending.status != MonitorCutViewStatus::Invalid)
                    out.pending = emptyView(MonitorCutViewStatus::None, nullptr);
            }
            open = false;
            sourceFanoutHit = false;
            break;
        }
        default:
            break;
        }
    }

    closeOpenAsMissingEnd();
    if (out.current.status == MonitorCutViewStatus::None &&
             out.previousComplete.status == MonitorCutViewStatus::Complete)
    {
        out.current = out.previousComplete;
    }

    return out;
}

} // namespace Monitor
} // namespace VNX
