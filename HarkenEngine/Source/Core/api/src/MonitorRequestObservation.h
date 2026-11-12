#ifndef HARKEN_API_MONITOR_REQUEST_OBSERVATION_H
#define HARKEN_API_MONITOR_REQUEST_OBSERVATION_H

#include "MonitorCaptureHub.h"
#include "MonitorExecutionBuilder.h"
#include "harken/core/LogRing.h"

namespace harken::monitor {

// A synchronous observer owned by the external call. It never retains caller
// pointers or becomes part of the persistent request context.
class RequestObservation {
public:
    explicit RequestObservation(const MonitorRequestContext& request) noexcept
        : active_(ExecutionObservationEnabled() && LogicThreadReady()) {
        if (!active_) return;
        fact_ = {};
        fact_.flags = kMonitorFactCallBody;
        fact_.engine = ExecutionEngineInstance();
        fact_.timestamp = LogTimestampNs();
        SetSourceField(fact_, VN_MONITOR_FACT_FIELD_TIMESTAMP);
        fact_.body.call.request = CaptureFactRequest(fact_, request,
            VN_MONITOR_FACT_FIELD_REQUEST, VN_MONITOR_FACT_FIELD_REQUEST_OPERATION,
            VN_MONITOR_FACT_FIELD_REQUEST_ORIGIN);
    }

    RequestObservation(const RequestObservation&) = delete;
    RequestObservation& operator=(const RequestObservation&) = delete;
    bool Active() const noexcept { return active_; }

    void TargetHandle(unsigned kind, std::uint64_t handle) noexcept {
        if (!active_) return;
        fact_.body.call.target.kind = kind;
        fact_.body.call.target.handle = handle;
        SetSourceField(fact_, VN_MONITOR_FACT_FIELD_TARGET_KIND);
        SetSourceField(fact_, VN_MONITOR_FACT_FIELD_TARGET_HANDLE);
    }

    void TargetContent(VnObjectId object, unsigned kind) noexcept {
        if (!active_) return;
        auto& target = fact_.body.call.target;
        target.kind = VN_MONITOR_TARGET_CONTENT;
        target.object = object;
        SetSourceField(fact_, VN_MONITOR_FACT_FIELD_TARGET_KIND);
        SetSourceField(fact_, VN_MONITOR_FACT_FIELD_TARGET_OBJECT, object != VnObjectId{});
        if (kind > VN_MONITOR_CONTENT_UNKNOWN && kind <= VN_MONITOR_CONTENT_MEDIA) {
            target.contentKind = kind;
            SetSourceField(fact_, VN_MONITOR_FACT_FIELD_TARGET_CONTENT_KIND);
        }
    }

    void TargetAll() noexcept {
        if (!active_) return;
        fact_.body.call.target.scope = 1;
        SetSourceField(fact_, VN_MONITOR_FACT_FIELD_TARGET_SCOPE);
    }

    void TargetValidated() noexcept {
        if (active_) SetSourceField(fact_, VN_MONITOR_FACT_FIELD_TARGET_VALIDATED);
    }

    void Subject(unsigned subject, std::uint64_t handle) noexcept {
        if (!active_) return;
        if (!ValidSubjectHandle(subject, handle)) {
            fact_.flags |= kMonitorFactIdentityIncomplete;
            return;
        }
        fact_.subjectNamespace = static_cast<std::uint8_t>(subject);
        fact_.body.call.identity.handle = handle;
        SetSourceField(fact_, VN_MONITOR_FACT_FIELD_SUBJECT);
    }

    void Content(VnObjectId project, VnObjectId object, unsigned kind,
                 std::uint64_t world, std::uint64_t semantic,
                 std::uint64_t revision = 0) noexcept {
        if (!active_) return;
        auto& identity = fact_.body.call.identity;
        fact_.contentKind = static_cast<std::uint8_t>(kind);
        identity.project = project;
        identity.object = object;
        identity.world = world;
        identity.semantic = semantic;
        identity.revision = revision;
        SetSourceField(fact_, VN_MONITOR_FACT_FIELD_PROJECT, project != VnObjectId{});
        SetSourceField(fact_, VN_MONITOR_FACT_FIELD_CONTENT_OBJECT, object != VnObjectId{});
        SetSourceField(fact_, VN_MONITOR_FACT_FIELD_CONTENT_WORLD, world != 0);
        SetSourceField(fact_, VN_MONITOR_FACT_FIELD_CONTENT_SEMANTIC, semantic != 0);
        SetSourceField(fact_, VN_MONITOR_FACT_FIELD_CONTENT_REVISION, revision != 0);
        auto& target = fact_.body.call.target;
        if (target.kind == VN_MONITOR_TARGET_CONTENT && target.object == object) {
            target.project = project;
            target.world = world;
            target.semantic = semantic;
            target.revision = revision;
            SetSourceField(fact_, VN_MONITOR_FACT_FIELD_TARGET_PROJECT, project != VnObjectId{});
            SetSourceField(fact_, VN_MONITOR_FACT_FIELD_TARGET_WORLD, world != 0);
            SetSourceField(fact_, VN_MONITOR_FACT_FIELD_TARGET_SEMANTIC, semantic != 0);
            SetSourceField(fact_, VN_MONITOR_FACT_FIELD_TARGET_REVISION, revision != 0);
        }
    }

    void Emitters(const MonitorEmitterOrigin& origin, const MonitorEmitterBinding& binding) noexcept {
        if (active_) CaptureFactEmitters(fact_, origin, binding);
    }

    void Relations(std::uint64_t root, std::uint64_t action, std::uint64_t logicCommit) noexcept {
        if (!active_) return;
        fact_.body.call.root = root;
        fact_.body.call.action = action;
        fact_.body.call.position.logicCommit = logicCommit;
        SetSourceField(fact_, VN_MONITOR_FACT_FIELD_ROOT, root != 0);
        SetSourceField(fact_, VN_MONITOR_FACT_FIELD_RELATED_ACTION, action != 0);
        SetSourceField(fact_, VN_MONITOR_FACT_FIELD_LOGIC_COMMIT, logicCommit != 0);
    }

    void Submitted() noexcept {
        if (!active_ || submitted_) return;
        submitted_ = true;
        fact_.kind = VN_MONITOR_FACT_REQUEST_SUBMITTED;
        PublishExecutionFact(fact_);
    }

    template<class Result> Result Returned(Result result) noexcept {
        if (!active_) return result;
        Submitted();
        fact_.kind = VN_MONITOR_FACT_REQUEST_RETURNED;
        fact_.timestamp = LogTimestampNs();
        fact_.result = static_cast<std::int32_t>(result);
        SetSourceField(fact_, VN_MONITOR_FACT_FIELD_RESULT);
        PublishExecutionFact(fact_);
        active_ = false;
        return result;
    }

private:
    IdentityMutationScope identityMutation_;
    MonitorExecutionFact fact_;
    bool active_ = false;
    bool submitted_ = false;
};

} // namespace harken::monitor

#endif
