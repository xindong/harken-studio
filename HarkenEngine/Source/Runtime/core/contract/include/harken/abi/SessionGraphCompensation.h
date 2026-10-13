// The published compensation between the session's execute domain and its graph
// output domain, which a tooling-side consumer and the runtime must agree on.
//
// This header is deliberately free of engine and platform dependencies — it
// includes only <cstdint> — so a tooling-side consumer can map an execute-domain
// timeline frame to the graph's output coordinate without linking harken_core or
// pulling in Harken.h. Keeping the single definition point here lets both sides
// add one number instead of each carrying its own literal.
#ifndef HARKEN_ABI_SESSION_GRAPH_COMPENSATION_H
#define HARKEN_ABI_SESSION_GRAPH_COMPENSATION_H

#include <cstdint>

namespace harken {

// How many frames after an execute-domain moment the same signal appears at the
// graph's output: a graph presentation frame is the execute event frame plus
// this constant. It models fixed pipeline latency only — buffering or lookahead
// a node inserts on every sample regardless of content. The current graph
// inserts none, so the value is zero and the two domains coincide numerically;
// they remain distinct coordinates, and a consumer adds the constant rather
// than assuming the coincidence, so the first node with real lookahead changes
// one number instead of every call site.
//
// Ownership is transitional. When a session presentation owner exists, its
// resolved sessionPresentationLatencyCeilingFrames becomes the authoritative
// source of this distance and this constant's value moves there; the constant
// is the single seam that migration will retarget.
//
// Not to be confused with a scheduling lead. TimeCoordinateObservation's
// publishLead is the distance the scheduling cursor runs ahead of execution so
// commands land in time — a property of when work is filed, not of where the
// output signal sits. Borrowing it here would add a scheduling policy number to
// a signal-path coordinate.
inline constexpr std::uint64_t kSessionGraphCompensationFrames = 0;

}  // namespace harken

#endif  // HARKEN_ABI_SESSION_GRAPH_COMPENSATION_H
