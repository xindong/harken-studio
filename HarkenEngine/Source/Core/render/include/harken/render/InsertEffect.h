// Insert-effect seam: one slot per voice in M1, a full chain in M2. The seam
// is a POD of plain function pointers over caller-owned POD state — no virtual
// dispatch, no heap ownership, nothing for the audio thread to allocate or
// lock. An effect that needs memory gets it from init-time pool storage wired
// into `state` on the logic thread.
//
// The protocol is context-free: an effect knows only its own block in/out, its
// tail report, a parameter set entry, and a reset entry — never the hosting
// voice or any other host. This keeps the same instance reusable by voice
// chains and future bus chains alike, and keeps pooled instances ownerless
// (chain slots hold references; the pool serves every chain).
#ifndef HARKEN_RENDER_INSERT_EFFECT_H
#define HARKEN_RENDER_INSERT_EFFECT_H

#include <cstdint>

namespace harken {

// Tail protocol sentinel: an effect that never stops sounding after its input
// ends (a self-oscillating feedback, a frozen reverb) reports this instead of
// a frame count. The cascade's serial aggregation stays well-defined
// with it (any infinite contribution makes the aggregate infinite), and a
// voice whose
// aggregate tail is infinite never auto-completes — only a Stop ends it. The
// two real built-in effects always report finite tails; the sentinel exists so
// the protocol and the aggregation are correct for effects that don't.
inline constexpr std::uint32_t kInfiniteTail = 0xFFFFFFFFu;

// Adds two tail reports without allowing a finite sum to alias the infinite
// sentinel. Infinite dominates every finite contribution; finite sums use the
// largest representable value below the sentinel as their cap.
inline constexpr std::uint32_t SaturatingTailAdd(std::uint32_t aggregate,
                                                 std::uint32_t contribution) {
    if (aggregate == kInfiniteTail || contribution == kInfiniteTail) {
        return kInfiniteTail;
    }
    constexpr std::uint32_t finiteCap = kInfiniteTail - 1u;
    if (contribution > finiteCap - aggregate) {
        return finiteCap;
    }
    return aggregate + contribution;
}

// Sentinel effect-type id marking a chain slot as empty (no effect installed).
// The real type ids are the public VnEffectType values (0, 1, ...), so the
// sentinel sits at the top of the u32 space where no real type can collide.
inline constexpr std::uint32_t kEffectTypeNone = 0xFFFFFFFFu;

// Identifies one pooled effect instance by its type and its index within that
// type's pool. A chain slot stores this alongside its InsertEffect so the
// audio thread can name the instance when it retires it (voice end, clear, or
// in-place replace), letting the logic thread return it to the right pool.
// type == kEffectTypeNone marks an empty slot.
struct EffectInstanceRef {
    std::uint32_t type = kEffectTypeNone;
    std::uint32_t poolIndex = 0;
    std::uint64_t generation = 0;
};

inline std::uint64_t PackedMonitorEffectHandle(std::uint32_t busIndex,
                                               std::uint32_t slot) noexcept {
    return (static_cast<std::uint64_t>(busIndex) << 32) |
           (static_cast<std::uint64_t>(slot) + 1u);
}

struct EffectDynamicInputView {
    const float* detectorMagnitude = nullptr;
    std::uint32_t frameCount = 0;
    std::uint32_t signalForm = 0;
    std::uint32_t flags = 0;
};

struct InsertEffect {
    // Processes planar channel data in place over [offset, offset+frames).
    // Runs on the audio thread under the section-4 rules.
    void (*process)(void* state, float* const* channels,
                    std::uint32_t channelCount, std::uint32_t offset,
                    std::uint32_t frames) = nullptr;

    void (*processDynamic)(void* state, float* const* channels,
                           std::uint32_t channelCount, std::uint32_t offset,
                           std::uint32_t frames,
                           const EffectDynamicInputView* input) = nullptr;

    // Frames of tail the effect still emits after its input has ended
    // (reverb/delay decay). Zero for memoryless effects, kInfiniteTail for
    // an unending tail. Queried when the source finishes; the cascade then
    // feeds silence for exactly this many frames before the voice is Done.
    std::uint32_t (*tailFrames)(const void* state) = nullptr;

    // Updates one parameter to `value`. paramIndex is a per-effect-type
    // private index space (never the base spec's global parameter-target
    // IDs). The value takes effect at the block boundary where the command
    // applies. Effects whose coefficient design uses transcendental math carry
    // a separately prepared section through their host apply path; other
    // effects use this callback directly. nullptr for effects with no
    // parameters.
    void (*setParam)(void* state, std::uint32_t paramIndex,
                     float value) = nullptr;

    // Returns the instance to a deterministic initial state (clears delay
    // lines, filter memory, detector history, ring positions). Called on the
    // logic thread during pool reuse. A stateful effect whose contract resets
    // on bypass-to-enable may also invoke its allocation-free reset at the
    // audio block boundary. nullptr for stateless effects.
    void (*reset)(void* state) = nullptr;

    void* state = nullptr;
};

// An explicit identity effect. Chain slots default to EMPTY (skipped at zero
// cost, bit-identical to identity), so this is not auto-installed; it exists
// as a real in-circuit identity for code that wants the process callback to
// actually run over every produced frame.
InsertEffect MakePassthroughEffect();

} // namespace harken

#endif // HARKEN_RENDER_INSERT_EFFECT_H
