// The published stop-fade contract for timeline-domain stops that a tooling-side
// consumer and the runtime executor must agree on.
//
// This header is deliberately free of engine and platform dependencies — it
// includes only <cstdint> — so a tooling-side consumer can resolve the same
// declick value the runtime-side executor will apply, without linking harken_core
// or pulling in Harken.h. Keeping the single definition point here lets both
// sides supply one number instead of each carrying its own literal.
#ifndef HARKEN_ABI_TIMELINE_STOP_CONTRACT_H
#define HARKEN_ABI_TIMELINE_STOP_CONTRACT_H

#include <cstdint>

namespace harken {

// The fade, in milliseconds, every timeline-domain stop supplies when the
// occurrence authored no fade of its own. The engine's public stop treats a
// zero fade as an immediate cut, so a consumer that forwarded zero would put a
// full-amplitude discontinuity on the output; five milliseconds is a declick,
// not an audible fade. The engine resolves milliseconds to frames rounding up
// ((ms * rate + 999) / 1000), so the value never resolves to zero frames at
// any accepted sample rate.
//
// One value, three sharing paths, all of them stops of voices nothing authored
// a fade for: the natural stop event a compiled occurrence carries, a caller's
// requested stop that ends a preview early, and the retry of a stop the
// command queue previously refused. Sharing the constant is what keeps the
// three indistinguishable on the output.
//
// Domain boundary. This constant governs stops of committed voices only. An
// install rollback is not in this domain: a staged install publishes nothing
// until its commit, so a failed install cancels a voice that never sounded — a
// logic-side bookkeeping step that cannot fail and has nothing to ramp. The
// engine's steal declick is not in this domain either: it is frame-denominated,
// bounded to one block, and owned by the voice pool.
//
// Authored fades. The schema carries a fade envelope on every clip and it
// survives into the compiled program, but no stage lowers it to a runtime fade
// yet. When authored fades lower, an authored value overrides this declick for
// its occurrence; fade-endpoint goldens belong to the runtime executor's
// acceptance, where the lowering will land, not to this constant.
//
// Exhausted versus live. A single-pass Sound occurrence does carry a stop
// event — its natural length is known, so the stop frame is compiled. At a
// resample ratio of one, when rendering has already advanced the voice to its
// natural endpoint, the stop usually lands on a stale handle: no cut, no ramp,
// nothing to declick. If rendering has not reached that endpoint, the current
// voice state decides the result instead: a Pending voice is cancelled
// synchronously, while a Live voice takes this declick path. A ratio below one
// stretches the material past the compiled stop frame, so the stop then lands
// on a live voice; that is a material-duration fact, not a liveness oracle.
// A looped occurrence is live at its stop frame by construction.
inline constexpr std::uint32_t kTimelineStopDeclickMs = 5;

}  // namespace harken

#endif  // HARKEN_ABI_TIMELINE_STOP_CONTRACT_H
