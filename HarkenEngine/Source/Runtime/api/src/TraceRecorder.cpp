#include "TraceRecorder.h"

#include "Engine.h"
#include "PlayableRuntime.h"
#include "TraceCodec.h"
#include "harken/core/CpuClock.h"
#include "harken/core/Utf8Path.h"
#include "harken/media/CurrentWorldBodyVintage.h"
#include "harken/media/PackFormat.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <limits>
#include <cstring>
#include <filesystem>
#include <new>

namespace harken {

namespace {

TraceRecorderFault g_traceRecorderFault = TraceRecorderFault::None;

constexpr std::size_t kFlushThresholdBytes = 256 * 1024;

// Offsets inside a record's header, for the commit-time patches.
constexpr std::size_t kRecordPayloadBytesOffset = 0;
constexpr std::size_t kRecordFlagsOffset = 6;

void StoreLe16(std::uint8_t* out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value);
    out[1] = static_cast<std::uint8_t>(value >> 8);
}

void StoreLe32(std::uint8_t* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value);
    out[1] = static_cast<std::uint8_t>(value >> 8);
    out[2] = static_cast<std::uint8_t>(value >> 16);
    out[3] = static_cast<std::uint8_t>(value >> 24);
}

void StoreLe64(std::uint8_t* out, std::uint64_t value) noexcept {
    StoreLe32(out, static_cast<std::uint32_t>(value));
    StoreLe32(out + 4, static_cast<std::uint32_t>(value >> 32));
}

std::uint16_t LoadLe16(const std::uint8_t* in) noexcept {
    return static_cast<std::uint16_t>(in[0]) |
           static_cast<std::uint16_t>(in[1] << 8);
}

} // namespace

void SetTraceRecorderFaultForTests(TraceRecorderFault fault) noexcept {
    g_traceRecorderFault = fault;
}

TraceRecorderFault TraceRecorderFaultForTests() noexcept {
    return g_traceRecorderFault;
}

bool HashFileContents(const char* utf8Path, std::uint64_t* outHash) {
    // A nullptr or ill-formed-UTF-8 path is content that cannot be hashed:
    // the caller records the file reference as unverifiable and carries on.
    // This is not a journal write failure and must not touch traceFailed.
    std::filesystem::path path;
    if (!TryMakeUtf8Path(utf8Path, &path)) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    char chunk[64 * 1024];
    std::uint64_t hash = kFnv1a64OffsetBasis;
    while (in) {
        in.read(chunk, sizeof chunk);
        const std::streamsize got = in.gcount();
        if (got > 0) {
            hash = Fnv1a64(chunk, static_cast<std::size_t>(got), hash);
        }
    }
    if (in.bad()) {
        return false; // a read error, as opposed to ordinary EOF
    }
    *outHash = hash;
    return true;
}

// --- TraceRecorder ---

TraceRecorder::TraceRecorder(Engine& engine) : m_engine(engine) {}

TraceRecorder::~TraceRecorder() noexcept = default;

VnResult TraceRecorder::Start(const char* utf8Path) {
    // An ill-formed-UTF-8 path refuses the open before any state is touched:
    // no recorder installs and the sticky traceFailed flag is left alone, so
    // a rejected open reads as a refusal, never a mid-session write failure.
    std::filesystem::path path;
    if (!TryMakeUtf8Path(utf8Path, &path)) {
        return VN_BAD_TRACE_FILE;
    }
    m_appendFailed = false;
    m_ticksPerSecond = CpuClockTicksPerSecond();
    m_startTicks = CpuClockTicks();

    m_file.open(path, std::ios::binary | std::ios::trunc | std::ios::out);
    if (!m_file) {
        return VN_BAD_TRACE_FILE;
    }

    // The base block count: blocks published before the journal began.
    // Replay pumps them ahead of the first record so absolute block
    // arithmetic reproduces; they are provably silent, because no stateful
    // call has happened yet (the start gate) and a silent engine renders
    // exact zeros.
    SnapshotPayload payload;
    std::uint64_t baseBlocks = m_engine.lastSnapshotPayload.blocksRendered;
    if (m_engine.mixer.Snapshots().Capture(payload)) {
        baseBlocks = payload.blocksRendered;
    }
    m_blocksJournaled = baseBlocks;

    std::vector<std::uint8_t> configWire;
    if (!EncodeTraceConfig(m_engine.traceOrigin, configWire)) {
        m_file.close();
        m_buffer.clear();
        return VN_BAD_TRACE_FILE;
    }
    const std::uint32_t configVintage =
        ConfigVintageFromNativeSize(
            m_engine.traceOrigin.requested.structSize);

    // Fixed header fields, serialized individually (container discipline).
    m_buffer.reserve(kFlushThresholdBytes + 4096);
    Append(kTraceMagic, sizeof kTraceMagic);
    const std::uint32_t version = kTraceFormatVersion;
    AppendU32(version);
    const std::uint32_t headerBytes = static_cast<std::uint32_t>(
        kTraceFixedHeaderBytes + configWire.size());
    AppendU32(headerBytes);
    AppendU32(configVintage);
    AppendU64(baseBlocks);
    AppendU64(m_ticksPerSecond);
    Append(configWire.data(), configWire.size());

    if (m_appendFailed) {
        m_file.close();
        m_buffer.clear();
        return VN_BAD_TRACE_FILE;
    }

    m_active = true;
    if (m_engine.variables.CurrentWorldIdentity() != nullptr) {
        const ContainerWorldInstallViewV1 origin =
            m_engine.variables.CurrentWorldInstallView();
        WorldInstallResult captured{};
        captured.status = WorldInstallStatus::Ok;
        captured.priorGeneration = 0;
        captured.resultGeneration =
            m_engine.variables.CurrentWorldGeneration();
        RecordCurrentWorldInstall(origin, captured);
    }
    struct OriginProbe {
        MeterProbeIdentity identity{};
        std::uint32_t attackMs = 0;
        std::uint32_t releaseMs = 0;
    };
    std::vector<OriginProbe> probes;
    m_engine.meterDetectors.ForEachInstalled(
        [&](const MeterProbeIdentity& probe, std::uint32_t attackMs,
            std::uint32_t releaseMs) {
            probes.push_back({probe, attackMs, releaseMs});
        });
    std::sort(probes.begin(), probes.end(),
              [](const OriginProbe& a, const OriginProbe& b) {
                  return a.identity.probeStableId < b.identity.probeStableId;
              });
    for (const OriginProbe& probe : probes) {
        RecordMeterProbeInstall(
            probe.identity.probeStableId,
            probe.identity.sourceBusStableId, probe.identity.tap,
            probe.attackMs, probe.releaseMs,
            probe.identity.probeGeneration, VN_OK);
    }

    std::vector<MeterProbeBindingDefinition> bindings;
    m_engine.bindings.ForEachProbeDefinition(
        [&](const BindingTable::Definition& def) {
            MeterProbeBindingDefinition definition{};
            definition.bindingId = def.id;
            definition.probeStableId = def.probeStableId;
            definition.probeGeneration = def.probeGeneration;
            definition.targetScope = def.scopeKind;
            definition.targetId = def.targetId;
            definition.transitionMs = def.transitionMs;
            definition.curve = def.curve;
            bindings.push_back(definition);
        });
    std::sort(bindings.begin(), bindings.end(),
              [](const MeterProbeBindingDefinition& a,
                 const MeterProbeBindingDefinition& b) {
                  return a.bindingId < b.bindingId;
              });
    for (const MeterProbeBindingDefinition& definition : bindings) {
        RecordMeterProbeBindingDefine(
            definition.bindingId, definition.probeStableId,
            definition.probeGeneration, definition.targetScope,
            definition.targetId, definition.curve.points,
            definition.curve.handles, definition.curve.pointCount,
            definition.transitionMs, VN_OK);
    }

    std::vector<MeterProbeAttachment> attachments;
    m_engine.bindings.ForEachProbeAttachment(
        [&](const BindingTable::Attachment& row,
            const BindingTable::Definition&) {
            MeterProbeAttachment attachment{};
            attachment.bindingId = row.bindingId;
            attachment.targetScope = row.scopeKind;
            attachment.targetStableId = row.objectId;
            attachment.targetInstanceHandle = row.instanceHandle;
            attachment.targetGeneration = row.targetGeneration;
            attachments.push_back(attachment);
        });
    std::sort(attachments.begin(), attachments.end(),
              [](const MeterProbeAttachment& a,
                 const MeterProbeAttachment& b) {
                  if (a.bindingId != b.bindingId) {
                      return a.bindingId < b.bindingId;
                  }
                  if (a.targetScope != b.targetScope) {
                      return a.targetScope < b.targetScope;
                  }
                  if (a.targetStableId != b.targetStableId) {
                      return a.targetStableId < b.targetStableId;
                  }
                  if (a.targetInstanceHandle != b.targetInstanceHandle) {
                      return a.targetInstanceHandle < b.targetInstanceHandle;
                  }
                  return a.targetGeneration < b.targetGeneration;
              });
    for (const MeterProbeAttachment& attachment : attachments) {
        RecordMeterProbeBindingAttachment(
            kTraceRecordMeterProbeBindingAttach, attachment, VN_OK);
    }
    FlushBuffer();
    if (!m_file.is_open() || m_file.fail()) {
        // A header that cannot be written is an unusable trace file, the
        // same refusal class as a failed open; nothing was captured yet, so
        // the sticky mid-session flag stays untouched.
        m_file.close();
        m_active = false;
        return VN_BAD_TRACE_FILE;
    }

    return VN_OK;
}

void TraceRecorder::RecordSessionTerminate() noexcept {
    if (!m_active) return;
    SyncBlocks();
    const std::size_t start = BeginRecord(kTraceRecordSessionTerminate);
    CommitRecord(start);
}

void TraceRecorder::Stop() noexcept {
    if (!m_active) {
        return;
    }
    SyncBlocks();
    if (m_active) {
        const std::size_t start = BeginRecord(kTraceRecordEnd);
        if (m_sawCallbackContextCall) {
            // Aggregate marker: analysis tooling can classify the journal
            // from its end record without walking every call.
            SetFlag(start, kTraceFlagCallbackContext);
        }
        CommitRecord(start);
    }
    if (m_active) {
        FlushBuffer();
    }
    if (m_active) {
        m_file.flush();
        if (m_file.fail()) {
            FailTrace();
        } else {
            m_file.close();
            m_active = false;
        }
    }
}

void TraceRecorder::UpdateBegin() noexcept {
    if (!m_active) {
        return;
    }
    SyncBlocks();
    EmitBarrier(kTraceRecordUpdateBegin, m_updateIndex);
    ++m_updateDepth;
}

bool TraceRecorder::PrepareProjectGenerationTraceV1(
    Engine& engine, const TopologyPlanView& plan,
    const ContainerWorldInstallViewV1& world,
    const WorldInstallResult& result,
    const RuntimeProjectGenerationTokenV1& token,
    std::uint64_t publishedBusFingerprint,
    std::uint64_t compileTargetProfileFingerprint,
    std::uint32_t publicationVintage,
    PreparedProjectGenerationTraceV1* out) noexcept {
    if (out == nullptr || out->prepared_) return false;
    out->Reset();
    const auto& domain = engine.projectGenerationReservationDomain;
    if (domain == nullptr || !domain->live || domain->liveEngine != &engine) return false;
    out->domain_ = domain;
    out->domainEpoch_ = domain->epoch;
    out->traceEpoch_ = engine.traceSessionEpoch;
    out->recorder_ = engine.trace.get();
    auto* recorder = out->recorder_;
    if (recorder != nullptr) {
        out->active_ = recorder->m_active;
        out->ordinal_ = recorder->m_ordinal;
        out->bufferSize_ = recorder->m_buffer.size();
    }
    if (!out->active_) {
        out->prepared_ = true;
        return true;
    }
    if (!plan.valid || plan.slotCount == 0 || plan.slotsPerBus == 0 ||
        plan.busIds == nullptr || plan.parents == nullptr || plan.alive == nullptr ||
        plan.sendTargets == nullptr || !TokenIsPaired(token) ||
        result.status != WorldInstallStatus::Ok || recorder->m_appendFailed ||
        recorder->m_ordinal > UINT64_MAX - 2) {
        out->Reset();
        return false;
    }
    const std::uint64_t count = plan.slotCount;
    const std::uint64_t sends = count * plan.slotsPerBus;
    if (count > engine.config.maxBuses || plan.slotsPerBus > engine.config.busSendSlotsPerBus ||
        sends > (1u << 20)) {
        out->Reset();
        return false;
    }
    try {
        if (TraceRecorderFaultForTests() == TraceRecorderFault::Append) throw std::bad_alloc{};
        std::vector<std::uint8_t> body;
        if (TraceRecorderFaultForTests() == TraceRecorderFault::ProjectWorldEncode) throw std::bad_alloc{};
        if (!EncodeTraceCurrentWorldBody(world, body)) {
            out->Reset();
            return false;
        }
        const std::uint64_t topologyBytes = 16 + count * 32 + sends * 12;
        const std::uint64_t worldBytes = 32 + static_cast<std::uint64_t>(body.size()) + 52;
        const std::uint64_t totalBytes = topologyBytes + worldBytes + 2 * kTraceRecordHeaderBytes;
        if (topologyBytes > UINT32_MAX || worldBytes > UINT32_MAX ||
            totalBytes > out->bytes_.max_size() ||
            totalBytes > recorder->m_buffer.max_size() - recorder->m_buffer.size()) {
            out->Reset();
            return false;
        }
        out->bytes_.resize(static_cast<std::size_t>(totalBytes));
        auto* cursor = out->bytes_.data();
        const auto u16 = [&](std::uint16_t value) { StoreLe16(cursor, value); cursor += 2; };
        const auto u32 = [&](std::uint32_t value) { StoreLe32(cursor, value); cursor += 4; };
        const auto u64 = [&](std::uint64_t value) { StoreLe64(cursor, value); cursor += 8; };
        const auto header = [&](std::uint32_t bytes, std::uint16_t type, std::uint64_t ordinal) {
            u32(bytes);
            u16(type);
            u16(0);
            u64(ordinal);
            u64(recorder->NowMicros());
        };
        header(static_cast<std::uint32_t>(topologyBytes), kTraceRecordPreparedTopologyPlan, out->ordinal_);
        u32(kTracePreparedTopologyPlanVintage);
        u32(plan.slotCount);
        u32(plan.slotsPerBus);
        u32(0);
        for (std::uint32_t i = 0; i < plan.slotCount; ++i) {
            const auto idBytes = ObjectIdBytes(plan.busIds[i]);
            std::memcpy(cursor, idBytes.data(), idBytes.size());
            cursor += idBytes.size();
            u32(plan.generations != nullptr ? plan.generations[i] : 1u);
            u32(plan.parents[i]);
            u32(plan.alive[i]);
            u32(0);
        }
        for (std::uint64_t i = 0; i < sends; ++i) {
            u32(plan.sendTargets[i]);
            std::uint32_t bits = 0;
            const float level = plan.preparedSendLevels != nullptr ? plan.preparedSendLevels[i] : 0.0f;
            std::memcpy(&bits, &level, sizeof(bits));
            u32(bits);
            u32(plan.sendLevelApply != nullptr ? plan.sendLevelApply[i] : 0);
        }
        header(static_cast<std::uint32_t>(worldBytes), kTraceRecordCurrentWorldInstall, out->ordinal_ + 1);
        u32(kCurrentWorldShippingBodyVintage);
        u32(0);
        u32(static_cast<std::uint32_t>(result.status));
        u32(0);
        u64(result.priorGeneration);
        u64(result.resultGeneration);
        if (!body.empty()) std::memcpy(cursor, body.data(), body.size());
        cursor += body.size();
        u64(token.projectSessionId);
        u64(token.runtimeEpoch);
        u64(token.authoringRevision);
        u64(token.submissionSerial);
        u64(publishedBusFingerprint);
        u64(compileTargetProfileFingerprint);
        u32(publicationVintage);
        assert(cursor == out->bytes_.data() + out->bytes_.size());
        out->requiredEnd_ = out->bufferSize_ + out->bytes_.size();
        if (TraceRecorderFaultForTests() == TraceRecorderFault::ProjectBufferReserve) throw std::bad_alloc{};
        recorder->m_buffer.reserve(out->requiredEnd_);
        out->prepared_ = true;
        return true;
    } catch (...) {
        out->Reset();
        return false;
    }
}

bool TraceRecorder::CanCommitProjectGenerationTraceV1(
    const PreparedProjectGenerationTraceV1& token) noexcept {
    const auto domain = token.domain_.lock();
    if (!token.prepared_ || domain == nullptr || !domain->live ||
        domain->epoch != token.domainEpoch_ || domain->liveEngine == nullptr) return false;
    const auto& engine = *domain->liveEngine;
    if (engine.traceSessionEpoch != token.traceEpoch_ || engine.trace.get() != token.recorder_) return false;
    const auto* recorder = token.recorder_;
    if (recorder == nullptr) return !token.active_ && token.bytes_.empty();
    if (recorder->m_active != token.active_ || recorder->m_ordinal != token.ordinal_ ||
        recorder->m_buffer.size() != token.bufferSize_) return false;
    if (!token.active_) return token.bytes_.empty();
    return !recorder->m_appendFailed && token.ordinal_ <= UINT64_MAX - 2 &&
        !token.bytes_.empty() && token.requiredEnd_ >= token.bufferSize_ &&
        token.requiredEnd_ - token.bufferSize_ == token.bytes_.size() &&
        recorder->m_buffer.capacity() >= token.requiredEnd_;
}

void TraceRecorder::CommitProjectGenerationTraceV1(
    PreparedProjectGenerationTraceV1* token) noexcept {
    assert(token != nullptr && CanCommitProjectGenerationTraceV1(*token));
    if (token->active_) {
        auto& recorder = *token->recorder_;
        recorder.m_buffer.resize(token->requiredEnd_);
        std::memcpy(recorder.m_buffer.data() + token->bufferSize_, token->bytes_.data(), token->bytes_.size());
        recorder.m_ordinal = token->ordinal_ + 2;
    }
    token->Reset();
}

void TraceRecorder::RecordCurrentWorldInstall(
    const ContainerWorldInstallViewV1& install,
    const WorldInstallResult& result, std::uint32_t encoding,
    const RuntimeProjectGenerationTokenV1* pairedToken,
    std::uint64_t publishedBusFingerprint,
    std::uint64_t compileTargetProfileFingerprint,
    std::uint32_t publicationVintage) noexcept {
    if (!m_active) {
        return;
    }
    const std::size_t start = BeginRecord(kTraceRecordCurrentWorldInstall);
    AppendU32(kCurrentWorldShippingBodyVintage);
    AppendU32(encoding);
    AppendU32(static_cast<std::uint32_t>(result.status));
    AppendU32(0);
    AppendU64(result.priorGeneration);
    AppendU64(result.resultGeneration);
    std::vector<std::uint8_t> body;
    if (!EncodeTraceCurrentWorldBody(install, body)) {
        if (result.status == WorldInstallStatus::Ok) {
            FailTrace();
            return;
        }
        SetFlag(start, kTraceFlagArgsUnreplayable);
        CommitRecord(start);
        return;
    }
    if (encoding == 1) {
        SetFlag(start, kTraceFlagRawReadableAttempt);
    }
    Append(body.data(), body.size());
    if (pairedToken != nullptr && TokenIsPaired(*pairedToken)) {
        AppendU64(pairedToken->projectSessionId);
        AppendU64(pairedToken->runtimeEpoch);
        AppendU64(pairedToken->authoringRevision);
        AppendU64(pairedToken->submissionSerial);
        AppendU64(publishedBusFingerprint);
        AppendU64(compileTargetProfileFingerprint);
        AppendU32(publicationVintage);
    }
    CommitRecord(start);
}

void TraceRecorder::RecordProjectGenerationInstallAck(
    const ProjectGenerationInstallAckV1& ack) noexcept {
    if (!m_active) {
        return;
    }
    const std::size_t start = BeginRecord(kTraceRecordProjectGenerationInstallAck);
    AppendU32(ack.planIndex);
    AppendU32(ack.zero);
    AppendU64(ack.slotGeneration);
    AppendU64(ack.planSequence);
    AppendU64(ack.planFingerprint);
    AppendU64(ack.claimSetFingerprint);
    CommitRecord(start);
}

void TraceRecorder::RecordPreparedTopologyPlan(
    const TopologyPlanView& plan) noexcept {
    if (!m_active) {
        return;
    }
    if (!plan.valid || plan.slotCount == 0 || plan.slotsPerBus == 0 ||
        plan.busIds == nullptr || plan.parents == nullptr ||
        plan.alive == nullptr || plan.sendTargets == nullptr) {
        FailTrace();
        return;
    }
    const std::uint32_t n = plan.slotCount;
    const std::uint32_t slots = plan.slotsPerBus;
    const std::uint64_t sendCount =
        static_cast<std::uint64_t>(n) * slots;
    if (n > m_engine.config.maxBuses ||
        slots > m_engine.config.busSendSlotsPerBus ||
        sendCount > (1u << 20)) {
        FailTrace();
        return;
    }
    const std::size_t start = BeginRecord(kTraceRecordPreparedTopologyPlan);
    AppendU32(kTracePreparedTopologyPlanVintage);
    AppendU32(n);
    AppendU32(slots);
    AppendU32(0);
    for (std::uint32_t i = 0; i < n; ++i) {
        AppendObjectId(plan.busIds[i]);
        AppendU32(plan.generations != nullptr ? plan.generations[i] : 1u);
        AppendU32(plan.parents[i]);
        AppendU32(plan.alive[i]);
        AppendU32(0);
    }
    for (std::uint64_t i = 0; i < sendCount; ++i) {
        AppendU32(plan.sendTargets[i]);
        std::uint32_t bits = 0;
        const float level = plan.preparedSendLevels != nullptr
                                ? plan.preparedSendLevels[i]
                                : 0.0f;
        std::memcpy(&bits, &level, sizeof(bits));
        AppendU32(bits);
        AppendU32(plan.sendLevelApply != nullptr ? plan.sendLevelApply[i] : 0);
    }
    CommitRecord(start);
}

void TraceRecorder::RecordMeterProbeInstall(
    VnObjectId probeStableId, VnBusId busId, std::uint32_t tap,
    std::uint32_t attackMs, std::uint32_t releaseMs,
    std::uint32_t generation, VnResult expectedStatus) noexcept {
    if (!m_active) return;
    const std::size_t start = BeginRecord(kTraceRecordMeterProbeInstall);
    AppendU32(1);
    AppendU32(static_cast<std::uint32_t>(expectedStatus));
    AppendObjectId(probeStableId);
    AppendObjectId(busId);
    AppendU32(tap);
    AppendU32(attackMs);
    AppendU32(releaseMs);
    AppendU32(generation);
    CommitRecord(start);
}

void TraceRecorder::RecordMeterProbeUninstall(
    VnObjectId probeStableId, std::uint32_t generation,
    VnResult expectedStatus) noexcept {
    if (!m_active) return;
    const std::size_t start = BeginRecord(kTraceRecordMeterProbeUninstall);
    AppendU32(1);
    AppendU32(static_cast<std::uint32_t>(expectedStatus));
    AppendObjectId(probeStableId);
    AppendU32(generation);
    CommitRecord(start);
}

void TraceRecorder::RecordMeterProbeBindingDefine(
    VnObjectId bindingId, VnObjectId probeStableId,
    std::uint32_t probeGeneration, std::uint32_t targetScope,
    std::uint32_t targetId, const CurvePoint* points,
    const CurveHandlePair* handles, std::uint32_t pointCount,
    std::uint32_t transitionMs, VnResult expectedStatus) noexcept {
    if (!m_active) return;
    const std::size_t start =
        BeginRecord(kTraceRecordMeterProbeBindingDefine);
    AppendU32(1);
    AppendU32(static_cast<std::uint32_t>(expectedStatus));
    AppendObjectId(bindingId);
    AppendObjectId(probeStableId);
    AppendU32(probeGeneration);
    AppendU32(targetScope);
    AppendU32(targetId);
    AppendU32(transitionMs);
    AppendU32(0); // the single binding-local curve
    if (pointCount == 0 || pointCount > kMaxCurvePoints || points == nullptr ||
        (pointCount > 1 && handles == nullptr)) {
        AppendU32(pointCount);
        SetFlag(start, kTraceFlagArgsUnreplayable);
        CommitRecord(start);
        return;
    }
    AppendU32(pointCount);
    SetFlag(start, kTraceFlagRawReadableAttempt);
    for (std::uint32_t i = 0; i < pointCount; ++i) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &points[i].x, sizeof bits);
        AppendU32(bits);
        std::memcpy(&bits, &points[i].y, sizeof bits);
        AppendU32(bits);
    }
    for (std::uint32_t i = 0; i + 1 < pointCount; ++i) {
        const CurveHandlePair& handle = handles[i];
        const float values[4] = {handle.h1x, handle.h1y, handle.h2x,
                                 handle.h2y};
        for (float value : values) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof bits);
            AppendU32(bits);
        }
    }
    CommitRecord(start);
}

void TraceRecorder::RecordMeterProbeBindingUndefine(
    VnObjectId bindingId, VnResult expectedStatus) noexcept {
    if (!m_active) return;
    const std::size_t start =
        BeginRecord(kTraceRecordMeterProbeBindingUndefine);
    AppendU32(1);
    AppendU32(static_cast<std::uint32_t>(expectedStatus));
    AppendObjectId(bindingId);
    CommitRecord(start);
}

void TraceRecorder::RecordMeterProbeBindingAttachment(
    std::uint16_t recordType, const MeterProbeAttachment& attachment,
    VnResult expectedStatus) noexcept {
    if (!m_active) return;
    const std::size_t start = BeginRecord(recordType);
    AppendU32(1);
    AppendU32(static_cast<std::uint32_t>(expectedStatus));
    AppendObjectId(attachment.bindingId);
    AppendU32(attachment.targetScope);
    AppendObjectId(attachment.targetStableId);
    AppendU64(attachment.targetInstanceHandle);
    AppendU32(attachment.targetGeneration);
    CommitRecord(start);
}

void TraceRecorder::RememberEmitterHandle(VnEmitterHandle handle) noexcept {
    if (handle != 0) {
        m_emitterHandles.push_back(handle);
    }
}

std::uint64_t TraceRecorder::RecordedEmitterHandle(
    VnEmitterHandle handle) const noexcept {
    for (std::size_t i = 0; i < m_emitterHandles.size(); ++i) {
        if (m_emitterHandles[i] == handle) {
            return static_cast<std::uint64_t>(i + 1u);
        }
    }
    return handle;
}

void TraceRecorder::RecordPreviewPlayPlayable(
    bool argsReadable, std::uint32_t structSize, std::uint32_t kind,
    VnObjectId stableId, std::uint64_t emitter, std::uint32_t flags,
    std::uint32_t reserved, std::int32_t result,
    std::uint32_t returnedKind, std::uint64_t handle) noexcept {
    if (!m_active) return;
    const std::size_t start = BeginRecord(kTraceRecordApiCall);
    AppendU16(kTraceCallPreviewPlayPlayable);
    AppendU16(0);
    AppendU32(structSize);
    AppendU32(kind);
    AppendObjectId(stableId);
    AppendU64(RecordedEmitterHandle(emitter));
    AppendU32(flags);
    AppendU32(reserved);
    AppendU32(static_cast<std::uint32_t>(result));
    std::uint64_t recordedHandle = 0;
    if (result == 0 && handle != 0) {
        if (std::find(m_previewHandles.begin(), m_previewHandles.end(),
                      handle) != m_previewHandles.end()) {
            FailTrace();
            return;
        }
        m_previewHandles.push_back(handle);
        recordedHandle = (UINT64_C(1) << 63) |
            static_cast<std::uint64_t>(m_previewHandles.size());
    }
    AppendU32(result == 0 ? returnedKind : 0);
    AppendU64(result == 0 ? recordedHandle : 0);
    const auto& token = m_engine.lastPairedProjectGenerationToken;
    AppendU64(token.projectSessionId);
    AppendU64(token.runtimeEpoch);
    AppendU64(token.authoringRevision);
    AppendU64(token.submissionSerial);
    AppendU64(m_engine.lastPairedPublishedBusFingerprint);
    AppendU64(m_engine.lastPairedCompileTargetProfileFingerprint);
    const std::int32_t admission =
        TokenIsPaired(token)
            ? static_cast<std::int32_t>(m_engine.lastPreviewBusAdmission)
            : 3;
    AppendU32(static_cast<std::uint32_t>(admission));
    if (!argsReadable) {
        SetFlag(start, kTraceFlagArgsUnreplayable);
    }
    CommitRecord(start);
}

void TraceRecorder::RecordPreviewResetPlayable(
    bool argsReadable, std::uint32_t structSize, std::uint32_t kind,
    VnObjectId stableId, std::uint64_t emitter, std::uint32_t flags,
    std::uint32_t reserved, std::int32_t result) noexcept {
    if (!m_active) return;
    const std::size_t start = BeginRecord(kTraceRecordApiCall);
    AppendU16(kTraceCallPreviewResetPlayable);
    AppendU16(0);
    AppendU32(structSize);
    AppendU32(kind);
    AppendObjectId(stableId);
    AppendU64(RecordedEmitterHandle(emitter));
    AppendU32(flags);
    AppendU32(reserved);
    AppendU32(static_cast<std::uint32_t>(result));
    if (!argsReadable) {
        SetFlag(start, kTraceFlagArgsUnreplayable);
    }
    CommitRecord(start);
}

void TraceRecorder::RecordPreviewStopPlayable(
    std::uint64_t handle, std::uint32_t fadeMs,
    std::int32_t result) noexcept {
    if (!m_active) return;
    std::uint64_t recordedHandle = handle;
    const auto found = std::find(m_previewHandles.begin(),
                                 m_previewHandles.end(), handle);
    if (found != m_previewHandles.end()) {
        recordedHandle = (UINT64_C(1) << 63) |
            static_cast<std::uint64_t>(
                std::distance(m_previewHandles.begin(), found) + 1);
    }
    const std::size_t start = BeginRecord(kTraceRecordApiCall);
    AppendU16(kTraceCallPreviewStopPlayable);
    AppendU16(0);
    AppendU64(recordedHandle);
    AppendU32(fadeMs);
    AppendU32(static_cast<std::uint32_t>(result));
    CommitRecord(start);
}

void TraceRecorder::RecordPreviewQueryPlayable(
    std::uint64_t handle, bool stateReadable, std::uint32_t structSize,
    std::int32_t result, const PlayableControlState* state) noexcept {
    if (!m_active) return;
    std::uint64_t recordedHandle = handle;
    const auto found = std::find(m_previewHandles.begin(),
                                 m_previewHandles.end(), handle);
    if (found != m_previewHandles.end()) {
        recordedHandle = (UINT64_C(1) << 63) |
            static_cast<std::uint64_t>(
                std::distance(m_previewHandles.begin(), found) + 1);
    }
    const std::size_t start = BeginRecord(kTraceRecordApiCall);
    AppendU16(kTraceCallPreviewQueryPlayable);
    AppendU16(0);
    AppendU64(recordedHandle);
    AppendU32(structSize);
    AppendU32(static_cast<std::uint32_t>(result));
    const bool success = result == 0 && state != nullptr;
    AppendU32(success ? sizeof(std::uint32_t) * 8 + sizeof(std::uint64_t) * 3 +
                            sizeof(VnObjectId)
                      : 0);
    AppendU32(success ? static_cast<std::uint32_t>(state->kind) : 0);
    AppendU32(success ? static_cast<std::uint32_t>(state->lifecycle) : 0);
    AppendU32(success ? static_cast<std::uint32_t>(state->reason) : 0);
    AppendObjectId(success ? state->stableId : VnObjectId{});
    AppendU64(success ? state->worldGeneration : 0);
    AppendU64(success ? state->acceptedLogicTicket : 0);
    AppendU64(success ? state->lastAppliedBlockSequence : 0);
    AppendU32(success ? state->childLiveCount : 0);
    AppendU32(success ? state->childAudibleRetiringCount : 0);
    AppendU32(success ? state->pendingFutureCount : 0);
    AppendU32(0);
    if (!stateReadable) {
        SetFlag(start, kTraceFlagArgsUnreplayable);
    }
    CommitRecord(start);
}

void TraceRecorder::RecordVariableOutcome(
    const VariableOutcomeRecord& outcome) noexcept {
    if (!m_active) return;
    const std::size_t start = BeginRecord(kTraceRecordVariableOutcome);
    AppendU32(1);
    AppendU32(static_cast<std::uint32_t>(outcome.outcome));
    AppendU64(outcome.acceptedTicket.sequence);
    AppendU64(outcome.relatedTicket.sequence);
    AppendU64(outcome.actualFrame);
    AppendU64(outcome.bankVersionStableOrdinal);
    AppendU64(outcome.world);
    AppendU64(outcome.transport);
    AppendObjectId(outcome.variableId);
    AppendU32(static_cast<std::uint32_t>(outcome.scope));
    AppendU32(outcome.emitterGeneration);
    CommitRecord(start);
}

void TraceRecorder::UpdateEnd() noexcept {
    if (!m_active) {
        return;
    }
    if (m_updateDepth > 0) {
        --m_updateDepth;
    }
    SyncBlocks();
    EmitBarrier(kTraceRecordUpdateEnd, m_updateIndex);
    ++m_updateIndex;
}

void TraceRecorder::SyncBlocks() noexcept {
    if (!m_active) {
        return;
    }
    SnapshotPayload payload;
    std::uint64_t published = m_blocksJournaled;
    if (m_engine.mixer.Snapshots().Capture(payload)) {
        published = payload.blocksRendered;
    } else {
        published = m_engine.lastSnapshotPayload.blocksRendered;
    }
    // One barrier per rendered block. A failed capture (writer collision
    // through every retry) simply defers the barriers to the next record
    // boundary — the counter is monotonic, so nothing is lost or reordered.
    while (m_active && m_blocksJournaled < published) {
        EmitBarrier(kTraceRecordBlockBarrier, m_blocksJournaled);
        ++m_blocksJournaled;
    }
}

void TraceRecorder::EmitBarrier(std::uint16_t type,
                                std::uint64_t index) noexcept {
    const std::size_t start = BeginRecord(type);
    AppendU64(index);
    CommitRecord(start);
}

std::uint64_t TraceRecorder::RecordPackBytes(const void* bytes, std::uint64_t count) noexcept {
    if (!m_active) return 0;
    if (bytes == nullptr || count == 0 || count > UINT32_MAX - 24u || count > SIZE_MAX) {
        FailTrace();
        return 0;
    }
    try {
        const auto hash = Fnv1a64(bytes, static_cast<std::size_t>(count));
        const auto key = std::make_pair(hash, count);
        const auto found = m_packResources.find(key);
        if (found != m_packResources.end()) return found->second;
        const auto id = static_cast<std::uint64_t>(m_packResources.size()) + 1;
        m_packResources.emplace(key, id);
        SyncBlocks();
        if (!m_active) return 0;
        const auto start = BeginRecord(kTraceRecordPackBytes);
        AppendU64(id);
        AppendU64(hash);
        AppendU64(count);
        Append(bytes, static_cast<std::size_t>(count));
        CommitRecord(start);
        return m_active ? id : 0;
    } catch (...) {
        FailTrace();
        return 0;
    }
}

std::size_t TraceRecorder::BeginRecord(std::uint16_t type) noexcept {
    const std::size_t start = m_buffer.size();
    const std::uint32_t payloadBytes = 0; // patched at commit
    const std::uint16_t flags = 0;
    const std::uint64_t ordinal = m_ordinal++;
    const std::uint64_t timestampUs = NowMicros();
    AppendU32(payloadBytes);
    AppendU16(type);
    AppendU16(flags);
    AppendU64(ordinal);
    AppendU64(timestampUs);
    return start;
}

void TraceRecorder::CommitRecord(std::size_t recordStart) noexcept {
    if (!m_active) {
        return;
    }
    const std::uint32_t payloadBytes = static_cast<std::uint32_t>(
        m_buffer.size() - recordStart - kTraceRecordHeaderBytes);
    StoreLe32(m_buffer.data() + recordStart + kRecordPayloadBytesOffset,
              payloadBytes);
    if (m_buffer.size() >= kFlushThresholdBytes) {
        FlushBuffer();
    }
}

void TraceRecorder::SetFlag(std::size_t recordStart,
                            std::uint16_t flag) noexcept {
    if (!m_active) {
        return;
    }
    std::uint16_t flags = LoadLe16(
        m_buffer.data() + recordStart + kRecordFlagsOffset);
    flags |= flag;
    StoreLe16(m_buffer.data() + recordStart + kRecordFlagsOffset, flags);
}

void TraceRecorder::Append(const void* bytes, std::size_t count) noexcept {
    if (count == 0 || m_appendFailed ||
        (!m_active && !m_file.is_open())) {
        return;
    }
    const std::uint8_t* p = static_cast<const std::uint8_t*>(bytes);
    try {
        if (TraceRecorderFaultForTests() == TraceRecorderFault::Append) {
            throw std::bad_alloc{};
        }
        m_buffer.insert(m_buffer.end(), p, p + count);
    } catch (...) {
        m_appendFailed = true;
        if (m_active) {
            FailTrace();
        }
    }
}

void TraceRecorder::AppendU16(std::uint16_t value) noexcept {
    std::uint8_t bytes[2];
    StoreLe16(bytes, value);
    Append(bytes, sizeof bytes);
}

void TraceRecorder::AppendU32(std::uint32_t value) noexcept {
    std::uint8_t bytes[4];
    StoreLe32(bytes, value);
    Append(bytes, sizeof bytes);
}

void TraceRecorder::AppendObjectId(VnObjectId value) noexcept {
    const auto bytes = ObjectIdBytes(value);
    Append(bytes.data(), bytes.size());
}

void TraceRecorder::AppendU64(std::uint64_t value) noexcept {
    std::uint8_t bytes[8];
    StoreLe64(bytes, value);
    Append(bytes, sizeof bytes);
}

void TraceRecorder::FlushBuffer() noexcept {
    if (m_buffer.empty()) {
        return;
    }
    if (m_injectWriteFailure) {
        m_injectWriteFailure = false;
        if (m_active) {
            FailTrace();
        } else {
            // Start()-time injection: surface through the stream state so
            // the caller's header-write check refuses the start.
            m_file.setstate(std::ios::failbit);
        }
        return;
    }
    m_file.write(reinterpret_cast<const char*>(m_buffer.data()),
                 static_cast<std::streamsize>(m_buffer.size()));
    if (m_file.fail() && m_active) {
        FailTrace();
        return;
    }
    m_buffer.clear();
}

void TraceRecorder::FailTrace() noexcept {
    // Mid-session write failure: stop the trace, keep the engine running,
    // surface through the sticky snapshot flag and a log record. The journal
    // on disk keeps everything up to the last successful flush.
    m_active = false;
    m_engine.traceFailedSticky = true;
    LogRecord record{};
    record.level = VN_LOG_ERROR;
    record.code = kLogCodeTraceWriteFailed;
    record.arg0 = m_ordinal;
    m_engine.logicLogRing.Push(record);
    m_file.close();
    m_buffer.clear();
}

std::uint64_t TraceRecorder::NowMicros() const {
    if (m_ticksPerSecond == 0) {
        return 0;
    }
    const std::uint64_t now = CpuClockTicks();
    if (now <= m_startTicks) {
        return 0;
    }
    const std::uint64_t ticks = now - m_startTicks;
    const std::uint64_t seconds = ticks / m_ticksPerSecond;
    const std::uint64_t rem = ticks % m_ticksPerSecond;
    return seconds * 1000000ull + (rem * 1000000ull) / m_ticksPerSecond;
}

// --- TraceCallScribe ---

TraceCallScribe::TraceCallScribe(TraceRecorder* recorder,
                                 std::uint16_t callId) noexcept
    : m_recorder(recorder) {
    if (m_recorder != nullptr && !m_recorder->Active()) {
        m_recorder = nullptr;
    }
    if (m_recorder == nullptr) {
        return;
    }
    m_recorder->SyncBlocks();
    if (!m_recorder->Active()) { // a barrier flush may have failed the trace
        m_recorder = nullptr;
        return;
    }
    m_recordStart = m_recorder->BeginRecord(kTraceRecordApiCall);
    if (!m_recorder->Active()) {
        m_recorder = nullptr;
        return;
    }
    if (m_recorder->m_updateDepth > 0) {
        // Issued from a callback that re-entered the API inside Vn_Update:
        // journaled at its real ordinal, replayed after the enclosing
        // update returns — flagged so the replayer warns instead of
        // silently reordering.
        m_recorder->SetFlag(m_recordStart, kTraceFlagCallbackContext);
        m_recorder->m_sawCallbackContextCall = true;
    }
    const std::uint16_t reserved = 0;
    AppendU16(callId);
    AppendU16(reserved);
}

TraceCallScribe::TraceCallScribe(TraceCallScribe&& other) noexcept
    : m_recorder(other.m_recorder), m_recordStart(other.m_recordStart) {
    other.m_recorder = nullptr;
}

TraceCallScribe::~TraceCallScribe() noexcept {
    if (m_recorder != nullptr) {
        m_recorder->CommitRecord(m_recordStart);
    }
}

void TraceCallScribe::Append(const void* bytes,
                             std::size_t count) noexcept {
    if (m_recorder != nullptr) {
        m_recorder->Append(bytes, count);
        if (!m_recorder->Active()) {
            m_recorder = nullptr;
        }
    }
}

void TraceCallScribe::AppendU16(std::uint16_t value) noexcept {
    std::uint8_t bytes[2];
    StoreLe16(bytes, value);
    Append(bytes, sizeof bytes);
}

void TraceCallScribe::AppendU32(std::uint32_t value) noexcept {
    std::uint8_t bytes[4];
    StoreLe32(bytes, value);
    Append(bytes, sizeof bytes);
}

void TraceCallScribe::AppendObjectId(VnObjectId value) noexcept {
    const auto bytes = ObjectIdBytes(value);
    Append(bytes.data(), bytes.size());
}

void TraceCallScribe::AppendU64(std::uint64_t value) noexcept {
    std::uint8_t bytes[8];
    StoreLe64(bytes, value);
    Append(bytes, sizeof bytes);
}

void TraceCallScribe::AppendFloats(const float* values,
                                   std::size_t count) noexcept {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    for (std::size_t i = 0; i < count; ++i) {
        F32(values[i]);
    }
#else
    Append(values, count * sizeof(float));
#endif
}

void TraceCallScribe::MarkUnreplayable() noexcept {
    if (m_recorder != nullptr) {
        m_recorder->SetFlag(m_recordStart, kTraceFlagArgsUnreplayable);
    }
}

TraceCallScribe& TraceCallScribe::U32(std::uint32_t value) noexcept {
    AppendU32(value);
    return *this;
}

TraceCallScribe& TraceCallScribe::ObjectId(VnObjectId value) noexcept {
    AppendObjectId(value);
    return *this;
}

TraceCallScribe& TraceCallScribe::U64(std::uint64_t value) noexcept {
    AppendU64(value);
    return *this;
}

TraceCallScribe& TraceCallScribe::F32(float value) noexcept {
    std::uint32_t bits = 0;
    static_assert(sizeof bits == sizeof value);
    std::memcpy(&bits, &value, sizeof bits);
    AppendU32(bits);
    return *this;
}

TraceCallScribe& TraceCallScribe::F64(double value) noexcept {
    std::uint64_t bits = 0;
    static_assert(sizeof bits == sizeof value);
    std::memcpy(&bits, &value, sizeof bits);
    AppendU64(bits);
    return *this;
}

TraceCallScribe& TraceCallScribe::Vec3(VnVec3 value) noexcept {
    F32(value.x);
    F32(value.y);
    F32(value.z);
    return *this;
}

TraceCallScribe& TraceCallScribe::Str(const char* utf8) noexcept {
    if (m_recorder == nullptr) {
        return *this;
    }
    if (utf8 == nullptr) {
        const std::uint32_t nullLen = kTraceStrNull;
        U32(nullLen);
        return *this;
    }
    const std::size_t len = std::strlen(utf8);
    if (len >= kTraceStrNull) {
        const std::uint32_t nullLen = kTraceStrNull;
        U32(nullLen);
        MarkUnreplayable();
        return *this;
    }
    const std::uint32_t len32 = static_cast<std::uint32_t>(len);
    U32(len32);
    Append(utf8, len);
    return *this;
}

TraceCallScribe& TraceCallScribe::Bytes(const void* bytes,
                                        std::size_t count) noexcept {
    if (bytes == nullptr && count != 0) {
        MarkUnreplayable();
        return *this;
    }
    Append(bytes, count);
    return *this;
}

TraceCallScribe& TraceCallScribe::StructSizeOf(
    const void* descBase) noexcept {
    if (m_recorder == nullptr) {
        return *this;
    }
    std::uint32_t declared = 0;
    if (descBase == nullptr) {
        MarkUnreplayable();
    } else {
        std::memcpy(&declared, descBase, sizeof declared);
    }
    U32(declared);
    return *this;
}

TraceCallScribe& TraceCallScribe::Scalar32Desc(
    const void* descBase, std::uint32_t nativeBytes,
    std::uint32_t semanticVintage) noexcept {
    if (m_recorder == nullptr) {
        return *this;
    }
    if (descBase == nullptr || nativeBytes < sizeof(std::uint32_t) ||
        nativeBytes % sizeof(std::uint32_t) != 0 || semanticVintage == 0) {
        U32(0);
        MarkUnreplayable();
        return *this;
    }
    U32(semanticVintage);
    const auto* bytes = static_cast<const std::uint8_t*>(descBase);
    for (std::uint32_t offset = sizeof(std::uint32_t);
         offset < nativeBytes; offset += sizeof(std::uint32_t)) {
        std::uint32_t word = 0;
        std::memcpy(&word, bytes + offset, sizeof word);
        U32(word);
    }
    return *this;
}

TraceCallScribe& TraceCallScribe::AttenuationDesc(
    const VnAttenuationDesc* d) noexcept {
    if (d == nullptr) {
        return Scalar32Desc(nullptr, 0, 0);
    }
    const std::uint32_t legacy = static_cast<std::uint32_t>(
        offsetof(VnAttenuationDesc, proximityFrequency));
    if (d->structSize == legacy) {
        return Scalar32Desc(d, legacy, 1);
    }
    const std::uint32_t proximity = static_cast<std::uint32_t>(
        offsetof(VnAttenuationDesc, highpassCurve));
    if (d->structSize == proximity) {
        return Scalar32Desc(d, proximity, 2);
    }
    if (d->structSize == sizeof(VnAttenuationDesc)) {
        return Scalar32Desc(d, sizeof(VnAttenuationDesc), 3);
    }
    return Scalar32Desc(nullptr, 0, 0);
}

TraceCallScribe& TraceCallScribe::DownmixMatrixDesc(
    const VnDownmixMatrixDesc* d) noexcept {
    if (d == nullptr || d->structSize != sizeof(VnDownmixMatrixDesc)) {
        return Scalar32Desc(nullptr, 0, 0);
    }
    return Scalar32Desc(d, sizeof(VnDownmixMatrixDesc), 1);
}

TraceCallScribe& TraceCallScribe::BindingDesc(
    const VnBindingDesc* d) noexcept {
    if (m_recorder == nullptr) {
        return *this;
    }
    if (d == nullptr || d->structSize != sizeof(VnBindingDesc) ||
        d->curve.structSize != sizeof(VnCurveDesc)) {
        U32(0);
        MarkUnreplayable();
        return *this;
    }
    U32(1)
        .ObjectId(d->variableId)
        .U32(d->targetScopeKind)
        .U32(d->targetId)
        .U32(d->curve.pointCount);
    for (const VnCurvePoint& point : d->curve.points) {
        F32(point.x).F32(point.y);
    }
    for (const VnCurveHandlePair& handle : d->curve.handles) {
        F32(handle.h1x).F32(handle.h1y).F32(handle.h2x).F32(handle.h2y);
    }
    U32(d->transitionMs);
    return *this;
}

TraceCallScribe& TraceCallScribe::BindingAttachDesc(
    const VnBindingAttachDesc* d) noexcept {
    if (m_recorder == nullptr) {
        return *this;
    }
    if (d == nullptr || d->structSize != sizeof(VnBindingAttachDesc)) {
        U32(0);
        MarkUnreplayable();
        return *this;
    }
    U32(1).U32(d->scopeKind).ObjectId(d->objectId);
    return *this;
}

TraceCallScribe& TraceCallScribe::EffectDynamicRouteDesc(
    const VnEffectDynamicRouteDesc* d) noexcept {
    if (m_recorder == nullptr) {
        return *this;
    }
    if (d == nullptr || d->structSize != sizeof(VnEffectDynamicRouteDesc)) {
        U32(0);
        MarkUnreplayable();
        return *this;
    }
    U32(1)
        .U32(d->signalForm)
        .U32(d->sourceKind)
        .U32(d->tap)
        .ObjectId(d->sourceId)
        .U64(d->sourceVoiceHandle)
        .F32(d->levelDb)
        .U32(d->expectedEffectType);
    return *this;
}

TraceCallScribe& TraceCallScribe::EffectDesc(std::uint32_t type,
                                             const VnEffectDesc* d) noexcept {
    std::uint32_t expectedBytes = 0;
    std::uint32_t paramCount = 0;
    switch (type) {
    case VN_EFFECT_FEEDBACK_DELAY:
        expectedBytes = sizeof(VnFeedbackDelayDesc);
        paramCount = 4;
        break;
    case VN_EFFECT_THREE_BAND_TONE:
        expectedBytes = sizeof(VnThreeBandToneDesc);
        paramCount = 7;
        break;
    case VN_EFFECT_GAIN:
        expectedBytes = sizeof(VnGainDesc);
        paramCount = 1;
        break;
    case VN_EFFECT_LIMITER:
        expectedBytes = sizeof(VnLimiterDesc);
        paramCount = 2;
        break;
    case VN_EFFECT_COMPRESSOR:
        expectedBytes = sizeof(VnCompressorDesc);
        paramCount = 6;
        break;
    default:
        break;
    }
    if (d == nullptr || expectedBytes == 0 ||
        d->structSize != expectedBytes) {
        return Scalar32Desc(nullptr, 0, 0);
    }
    U32(2).U32(d->reserved0).ObjectId(d->effectId);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(d);
    for (std::uint32_t parameter = 0; parameter < paramCount; ++parameter) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, bytes + sizeof(VnEffectDesc) + parameter * 4, 4);
        U32(bits);
    }
    return *this;
}

TraceCallScribe& TraceCallScribe::PcmDesc(const VnPCMDesc* d) noexcept {
    if (m_recorder == nullptr) {
        return *this;
    }
    std::uint8_t present = 0;
    if (d == nullptr || d->structSize != sizeof(VnPCMDesc)) {
        U32(0);
        Append(&present, sizeof present);
        MarkUnreplayable();
        return *this;
    }
    U32(1)
        .U32(d->sampleRate)
        .U32(d->channels)
        .U32(d->layout)
        .U32(d->policy)
        .U64(d->frames);
    // Mirror of the registration preconditions that bound the sample blob;
    // a descriptor outside them is refused by the engine, so the journal
    // keeps the slim record and skips the copy.
    const bool interleaved = d->layout == VN_PCM_LAYOUT_INTERLEAVED;
    const bool planar = d->layout == VN_PCM_LAYOUT_PLANAR;
    if ((!interleaved && !planar) || d->channels < 1 || d->channels > 8 ||
        d->frames == 0 ||
        d->frames > kTraceMaxPcmFloats / d->channels) {
        Append(&present, sizeof present);
        MarkUnreplayable();
        return *this;
    }
    const std::uint64_t floatCount = d->frames * d->channels;
    if (interleaved) {
        if (d->samplesInterleaved == nullptr) {
            Append(&present, sizeof present);
            MarkUnreplayable();
            return *this;
        }
        present = 1;
        Append(&present, sizeof present);
        U64(floatCount);
        AppendFloats(d->samplesInterleaved,
                     static_cast<std::size_t>(floatCount));
        return *this;
    }
    if (d->samplesPlanar == nullptr) {
        Append(&present, sizeof present);
        MarkUnreplayable();
        return *this;
    }
    for (std::uint32_t ch = 0; ch < d->channels; ++ch) {
        if (d->samplesPlanar[ch] == nullptr) {
            Append(&present, sizeof present);
            MarkUnreplayable();
            return *this;
        }
    }
    present = 1;
    Append(&present, sizeof present);
    U64(floatCount);
    for (std::uint32_t ch = 0; ch < d->channels; ++ch) {
        AppendFloats(d->samplesPlanar[ch],
                     static_cast<std::size_t>(d->frames));
    }
    return *this;
}

TraceCallScribe& TraceCallScribe::BusTopologyDesc(
    const VnBusTopologyDesc* d) noexcept {
    if (m_recorder == nullptr) {
        return *this;
    }
    if (d == nullptr) {
        U32(0);
        MarkUnreplayable();
        return *this;
    }
    U32(1)
        .U32(d->flags)
        .U64(d->ownerId)
        .U64(d->baseRevision)
        .U64(d->revision)
        .U32(d->busCount)
        .U32(d->sendCount);
    const Engine& engine = m_recorder->m_engine;
    const bool valid =
        d->structSize == sizeof(VnBusTopologyDesc) &&
        d->busStructSize == sizeof(VnBusTopologyBus) &&
        d->sendStructSize == sizeof(VnBusTopologySend) &&
        d->busCount < engine.config.maxBuses &&
        d->sendCount <=
            static_cast<std::uint64_t>(engine.config.maxBuses) *
                engine.config.busSendSlotsPerBus &&
        (d->busCount == 0 || d->buses != nullptr) &&
        (d->sendCount == 0 || d->sends != nullptr);
    if (!valid) {
        MarkUnreplayable();
        return *this;
    }
    for (std::uint32_t i = 0; i < d->busCount; ++i) {
        const VnBusTopologyBus& bus = d->buses[i];
        U32(1).U32(bus.flags).ObjectId(bus.busId).ObjectId(bus.parentId);
    }
    for (std::uint32_t i = 0; i < d->sendCount; ++i) {
        const VnBusTopologySend& send = d->sends[i];
        U32(1)
            .U32(send.rampMs)
            .ObjectId(send.fromBusId)
            .ObjectId(send.toBusId)
            .F32(send.levelDb);
    }
    return *this;
}

TraceCallScribe& TraceCallScribe::PackDesc(const VnPackDesc* d) noexcept {
    if (m_recorder == nullptr) {
        return *this;
    }
    if (d == nullptr || d->structSize != sizeof(VnPackDesc)) {
        U32(0);
        Str(nullptr);
        const std::uint64_t hash = 0;
        const std::uint8_t known = 0;
        U64(hash);
        Append(&known, sizeof known);
        MarkUnreplayable();
        return *this;
    }
    U32(1);
    return FileRef(d->utf8Path);
}

TraceCallScribe& TraceCallScribe::FileRef(const char* utf8Path) noexcept {
    if (m_recorder == nullptr) {
        return *this;
    }
    Str(utf8Path);
    if (m_recorder == nullptr) {
        return *this;
    }
    std::uint64_t hash = 0;
    std::uint8_t known = 0;
    try {
        if (utf8Path != nullptr && HashFileContents(utf8Path, &hash)) {
            known = 1;
        } else {
            // An unreadable file cannot be re-verified at replay; the engine
            // refuses the call it feeds anyway, so the journal keeps the slim
            // record.
            hash = 0;
            MarkUnreplayable();
        }
    } catch (...) {
        // A recorder-side allocation or filesystem exception fails the trace,
        // while the observed API call continues on its normal path.
        if (m_recorder != nullptr) {
            m_recorder->FailTrace();
            m_recorder = nullptr;
        }
        return *this;
    }
    U64(hash);
    Append(&known, sizeof known);
    return *this;
}

} // namespace harken
