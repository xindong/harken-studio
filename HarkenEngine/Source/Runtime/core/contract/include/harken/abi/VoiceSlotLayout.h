// The published per-voice slot-layout bounds that a tooling-side compiler and
// the runtime must agree on.
//
// This header is deliberately free of engine and platform dependencies — it
// includes only <cstdint> — so a tooling-side compiler can read the same POD
// layout ceiling the runtime sizes its arrays to, without linking harken_core or
// pulling in Harken.h. The values are POD-layout constants: they size fixed
// per-voice arrays and are the ceilings the resolved runtime config may not
// exceed. Keeping the single definition point here lets the runtime's own
// EngineConfig and an offline program compiler resolve the same number instead
// of each carrying its own literal.
#ifndef HARKEN_ABI_VOICE_SLOT_LAYOUT_H
#define HARKEN_ABI_VOICE_SLOT_LAYOUT_H

#include <cstdint>

namespace harken {

// Compile-time upper bound on a voice's send slots. This is the POD-layout
// constant sizing the per-voice send array and the ceiling the resolved
// voiceSendSlots config value may not exceed; the active count per voice is the
// resolved config value at or below it. Single definition point.
inline constexpr std::uint32_t kMaxVoiceSendSlots = 8;

// Compile-time upper bound on a voice's insert-effect slots. This is the
// POD-layout constant sizing the per-voice effect-slot array and the ceiling the
// resolved fxSlotsPerVoice config value may not exceed; the active count per
// voice is the resolved config value at or below it. It is also the hard ceiling
// a compiler lowering an effect chain onto fixed slots must refuse past, which is
// why it lives beside the send bound here rather than only inside the engine's
// own config: both sides resolve one number instead of each carrying a literal.
inline constexpr std::uint32_t kMaxVoiceFxSlots = 8;

}  // namespace harken

#endif  // HARKEN_ABI_VOICE_SLOT_LAYOUT_H
