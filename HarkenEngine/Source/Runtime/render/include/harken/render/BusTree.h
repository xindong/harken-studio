// Bus graph: signal-flow mix nodes rooted at the pre-existing master bus, plus
// the additive send edges of the unified bus model. Tree storage is a
// fixed-capacity array sized at engine init. Dense indices are assigned
// append-only; destruction tombstones an index instead of reusing it.
//
// Two edge kinds fold into the master each block:
//  - tree edges (exclusive output path): each bus sums into its parent at unity.
//  - send edges (additive post-fader taps): a bus->bus send adds a scaled copy
//    of the source bus's finalized output into a target bus's input.
//
// Evaluation order is a logic-side published property: on any structural change
// the logic thread cycle-checks and topologically sorts the combined tree+send
// graph and publishes a topology plan (a pool slot handed over by command). The
// audio thread walks the installed plan's feeds-first order so every bus is
// finalized before any bus it feeds; before the first plan exists it uses the
// descending-index fold, which is already feeds-first on a pure tree. Plan
// contents are STRUCTURE ONLY: send levels and their ramps are ordinary
// audio-thread state kept here, not in the plan, so changing a level
// republishes nothing.
#ifndef HARKEN_RENDER_BUS_TREE_H
#define HARKEN_RENDER_BUS_TREE_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>

#include "harken/core/BusSlotRef.h"
#include "harken/core/EngineConfig.h"
#include "harken/core/CommandQueue.h"
#include "harken/dsp/GainRamp.h"
#include "harken/render/EffectPool.h"
#include "harken/render/DynamicRoute.h"
#include "harken/render/InsertEffect.h"
#include "harken/render/TopologyPlan.h"

#include <cstdint>
#include <atomic>
#include <vector>

namespace harken {

inline constexpr std::uint32_t kInvalidBusIndex = 0xFFFFFFFFu;

class BusTree {
public:
    // The plan pool is logic-constructed and audio-consumed; the tree holds a
    // reference to install plans from and retire superseded ones into. The
    // effect bank is shared with voices: bus chains acquire/retire instances
    // from the same pools (pure reuse), so the tree holds a reference to name
    // retired instances on its retire queue.
    BusTree(const EngineConfig& config, TopologyPlanPool& plans,
            EffectPoolBank& effects, DynamicRouteSystem& dynamicRoutes,
            const BiquadTrigTable* biquadTrig = nullptr);

    struct PreparedBusSlot {
        VnBusId id{};
        std::uint32_t parentIndex = 0;
        std::uint32_t generation = 0;
    };

    // Prepared Pack activation seam. Capacity and append-position validation
    // happens before the non-failing materialization/commit tail. Hidden slots
    // are fully initialized in preallocated storage but remain outside every
    // logic-visible range until one visibility commit publishes the batch.
    bool CanMaterializePreparedBusSlots(std::uint32_t firstIndex,
                                        std::uint32_t slotCount) const noexcept;
    void MaterializePreparedBusSlots(std::uint32_t firstIndex,
                                     const PreparedBusSlot* slots,
                                     std::uint32_t slotCount) noexcept;
    void CommitPreparedBusVisibility(std::uint32_t firstIndex,
                                     std::uint32_t slotCount) noexcept;

    // Creates a bus under an existing parent. Logic thread. Refusals:
    // id 0 -> VN_INVALID_ARG, id already present (including the master id)
    // -> VN_IN_USE, unknown parent -> VN_UNKNOWN_ID, table full ->
    // VN_NO_BUS.
    VnResult CreateBus(VnBusId id, VnBusId parentId);

    // Resolves a caller-facing bus id to its dense index, or
    // kInvalidBusIndex. Logic thread (commands carry the resolved index so
    // the audio side never searches).
    std::uint32_t FindIndex(VnBusId id) const;
    BusSlotRef FindRef(VnBusId id) const;
    BusSlotRef RefAt(std::uint32_t busIndex) const;
    std::uint32_t GenerationAt(std::uint32_t busIndex) const;
    std::uint32_t NextGenerationForSlot(std::uint32_t busIndex) const;
    bool LogicRefMatches(BusSlotRef ref) const;
    bool AudioRefMatches(BusSlotRef ref) const;

    // The public bus id at a dense index (0 when out of range or the slot is
    // tombstoned): the reverse lookup attribution capture uses to report
    // public locators. Logic thread.
    VnBusId IdAt(std::uint32_t busIndex) const {
        return busIndex < m_count && m_logicAlive[busIndex] != 0
                   ? m_logicId[busIndex]
                   : VnBusId{};
    }

    // Logic-visible bus count: advances immediately when CreateBus appends a
    // bus. The topology planner and the create preflight (logic thread) read
    // it. The audio thread must NOT read this — it learns of a new bus only
    // through AudioBusCount(), which advances at the plan swap.
    std::uint32_t BusCount() const { return m_count; }
    std::uint32_t ActiveBusCount() const { return m_activeCount; }
    std::uint32_t FreeBusSlotCount() const;
    std::uint32_t QuarantinedBusSlotCount() const {
        std::uint32_t count = 0;
        for (std::uint32_t slot = 1; slot < m_count; ++slot) {
            count += m_logicAlive[slot] == 0 && QuarantinedAt(slot);
        }
        return count;
    }
    bool QuarantinedAt(std::uint32_t busIndex) const {
        return busIndex < m_capacity &&
            (m_logicQuarantined[busIndex] != 0 || m_logicRetireRevision[busIndex] != 0);
    }
    std::uint64_t RetirementRevisionAt(std::uint32_t busIndex) const noexcept {
        return busIndex < m_capacity ? m_logicRetireRevision[busIndex] : 0;
    }
    bool HoldSlotReuse(BusSlotRef ref, std::uint64_t revision) noexcept;
    bool HoldRetiredSlot(BusSlotRef ref, std::uint64_t revision) noexcept;
    bool ReleaseRetiredSlot(BusSlotRef ref, std::uint64_t revision) noexcept;
    std::uint64_t SlotReuseCount() const { return m_slotReuseCount; }

    // Audio-visible bus count: the buses the audio thread may touch this block.
    // Advances ONLY inside ApplyTopologyPlan, to the committed count the
    // installed plan carries (its order lists every live bus exactly once, so
    // orderCount is that count). A bus created on the logic thread thus becomes
    // audible exactly when its plan-swap command applies at a block boundary,
    // never before — the mixer sizes its plane set, folds, and snapshots
    // against this. Audio thread.
    std::uint32_t AudioBusCount() const { return m_audioCount; }
    std::uint32_t AudioGenerationAt(std::uint32_t busIndex) const {
        return busIndex < m_audioCount ? m_buses[busIndex].generation : 0;
    }

    // Tree parent of a bus (itself for the master), from the LOGIC-side parent
    // shadow: the topology planner reads it to include tree edges in the
    // combined feeds-first sort, and a live reparent rewrites it here first.
    // The audio-side Bus::parentIndex is written only by ApplyTopologyPlan
    // from the plan's parents[], so a parent change never races the MixDown
    // tree fold. Logic thread.
    std::uint32_t ParentOf(std::uint32_t busIndex) const {
        return busIndex < m_count ? m_logicParent[busIndex] : 0;
    }

    // Rewrites a live bus's LOGIC-side parent (the reparent commit step). The
    // caller owns validation (existing bus, existing parent, no cycle) and
    // publishes a plan in the same call so the audio side installs the new
    // parent at the next block boundary. Logic thread.
    void SetLogicParent(std::uint32_t busIndex, std::uint32_t parentIndex) {
        if (busIndex < m_count && parentIndex < m_count) {
            m_logicParent[busIndex] = parentIndex;
        }
    }

    // Slot liveness (tombstone state). A destroyed slot keeps its dense index
    // forever (never reused, capacity back only at term); FindIndex skips it
    // and IdAt reads 0, so its public id resolves nowhere and may be recreated
    // as a fresh slot. Logic thread.
    bool AliveAt(std::uint32_t busIndex) const {
        return busIndex < m_count && m_logicAlive[busIndex] != 0;
    }
    // Tombstones one slot (the destroy commit step; the caller owns the
    // cascade walk, reference scrub, and the plan publish that delivers the
    // exclusion to the audio thread). Logic thread.
    void MarkLogicDead(std::uint32_t busIndex) {
        if (busIndex < m_count && busIndex != 0) {
            if (m_logicAlive[busIndex] != 0) {
                m_logicAlive[busIndex] = 0;
                if (m_activeCount > 0) {
                    --m_activeCount;
                }
            }
        }
    }

    void CommitLogicSlot(std::uint32_t busIndex, VnBusId id,
                         std::uint32_t parentIndex,
                         std::uint32_t generation) noexcept;

    // The installed plan's liveness and parents arrays (nullptr before the
    // first slot-aware plan): the mixer's swap-time voice scrub reads them in
    // the same block the swap applied. Audio thread.
    const std::uint8_t* InstalledAlive() const;
    const std::uint32_t* InstalledParents() const;
    const std::uint32_t* InstalledGenerations() const;
    const std::uint32_t* InstalledMigrationTargets() const;

    // Applies a queued volume command. Audio thread, at block start. The dB
    // value converts to linear here; the first value ever set on a bus
    // snaps, as does rampMs == 0. A ramp spans ceil(rampFrames/blockFrames)
    // whole blocks: each block ramps linearly to an equally spaced
    // intermediate target, which composes to one exactly linear curve that
    // lands on the final value at a block's last sample.
    void ApplyVolume(std::uint32_t busIndex, float volumeDb,
                     std::uint32_t rampMs,
                     std::uint32_t busGeneration = 0, bool outputOnly = false);

    // Installs a published topology plan at the block boundary. Audio thread.
    // Retires whatever plan the tree held (the logic side reclaims it), stores
    // the new plan index, and reconciles each send-level slot against the same
    // authoritative membership slot: equal target preserves its level/ramp,
    // empty clears it, and a changed target resets it. Prepared level actions
    // then snap the matching slot and clear all ramp bookkeeping. A legacy
    // connect payload (connectFrom != kInvalidBusIndex) still snaps the new
    // edge to its initial level in the same atomic apply.
    void ApplyTopologyPlan(std::uint32_t planIndex, std::uint32_t connectFrom,
                           std::uint32_t connectTo, float connectLevelDb,
                           std::uint64_t acceptedGraphRevision = 0);
    std::uint64_t InstalledGraphRevision() const noexcept {
        return m_installedGraphRevision.load(std::memory_order_acquire);
    }
    void AcknowledgeInstalledGraphRevision(
        std::uint64_t acceptedGraphRevision) noexcept {
        if (acceptedGraphRevision != 0) {
            m_installedGraphRevision.store(acceptedGraphRevision,
                                           std::memory_order_release);
        }
    }

    // Applies a queued bus->bus send-level change. Audio thread, at block
    // start. Same ramp discipline as ApplyVolume; a no-op when the edge is not
    // present in the current membership (its level slot then holds no target).
    void ApplySetBusSendLevel(std::uint32_t fromBus, std::uint32_t toBus,
                              float levelDb, std::uint32_t rampMs,
                              std::uint32_t fromGeneration = 0,
                              std::uint32_t toGeneration = 0);

    // Bus insert-effect chain apply methods (audio thread, at block start).
    // Pure mirrors of the voice-pool effect apply path: install an already-built
    // pooled instance (retiring whatever the slot held), clear/retire one,
    // update a live parameter, or toggle a slot's bypass (one-block crossfade,
    // with the empty-slot settle-at-apply rule). A bus index past the
    // audio-visible count — never reached in practice, since a bus's effect
    // command drains after its create swap — retires any handed-in instance
    // rather than leaking it.
    void ApplySetBusEffect(std::uint32_t busIndex, std::uint32_t slot,
                           std::uint32_t effectType, std::uint32_t poolIndex,
                           std::uint64_t poolGeneration,
                           std::uint32_t busGeneration = 0);
    void ApplyClearBusEffect(std::uint32_t busIndex, std::uint32_t slot,
                             std::uint32_t busGeneration = 0);
    void ApplySetBusEffectParam(std::uint32_t busIndex, std::uint32_t slot,
                                std::uint32_t paramIndex, float value,
                                std::uint32_t effectType,
                                const BiquadCoeffs& coeffs,
                                std::uint32_t tailFrames,
                                std::uint32_t transitionFrames = 0,
                                std::uint32_t busGeneration = 0);
    void ApplySetBusCompressorParameters(
        std::uint32_t busIndex, std::uint32_t slot,
        std::uint32_t effectType,
        const CompressorPreparedParameters& prepared,
        std::uint32_t transitionFrames,
        std::uint32_t busGeneration = 0);
    void ApplySetBusEffectBypass(std::uint32_t busIndex, std::uint32_t slot,
                                 std::uint32_t bypass,
                                 std::uint32_t action = kEffectActionBypass,
                                 std::uint32_t expectedEffectType =
                                     kEffectTypeNone,
                                 std::uint32_t busGeneration = 0);
    // Repositions the chain's fader split: fx slots [0, faderAfterSlot) run
    // pre-fader (before BusVolume), the rest post-fader. Applied at the block
    // boundary as a plain swap of the split index (see the .cpp for the click
    // discipline). Audio thread.
    void ApplySetBusFaderPoint(std::uint32_t busIndex,
                               std::uint32_t faderAfterSlot,
                               std::uint32_t busGeneration = 0);
    void ApplySetBusFilter(std::uint32_t busIndex, std::uint32_t filterKind,
                           bool enable, float cutoffHz, float q,
                           const BiquadCoeffs& coeffs,
                           std::uint32_t tailFrames,
                           std::uint32_t transitionFrames, bool outputOnly,
                           std::uint32_t busGeneration = 0);

    // Applies every bus's gain (advancing active ramps by one block), folds
    // tree children into parents, and scatters send edges — all in the
    // installed plan's feeds-first order (descending index before the first
    // plan). `busPlanes` holds BusCount() * mixChannels planes laid out
    // bus-major; on return the master planes (indices 0..mixChannels-1) hold
    // the final mix at the mix layout. A non-master bus configured with a
    // narrower output layout folds through its own matrix and name-places the
    // result back into the mix-width planes before its parent sum and sends;
    // the master->device fold is applied downstream by the mixer at the device
    // boundary. Audio thread.
    void MixDown(float* const* busPlanes, std::uint32_t blockFrames,
                 std::uint64_t blockEpoch, std::uint64_t sourceFrameBegin = 0);

    // The mix-layout channel count every bus renders at (resolved config).
    std::uint32_t MixChannels() const { return m_mixChannels; }
    std::uint64_t InvalidVolumeApplyCount() const {
        return m_invalidVolumeApplies;
    }

    // A bus's live downmix matrix (the dst x src table, coeffs[r*8+c] = gain
    // src c -> dst r), or nullptr out of range. The mixer reads the master's
    // (index 0) for the master->device output fold; installed by
    // ApplyTopologyPlan, seeded to the Appendix A default at construction.
    // Audio thread.
    const float* BusDownmixCoeffs(std::uint32_t busIndex) const {
        return busIndex < m_audioCount ? m_buses[busIndex].downmixCoeffs
                                       : nullptr;
    }
    // A bus's configured output layout (VnChannelLayout); for the master this
    // is the device layout. Audio thread.
    std::uint32_t BusOutputLayout(std::uint32_t busIndex) const {
        return busIndex < m_audioCount ? m_buses[busIndex].outputLayout : 0;
    }

    // OutputPeak is the existing Post Effects bus meter: the peak absolute
    // sample after post-fader inserts and any per-bus output fold.
    // The master's value (index 0) reflects the final mix, because every
    // feeder folds in before the master is processed. Reads 0 for an
    // out-of-range index. Compiled into ALL builds (section 11.7: the scan
    // feeds the meter Variable transport, so it does not ride the stage-tap
    // gate that still covers the per-voice taps). Audio thread writes
    // during MixDown; the mixer copies it into the snapshot payload and the
    // meter block ring in the same block.
    float OutputPeak(std::uint32_t busIndex) const {
        return busIndex < m_audioCount ? m_buses[busIndex].outputPeak : 0.0f;
    }
    float OutputChannelPeak(std::uint32_t busIndex, std::uint32_t channel) const {
        return busIndex < m_audioCount && channel < m_mixChannels
            ? m_buses[busIndex].outputChannelPeaks[channel] : 0.0f;
    }
    float PreFaderPeak(std::uint32_t busIndex) const {
        return busIndex < m_audioCount ? m_buses[busIndex].preFaderPeak
                                       : 0.0f;
    }
    float PostFaderPeak(std::uint32_t busIndex) const {
        return busIndex < m_audioCount ? m_buses[busIndex].postFaderPeak
                                       : 0.0f;
    }

    // Audio-thread render state of one bus (fields below). Exposed on this
    // internal header (never the C ABI) so tests can inject synthetic chain
    // effects to prove chain order / bypass / fader behavior — the voice-chain
    // golden technique — and so future host diagnostics can read chain state.
    struct Bus {
        VnBusId id{};
        std::uint32_t generation = 0;
        std::uint32_t parentIndex = 0;
        OutputGain gain;
        OutputGain outputGain;
        // First ApplyVolume snaps even if rampMs != 0 (no fictitious prior
        // value). Kept on Bus so OutputGain does not grow a started field.
        bool gainStarted = false;
        float preFaderPeak = 0.0f;
        float postFaderPeak = 0.0f;
        // Post-effects output tap for the last mixed block (see OutputPeak).
        float outputPeak = 0.0f;
        float outputChannelPeaks[kMaxChannels] = {};

        // Insert-effect chain — pure reuse of the voice chain mechanics: a
        // fixed-capacity slot array processed in ascending order, m_fxSlots
        // active. An empty slot (process == nullptr) is skipped at zero cost.
        // Per slot, fxBypassMix is the settled dry/wet blend (0 = wet/active,
        // 1 = bypassed/dry); a toggle sets fxBypassTarget and arms
        // fxBypassRamping for a one-block crossfade. fxSlotEffectRef names the
        // pooled instance so the audio thread can retire it on replace/clear
        // (the logic thread returns it to its pool).
        InsertEffect fxSlots[kMaxFxSlotsPerBus];
        float fxBypassMix[kMaxFxSlotsPerBus] = {};
        float fxBypassTarget[kMaxFxSlotsPerBus] = {};
        bool fxBypassRamping[kMaxFxSlotsPerBus] = {};
        EffectInstanceRef fxSlotEffectRef[kMaxFxSlotsPerBus];

        // Chain split around BusVolume: fx slots [0, faderAfterSlot) run
        // pre-fader (before the gain), the rest post-fader. Set to m_fxSlots
        // (all pre-fader) when the bus is created, mirroring the frozen voice
        // cascade; with an empty chain this is bit-identical to the prior
        // gain-only fold. Repositioned by ApplySetBusFaderPoint.
        std::uint32_t faderAfterSlot = 0;

        // Per-bus output fold (multichannel amendment). outputLayout is the
        // layout this bus presents its output at: equal to the mix layout means
        // no fold (prior behavior, bit-identical at stereo); a narrower
        // concrete layout folds the bus's mix-width output through
        // downmixCoeffs at its output boundary, then name-places the result
        // back into the mix-width planes before the parent sum and sends. For
        // the master (index 0) outputLayout is the device layout and
        // downmixCoeffs is the master->device matrix, which the mixer applies
        // at the device boundary (never in the tree fold). downmixCoeffs is the
        // dst x src table, coeffs[r*8 + c] = gain from src channel c into dst
        // channel r. Installed by ApplyTopologyPlan; the constructor seeds the
        // master's default and CreateBus seeds a no-fold non-master bus.
        std::uint32_t outputLayout = 0;
        float downmixCoeffs[64] = {};

        // Preallocated tone sections. S2 runs busLpf/busHpf after Bus Volume
        // and before the post-fader tap. S3 runs outputLpf/outputHpf on the
        // Direct/output leg only. Default bypass; InitializeBusSlot zeros them.
        struct ToneSection {
            bool enabled = false;
            BiquadCoeffs coeffs;
            std::uint32_t tailFrames = 0;
            BiquadState state[kMaxChannels];
            BiquadTransitionState<kMaxChannels> transition;
            BiquadParameterGlide glide;
        };
        ToneSection busLpf;
        ToneSection busHpf;
        ToneSection outputLpf;
        ToneSection outputHpf;
    };

    // Render state of a bus by dense index, or nullptr out of range. Internal
    // (non-ABI) accessor mirroring VoicePool::Resolve; see Bus. Bounded by the
    // logic-visible count so a just-created bus is reachable before its first
    // pump.
    Bus* BusState(std::uint32_t busIndex);

private:
    void InitializeBusSlot(std::uint32_t busIndex, VnBusId id,
                           std::uint32_t parentIndex,
                           std::uint32_t generation) noexcept;
    void RetireBusSlotEffects(Bus& bus) noexcept;

    // One bus->bus send edge's audio-thread level state. Its array slot mirrors
    // the authoritative topology-plan membership slot; equal slot+target keeps
    // this level and ramp across ordinary republishes. Send still uses the
    // whole-block GainRamp plan; Bus Volume does not.
    struct SendEdge {
        std::uint32_t toIndex = kInvalidBusIndex; // invalid = empty level slot
        std::uint32_t toGeneration = 0;
        GainRamp gain;
        bool ramping = false;
        float rampStartGain = 1.0f;
        float rampEndGain = 1.0f;
        std::uint64_t rampBlocksTotal = 0;
        std::uint64_t rampBlocksDone = 0;
    };

    // Renders one bus's §7.3 pipeline over its accumulated planes for this
    // block: pre-fader fx slots [0, faderAfterSlot) -> pre-fader tap ->
    // BusVolume gain (frame-accurate OutputGain over mix frames) ->
    // post-fader tap -> post-fader fx slots [faderAfterSlot, m_fxSlots) ->
    // post-effects output tap. With an empty chain and the default all-pre-
    // fader split this reduces exactly to the prior gain-only finalize. Shared
    // by the fold so the pre-plan and plan-order paths finalize identically.
    void FinalizeBus(Bus& bus, float* const* planes,
                     std::uint32_t blockFrames,
                     std::uint64_t blockEpoch,
                     std::uint64_t sourceFrameBegin);

    // Folds a non-master bus's finalized mix-width output to its narrower
    // output layout through its own matrix, then name-places the folded
    // channels back into the mix-width planes (matching role -> matching mix
    // channel, silence elsewhere) so the parent sum and sends consume the
    // folded output. Re-takes the output tap post-fold (the §6.4 bus meter).
    // Only called for a bus whose outputLayout is a concrete layout narrower
    // than the mix. Audio thread.
    void FoldBusOutput(Bus& bus, float* const* planes,
                       std::uint32_t blockFrames);

    // Multiply-accumulates a source bus's finalized planes, scaled by an edge's
    // ramped level, into a target bus's planes. Advances the edge ramp one
    // block (same block-boundary linear discipline as bus gain).
    void ScatterSend(const float* const* src, float* const* dst,
                     std::uint32_t blockFrames, SendEdge& edge);

    // Row of send-level slots for a from-bus. Returns nullptr for the master's
    // row only if out of range (never in practice).
    SendEdge* SendRow(std::uint32_t fromBus);
    // Finds the level slot addressing `toBus` in a from-bus's row, or nullptr.
    SendEdge* FindEdge(std::uint32_t fromBus, std::uint32_t toBus);
    // Applies a send ramp request to a GainRamp + its multi-block plan
    // fields (first-set/zero snaps, else a whole-block linear ramp).
    // Bus Volume no longer uses this; SendEdge still does.
    void ArmRamp(GainRamp& gain, bool& ramping, float& rampStartGain,
                 float& rampEndGain, std::uint64_t& rampBlocksTotal,
                 std::uint64_t& rampBlocksDone, float targetLinear,
                 std::uint32_t rampMs);

    TopologyPlanPool& m_plans;
    // Shared effect instance pools (same bank voices use). Bus chains acquire
    // logic-side and retire audio-side through it — pure reuse of the handoff.
    EffectPoolBank& m_effectBank;
    DynamicRouteSystem& m_dynamicRoutes;
    std::uint32_t m_currentPlanIndex = kInvalidTopologyPlanIndex;

    // Resized to capacity at Init. [0, m_count) is logic-visible;
    // [m_count, m_materializedCount) may hold one hidden prepared append batch.
    std::vector<Bus> m_buses;
    // Logic-owned tree-parent shadow and slot liveness, indexed by dense bus
    // index and sized to capacity at init. CreateBus (and a live reparent)
    // write these; the planner reads them through ParentOf; the plan carries
    // them to the audio thread, where ApplyTopologyPlan installs the parents
    // into Bus::parentIndex at the block boundary.
    std::vector<std::uint32_t> m_logicParent;
    std::vector<std::uint8_t> m_logicAlive;
    std::vector<VnBusId> m_logicId;
    std::vector<std::uint32_t> m_logicGeneration;
    std::vector<std::uint8_t> m_logicQuarantined;
    std::vector<std::uint64_t> m_logicRetireRevision;
    // Audio-visible bus count (see AudioBusCount). Distinct from m_count: the
    // logic thread appends a bus (m_count++) and only the plan swap advances
    // this, so the audio thread never observes half-published structure. With
    // the parents/alive amendment this is the dense SLOT bound the audio
    // thread may index (plane sets, per-bus commands); the live mix walk
    // covers the installed plan's orderCount, at or below it.
    std::uint32_t m_audioCount = 0;
    std::uint64_t m_invalidVolumeApplies = 0;
    // Send-level table, row-major by from-bus: m_sends[from*m_sendSlots + s].
    // Audio-thread state; sized to capacity at init and never resized.
    std::vector<SendEdge> m_sends;
    std::uint32_t m_sendSlots = 0;
    std::uint32_t m_count = 0;
    std::uint32_t m_activeCount = 0;
    std::uint32_t m_quarantinedCount = 0;
    std::uint64_t m_slotReuseCount = 0;
    std::atomic<std::uint64_t> m_installedGraphRevision{0};
    // Physical append bound. It may temporarily lead m_count while a prepared
    // Pack batch owns hidden, fully initialized Bus slots.
    std::uint32_t m_materializedCount = 0;
    std::uint32_t m_capacity = 0;
    std::uint32_t m_blockFrames = 0;
    std::uint32_t m_sampleRate = 0;
    // Mix layout every bus renders at, and the device layout the master's fold
    // targets (resolved config). Stereo at the default, at which no bus folds
    // and the render path is byte-identical to M4.
    std::uint32_t m_mixChannels = 2;
    std::uint32_t m_mixLayout = 0;
    std::uint32_t m_deviceLayout = 0;

    // Active insert-effect chain length per bus (resolved fxSlotsPerBus, at or
    // below kMaxFxSlotsPerBus). Every bus shares it; the default fader split is
    // this value (all pre-fader).
    std::uint32_t m_fxSlots = 0;
    // Audio-thread dry scratch for the fx bypass crossfade (and reused by the
    // per-bus output fold): m_mixChannels planes of m_blockFrames, reused per
    // bus (buses finalize one at a time in MixDown). Preallocated at
    // construction so the audio thread never allocates. m_fxDry holds the
    // per-channel base pointers into the storage.
    std::vector<float> m_fxDryStorage;
    std::vector<float*> m_fxDry;
    // Direct/output-filter scratch: copy of finalized planes after Output
    // Volume, then Output LPF/HPF, then MixAdd into the parent. Send still
    // reads the unfiltered finalized planes. Preallocated like m_fxDry.
    std::vector<float> m_outputScratchStorage;
    std::vector<float*> m_outputScratch;
    const BiquadTrigTable* m_biquadTrig = nullptr;
};

} // namespace harken

#endif // HARKEN_RENDER_BUS_TREE_H
