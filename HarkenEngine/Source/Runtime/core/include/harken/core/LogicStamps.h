// Stamps that make a logic-side value answerable after the fact: when a Variable
// write took effect, which cut a resolved snapshot was taken at, and the numeric
// trail that lets a telemetry record be traced back without symbols.
//
// A Variable write carries both a commit order and an effective frame because
// the two answer different questions. The order says which of two writes came
// first even when they land in the same frame; the frame says when the value
// starts applying. Keeping only the frame would make same-frame writes
// unorderable, and keeping only the order would make "value as of frame N"
// unanswerable.
//
// Evaluation reads through a cut rather than reading whatever a Variable holds
// now. Two decisions taken in one tick must see the same values, or a selection
// and the transition that follows it can disagree about the input that drove
// them. The reader below is the seam that expresses that: a caller asks for a
// value as of a stamp, never for the current one.
//
// Correlation is numeric on purpose. A telemetry consumer has to be able to
// attribute a record to an arranging instance, a clip and an occurrence with no
// name table present, because names are a strippable sidecar and the attribution
// is not.
#ifndef HARKEN_CORE_LOGIC_STAMPS_H
#define HARKEN_CORE_LOGIC_STAMPS_H

#include <harken/ObjectId.h>
#include "harken/core/LogicCommitCoordinator.h"
#include "harken/core/PlayableInstanceTable.h"
#include "harken/core/TimeCoordinates.h"

#include <cstdint>

namespace harken {

// When one Variable write takes effect and where it sits in commit order.
struct VariableWriteStamp {
    LogicCommitSequence commitSequence = 0;
    ExecuteFrame effectiveFrame{};

    bool Stamped() const { return commitSequence != 0; }
};

// The cut a resolved snapshot was taken at. Everything evaluated against this
// snapshot sees the same values, whatever has been written since.
struct ResolvedVariableSnapshotStamp {
    LogicCommitSequence commitSequence = 0;
    ExecuteFrame decisionFrame{};

    bool Stamped() const { return commitSequence != 0; }
};

// Whether a write is in force at a cut. A write is visible when it was committed
// no later than the cut and its effective frame has arrived. Both conditions are
// needed: a write committed before the cut but effective later has not started
// applying, and a write effective earlier but committed after the cut belongs to
// a decision that had not happened yet.
inline bool WriteVisibleAt(const VariableWriteStamp& write,
                           const ResolvedVariableSnapshotStamp& cut) {
    if (!write.Stamped() || !cut.Stamped()) {
        return false;
    }
    return write.commitSequence <= cut.commitSequence &&
           write.effectiveFrame.value <= cut.decisionFrame.value;
}

// The seam through which evaluation reads a Variable. Deliberately takes a cut
// rather than offering a "current value" call: an evaluator that could read the
// present would eventually do so, and two decisions in one tick would stop
// agreeing. A null callback answers "no value", so an unwired reader degrades to
// an unset input instead of undefined behaviour.
struct VariableSnapshotReader {
    void* ctx = nullptr;
    bool (*valueAt)(void* ctx, VnObjectId variableId,
                    const ResolvedVariableSnapshotStamp& cut,
                    float* outValue) = nullptr;

    bool Read(VnObjectId variableId,
              const ResolvedVariableSnapshotStamp& cut, float* outValue) const {
        return valueAt != nullptr && valueAt(ctx, variableId, cut, outValue);
    }
};

// The numeric trail a telemetry record carries. Everything here is an id or a
// count, so a consumer can attribute the record with no symbol sidecar loaded --
// which is the case whenever symbols were stripped for shipping.
struct TelemetryCorrelation {
    // The arranging instance: the Piece instance for a Track inside a Piece, the
    // Track instance for a directly triggered Track. Both lineage depths are
    // expressed by this one field rather than by two record shapes.
    PlayableInstanceHandle arrangingInstance = 0;
    VnObjectId clipId{};
    std::uint32_t loopIteration = 0;
    std::uint32_t occurrenceOrdinal = 0;
    // Which commit produced the record, and where it applied.
    LogicCommitSequence commitSequence = 0;
    ExecuteFrame executeFrame{};

    // A record with no arranging instance and no commit cannot be attributed to
    // anything, and is better reported as unattributable than published as if it
    // described something.
    bool Attributable() const {
        return arrangingInstance != 0 && commitSequence != 0;
    }
};

} // namespace harken

#endif // HARKEN_CORE_LOGIC_STAMPS_H
