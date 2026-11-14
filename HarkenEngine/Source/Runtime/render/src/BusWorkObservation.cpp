#include "harken/render/BusTree.h"
#include "harken/render/BusActivityPlan.h"
#include "harken/core/SnapshotBuffer.h"
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
    std::uint32_t frames, bool rendered, std::uint64_t commandFrontier,
    std::uint64_t initialSourceFrontier) noexcept {
    const auto detectorPlan = m_dynamicRoutes.InstalledPlan();
    for (std::uint32_t i = 0; rendered && i < detectorPlan.rowCount; ++i) {
        const auto& route = detectorPlan.rows[i];
        if (HasWorkRow(route.targetBusIndex) && m_dynamicRoutes.HasPendingVoiceInput(route))
            WorkState(route.targetBusIndex).activityDetector = true;
    }
    const auto publish = [&](std::uint32_t index) {
        auto& row = m_workRows[m_workSlots[index].slot];
        auto& fact = row.observed;
        const auto& bus = row.bus;
        const bool changed = fact.stateRevision != bus.activityRevision ||
            fact.graphRevision != InstalledGraphRevision();
        fact.bus = row.host;
        fact.work = m_workSlots[index];
        fact.blockSequence = blockSequence;
        fact.activityPublication = AudioActivityPublication();
        fact.peakPreFader = rendered ? bus.preFaderPeak : 0;
        fact.peakPostFader = rendered ? bus.postFaderPeak : 0;
        fact.peakPostEffects = rendered ? bus.outputPeak : 0;
        fact.frameEnd = frameEnd;
        fact.graphRevision = InstalledGraphRevision();
        fact.stateRevision = bus.activityRevision;
        fact.commandFrontier = commandFrontier;
        fact.initialSourceFrontier = initialSourceFrontier;
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


std::uint64_t BusTree::AudioActivityPublication() const noexcept {
    return m_activeActivity ? m_activeActivity->publication : 0;
}

const BusActivityPlan* BusTree::ObservedActivity(std::uint64_t publication) const noexcept {
    if (publication == 0) return nullptr;
    for (const auto& plan : m_activities)
        if (plan->publication == publication) return plan.get();
    return nullptr;
}

void BusTree::CaptureObservedBusState(BusSlotRef bus, const SnapshotPayload& receipt,
    const BusActivityPlan* activity, VnBusState& out) noexcept {
    if (!bus.Valid() || receipt.blocksRendered == 0) return;
    WorkRef work;
    if (receipt.busActivityPublication == 0) {
        // The bootstrap Master is never asleep; published fixed-size peaks
        // suffice until the first immutable activity plan is adopted.
        if (bus.index != 0) return;
        out.blockSequence = receipt.blocksRendered;
        out.flags = VN_BUS_STATE_VALID | VN_BUS_STATE_WORKING |
            (receipt.busBlockRendered ? VN_BUS_STATE_RENDERED : 0);
        out.peakPreFader = receipt.busPeaksPreFader[0];
        out.peakPostFader = receipt.busPeaksPostFader[0];
        out.peakPostEffects = receipt.busPeaks[0];
        return;
    }
    if (activity == nullptr || activity->publication != receipt.busActivityPublication ||
        bus.index >= activity->directory.size()) return;
    const auto& entry = activity->directory[bus.index];
    if (entry.generation != bus.generation) return;
    if (entry.plane == UINT32_MAX) {
        out.blockSequence = receipt.blocksRendered;
        out.flags = VN_BUS_STATE_VALID;
        return;
    }
    if (entry.plane >= activity->working.size()) return;
    work = activity->working[entry.plane].work;
    WorkObservation fact;
    if (!CaptureWorkObservation(bus, work, fact) ||
        fact.activityPublication != activity->publication ||
        fact.blockSequence < receipt.blocksRendered) return;
    out.blockSequence = fact.blockSequence;
    out.flags = VN_BUS_STATE_VALID | VN_BUS_STATE_WORKING |
        (fact.rendered ? VN_BUS_STATE_RENDERED : 0);
    out.peakPreFader = fact.peakPreFader;
    out.peakPostFader = fact.peakPostFader;
    out.peakPostEffects = fact.peakPostEffects;
}

} // namespace harken
