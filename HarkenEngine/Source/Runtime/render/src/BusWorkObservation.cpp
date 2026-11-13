#include "harken/render/BusTree.h"
#include "harken/render/BuiltinEffectDormancy.h"

namespace harken {

bool BusTree::CaptureWorkObservation(BusSlotRef bus, WorkRef work, WorkObservation& out) noexcept {
    auto* row = ResolveWorkRow(work);
    if (row == nullptr || row->host != bus ||
        row->phase.load(std::memory_order_acquire) != WorkPhase::Published) return false;
    WorkObservation next;
    if (!row->observation.Capture(next) || next.bus != bus || next.work != work || next.blockSequence == 0)
        return false;
    out = next;
    return true;
}

void BusTree::BeginWorkObservations() noexcept {
    const auto reset = [&](std::uint32_t index) {
        auto& row = m_workRows[m_workSlots[index].slot];
        row.observed.physicalVoice = false;
        row.bus.activityInput = row.bus.activityDetector = false;
    };
    const auto order = WorkingOrder();
    if (!order.empty()) for (auto index : order) reset(index);
    else for (std::uint32_t i = 0; i < m_audioCount; ++i) if (HasWorkRow(i)) reset(i);
}

void BusTree::NoteAudioBusUser(BusSlotRef bus) noexcept {
    if (!HasWorkRow(bus.index) || !AudioRefMatches(bus)) return;
    m_workRows[m_workSlots[bus.index].slot].observed.physicalVoice = true;
}

bool BusTree::LocalRecoveryReady(const Bus& bus) const noexcept {
    if (bus.activityRevision == UINT64_MAX || bus.fxChainTransition != nullptr ||
        bus.gain.remainingFrames != 0 || bus.outputGain.remainingFrames != 0 ||
        !std::isfinite(bus.gain.current) || !std::isfinite(bus.outputGain.current)) return false;
    DormantToneSection tone;
    for (const auto* section : {&bus.busLpf, &bus.busHpf, &bus.outputLpf, &bus.outputHpf})
        if (!CaptureDormantToneSection(*section, m_mixChannels, tone)) return false;
    const auto plan = m_plans.View(m_currentPlanIndex);
    const auto sends = plan.valid ? plan.Sends(bus.busIndex) : TopologySendRange{};
    for (std::uint32_t s = 0; s < sends.count; ++s)
        if (plan.sendStates[sends.first + s].ramping) return false;
    for (std::uint32_t slot = 0; slot < m_fxSlots; ++slot) {
        if (bus.fxBypassRamping[slot]) return false;
        if (bus.fxSlots[slot].process == nullptr) continue;
        BuiltinEffectDormantState effect;
        ToneDormantState extra;
        if (!CaptureBuiltinEffectDormantState(bus.fxSlotEffectRef[slot].type,
                bus.fxSlots[slot].state, m_mixChannels, effect, &extra)) return false;
    }
    return true;
}

void BusTree::PublishWorkObservations(std::uint64_t blockSequence, std::uint64_t frameEnd,
    std::uint32_t frames, bool rendered) noexcept {
    const auto publish = [&](std::uint32_t index) {
        auto& row = m_workRows[m_workSlots[index].slot];
        auto& fact = row.observed;
        const auto& bus = row.bus;
        const bool changed = fact.stateRevision != bus.activityRevision ||
            fact.graphRevision != InstalledGraphRevision();
        fact.bus = row.host;
        fact.work = m_workSlots[index];
        fact.blockSequence = blockSequence;
        fact.frameEnd = frameEnd;
        fact.graphRevision = InstalledGraphRevision();
        fact.stateRevision = bus.activityRevision;
        fact.rendered = rendered;
        fact.inputActive = bus.activityInput;
        fact.detectorActive = bus.activityDetector;
        if (!rendered || changed || fact.inputActive || fact.detectorActive || fact.physicalVoice) {
            fact.quietFrames = 0;
            fact.localRecoveryReady = false;
            row.nextRecoveryProbeFrame = 0;
            row.recoveryProbeDelay = 0;
        }
        if (rendered) {
            if (changed || fact.inputActive) {
                std::uint32_t tail = 0;
                for (std::uint32_t s = 0; s < m_fxSlots; ++s) {
                    const auto& fx = bus.fxSlots[s];
                    if (fx.process != nullptr && fx.tailFrames != nullptr)
                        tail = SaturatingTailAdd(tail, fx.tailFrames(fx.state));
                }
                const auto filterTail = [](const Bus::ToneSection& section) {
                    return BiquadSectionTailFrames(section.transition, section.glide,
                        section.enabled, section.tailFrames);
                };
                tail = SaturatingTailAdd(tail, filterTail(bus.busLpf));
                fact.sendTailFrames = SaturatingTailAdd(tail, filterTail(bus.busHpf));
                tail = SaturatingTailAdd(fact.sendTailFrames, filterTail(bus.outputLpf));
                fact.outputTailFrames = SaturatingTailAdd(tail, filterTail(bus.outputHpf));
            } else {
                const auto advance = [frames](std::uint32_t& tail) {
                    if (tail != kInfiniteTail) tail = tail > frames ? tail - frames : 0;
                };
                advance(fact.sendTailFrames);
                advance(fact.outputTailFrames);
            }
            const bool quiet = !fact.inputActive && !fact.detectorActive && !fact.physicalVoice &&
                fact.outputTailFrames == 0 && fact.sendTailFrames == 0;
            if (!quiet) {
                fact.quietFrames = 0;
                fact.localRecoveryReady = false;
            } else {
                const bool recoveredBeforeBlock = fact.localRecoveryReady;
                if (!fact.localRecoveryReady && frameEnd >= row.nextRecoveryProbeFrame) {
                    fact.localRecoveryReady = LocalRecoveryReady(bus);
                    fact.recoveryProbeFrame = frameEnd;
                    // Delay's finite tail is a bound, not proof of a zero line.
                    // Failed exact probes back off rather than rescanning its
                    // potentially large backing on every audio block.
                    row.recoveryProbeDelay = row.recoveryProbeDelay == 0
                        ? std::max(frames, m_sampleRate / 10)
                        : static_cast<std::uint32_t>(std::min<std::uint64_t>(m_sampleRate,
                            static_cast<std::uint64_t>(row.recoveryProbeDelay) * 2));
                    row.nextRecoveryProbeFrame = row.recoveryProbeDelay > UINT64_MAX - frameEnd
                        ? UINT64_MAX : frameEnd + row.recoveryProbeDelay;
                }
                // The successful probe proves the state at block end. Only
                // subsequent complete quiet blocks count toward the hold.
                if (fact.localRecoveryReady && recoveredBeforeBlock)
                    fact.quietFrames = frames > UINT64_MAX - fact.quietFrames ? UINT64_MAX : fact.quietFrames + frames;
                else fact.quietFrames = 0;
            }
        }
        row.observation.BeginWrite() = fact;
        row.observation.EndWrite();
    };
    const auto order = WorkingOrder();
    if (!order.empty()) for (auto index : order) publish(index);
    else for (std::uint32_t i = 0; i < m_audioCount; ++i) if (HasWorkRow(i)) publish(i);
}

} // namespace harken
