// trace-replay: deterministic re-execution of a VTRC session journal.
//
// The journal defines the deterministic logic/DSP inputs: the caller config
// (with randomSeed), every public API call with deep-copied arguments, and the
// block/tick schedule. Replay always runs the
// null backend — blocks advance only when this loop pumps them at their
// barrier positions, which is exactly the schedule the recording logic
// thread observed — so a given journal renders byte-identically on every
// replay. Content files are re-verified by hash before the call that reads
// them executes; a mismatch refuses the replay with a report naming the
// file, never a silently different render. A real-device config that used
// native rate or layout defaults lacks those resolved values in VTRC v2, so
// replay reports device-format identity as not provable.
#include "TraceReplay.h"

#include <harken/Harken.h>

#include "Engine.h"       // internal: the null-backend pump (test-clock precedent)
#include "ProjectGenerationTransaction.h"
#include "TraceCodec.h"
#include "TraceFormat.h"
#include "harken/core/CurrentWorldArtifact.h"
#include "TraceRecorder.h" // HashFileContents
#include "harken/core/Utf8Path.h" // non-throwing UTF-8 path construction
#include "harken/media/PackFormat.h" // Fnv1a64 for the render capture hash
#include "harken_preview/PreviewAbi.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace harkenhost {

namespace {

class VariableOutcomeQueue {
public:
    explicit VariableOutcomeQueue(std::size_t capacity) : records_(capacity) {}
    static void Capture(void* context, const harken::VariableOutcomeRecord& record) noexcept {
        auto& queue = *static_cast<VariableOutcomeQueue*>(context);
        if (queue.count_ == queue.records_.size()) {
            queue.overflowed_ = true;
            return;
        }
        queue.records_[(queue.begin_ + queue.count_) % queue.records_.size()] = record;
        ++queue.count_;
    }
    bool Pop(harken::VariableOutcomeRecord* record) noexcept {
        if (count_ == 0) return false;
        *record = records_[begin_];
        begin_ = (begin_ + 1u) % records_.size();
        --count_;
        return true;
    }
    bool Empty() const noexcept { return count_ == 0; }
    bool Overflowed() const noexcept { return overflowed_; }
private:
    std::vector<harken::VariableOutcomeRecord> records_;
    std::size_t begin_ = 0;
    std::size_t count_ = 0;
    bool overflowed_ = false;
};

// Bounds-checked little-endian cursor over the loaded journal bytes. Any
// out-of-bounds read latches `failed`; callers check it once per record.
struct ByteReader {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t offset = 0;
    bool failed = false;
    std::uint32_t formatVersion = harken::kTraceFormatVersion;

    bool Take(void* out, std::size_t count) {
        if (failed || size - offset < count) {
            failed = true;
            return false;
        }
        std::memcpy(out, data + offset, count);
        offset += count;
        return true;
    }
    std::uint16_t U16() {
        const std::uint8_t* p = Bytes(2);
        return p == nullptr
                   ? 0
                   : static_cast<std::uint16_t>(p[0]) |
                         static_cast<std::uint16_t>(p[1] << 8);
    }
    std::uint32_t U32() {
        const std::uint8_t* p = Bytes(4);
        return p == nullptr
                   ? 0
                   : static_cast<std::uint32_t>(p[0]) |
                         (static_cast<std::uint32_t>(p[1]) << 8) |
                         (static_cast<std::uint32_t>(p[2]) << 16) |
                         (static_cast<std::uint32_t>(p[3]) << 24);
    }
    std::uint64_t U64() {
        const std::uint64_t lo = U32();
        const std::uint64_t hi = U32();
        return lo | (hi << 32);
    }
    VnObjectId ObjectId() {
        const auto* bytes = Bytes(16);
        return bytes == nullptr ? VnObjectId{} : harken::LoadObjectId(bytes);
    }
    float F32() {
        const std::uint32_t bits = U32();
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof value);
        return value;
    }
    double F64() {
        const std::uint64_t bits = U64();
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof value);
        return value;
    }
    VnVec3 Vec3() {
        VnVec3 v{};
        v.x = F32();
        v.y = F32();
        v.z = F32();
        return v;
    }
    const std::uint8_t* Bytes(std::size_t count) {
        if (failed || size - offset < count) {
            failed = true;
            return nullptr;
        }
        const std::uint8_t* p = data + offset;
        offset += count;
        return p;
    }
    std::size_t Remaining() const {
        return failed || offset > size ? 0 : size - offset;
    }
    // Str encoding: u32 length (kTraceStrNull = the caller passed NULL) +
    // bytes. Returns false for the NULL sentinel.
    bool Str(std::string* out) {
        const std::uint32_t len = U32();
        if (failed || len == harken::kTraceStrNull) {
            return false;
        }
        const std::uint8_t* p = Bytes(len);
        if (p == nullptr) {
            return false;
        }
        out->assign(reinterpret_cast<const char*>(p), len);
        return true;
    }
};

bool ReadScalar32Desc(ByteReader& r, void* out, std::size_t outBytes,
                      std::uint32_t expectedVintage = 1) {
    const std::uint32_t vintage = r.U32();
    if (r.failed || vintage != expectedVintage || outBytes < 4 ||
        outBytes % 4 != 0) {
        return false;
    }
    auto* bytes = static_cast<std::uint8_t*>(out);
    std::memset(bytes, 0, outBytes);
    const std::uint32_t nativeSize = static_cast<std::uint32_t>(outBytes);
    std::memcpy(bytes, &nativeSize, sizeof nativeSize);
    for (std::size_t offset = 4; offset < outBytes; offset += 4) {
        const std::uint32_t word = r.U32();
        std::memcpy(bytes + offset, &word, sizeof word);
    }
    return !r.failed;
}

template <typename Desc>
bool ReadPortableDesc(ByteReader&, Desc*) {
    return false;
}

bool ReadPortableDesc(ByteReader& r, VnPCMDesc* out) {
    const std::uint32_t vintage = r.U32();
    if (vintage != 1) return false;
    *out = {};
    out->structSize = sizeof(VnPCMDesc);
    out->sampleRate = r.U32();
    out->channels = r.U32();
    out->layout = r.U32();
    out->policy = r.U32();
    out->frames = r.U64();
    return !r.failed;
}

bool ReadPortableDesc(ByteReader& r, VnPlayDesc* out) {
    const std::uint32_t vintage = r.U32();
    if (vintage < 1 || vintage > 4) return false;
    *out = {};
    out->structSize =
        vintage == 1
            ? static_cast<std::uint32_t>(offsetof(VnPlayDesc, loopCount))
            : vintage == 2
                  ? static_cast<std::uint32_t>(
                        offsetof(VnPlayDesc, emitterHandle))
                  : static_cast<std::uint32_t>(sizeof(VnPlayDesc));
    out->soundId = r.ObjectId();
    out->busId = r.ObjectId();
    out->volumeDb = r.F32();
    out->pitchRatio = r.F32();
    out->positionX = r.F32();
    out->positionY = r.F32();
    out->positionZ = r.F32();
    out->spatialized = r.U32();
    if (vintage >= 2) out->loopCount = r.U32();
    if (vintage >= 3) {
        out->emitterHandle = r.U64();
        out->priority = r.U32();
    }
    // Vintage 4 carries the play flags; an earlier vintage recorded none, and
    // the zeroed desc above is already the flag-free play.
    if (vintage == 4) out->reserved0 = r.U32();
    return !r.failed;
}

bool ReadPortableDesc(ByteReader& r, VnAttenuationDesc* out) {
    const std::uint32_t vintage = r.U32();
    const std::size_t nativeBytes =
        vintage == 1 ? offsetof(VnAttenuationDesc, proximityFrequency)
                     : vintage == 2 ? sizeof(VnAttenuationDesc) : 0;
    if (nativeBytes == 0 || nativeBytes % 4 != 0) return false;
    std::memset(out, 0, sizeof(*out));
    out->structSize = static_cast<std::uint32_t>(nativeBytes);
    auto* bytes = reinterpret_cast<std::uint8_t*>(out);
    for (std::size_t offset = 4; offset < nativeBytes; offset += 4) {
        const std::uint32_t word = r.U32();
        std::memcpy(bytes + offset, &word, sizeof word);
    }
    return !r.failed;
}

bool ReadPortableDesc(ByteReader& r, VnPackDesc* out) {
    const std::uint32_t vintage = r.U32();
    if (vintage != 1) return false;
    *out = {};
    out->structSize = sizeof(VnPackDesc);
    return !r.failed;
}

bool ReadPortableDesc(ByteReader& r, VnDownmixMatrixDesc* out) {
    return ReadScalar32Desc(r, out, sizeof(*out));
}

bool ReadPortableDesc(ByteReader& r, VnEffectDynamicRouteDesc* out) {
    const std::uint32_t vintage = r.U32();
    if (vintage != 1) return false;
    *out = {};
    out->structSize = sizeof(VnEffectDynamicRouteDesc);
    out->signalForm = r.U32();
    out->sourceKind = r.U32();
    out->tap = r.U32();
    out->sourceId = r.ObjectId();
    out->sourceVoiceHandle = r.U64();
    out->levelDb = r.F32();
    out->expectedEffectType = r.U32();
    return !r.failed;
}

bool ReadPortableDesc(ByteReader& r, VnBindingDesc* out) {
    const std::uint32_t vintage = r.U32();
    if (vintage != 1) return false;
    *out = {};
    out->structSize = sizeof(VnBindingDesc);
    out->variableId = r.ObjectId();
    out->targetScopeKind = r.U32();
    out->targetId = r.U32();
    out->curve.structSize = sizeof(VnCurveDesc);
    out->curve.pointCount = r.U32();
    for (VnCurvePoint& point : out->curve.points) {
        point.x = r.F32();
        point.y = r.F32();
    }
    for (VnCurveHandlePair& handle : out->curve.handles) {
        handle.h1x = r.F32();
        handle.h1y = r.F32();
        handle.h2x = r.F32();
        handle.h2y = r.F32();
    }
    out->transitionMs = r.U32();
    return !r.failed;
}

bool ReadPortableDesc(ByteReader& r, VnBindingAttachDesc* out) {
    const std::uint32_t vintage = r.U32();
    if (vintage != 1) return false;
    *out = {};
    out->structSize = sizeof(VnBindingAttachDesc);
    out->scopeKind = r.U32();
    out->objectId = r.ObjectId();
    return !r.failed;
}

// Session-lifetime storage for decoded argument blobs whose addresses must
// outlive the call: PCM sample data registered under the REFERENCE policy
// stays pinned inside the engine until unregister, so every decoded blob
// simply lives for the whole replay. Deques keep element addresses stable.
struct BlobStore {
    std::deque<std::vector<float>> pcm;
    std::deque<std::vector<const float*>> planes;
    // Pack spans point into the immutable journal, which outlives replay.
    std::vector<VnPackByteSpan> packs;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> packGenerations;
};

using VoiceHandleMap =
    std::vector<std::pair<VnVoiceHandle, VnVoiceHandle>>;
using PackHandleMap =
    std::vector<std::pair<VnPackHandle, VnPackHandle>>;

VnPackHandle RemapPackHandle(const PackHandleMap& handles, VnPackHandle recorded) {
    if (recorded == 0) return 0;
    for (const auto& entry : handles)
        if (entry.first == recorded) return entry.second;
    // An unknown recorded handle is stale, never an accidentally live slot.
    return UINT64_MAX;
}

bool RememberPackHandle(PackHandleMap& handles, VnPackHandle recorded, VnPackHandle live) {
    if (recorded == 0 || live == 0) return recorded == live;
    for (const auto& entry : handles)
        if (entry.first == recorded) return entry.second == live;
    handles.emplace_back(recorded, live);
    return true;
}

std::uint64_t PackSetRevision(const harken::Engine& engine) {
    const auto* published = engine.packComposition.Published();
    return published != nullptr ? published->ledger.setRevision : 0;
}

bool CheckPackPublication(harken::Engine& engine, BlobStore& blobs,
    std::uint64_t revision, std::uint64_t activation, std::uint64_t generation) {
    const auto* published = engine.packComposition.Published();
    if (published == nullptr) return revision == 0 && activation == 0 && generation == 0;
    if (revision != published->ledger.setRevision || activation != published->ledger.activationGeneration ||
        generation == 0) return false;
    for (const auto& entry : blobs.packGenerations)
        if (entry.first == generation) return entry.second == published->compositeGeneration;
    for (const auto& entry : blobs.packGenerations)
        if (entry.second == published->compositeGeneration ||
            (entry.first < generation) != (entry.second < published->compositeGeneration)) return false;
    blobs.packGenerations.emplace_back(generation, published->compositeGeneration);
    return true;
}

VnResult ReplayPackBytes(void* context, std::uint64_t, const char*, std::uint32_t,
                         std::uint64_t, VnPackByteSpan* out) {
    const auto& span = *static_cast<const VnPackByteSpan*>(context);
    if (span.bytes == nullptr) return VN_PACK_PROVIDER_FAILED;
    *out = span;
    return VN_OK;
}

void ReleaseReplayPackBytes(void*, std::uint64_t, VnPackByteSpan) {}

struct ReplayPackNames {
    VnPackNamesV1 value{};
    std::string root, language;
    std::vector<VnPackVariableNameV1> variables;
    std::vector<VnPackLabelNameV1> labels;
    std::vector<std::string> variableNames, labelNames;

    bool Read(ByteReader& reader) {
        value.structSize = sizeof(value);
        value.projectId = reader.ObjectId();
        value.rootActionId = reader.ObjectId();
        if (!reader.Str(&root) || !reader.Str(&language)) return false;
        value.variableCount = reader.U32();
        value.labelCount = reader.U32();
        if (reader.failed || value.variableCount > reader.Remaining() / 20 ||
            value.labelCount > reader.Remaining() / 36) return false;
        variables.resize(value.variableCount);
        variableNames.resize(value.variableCount);
        labels.resize(value.labelCount);
        labelNames.resize(value.labelCount);
        for (std::uint32_t i = 0; i < value.variableCount; ++i) {
            variables[i].variableId = reader.ObjectId();
            if (!reader.Str(&variableNames[i])) return false;
            variables[i].utf8Name = variableNames[i].c_str();
        }
        for (std::uint32_t i = 0; i < value.labelCount; ++i) {
            labels[i].variableId = reader.ObjectId();
            labels[i].labelId = reader.ObjectId();
            if (!reader.Str(&labelNames[i])) return false;
            labels[i].utf8Name = labelNames[i].c_str();
        }
        value.utf8RootName = root.c_str();
        value.utf8Language = language.c_str();
        value.variables = variables.data();
        value.labels = labels.data();
        return !reader.failed;
    }
};
using ActionFiringHandleMap = std::vector<
    std::pair<VnActionFiringHandle, VnActionFiringHandle>>;
using ActionSeekTicketMap = std::vector<
    std::pair<VnActionSeekTicket, VnActionSeekTicket>>;
using PlayableObservationMap = std::vector<
    std::pair<std::uint64_t, std::uint64_t>>;
using EmitterHandleMap = std::vector<VnEmitterHandle>;
using PreviewHandleMap = std::vector<
    std::pair<VnPreviewPlayableHandle, VnPreviewPlayableHandle>>;
using WorldGenerationMap =
    std::vector<std::pair<std::uint64_t, std::uint64_t>>;
struct ProbeGenerationMapping {
    VnObjectId stableId{};
    std::uint32_t recorded = 0;
    std::uint32_t replayed = 0;
};
using ProbeGenerationMap = std::vector<ProbeGenerationMapping>;
using LogicTicketMap =
    std::vector<std::pair<std::uint64_t, std::uint64_t>>;

VnVoiceHandle RemapVoiceHandle(const VoiceHandleMap& handles,
                               VnVoiceHandle recorded) {
    for (const auto& entry : handles) {
        if (entry.first == recorded) {
            return entry.second;
        }
    }
    // A miss preserves intentionally forged, unknown, or stale-handle calls.
    return recorded;
}

bool TryRemapVoiceHandle(const VoiceHandleMap& handles,
                         VnVoiceHandle recorded,
                         VnVoiceHandle* replayed) {
    for (const auto& entry : handles) {
        if (entry.first == recorded) {
            *replayed = entry.second;
            return true;
        }
    }
    return false;
}

void RememberVoiceHandle(VoiceHandleMap& handles, VnVoiceHandle recorded,
                          VnVoiceHandle replayed) {
    for (auto& entry : handles) {
        if (entry.first == recorded) {
            entry.second = replayed;
            return;
        }
    }
    handles.emplace_back(recorded, replayed);
}

VnEmitterHandle RemapEmitterHandle(const EmitterHandleMap& handles,
                                   VnEmitterHandle recorded) {
    if (recorded == 0 || recorded > handles.size()) {
        return recorded;
    }
    return handles[static_cast<std::size_t>(recorded - 1u)];
}

bool TryRemapActionFiringHandle(const ActionFiringHandleMap& handles,
                                VnActionFiringHandle recorded,
                                VnActionFiringHandle* replayed) {
    if (recorded == 0) {
        *replayed = 0;
        return true;
    }
    for (const auto& entry : handles) {
        if (entry.first == recorded) {
            *replayed = entry.second;
            return true;
        }
    }
    return false;
}

bool RememberActionFiringHandle(ActionFiringHandleMap& handles,
                                VnActionFiringHandle recorded,
                                VnActionFiringHandle replayed) {
    if (recorded == 0 || replayed == 0) {
        return false;
    }
    for (const auto& entry : handles) {
        if (entry.first == recorded || entry.second == replayed) {
            return false;
        }
    }
    handles.emplace_back(recorded, replayed);
    return true;
}

bool TryRemapActionSeekTicket(const ActionSeekTicketMap& tickets,
                              VnActionSeekTicket recorded,
                              VnActionSeekTicket* replayed) {
    for (const auto& entry : tickets) {
        if (entry.first == recorded) {
            *replayed = entry.second;
            return true;
        }
    }
    return false;
}

bool RememberActionSeekTicket(ActionSeekTicketMap& tickets,
                              VnActionSeekTicket recorded,
                              VnActionSeekTicket replayed) {
    if (recorded == 0 || replayed == 0) {
        return false;
    }
    for (const auto& entry : tickets) {
        if (entry.first == recorded || entry.second == replayed) {
            return false;
        }
    }
    tickets.emplace_back(recorded, replayed);
    return true;
}

bool ObserveIdentityMapping(
    std::vector<std::pair<std::uint64_t, std::uint64_t>>& handles,
    std::uint64_t recorded, std::uint64_t replayed) {
    if (recorded == 0 || replayed == 0) {
        return recorded == replayed;
    }
    for (const auto& entry : handles) {
        if (entry.first == recorded) {
            return entry.second == replayed;
        }
        if (entry.second == replayed) {
            return false;
        }
    }
    handles.emplace_back(recorded, replayed);
    return true;
}

// One decoded fixed-layout descriptor: the recorded declared size copied
// over a zeroed full-size image, so appended fields read as zero exactly
// like the engine's own vintage handling.
template <typename Desc>
bool ReadDescInto(ByteReader& r, Desc* out) {
    if (r.formatVersion >= harken::kTraceFormatVersionV3) {
        return ReadPortableDesc(r, out);
    }
    std::memset(out, 0, sizeof(Desc));
    const std::uint32_t declared = r.U32();
    if (r.failed || declared == 0 || declared > sizeof(Desc)) {
        return false;
    }
    const std::uint8_t* p = r.Bytes(declared);
    if (p == nullptr) {
        return false;
    }
    std::memcpy(out, p, declared);
    return true;
}

// fileref: path + recorded content hash. Verifies the file on disk still
// hashes to the journal's value before the consuming call may execute.
struct FileRef {
    std::string path;
    bool pathValid = false;
    std::uint64_t recordedHash = 0;
    bool hashKnown = false;
};

bool ReadFileRef(ByteReader& r, FileRef* out) {
    out->pathValid = r.Str(&out->path);
    out->recordedHash = r.U64();
    std::uint8_t known = 0;
    r.Take(&known, sizeof known);
    out->hashKnown = known != 0;
    return !r.failed;
}

struct ReplayStats {
    std::uint64_t calls = 0;
    std::uint64_t callsNotOk = 0;
    std::uint64_t skippedUnreplayable = 0;
    std::uint64_t blocks = 0;
    std::uint64_t updates = 0;
};

struct RenderCapture {
    std::ofstream file;
    bool toFile = false;
    std::string capturePath;
    std::uint64_t hash = harken::kFnv1a64OffsetBasis;
    std::uint64_t frames = 0;
    float peak = 0.0f;
};

// Content-hash gate: exits the process contract via return value. 0 = ok,
// 4 = refused (report printed).
int VerifyFileRef(const FileRef& ref) {
    if (!ref.pathValid || !ref.hashKnown) {
        // The recorder flags such records unreplayable, so a well-formed
        // journal never reaches here; a hand-damaged one refuses loudly.
        std::fprintf(stderr,
                     "trace-replay: journal call references a file without a "
                     "recorded hash - replay refused\n");
        return 4;
    }
    std::uint64_t actual = 0;
    if (!harken::HashFileContents(ref.path.c_str(), &actual)) {
        std::fprintf(stderr,
                     "trace-replay: cannot read content file '%s' referenced "
                     "by the journal - replay refused\n",
                     ref.path.c_str());
        return 4;
    }
    if (actual != ref.recordedHash) {
        std::fprintf(stderr,
                     "trace-replay: content hash mismatch for '%s': journal "
                     "%016llx, file now %016llx - replay refused (the render "
                     "would differ from the recorded session)\n",
                     ref.path.c_str(),
                     static_cast<unsigned long long>(ref.recordedHash),
                     static_cast<unsigned long long>(actual));
        return 4;
    }
    return 0;
}

// Re-executes one journaled attribution-capture query. The journaled
// capacity is caller-controlled and must never size an allocation
// blindly: a refused-but-journaled call may carry UINT32_MAX. A refused
// argument shape (missing requiredCount, or a row vintage the engine
// does not know) re-executes against a single scratch row — the refusal
// reads at most row 0, so one row reproduces the exact code. A valid
// shape sizes the buffer with a zero-side-effect capacity-0 probe (the
// probe is the documented sizing idiom: never counted, never logged) and
// allocates min(capacity, required) rows, while the ORIGINAL capacity
// still travels to the API so the truncation side effects (counter, log
// record) reproduce byte-exactly; the engine writes min(required,
// capacity) rows, which the probe proved fits. Replay is
// single-threaded, so required cannot move between probe and call.
template <typename Row>
VnResult ReplayCaptureQuery(VnResult (*capture)(Row*, std::uint32_t,
                                                std::uint32_t*),
                            std::uint32_t capacity,
                            std::uint32_t rowStructSize,
                            std::uint32_t hasRequiredCount,
                            std::uint32_t* replayedRequiredCount) {
    std::uint32_t required = 0;
    std::uint32_t* requiredPtr = hasRequiredCount != 0 ? &required : nullptr;
    const bool validShape =
        hasRequiredCount != 0 &&
        (capacity == 0 || rowStructSize == sizeof(Row));
    std::size_t allocRows = 1;
    if (validShape && capacity > 0) {
        std::uint32_t probeRequired = 0;
        (void)capture(nullptr, 0, &probeRequired);
        allocRows = probeRequired < capacity ? probeRequired : capacity;
        if (allocRows == 0) {
            allocRows = 1;
        }
    }
    std::vector<Row> rows(allocRows);
    rows[0].structSize = rowStructSize;
    const VnResult result =
        capture(capacity > 0 ? rows.data() : nullptr, capacity,
                requiredPtr);
    *replayedRequiredCount =
        result == VN_OK && requiredPtr != nullptr
            ? required
            : harken::kTraceRequiredCountUnavailable;
    return result;
}

harken::VariableCallObservation ObserveReplayVariableCall(
    const harken::Engine& engine) {
    return harken::ObserveVariableCallAt(
        engine.variables.CurrentWorldGeneration(),
        engine.variables.CurrentTransportEpoch(), engine.config.blockFrames,
        engine.timelinePool.WindowStartFrame(),
        engine.timelinePool.PublishCutoffFrame());
}

bool WorldGenerationMatches(const WorldGenerationMap& generations,
                            std::uint64_t recorded, std::uint64_t replayed);
bool RecordLogicTicket(LogicTicketMap& tickets, std::uint64_t recorded,
                       std::uint64_t replayed);

bool RememberWorldGeneration(WorldGenerationMap& worlds,
                             std::uint64_t recorded, std::uint64_t replayed) {
    if (recorded == 0 || replayed == 0)
        return recorded == replayed;
    for (const auto& mapping : worlds) {
        if (mapping.first == recorded || mapping.second == replayed)
            return mapping.first == recorded && mapping.second == replayed;
    }
    worlds.emplace_back(recorded, replayed);
    return true;
}

struct ReplayedVariableBatch {
    VnVariableBatchDescV1 desc{};
    VnVariableWriteV1 writes[VN_VARIABLE_BATCH_WRITE_CAPACITY]{};
};

void ReadVariableBatch(ByteReader& r, ReplayedVariableBatch& batch) {
    batch.desc.structSize = r.U32();
    batch.desc.writeCount = r.U32();
    batch.desc.reserved0 = r.U32();
    batch.desc.reserved1 = r.U32();
    const std::uint32_t present = r.U32();
    const std::uint32_t journaled =
        present != 0 &&
                batch.desc.writeCount <= VN_VARIABLE_BATCH_WRITE_CAPACITY
            ? batch.desc.writeCount
            : 0u;
    for (std::uint32_t i = 0; i < journaled; ++i) {
        batch.writes[i].variableId = r.ObjectId();
        batch.writes[i].value = r.F32();
        batch.writes[i].reserved0 = r.U32();
    }
    batch.desc.writes = journaled == 0 ? nullptr : batch.writes;
}

bool MatchVariableAcceptTail(ByteReader& r, LogicTicketMap& logicTickets,
                             WorldGenerationMap& worldGenerations,
                             const harken::VariableAcceptResult& accepted,
                             VnResult* result) {
    const std::uint64_t recordedWorld = r.U64();
    const std::uint64_t recordedTransport = r.U64();
    const std::uint64_t recordedFrontier = r.U64();
    const std::uint64_t recordedRequested = r.U64();
    const std::uint64_t recordedTicket = r.U64();
    const VnResult recordedResult =
        static_cast<VnResult>(static_cast<std::int32_t>(r.U32()));
    const std::uint32_t reserved = r.U32();
    *result = accepted.result;
    return reserved == 0 && accepted.result == recordedResult &&
           RememberWorldGeneration(worldGenerations, recordedWorld,
                                  accepted.observation.world) &&
           recordedTransport == accepted.observation.transport &&
           recordedFrontier ==
               accepted.observation.publicationFrontierFrame &&
           recordedRequested == accepted.observation.requestedEffectiveFrame &&
           RecordLogicTicket(logicTickets, recordedTicket,
                             accepted.ticket.sequence);
}

bool WorldGenerationMatches(const WorldGenerationMap& generations,
                            std::uint64_t recorded,
                            std::uint64_t replayed) {
    for (const auto& mapping : generations) {
        if (mapping.first == recorded && mapping.second == replayed) {
            return true;
        }
    }
    return recorded == 0 && replayed == 0;
}

bool RecordLogicTicket(LogicTicketMap& tickets, std::uint64_t recorded,
                       std::uint64_t replayed) {
    if (recorded == 0 || replayed == 0) {
        return recorded == replayed;
    }
    // Tickets issue in accept order on both sides, so the map stays
    // sorted by construction: a repeated recorded ticket must repeat its
    // established pairing, and a fresh one must extend both monotone
    // sequences.
    if (!tickets.empty()) {
        const auto& last = tickets.back();
        if (recorded == last.first) {
            return replayed == last.second;
        }
        if (recorded < last.first || replayed <= last.second) {
            const auto it = std::lower_bound(
                tickets.begin(), tickets.end(), recorded,
                [](const std::pair<std::uint64_t, std::uint64_t>& entry,
                   std::uint64_t key) { return entry.first < key; });
            return it != tickets.end() && it->first == recorded &&
                   it->second == replayed;
        }
    }
    tickets.emplace_back(recorded, replayed);
    return true;
}

std::uint32_t RemapProbeGeneration(const ProbeGenerationMap& generations,
                                   VnObjectId stableId,
                                   std::uint32_t recorded) {
    for (const ProbeGenerationMapping& mapping : generations) {
        if (mapping.stableId == stableId && mapping.recorded == recorded) {
            return mapping.replayed;
        }
    }
    return recorded;
}

std::uint64_t RemapLogicTicket(const LogicTicketMap& tickets,
                               std::uint64_t recorded) {
    if (recorded == 0) {
        return 0;
    }
    const auto it = std::lower_bound(
        tickets.begin(), tickets.end(), recorded,
        [](const std::pair<std::uint64_t, std::uint64_t>& entry,
           std::uint64_t key) { return entry.first < key; });
    return it != tickets.end() && it->first == recorded ? it->second
                                                        : UINT64_MAX;
}

bool RememberActionCommitIdentity(ByteReader& reader, const harken::Engine& engine,
                                  WorldGenerationMap& worlds, LogicTicketMap& tickets) {
    const auto recordedTicket = reader.U64();
    const auto recordedWorld = reader.U64();
    if (reader.failed || !RecordLogicTicket(tickets, recordedTicket,
                                             engine.actionCommitBundle.Sequence()))
        return false;
    return RememberWorldGeneration(worlds, recordedWorld,
                                    engine.variables.CurrentWorldGeneration());
}

// Executes one journaled API call. Returns 0, or 4 for a content-hash
// refusal, or 1 for a malformed argument stream. The call's own VnResult is
// not a replay error: a call that was refused at record time (backpressure,
// stale handle) deterministically refuses again here, with zero side
// effects both times; non-OK results are only counted for the summary.
int ExecuteCall(std::uint16_t callId, ByteReader& r, BlobStore& blobs,
                 harken::Engine& engine, VoiceHandleMap& voiceHandles,
                 ActionFiringHandleMap& actionFiringHandles,
                 ActionSeekTicketMap& actionSeekTickets,
                 PlayableObservationMap& playableObservations,
                 EmitterHandleMap& emitterHandles,
                 PreviewHandleMap& previewHandles,
                  WorldGenerationMap& worldGenerations,
                 LogicTicketMap& logicTickets, PackHandleMap& packHandles,
                 std::uint64_t baseBlocks,
                 ReplayStats& stats) {
    VnResult result = VN_OK;
    switch (callId) {
    case harken::kTraceCallRegisterPCM:
    case harken::kTraceCallReplacePCM: {
        const VnObjectId soundId = r.ObjectId();
        VnPCMDesc desc{};
        const bool descOk = ReadDescInto(r, &desc);
        std::uint8_t present = 0;
        r.Take(&present, sizeof present);
        if (!descOk || present == 0) {
            return 1; // replayable PCM records always carry their samples
        }
        const std::uint64_t floatCount = r.U64();
        blobs.pcm.emplace_back(static_cast<std::size_t>(floatCount));
        if (r.formatVersion >= harken::kTraceFormatVersionV3) {
            for (float& sample : blobs.pcm.back()) {
                sample = r.F32();
            }
            if (r.failed) return 1;
        } else {
            const std::uint8_t* raw = r.Bytes(
                static_cast<std::size_t>(floatCount) * sizeof(float));
            if (raw == nullptr) return 1;
            std::memcpy(blobs.pcm.back().data(), raw,
                        static_cast<std::size_t>(floatCount) *
                            sizeof(float));
        }
        const float* samples = blobs.pcm.back().data();
        if (desc.layout == VN_PCM_LAYOUT_INTERLEAVED) {
            desc.samplesInterleaved = samples;
            desc.samplesPlanar = nullptr;
        } else {
            blobs.planes.emplace_back();
            std::vector<const float*>& planes = blobs.planes.back();
            planes.resize(desc.channels);
            for (std::uint32_t ch = 0; ch < desc.channels; ++ch) {
                planes[ch] =
                    samples + static_cast<std::size_t>(ch) * desc.frames;
            }
            desc.samplesInterleaved = nullptr;
            desc.samplesPlanar = planes.data();
        }
        result = (callId == harken::kTraceCallRegisterPCM)
                     ? Vn_RegisterPCM(soundId, &desc)
                     : Vn_ReplacePCM(soundId, &desc);
        break;
    }
    case harken::kTraceCallUnregisterPCM:
        result = Vn_UnregisterPCM(r.ObjectId());
        break;
    case harken::kTraceCallStop: {
        const std::uint64_t handle =
            RemapVoiceHandle(voiceHandles, r.U64());
        const std::uint32_t fadeMs = r.U32();
        result = Vn_Stop(handle, fadeMs);
        break;
    }
    case harken::kTraceCallCreateBus: {
        const VnObjectId busId = r.ObjectId();
        const VnObjectId parentId = r.ObjectId();
        result = Vn_CreateBus(busId, parentId);
        break;
    }
    case harken::kTraceCallApplyBusTopology: {
        VnBusTopologyDesc topology{};
        if (r.formatVersion >= harken::kTraceFormatVersionV3) {
            if (r.U32() != 1) return 1;
            topology.structSize = sizeof(VnBusTopologyDesc);
            topology.flags = r.U32();
            topology.ownerId = r.U64();
            topology.baseRevision = r.U64();
            topology.revision = r.U64();
            topology.busCount = r.U32();
            topology.busStructSize = sizeof(VnBusTopologyBus);
            topology.sendCount = r.U32();
            topology.sendStructSize = sizeof(VnBusTopologySend);
        } else {
            topology.structSize = r.U32();
            topology.flags = r.U32();
            topology.ownerId = r.U64();
            topology.baseRevision = r.U64();
            topology.revision = r.U64();
            topology.busCount = r.U32();
            topology.busStructSize = r.U32();
            topology.sendCount = r.U32();
            topology.sendStructSize = r.U32();
        }
        std::vector<VnBusTopologyBus> buses(topology.busCount);
        std::vector<VnBusTopologySend> sends(topology.sendCount);
        if (r.formatVersion >= harken::kTraceFormatVersionV3) {
            for (VnBusTopologyBus& bus : buses) {
                if (r.U32() != 1) return 1;
                bus.structSize = sizeof(VnBusTopologyBus);
                bus.flags = r.U32();
                bus.busId = r.ObjectId();
                bus.parentId = r.ObjectId();
            }
            for (VnBusTopologySend& send : sends) {
                if (r.U32() != 1) return 1;
                send.structSize = sizeof(VnBusTopologySend);
                send.rampMs = r.U32();
                send.fromBusId = r.ObjectId();
                send.toBusId = r.ObjectId();
                send.levelDb = r.F32();
            }
        } else if (!buses.empty()) {
            r.Take(buses.data(),
                   buses.size() * sizeof(VnBusTopologyBus));
        }
        if (r.formatVersion < harken::kTraceFormatVersionV3 && !sends.empty()) {
            r.Take(sends.data(),
                   sends.size() * sizeof(VnBusTopologySend));
        }
        if (!buses.empty()) topology.buses = buses.data();
        if (!sends.empty()) topology.sends = sends.data();
        result = Vn_ApplyBusTopology(&topology);
        break;
    }
    case harken::kTraceCallSetBusVolume: {
        const VnObjectId busId = r.ObjectId();
        const float volumeDb = r.F32();
        const std::uint32_t rampMs = r.U32();
        result = Vn_SetBusVolume(busId, volumeDb, rampMs);
        break;
    }
    case harken::kTraceCallSetBusFilter: {
        const VnObjectId busId = r.ObjectId();
        const std::uint32_t filterKind = r.U32();
        const float cutoffHz = r.F32();
        const float q = r.F32();
        const std::uint32_t enable01 = r.U32();
        const std::uint32_t rampMs = r.U32();
        result = Vn_SetBusFilter(busId, filterKind, cutoffHz, q, enable01,
                                 rampMs);
        break;
    }
    case harken::kTraceCallSetBusOutputFilter: {
        const VnObjectId busId = r.ObjectId();
        const std::uint32_t filterKind = r.U32();
        const float cutoffHz = r.F32();
        const float q = r.F32();
        const std::uint32_t enable01 = r.U32();
        const std::uint32_t rampMs = r.U32();
        result = Vn_SetBusOutputFilter(busId, filterKind, cutoffHz, q,
                                       enable01, rampMs);
        break;
    }
    case harken::kTraceCallSetBusOutputVolume: {
        const VnObjectId busId = r.ObjectId();
        const float volumeDb = r.F32();
        const std::uint32_t rampMs = r.U32();
        result = Vn_SetBusOutputVolume(busId, volumeDb, rampMs);
        break;
    }
    case harken::kTraceCallSetBusMute: {
        const VnObjectId busId = r.ObjectId();
        const std::uint32_t mute01 = r.U32();
        const std::uint32_t fadeMs = r.U32();
        result = Vn_SetBusMute(busId, mute01, fadeMs);
        break;
    }
    case harken::kTraceCallSetBusSolo: {
        const VnObjectId busId = r.ObjectId();
        const std::uint32_t solo01 = r.U32();
        const std::uint32_t fadeMs = r.U32();
        result = Vn_SetBusSolo(busId, solo01, fadeMs);
        break;
    }
    case harken::kTraceCallReparentBus: {
        const VnObjectId busId = r.ObjectId();
        const VnObjectId newParentId = r.ObjectId();
        result = Vn_ReparentBus(busId, newParentId);
        break;
    }
    case harken::kTraceCallDestroyBus: {
        result = Vn_DestroyBus(r.ObjectId());
        break;
    }
    case harken::kTraceCallSetVoiceVolume: {
        const std::uint64_t handle =
            RemapVoiceHandle(voiceHandles, r.U64());
        const float volumeDb = r.F32();
        const std::uint32_t rampMs = r.U32();
        result = Vn_SetVoiceVolume(handle, volumeDb, rampMs);
        break;
    }
    case harken::kTraceCallSetVoiceOutputVolume: {
        const std::uint64_t handle =
            RemapVoiceHandle(voiceHandles, r.U64());
        const float volumeDb = r.F32();
        const std::uint32_t rampMs = r.U32();
        result = Vn_SetVoiceOutputVolume(handle, volumeDb, rampMs);
        break;
    }
    case harken::kTraceCallSetVoiceFilter: {
        const std::uint64_t handle =
            RemapVoiceHandle(voiceHandles, r.U64());
        const std::uint32_t filterKind = r.U32();
        const float cutoffHz = r.F32();
        const float q = r.F32();
        const std::uint32_t enable01 = r.U32();
        const std::uint32_t rampMs = r.U32();
        result = Vn_SetVoiceFilter(handle, filterKind, cutoffHz, q, enable01,
                                   rampMs);
        break;
    }
    case harken::kTraceCallSetVoicePitch: {
        const std::uint64_t handle =
            RemapVoiceHandle(voiceHandles, r.U64());
        const float ratio = r.F32();
        result = Vn_SetVoicePitch(handle, ratio);
        break;
    }
    case harken::kTraceCallSetVoicePosition: {
        const std::uint64_t handle =
            RemapVoiceHandle(voiceHandles, r.U64());
        const VnVec3 position = r.Vec3();
        result = Vn_SetVoicePosition(handle, position);
        break;
    }
    case harken::kTraceCallSetVoiceDistanceScale: {
        const std::uint64_t handle =
            RemapVoiceHandle(voiceHandles, r.U64());
        const float scale = r.F32();
        result = Vn_SetVoiceDistanceScale(handle, scale);
        break;
    }
    case harken::kTraceCallSetVoiceSpatializationMix: {
        const std::uint64_t handle =
            RemapVoiceHandle(voiceHandles, r.U64());
        const float mix = r.F32();
        result = Vn_SetVoiceSpatializationMix(handle, mix);
        break;
    }
    case harken::kTraceCallSetVoicePan: {
        const std::uint64_t handle =
            RemapVoiceHandle(voiceHandles, r.U64());
        const float pan = r.F32();
        result = Vn_SetVoicePan(handle, pan);
        break;
    }
    case harken::kTraceCallSetVoiceWidth: {
        const std::uint64_t handle =
            RemapVoiceHandle(voiceHandles, r.U64());
        const float width = r.F32();
        result = Vn_SetVoiceWidth(handle, width);
        break;
    }
    case harken::kTraceCallCommitVoiceStart: {
        const std::uint64_t handle =
            RemapVoiceHandle(voiceHandles, r.U64());
        result = Vn_CommitVoiceStart(handle);
        break;
    }
    case harken::kTraceCallPreviewPlayPlayable: {
        VnPreviewPlayPlayableDescV2 desc{};
        desc.structSize = r.U32();
        desc.playableKind = r.U32();
        desc.stableId = r.ObjectId();
        desc.emitterHandle = RemapEmitterHandle(emitterHandles, r.U64());
        desc.flags = r.U32();
        desc.reserved0 = r.U32();
        const VnPreviewResult expected = static_cast<VnPreviewResult>(
            static_cast<std::int32_t>(r.U32()));
        const std::uint32_t returnedKind = r.U32();
        const VnPreviewPlayableHandle recordedHandle = r.U64();
        VnPreviewPlayableHandle replayedHandle = 0;
        const VnPreviewResult replayed =
            VnPreview_PlayPlayable(&desc, &replayedHandle);
        if (replayed != expected ||
            (expected == VN_PREVIEW_OK &&
             (returnedKind != desc.playableKind || recordedHandle == 0 ||
              (recordedHandle & (UINT64_C(1) << 63)) == 0 ||
              replayedHandle == 0 ||
              (replayedHandle & (UINT64_C(1) << 63)) == 0)) ||
            (expected != VN_PREVIEW_OK &&
             (returnedKind != 0 || recordedHandle != 0 ||
              replayedHandle != 0))) {
            return 1;
        }
        if (expected == VN_PREVIEW_OK) {
            const auto duplicate = std::find_if(
                previewHandles.begin(), previewHandles.end(),
                [&](const auto& entry) {
                    return entry.first == recordedHandle ||
                           entry.second == replayedHandle;
                });
            if (duplicate != previewHandles.end()) return 1;
            previewHandles.emplace_back(recordedHandle, replayedHandle);
        }
        result = static_cast<VnResult>(replayed);
        break;
    }
    case harken::kTraceCallPreviewResetPlayable: {
        VnPreviewResetPlayableDescV2 desc{};
        desc.structSize = r.U32();
        desc.playableKind = r.U32();
        desc.stableId = r.ObjectId();
        desc.emitterHandle = RemapEmitterHandle(emitterHandles, r.U64());
        desc.flags = r.U32();
        desc.reserved0 = r.U32();
        const VnPreviewResult expected = static_cast<VnPreviewResult>(
            static_cast<std::int32_t>(r.U32()));
        const VnPreviewResult replayed = VnPreview_ResetPlayable(&desc);
        if (replayed != expected) return 1;
        result = static_cast<VnResult>(replayed);
        break;
    }
    case harken::kTraceCallPreviewStopPlayable: {
        const VnPreviewPlayableHandle recordedHandle = r.U64();
        VnPreviewPlayableHandle replayedHandle = recordedHandle;
        for (const auto& entry : previewHandles) {
            if (entry.first == recordedHandle) {
                replayedHandle = entry.second;
                break;
            }
        }
        const std::uint32_t fadeMs = r.U32();
        const VnPreviewResult expected = static_cast<VnPreviewResult>(
            static_cast<std::int32_t>(r.U32()));
        const VnPreviewResult replayed =
            VnPreview_StopPlayable(replayedHandle, fadeMs);
        if (replayed != expected) return 1;
        result = static_cast<VnResult>(replayed);
        break;
    }
    case harken::kTraceCallPreviewQueryPlayable: {
        const VnPreviewPlayableHandle recordedHandle = r.U64();
        VnPreviewPlayableHandle replayedHandle = recordedHandle;
        for (const auto& entry : previewHandles) {
            if (entry.first == recordedHandle) {
                replayedHandle = entry.second;
                break;
            }
        }
        const std::uint32_t callerStructSize = r.U32();
        const VnPreviewResult expected = static_cast<VnPreviewResult>(
            static_cast<std::int32_t>(r.U32()));
        VnPreviewPlayableStateV2 recorded{};
        recorded.structSize = r.U32();
        recorded.playableKind = r.U32();
        recorded.lifecycle = r.U32();
        recorded.reason = r.U32();
        recorded.stableId = r.ObjectId();
        recorded.worldGeneration = r.U64();
        recorded.acceptedLogicTicket = r.U64();
        recorded.lastAppliedBlockSequence = r.U64();
        recorded.childLiveCount = r.U32();
        recorded.childAudibleRetiringCount = r.U32();
        recorded.pendingFutureCount = r.U32();
        recorded.reserved0 = r.U32();
        VnPreviewPlayableStateV2 replayed{};
        std::memset(&replayed, 0xA5, sizeof replayed);
        replayed.structSize = callerStructSize;
        const VnPreviewPlayableStateV2 before = replayed;
        const VnPreviewResult replayedResult =
            VnPreview_QueryPlayable(replayedHandle, &replayed);
        if (replayedResult != expected) return 1;
        if (expected != VN_PREVIEW_OK) {
            const VnPreviewPlayableStateV2 zero{};
            if (std::memcmp(&recorded, &zero, sizeof zero) != 0 ||
                std::memcmp(&replayed, &before, sizeof before) != 0) {
                return 1;
            }
        } else {
            const bool fixedFieldsMatch =
                recorded.structSize == sizeof(recorded) &&
                replayed.structSize == sizeof(replayed) &&
                recorded.playableKind == replayed.playableKind &&
                recorded.lifecycle == replayed.lifecycle &&
                recorded.reason == replayed.reason &&
                recorded.stableId == replayed.stableId &&
                recorded.childLiveCount == replayed.childLiveCount &&
                recorded.childAudibleRetiringCount ==
                    replayed.childAudibleRetiringCount &&
                recorded.pendingFutureCount == replayed.pendingFutureCount &&
                recorded.reserved0 == 0 && replayed.reserved0 == 0;
            const bool worldMatches = WorldGenerationMatches(
                worldGenerations, recorded.worldGeneration,
                replayed.worldGeneration);
            bool ticketMatches =
                recorded.acceptedLogicTicket == 0 &&
                replayed.acceptedLogicTicket == 0;
            if (recorded.acceptedLogicTicket != 0 &&
                replayed.acceptedLogicTicket != 0) {
                ticketMatches = RecordLogicTicket(
                    logicTickets, recorded.acceptedLogicTicket,
                    replayed.acceptedLogicTicket);
            }
            bool blockMatches =
                recorded.lastAppliedBlockSequence == 0 &&
                replayed.lastAppliedBlockSequence == 0;
            if (recorded.lastAppliedBlockSequence != 0 &&
                replayed.lastAppliedBlockSequence != 0 &&
                recorded.lastAppliedBlockSequence >= baseBlocks &&
                replayed.lastAppliedBlockSequence >= baseBlocks) {
                blockMatches =
                    recorded.lastAppliedBlockSequence - baseBlocks ==
                    replayed.lastAppliedBlockSequence - baseBlocks;
            }
            if (!fixedFieldsMatch || !worldMatches || !ticketMatches ||
                !blockMatches) {
                return 1;
            }
        }
        result = static_cast<VnResult>(replayedResult);
        break;
    }
    case harken::kTraceCallLoadCurrentWorld: {
        const std::uint32_t schema = r.U32();
        std::string path;
        const bool pathValid = r.Str(&path);
        const VnResult expected = static_cast<VnResult>(
            static_cast<std::int32_t>(r.U32()));
        VnCurrentWorldLoadReport report{};
        report.structSize = r.U32();
        report.status = r.U32();
        report.result = static_cast<VnResult>(
            static_cast<std::int32_t>(r.U32()));
        report.reserved0 = r.U32();
        report.priorGeneration = r.U64();
        report.resultGeneration = r.U64();
        report.runtimeSemanticFingerprint = r.U64();
        report.capabilityMask = r.U64();
        report.bodyBytes = r.U64();
        report.bodyChecksum = r.U64();
        const auto imageTag =
            static_cast<harken::TraceCurrentWorldImageTagV1>(r.U32());
        const auto disposition =
            static_cast<harken::TraceCurrentWorldCaptureDispositionV1>(
                r.U32());
        const std::uint32_t observedLengthKnown = r.U32();
        const std::uint32_t wholeArtifactHashKnown = r.U32();
        const std::uint64_t observedFileBytes = r.U64();
        const std::uint64_t wholeArtifactHash = r.U64();
        const std::uint64_t capturedBytes = r.U64();
        if (capturedBytes > harken::kCurrentWorldArtifactMaxBodyBytes +
                                harken::kCurrentWorldArtifactHeaderBytes) {
            return 1;
        }
        const std::uint8_t* captured =
            r.Bytes(static_cast<std::size_t>(capturedBytes));
        const bool validUnavailableResult =
            (report.status == VN_CURRENT_WORLD_LOAD_IO_ERROR &&
             expected == VN_CURRENT_WORLD_IO_ERROR) ||
            (report.status == VN_CURRENT_WORLD_LOAD_BACKPRESSURE &&
             expected == VN_BACKPRESSURE) ||
            (report.status == VN_CURRENT_WORLD_LOAD_CAPACITY &&
             expected == VN_CURRENT_WORLD_CAPACITY);
        if (schema != 1 || !pathValid || path.empty() ||
            report.structSize != sizeof(report) ||
            report.result != expected || report.reserved0 != 0 ||
            captured == nullptr ||
            !WorldGenerationMatches(
                worldGenerations, report.priorGeneration,
                engine.variables.CurrentWorldGeneration())) {
            return 1;
        }
        if (imageTag == harken::TraceCurrentWorldImageTagV1::Unavailable) {
            if (report.resultGeneration != 0 ||
                report.runtimeSemanticFingerprint != 0 ||
                report.capabilityMask != 0 || report.bodyBytes != 0 ||
                report.bodyChecksum != 0 ||
                disposition !=
                    harken::TraceCurrentWorldCaptureDispositionV1::None ||
                observedLengthKnown != 0 || wholeArtifactHashKnown != 0 ||
                observedFileBytes != 0 || wholeArtifactHash != 0 ||
                capturedBytes != 0 || !validUnavailableResult) {
                return 1;
            }
            result = expected;
            break;
        }
        if (imageTag == harken::TraceCurrentWorldImageTagV1::RawAttempt &&
            (disposition == harken::TraceCurrentWorldCaptureDispositionV1::
                                HeaderOnlyTooLarge ||
             disposition == harken::TraceCurrentWorldCaptureDispositionV1::
                                HeaderOnlySizeMismatch)) {
            if (observedLengthKnown != 1 || wholeArtifactHashKnown != 0 ||
                wholeArtifactHash != 0 ||
                capturedBytes != harken::kCurrentWorldArtifactHeaderBytes ||
                report.resultGeneration != 0 ||
                report.runtimeSemanticFingerprint != 0 ||
                report.capabilityMask != 0) {
                return 1;
            }
            harken::CurrentWorldArtifactHeaderV1 header{};
            const harken::CurrentWorldLoadStatusV1 replayedStatus =
                harken::InspectCurrentWorldArtifactHeaderV1(
                    captured, static_cast<std::size_t>(capturedBytes),
                    observedFileBytes, header);
            const harken::CurrentWorldLoadStatusV1 dispositionStatus =
                disposition ==
                        harken::TraceCurrentWorldCaptureDispositionV1::
                            HeaderOnlyTooLarge
                    ? harken::CurrentWorldLoadStatusV1::TooLarge
                    : harken::CurrentWorldLoadStatusV1::SizeMismatch;
            if (replayedStatus != dispositionStatus ||
                report.status !=
                    static_cast<VnCurrentWorldLoadStatus>(replayedStatus) ||
                expected != VN_BAD_CURRENT_WORLD ||
                report.bodyBytes != header.bodyBytes ||
                report.bodyChecksum != header.bodyChecksum) {
                return 1;
            }
            result = expected;
            break;
        }
        if (imageTag == harken::TraceCurrentWorldImageTagV1::RawAttempt &&
            disposition ==
                harken::TraceCurrentWorldCaptureDispositionV1::
                    CompleteArtifact) {
            if (observedLengthKnown != 1 || wholeArtifactHashKnown != 1 ||
                capturedBytes > harken::kCurrentWorldArtifactMaxBodyBytes +
                                    harken::kCurrentWorldArtifactHeaderBytes ||
                capturedBytes != observedFileBytes ||
                capturedBytes < harken::kCurrentWorldArtifactHeaderBytes ||
                harken::Fnv1a64(
                    captured, static_cast<std::size_t>(capturedBytes)) !=
                    wholeArtifactHash ||
                report.resultGeneration != 0) {
                return 1;
            }
            harken::CurrentWorldArtifactHeaderV1 header{};
            harken::CurrentWorldLoadStatusV1 replayedStatus =
                harken::InspectCurrentWorldArtifactHeaderV1(
                    captured, static_cast<std::size_t>(capturedBytes),
                    observedFileBytes, header);
            harken::CurrentWorldBodyImage image;
            if (replayedStatus == harken::CurrentWorldLoadStatusV1::Ok) {
                replayedStatus = harken::DecodeCurrentWorldArtifactBodyV1(
                    header,
                    captured + harken::kCurrentWorldArtifactHeaderBytes,
                    static_cast<std::size_t>(capturedBytes) -
                        harken::kCurrentWorldArtifactHeaderBytes,
                    image);
            }
            std::uint64_t replayedFingerprint = 0;
            std::uint64_t replayedCapabilityMask = 0;
            if (replayedStatus == harken::CurrentWorldLoadStatusV1::Ok ||
                replayedStatus ==
                    harken::CurrentWorldLoadStatusV1::SemanticInvalid) {
                replayedFingerprint =
                    image.ProductionView()->identity.runtimeSemanticFingerprint;
                replayedCapabilityMask = image.ProductionView()->identity.capabilityMask;
            }
            if (replayedStatus == harken::CurrentWorldLoadStatusV1::Ok ||
                report.status !=
                    static_cast<VnCurrentWorldLoadStatus>(replayedStatus) ||
                expected != VN_BAD_CURRENT_WORLD ||
                report.bodyBytes != header.bodyBytes ||
                report.bodyChecksum != header.bodyChecksum ||
                report.runtimeSemanticFingerprint != replayedFingerprint ||
                report.capabilityMask != replayedCapabilityMask) {
                return 1;
            }
            result = expected;
            break;
        }
        if (imageTag == harken::TraceCurrentWorldImageTagV1::RawAttempt &&
            (disposition == harken::TraceCurrentWorldCaptureDispositionV1::
                                EndOfFileAfterPrefix ||
             disposition == harken::TraceCurrentWorldCaptureDispositionV1::
                                IoErrorAfterPrefix)) {
            if (observedLengthKnown != 1 || wholeArtifactHashKnown != 0 ||
                wholeArtifactHash != 0 ||
                capturedBytes > harken::kCurrentWorldArtifactMaxBodyBytes +
                                    harken::kCurrentWorldArtifactHeaderBytes ||
                report.resultGeneration != 0 ||
                report.runtimeSemanticFingerprint != 0 ||
                report.capabilityMask != 0) {
                return 1;
            }
            harken::CurrentWorldArtifactHeaderV1 header{};
            harken::CurrentWorldLoadStatusV1 prefixStatus =
                harken::CurrentWorldLoadStatusV1::Truncated;
            if (capturedBytes >= harken::kCurrentWorldArtifactHeaderBytes) {
                prefixStatus = harken::InspectCurrentWorldArtifactHeaderV1(
                    captured, harken::kCurrentWorldArtifactHeaderBytes,
                    observedFileBytes, header);
            }
            const bool endOfFile =
                disposition ==
                harken::TraceCurrentWorldCaptureDispositionV1::
                    EndOfFileAfterPrefix;
            if (!endOfFile &&
                prefixStatus != harken::CurrentWorldLoadStatusV1::Ok &&
                capturedBytes >= harken::kCurrentWorldArtifactHeaderBytes) {
                return 1;
            }
            harken::CurrentWorldLoadStatusV1 replayedStatus =
                endOfFile &&
                    prefixStatus != harken::CurrentWorldLoadStatusV1::Ok
                ? prefixStatus
                : endOfFile ? harken::CurrentWorldLoadStatusV1::Truncated
                            : harken::CurrentWorldLoadStatusV1::IoError;
            if (replayedStatus == harken::CurrentWorldLoadStatusV1::TooLarge ||
                replayedStatus ==
                    harken::CurrentWorldLoadStatusV1::SizeMismatch) {
                return 1;
            }
            if (report.status !=
                    static_cast<VnCurrentWorldLoadStatus>(replayedStatus) ||
                expected !=
                    (replayedStatus == harken::CurrentWorldLoadStatusV1::IoError
                         ? VN_CURRENT_WORLD_IO_ERROR
                         : VN_BAD_CURRENT_WORLD) ||
                report.bodyBytes != header.bodyBytes ||
                report.bodyChecksum != header.bodyChecksum) {
                return 1;
            }
            result = expected;
            break;
        }
        if (imageTag == harken::TraceCurrentWorldImageTagV1::RawAttempt &&
            disposition ==
                harken::TraceCurrentWorldCaptureDispositionV1::
                    CapacityFailureAfterPrefix) {
            if (observedLengthKnown != 1 || wholeArtifactHashKnown != 0 ||
                wholeArtifactHash != 0 ||
                capturedBytes != harken::kCurrentWorldArtifactHeaderBytes ||
                report.resultGeneration != 0 ||
                report.runtimeSemanticFingerprint != 0 ||
                report.capabilityMask != 0 ||
                report.status != VN_CURRENT_WORLD_LOAD_CAPACITY ||
                expected != VN_CURRENT_WORLD_CAPACITY) {
                return 1;
            }
            harken::CurrentWorldArtifactHeaderV1 header{};
            if (harken::InspectCurrentWorldArtifactHeaderV1(
                    captured, static_cast<std::size_t>(capturedBytes),
                    observedFileBytes, header) !=
                    harken::CurrentWorldLoadStatusV1::Ok ||
                report.bodyBytes != header.bodyBytes ||
                report.bodyChecksum != header.bodyChecksum) {
                return 1;
            }
            result = expected;
            break;
        }
        if (imageTag !=
                harken::TraceCurrentWorldImageTagV1::CanonicalV6Body ||
            disposition !=
                harken::TraceCurrentWorldCaptureDispositionV1::CanonicalBody ||
            observedLengthKnown != 1 || wholeArtifactHashKnown != 1 ||
            capturedBytes > harken::kCurrentWorldArtifactMaxBodyBytes ||
            report.bodyBytes != capturedBytes ||
            observedFileBytes !=
                harken::kCurrentWorldArtifactHeaderBytes + capturedBytes) {
            return 1;
        }
        harken::CurrentWorldArtifactHeaderV1 header{};
        header.bodyBytes = report.bodyBytes;
        header.bodyChecksum = report.bodyChecksum;
        header.runtimeSemanticFingerprint =
            report.runtimeSemanticFingerprint;
        header.capabilityMask = report.capabilityMask;
        harken::CurrentWorldBodyImage image;
        if (harken::DecodeCurrentWorldArtifactBodyV1(
                header, captured, static_cast<std::size_t>(capturedBytes),
                image) != harken::CurrentWorldLoadStatusV1::Ok) {
            return 1;
        }
        std::vector<std::uint8_t> artifact;
        if (!harken::EncodeCurrentWorldArtifactV1(*image.ProductionView(), artifact) ||
            artifact.size() != observedFileBytes ||
            std::memcmp(
                artifact.data() + harken::kCurrentWorldArtifactHeaderBytes,
                captured, static_cast<std::size_t>(capturedBytes)) != 0 ||
            harken::Fnv1a64(artifact.data(), artifact.size()) !=
                wholeArtifactHash) {
            return 1;
        }
        const harken::WorldInstallResult replayed =
            harken::InstallContainerWorldV1(
                engine, *image.ProductionView(),
                harken::CurrentWorldTraceOwnerV1::OuterCall98);
        harken::CurrentWorldLoadStatusV1 replayedLoadStatus =
            harken::CurrentWorldLoadStatusV1::SemanticInvalid;
        VnResult replayedResult = VN_BAD_CURRENT_WORLD;
        switch (replayed.status) {
        case harken::WorldInstallStatus::Ok:
            replayedLoadStatus = harken::CurrentWorldLoadStatusV1::Ok;
            replayedResult = VN_OK;
            break;
        case harken::WorldInstallStatus::InvalidFormat:
        case harken::WorldInstallStatus::InvalidWorld:
            break;
        case harken::WorldInstallStatus::Capacity:
            replayedLoadStatus = harken::CurrentWorldLoadStatusV1::Capacity;
            replayedResult = VN_CURRENT_WORLD_CAPACITY;
            break;
        case harken::WorldInstallStatus::CapabilityUnavailable:
            replayedLoadStatus =
                harken::CurrentWorldLoadStatusV1::CapabilityUnavailable;
            replayedResult = VN_CAPABILITY_UNAVAILABLE;
            break;
        case harken::WorldInstallStatus::GenerationExhausted:
            replayedLoadStatus =
                harken::CurrentWorldLoadStatusV1::RuntimeRecreateRequired;
            replayedResult = VN_RUNTIME_RECREATE_REQUIRED;
            break;
        case harken::WorldInstallStatus::Backpressure:
            replayedLoadStatus =
                harken::CurrentWorldLoadStatusV1::Backpressure;
            replayedResult = VN_BACKPRESSURE;
            break;
        }
        if (report.status !=
                static_cast<VnCurrentWorldLoadStatus>(replayedLoadStatus) ||
            expected != replayedResult) {
            return 1;
        }
        if (replayed.status == harken::WorldInstallStatus::Ok) {
            if (report.resultGeneration == 0 ||
                replayed.resultGeneration == 0) {
                return 1;
            }
            worldGenerations.emplace_back(report.resultGeneration,
                                          replayed.resultGeneration);
        } else if (report.resultGeneration != 0 ||
                   replayed.resultGeneration != 0) {
            return 1;
        }
        result = expected;
        break;
    }
    case harken::kTraceCallSetValue: {
        const VnObjectId variableId = r.ObjectId();
        const float value = r.F32();
        if (r.formatVersion < harken::kTraceFormatVersionV4) {
            result = Vn_SetValue(variableId, value);
            break;
        }
        const std::uint64_t recordedWorld = r.U64();
        const std::uint64_t recordedTransport = r.U64();
        const std::uint64_t recordedFrontier = r.U64();
        const std::uint64_t recordedRequested = r.U64();
        const std::uint64_t recordedTicket = r.U64();
        const VnResult recordedResult = static_cast<VnResult>(
            static_cast<std::int32_t>(r.U32()));
        const std::uint32_t reserved = r.U32();
        const harken::VariableCallObservation observation =
            ObserveReplayVariableCall(engine);
        const harken::VariableAcceptResult accepted =
            engine.variables.AcceptGlobal(
                variableId, value, observation, engine.params,
                engine.bindings, engine.busTargets);
        result = accepted.result;
        if (reserved != 0 || result != recordedResult ||
            !RememberWorldGeneration(worldGenerations, recordedWorld,
                                    accepted.observation.world) ||
            recordedTransport != accepted.observation.transport ||
            recordedFrontier !=
                accepted.observation.publicationFrontierFrame ||
            recordedRequested != accepted.observation.requestedEffectiveFrame ||
            !RecordLogicTicket(logicTickets, recordedTicket,
                               accepted.ticket.sequence)) {
            return 1;
        }
        break;
    }
    case harken::kTraceCallCaptureSnapshot: {
        const std::uint32_t structSize = r.U32();
        VnSnapshot scratch{};
        scratch.structSize = structSize;
        result = Vn_CaptureSnapshot(&scratch);
        break;
    }
    case harken::kTraceCallSetListener: {
        const VnVec3 pos = r.Vec3();
        const VnVec3 forward = r.Vec3();
        const VnVec3 up = r.Vec3();
        result = Vn_SetListener(pos, forward, up);
        break;
    }
    case harken::kTraceCallRegisterAttenuation:
    case harken::kTraceCallReplaceAttenuation: {
        const VnObjectId id = r.ObjectId();
        VnAttenuationDesc desc{};
        if (!ReadDescInto(r, &desc)) {
            return 1;
        }
        result = (callId == harken::kTraceCallRegisterAttenuation)
                     ? Vn_RegisterAttenuation(id, &desc)
                     : Vn_ReplaceAttenuation(id, &desc);
        break;
    }
    case harken::kTraceCallBindVoiceAttenuation: {
        const std::uint64_t handle =
            RemapVoiceHandle(voiceHandles, r.U64());
        const VnObjectId attenuationId = r.ObjectId();
        result = Vn_BindVoiceAttenuation(handle, attenuationId);
        break;
    }
    case harken::kTraceCallSetVoiceOrientation: {
        const std::uint64_t handle =
            RemapVoiceHandle(voiceHandles, r.U64());
        const VnVec3 forward = r.Vec3();
        result = Vn_SetVoiceOrientation(handle, forward);
        break;
    }
    case harken::kTraceCallLoadPack:
    case harken::kTraceCallReplacePack: {
        std::uint64_t recordedPackHandle = 0;
        if (callId == harken::kTraceCallReplacePack) {
            recordedPackHandle = r.U64();
        }
        VnPackDesc desc{};
        const bool descOk = ReadDescInto(r, &desc);
        FileRef ref;
        if (!ReadFileRef(r, &ref) || !descOk) {
            return 1;
        }
        const bool vintage2 = r.Remaining() != 0;
        std::uint32_t vintage = 1;
        std::uint64_t packStableId = 0;
        std::uint64_t contentRevision = 0;
        std::uint64_t expectedContentHash = 0;
        std::uint32_t providerTag = 0;
        std::uint32_t reserved0 = 0;
        std::uint32_t closureCount = 0;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> closures;
        VnPackHandle recordedReturnedHandle = 0;
        if (vintage2) {
            vintage = r.U32();
            packStableId = r.U64();
            contentRevision = r.U64();
            expectedContentHash = r.U64();
            providerTag = r.U32();
            reserved0 = r.U32();
            closureCount = r.U32();
            if (vintage != 2 || reserved0 != 0) {
                return 1;
            }
            closures.reserve(closureCount);
            for (std::uint32_t i = 0; i < closureCount; ++i) {
                const std::uint64_t prerequisite = r.U64();
                const std::uint64_t prerequisiteHash = r.U64();
                closures.emplace_back(prerequisite, prerequisiteHash);
            }
            if (callId == harken::kTraceCallLoadPack) {
                recordedReturnedHandle = r.U64();
            }
            (void)r.U64();
            (void)r.U32();
        }
        if (r.failed) {
            return 1;
        }
        if (!vintage2) {
            const int gate = VerifyFileRef(ref);
            if (gate != 0) {
                return gate;
            }
            desc.utf8Path = ref.path.c_str();
            if (callId == harken::kTraceCallLoadPack) {
                VnPackHandle handle = 0;
                result = Vn_LoadPack(&desc, &handle);
            } else {
                result = Vn_ReplacePack(recordedPackHandle, &desc);
            }
            break;
        }
        if (providerTag == 0 || providerTag == 1) {
            const int gate = VerifyFileRef(ref);
            if (gate != 0) {
                return gate;
            }
        }
        if (const auto* published = engine.packComposition.Published()) {
            for (const auto& closure : closures) {
                if (closure.second == 0) {
                    continue;
                }
                bool matched = false;
                if (published->hasCatalog) {
                    for (const auto& row : published->catalog.catalogRows) {
                        if (row.ownerPackStableId == closure.first) {
                            if (row.expectedContentHash != closure.second) {
                                return 4;
                            }
                            matched = true;
                            break;
                        }
                    }
                }
                if (!matched) {
                    bool ownerLoaded = false;
                    for (const auto& owner : published->owners) {
                        if (owner.readiness ==
                                harken::PackOwnerReadiness::Active &&
                            owner.packStableId == closure.first) {
                            ownerLoaded = true;
                            break;
                        }
                    }
                    if (ownerLoaded) {
                        return 4;
                    }
                }
            }
        }
        VnPackLoadDesc load{};
        load.structSize = sizeof(load);
        std::string containmentRoot;
        if (providerTag == 1) {
            load.flags = VN_PACK_LOAD_FLAG_ROOT;
            if (ref.pathValid) {
                std::filesystem::path filePath;
                if (harken::TryMakeUtf8Path(ref.path.c_str(), &filePath)) {
                    containmentRoot = filePath.parent_path().generic_string();
                    if (!containmentRoot.empty()) {
                        load.utf8ContainmentRoot = containmentRoot.c_str();
                    }
                }
            }
        } else if (providerTag == 2) {
            load.flags = VN_PACK_LOAD_FLAG_PROVIDER;
        }
        load.packStableId = packStableId;
        load.utf8Path = ref.pathValid ? ref.path.c_str() : nullptr;
        (void)contentRevision;
        if (callId == harken::kTraceCallLoadPack) {
            VnPackHandle liveHandle = 0;
            result = Vn_LoadPack(reinterpret_cast<const VnPackDesc*>(&load),
                                 &liveHandle);
            if (result == VN_OK && recordedReturnedHandle != 0) {
                packHandles.emplace_back(recordedReturnedHandle, liveHandle);
            }
        } else {
            VnPackHandle liveHandle = 0;
            bool found = false;
            for (const auto& entry : packHandles) {
                if (entry.first == recordedPackHandle) {
                    liveHandle = entry.second;
                    found = true;
                    break;
                }
            }
            if (!found) {
                return 1;
            }
            result = Vn_ReplacePack(
                liveHandle, reinterpret_cast<const VnPackDesc*>(&load));
        }
        break;
    }
    case harken::kTraceCallUnloadPack:
    case harken::kTraceCallUnloadPackEx: {
        const std::uint64_t recordedHandle = r.U64();
        const bool extendedUnload = callId == harken::kTraceCallUnloadPackEx;
        const std::uint32_t unloadFlags = extendedUnload ? r.U32() : 0;
        if (r.Remaining() == 0) {
            result = extendedUnload
                         ? Vn_UnloadPackEx(recordedHandle, unloadFlags)
                         : Vn_UnloadPack(recordedHandle);
            break;
        }
        const std::uint32_t vintage = r.U32();
        (void)r.U64();
        (void)r.U64();
        (void)r.U64();
        (void)r.U32();
        const std::uint32_t reserved0 = r.U32();
        const std::uint32_t closureCount = r.U32();
        if (vintage != 2 || reserved0 != 0) {
            return 1;
        }
        for (std::uint32_t i = 0; i < closureCount; ++i) {
            (void)r.U64();
            (void)r.U64();
        }
        (void)r.U64();
        (void)r.U32();
        if (r.failed) {
            return 1;
        }
        VnPackHandle liveHandle = 0;
        std::size_t foundIndex = packHandles.size();
        for (std::size_t i = 0; i < packHandles.size(); ++i) {
            if (packHandles[i].first == recordedHandle) {
                liveHandle = packHandles[i].second;
                foundIndex = i;
                break;
            }
        }
        if (foundIndex == packHandles.size()) {
            return 1;
        }
        result = extendedUnload
                     ? Vn_UnloadPackEx(liveHandle, unloadFlags)
                     : Vn_UnloadPack(liveHandle);
        if (result == VN_OK) {
            packHandles.erase(packHandles.begin() +
                              static_cast<std::ptrdiff_t>(foundIndex));
        }
        break;
    }
    case harken::kTraceCallSetGameplayCatalog: {
        const VnPackHandle recorded = r.U64();
        const std::uint64_t count = r.U64();
        if (r.failed || count > 1048576) return 1;
        const auto* bytes = r.Bytes(static_cast<std::size_t>(count));
        if (r.failed || r.Remaining() != 0) return 1;
        const auto found = std::find_if(packHandles.begin(), packHandles.end(),
            [recorded](const auto& entry) { return entry.first == recorded; });
        if (found == packHandles.end()) return 1;
        VnPackByteSpan catalog{};
        catalog.structSize = sizeof(catalog);
        catalog.bytes = bytes;
        catalog.byteCount = count;
        result = Vn_SetGameplayCatalog(found->second, &catalog);
        if (result != VN_OK) return 1;
        break;
    }
    case harken::kTraceCallActivateCanonicalPackSetV2: {
        const auto vintage = r.U32();
        const auto recordedInit = r.U64();
        const auto baseRevision = r.U64();
        const auto setRevision = r.U64();
        const auto count = r.U32();
        if (r.failed || vintage != 1 || count == 0 || count > engine.config.maxPacks ||
            count > r.Remaining() / 32) return 1;
        std::vector<VnCanonicalPackSetMemberV2> members(count);
        std::vector<ReplayPackNames> names(count);
        std::vector<VnPackByteSpan> spans(count);
        std::vector<VnCanonicalPackSetMemberResultV1> outputs(count), expected(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            auto& member = members[i];
            member.structSize = sizeof(member);
            member.operation = r.U32();
            member.source.structSize = sizeof(member.source);
            member.source.packStableId = r.U64();
            member.source.expectedHandle = RemapPackHandle(packHandles, r.U64());
            member.source.packKind = r.U32();
            const auto resource = r.U64();
            if (r.failed || member.operation > VN_PACK_MEMBER_NAME_ONLY || resource > blobs.packs.size()) return 1;
            if (member.operation == VN_PACK_MEMBER_KEEP) {
                if (resource != 0) return 1;
            } else {
                if (!names[i].Read(r)) return 1;
                member.names = &names[i].value;
                if (resource != 0) spans[i] = blobs.packs[resource - 1];
                member.source.flags = VN_CANONICAL_PACK_SET_MEMBER_PROVIDER;
                member.source.providerUserData = &spans[i];
                member.source.provide = ReplayPackBytes;
                member.source.release = ReleaseReplayPackBytes;
            }
            outputs[i].structSize = sizeof(outputs[i]);
        }
        const auto recordedResult = static_cast<VnResult>(static_cast<std::int32_t>(r.U32()));
        const auto policy = r.U32();
        const auto priorRevision = r.U64();
        const auto afterRevision = r.U64();
        const auto afterActivation = r.U64();
        const auto afterGeneration = r.U64();
        VnPackHandle outputInit = 0;
        std::uint64_t busGeneration = 0, busFingerprint = 0, profileFingerprint = 0;
        if (recordedResult == VN_OK) {
            outputInit = r.U64();
            busGeneration = r.U64();
            busFingerprint = r.U64();
            profileFingerprint = r.U64();
            for (auto& row : expected) {
                row.packStableId = r.U64();
                row.handle = r.U64();
                row.flags = r.U32();
            }
        }
        if (r.failed || r.Remaining() != 0 || policy > 2 ||
            priorRevision != PackSetRevision(engine)) return 1;
        const auto before = engine.packComposition.PublishedHandle();
        if (policy != 0) {
            if (recordedResult == VN_OK || priorRevision != afterRevision ||
                (policy == 1 && !harken::IsTransientPackTraceRefusal(recordedResult)) ||
                (policy == 2 && recordedResult != VN_PACK_PROVIDER_FAILED && recordedResult != VN_BAD_PACK)) return 1;
            result = recordedResult;
        } else {
            VnCanonicalPackSetActivationDescV2 desc{};
            desc.structSize = sizeof(desc);
            desc.expectedCurrentInitHandle = RemapPackHandle(packHandles, recordedInit);
            desc.baseSetRevision = baseRevision;
            desc.setRevision = setRevision;
            desc.memberCount = count;
            desc.memberStructSize = sizeof(members.front());
            desc.members = members.data();
            desc.outputMemberCapacity = count;
            desc.outputMemberStructSize = sizeof(outputs.front());
            desc.outputMembers = outputs.data();
            VnCanonicalPackSetActivationReportV1 report{};
            report.structSize = sizeof(report);
            result = Vn_ActivateCanonicalPackSetV2(&desc, &report);
            if (result != recordedResult) return 1;
            if (result == VN_OK) {
                if (report.resultSetRevision != afterRevision || report.activationGeneration != afterActivation ||
                    report.busContentGeneration != busGeneration || report.publishedBusFingerprint != busFingerprint ||
                    report.compileTargetProfileFingerprint != profileFingerprint ||
                    !RememberPackHandle(packHandles, outputInit, report.initHandle)) return 1;
                for (std::uint32_t i = 0; i < count; ++i) {
                    if (outputs[i].packStableId != expected[i].packStableId || outputs[i].flags != expected[i].flags ||
                        !RememberPackHandle(packHandles, expected[i].handle, outputs[i].handle)) return 1;
                }
            }
        }
        if ((result != VN_OK && engine.packComposition.PublishedHandle() != before) ||
            !CheckPackPublication(engine, blobs, afterRevision, afterActivation, afterGeneration)) return 1;
        break;
    }
    case harken::kTraceCallUnloadCanonicalPackSet:
    case harken::kTraceCallUnloadCompanionPack: {
        const auto vintage = r.U32();
        const auto handle = RemapPackHandle(packHandles, r.U64());
        VnCanonicalPackSetUnloadDescV1 desc{};
        desc.structSize = sizeof(desc);
        desc.expectedInitHandle = handle;
        const bool wholeSet = callId == harken::kTraceCallUnloadCanonicalPackSet;
        std::uint32_t unloadFlags = 0;
        if (wholeSet) {
            desc.baseSetRevision = r.U64();
            desc.unloadRevision = r.U64();
        } else unloadFlags = r.U32();
        const auto recordedResult = static_cast<VnResult>(static_cast<std::int32_t>(r.U32()));
        const auto priorRevision = r.U64();
        const auto afterRevision = r.U64();
        if (r.failed || r.Remaining() != 0 || vintage != 1 ||
            priorRevision != PackSetRevision(engine) ||
            (unloadFlags & ~VN_PACK_UNLOAD_REFUSE_IF_USED) != 0) return 1;
        const auto before = engine.packComposition.PublishedHandle();
        if (harken::IsTransientPackTraceRefusal(recordedResult)) result = recordedResult;
        else result = wholeSet ? Vn_UnloadCanonicalPackSetV1(&desc) : Vn_UnloadPackEx(handle, unloadFlags);
        if (result != recordedResult || afterRevision != PackSetRevision(engine) ||
            (result != VN_OK && engine.packComposition.PublishedHandle() != before)) return 1;
        break;
    }
    case harken::kTraceCallActivateCanonicalPackSet: {
        const std::uint32_t vintage = r.U32();
        const std::uint32_t descStructSize = r.U32();
        const std::uint32_t descFlags = r.U32();
        const VnPackHandle expectedCurrentInitHandle = r.U64();
        const std::uint64_t baseSetRevision = r.U64();
        const std::uint64_t setRevision = r.U64();
        const std::uint32_t memberCount = r.U32();
        const std::uint32_t memberStructSize = r.U32();
        if (r.failed || vintage != 1 || descFlags != 0 || memberCount == 0 ||
            descStructSize != sizeof(VnCanonicalPackSetActivationDescV1) ||
            memberStructSize != sizeof(VnCanonicalPackSetMemberV1)) {
            return 1;
        }
        std::vector<VnCanonicalPackSetMemberV1> members(memberCount);
        std::vector<std::string> memberPaths(memberCount);
        std::vector<std::string> memberRoots(memberCount);
        for (std::uint32_t i = 0; i < memberCount; ++i) {
            VnCanonicalPackSetMemberV1 member{};
            member.structSize = r.U32();
            member.flags = r.U32();
            member.packStableId = r.U64();
            member.expectedHandle = r.U64();
            member.packKind = r.U32();
            member.reserved0 = r.U32();
            const std::uint32_t providerTag = r.U32();
            const std::uint32_t present = r.U32();
            FileRef ref;
            if (!ReadFileRef(r, &ref) || r.failed ||
                member.structSize != sizeof(VnCanonicalPackSetMemberV1) ||
                member.reserved0 != 0 || providerTag > 2 || present > 1) {
                return 1;
            }
            const bool provider =
                (member.flags & VN_CANONICAL_PACK_SET_MEMBER_PROVIDER) != 0;
            const bool root =
                (member.flags & VN_CANONICAL_PACK_SET_MEMBER_ROOT) != 0;
            const std::uint32_t expectedTag = provider ? 2u : (root ? 1u : 0u);
            if (providerTag != expectedTag) {
                return 1;
            }
            if (present == 1) {
                const int gate = VerifyFileRef(ref);
                if (gate != 0) {
                    return gate;
                }
                if (!ref.pathValid) {
                    return 1;
                }
                memberPaths[i] = ref.path;
                member.utf8Path = memberPaths[i].c_str();
                if (root) {
                    std::filesystem::path filePath;
                    if (harken::TryMakeUtf8Path(ref.path.c_str(), &filePath)) {
                        memberRoots[i] = filePath.parent_path().generic_string();
                        if (!memberRoots[i].empty()) {
                            member.utf8ContainmentRoot = memberRoots[i].c_str();
                        }
                    }
                }
            } else if (providerTag != 2 && ref.pathValid) {
                return 1;
            }
            members[i] = member;
        }
        const VnResult recordedResult =
            static_cast<VnResult>(static_cast<std::int32_t>(r.U32()));
        const std::uint32_t replayClass = r.U32();
        if (r.failed || recordedResult != VN_OK || replayClass != 0) {
            return 1;
        }
        const std::uint32_t reportStructSize = r.U32();
        const std::uint32_t reportFlags = r.U32();
        const VnPackHandle recordedInitHandle = r.U64();
        const std::uint64_t priorSetRevision = r.U64();
        const std::uint64_t resultSetRevision = r.U64();
        const std::uint64_t activationGeneration = r.U64();
        const std::uint64_t busContentGeneration = r.U64();
        const std::uint64_t publishedBusFingerprint = r.U64();
        const std::uint64_t compileTargetProfileFingerprint = r.U64();
        const std::uint32_t outputCount = r.U32();
        const std::uint32_t reportReserved0 = r.U32();
        if (r.failed ||
            reportStructSize != sizeof(VnCanonicalPackSetActivationReportV1) ||
            reportFlags != 0 || outputCount != memberCount ||
            reportReserved0 != 0) {
            return 1;
        }
        std::vector<std::uint32_t> recordedOutputFlags(memberCount);
        std::vector<std::uint64_t> recordedOutputIds(memberCount);
        std::vector<VnPackHandle> recordedOutputHandles(memberCount);
        for (std::uint32_t i = 0; i < memberCount; ++i) {
            recordedOutputFlags[i] = r.U32();
            recordedOutputIds[i] = r.U64();
            recordedOutputHandles[i] = r.U64();
            if (r.failed) {
                return 1;
            }
        }
        if (r.Remaining() != 0) {
            return 1;
        }
        (void)priorSetRevision;
        (void)resultSetRevision;
        (void)activationGeneration;
        (void)busContentGeneration;
        (void)publishedBusFingerprint;
        (void)compileTargetProfileFingerprint;
        VnPackHandle liveExpectedInit = expectedCurrentInitHandle;
        if (expectedCurrentInitHandle != 0) {
            bool foundExpected = false;
            for (const auto& entry : packHandles) {
                if (entry.first == expectedCurrentInitHandle) {
                    liveExpectedInit = entry.second;
                    foundExpected = true;
                    break;
                }
            }
            if (!foundExpected) {
                return 1;
            }
        }
        for (auto& member : members) {
            if (member.expectedHandle == 0) {
                continue;
            }
            bool foundMemberExpected = false;
            for (const auto& entry : packHandles) {
                if (entry.first == member.expectedHandle) {
                    member.expectedHandle = entry.second;
                    foundMemberExpected = true;
                    break;
                }
            }
            if (!foundMemberExpected) {
                return 1;
            }
        }
        std::vector<VnCanonicalPackSetMemberResultV1> outputs(memberCount);
        for (auto& row : outputs) {
            row.structSize = sizeof(row);
        }
        VnCanonicalPackSetActivationDescV1 desc{};
        desc.structSize = sizeof(desc);
        desc.expectedCurrentInitHandle = liveExpectedInit;
        desc.baseSetRevision = baseSetRevision;
        desc.setRevision = setRevision;
        desc.memberCount = memberCount;
        desc.memberStructSize = sizeof(VnCanonicalPackSetMemberV1);
        desc.members = members.data();
        desc.outputMemberCapacity = memberCount;
        desc.outputMemberStructSize = sizeof(VnCanonicalPackSetMemberResultV1);
        desc.outputMembers = outputs.data();
        VnCanonicalPackSetActivationReportV1 report{};
        report.structSize = sizeof(report);
        result = Vn_ActivateCanonicalPackSetV1(&desc, &report);
        if (result != VN_OK) {
            return 1;
        }
        auto rememberPack = [&](VnPackHandle recorded, VnPackHandle live) {
            if (recorded == 0 || live == 0) {
                return recorded == 0 && live == 0;
            }
            for (const auto& entry : packHandles) {
                if (entry.first == recorded) {
                    return entry.second == live;
                }
            }
            packHandles.emplace_back(recorded, live);
            return true;
        };
        if (!rememberPack(recordedInitHandle, report.initHandle)) {
            return 1;
        }
        for (std::uint32_t i = 0; i < memberCount; ++i) {
            if (outputs[i].packStableId != recordedOutputIds[i] ||
                outputs[i].flags != recordedOutputFlags[i]) {
                return 1;
            }
            if (!rememberPack(recordedOutputHandles[i], outputs[i].handle)) {
                return 1;
            }
        }
        break;
    }
    case harken::kTraceCallSetVoiceEffectBypass: {
        const std::uint64_t handle =
            RemapVoiceHandle(voiceHandles, r.U64());
        const std::uint32_t slot = r.U32();
        const std::uint32_t bypass01 = r.U32();
        result = Vn_SetVoiceEffectBypass(handle, slot, bypass01);
        break;
    }
    case harken::kTraceCallSetVoiceEffect:
    case harken::kTraceCallSetBusEffect: {
        VnVoiceHandle voice = 0;
        VnBusId bus{};
        if (callId == harken::kTraceCallSetVoiceEffect)
            voice = RemapVoiceHandle(voiceHandles, r.U64());
        else
            bus = r.ObjectId();
        const std::uint32_t slot = r.U32();
        const std::uint32_t type = r.U32();
        // The concrete effect descriptor travels as declared bytes under the
        // format cap; a fixed local image holds any current descriptor type.
        alignas(8) std::uint8_t image[harken::kTraceMaxDescBytes] = {};
        if (r.formatVersion >= harken::kTraceFormatVersionV3) {
            std::size_t nativeBytes = 0;
            std::uint32_t paramCount = 0;
            switch (type) {
            case VN_EFFECT_FEEDBACK_DELAY:
                nativeBytes = sizeof(VnFeedbackDelayDesc);
                paramCount = 4;
                break;
            case VN_EFFECT_THREE_BAND_TONE:
                nativeBytes = sizeof(VnThreeBandToneDesc);
                paramCount = 7;
                break;
            case VN_EFFECT_GAIN:
                nativeBytes = sizeof(VnGainDesc);
                paramCount = 1;
                break;
            case VN_EFFECT_LIMITER:
                nativeBytes = sizeof(VnLimiterDesc);
                paramCount = 2;
                break;
            case VN_EFFECT_COMPRESSOR:
                nativeBytes = sizeof(VnCompressorDesc);
                paramCount = 6;
                break;
            default:
                return 1;
            }
            if (r.U32() != 2) return 1;
            VnEffectDesc header{};
            header.structSize = static_cast<std::uint32_t>(nativeBytes);
            header.reserved0 = r.U32();
            header.effectId = r.ObjectId();
            std::memcpy(image, &header, sizeof(header));
            for (std::uint32_t parameter = 0; parameter < paramCount; ++parameter) {
                const std::uint32_t bits = r.U32();
                std::memcpy(image + sizeof(header) + parameter * 4, &bits, 4);
            }
            if (r.failed) return 1;
        } else {
            const std::uint32_t declared = r.U32();
            if (r.failed || declared == 0 || declared > sizeof(image)) {
                return 1;
            }
            const std::uint8_t* p = r.Bytes(declared);
            if (p == nullptr) {
                return 1;
            }
            std::memcpy(image, p, declared);
        }
        const VnEffectDesc* desc =
            reinterpret_cast<const VnEffectDesc*>(image);
        result = (callId == harken::kTraceCallSetVoiceEffect)
                     ? Vn_SetVoiceEffect(voice, slot, type, desc)
                     : Vn_SetBusEffect(bus, slot, type, desc);
        break;
    }
    case harken::kTraceCallClearVoiceEffect: {
        const std::uint64_t handle =
            RemapVoiceHandle(voiceHandles, r.U64());
        const std::uint32_t slot = r.U32();
        result = Vn_ClearVoiceEffect(handle, slot);
        break;
    }
    case harken::kTraceCallSetVoiceEffectParam: {
        const std::uint64_t handle =
            RemapVoiceHandle(voiceHandles, r.U64());
        const std::uint32_t slot = r.U32();
        const std::uint32_t paramIndex = r.U32();
        const float value = r.F32();
        result = Vn_SetVoiceEffectParam(handle, slot, paramIndex, value);
        break;
    }
    case harken::kTraceCallRegisterStreamFile:
    case harken::kTraceCallReplaceStreamFile: {
        const VnObjectId soundId = r.ObjectId();
        FileRef ref;
        if (!ReadFileRef(r, &ref)) {
            return 1;
        }
        const int gate = VerifyFileRef(ref);
        if (gate != 0) {
            return gate;
        }
        result = (callId == harken::kTraceCallRegisterStreamFile)
                     ? Vn_RegisterStreamFile(soundId, ref.path.c_str())
                     : Vn_ReplaceStreamFile(soundId, ref.path.c_str());
        break;
    }
    case harken::kTraceCallUnregisterStreamFile:
        result = Vn_UnregisterStreamFile(r.ObjectId());
        break;
    case harken::kTraceCallCreateEmitter:
    case harken::kTraceCallCreateEmitterEx: {
        VnEmitterHandle handle = 0;
        if (callId == harken::kTraceCallCreateEmitterEx) {
            VnEmitterCreateDescV1 desc{};
            desc.structSize = sizeof(desc);
            desc.flags = r.U32();
            result = Vn_CreateEmitterEx(&desc, &handle);
        } else {
            result = Vn_CreateEmitter(&handle);
        }
        if (r.formatVersion >= harken::kTraceFormatVersionV4) {
            const std::uint64_t recordedOrdinal = r.U64();
            if (result == VN_OK) {
                if (recordedOrdinal != emitterHandles.size() + 1u) {
                    return 1;
                }
                emitterHandles.push_back(handle);
            } else if (recordedOrdinal != 0) {
                return 1;
            }
        }
        break;
    }
    case harken::kTraceCallDestroyEmitter: {
        const std::uint64_t recorded = r.U64();
        result = Vn_DestroyEmitter(
            r.formatVersion >= harken::kTraceFormatVersionV4
                ? RemapEmitterHandle(emitterHandles, recorded)
                : recorded);
        break;
    }
    case harken::kTraceCallSetEmitterPosition: {
        const std::uint64_t recorded = r.U64();
        const std::uint64_t handle =
            r.formatVersion >= harken::kTraceFormatVersionV4
                ? RemapEmitterHandle(emitterHandles, recorded)
                : recorded;
        const VnVec3 position = r.Vec3();
        result = Vn_SetEmitterPosition(handle, position);
        break;
    }
    case harken::kTraceCallSetEmitterOrientation: {
        const std::uint64_t recorded = r.U64();
        const std::uint64_t handle =
            r.formatVersion >= harken::kTraceFormatVersionV4
                ? RemapEmitterHandle(emitterHandles, recorded)
                : recorded;
        const VnVec3 forward = r.Vec3();
        result = Vn_SetEmitterOrientation(handle, forward);
        break;
    }
    case harken::kTraceCallStopEmitter: {
        const std::uint64_t recorded = r.U64();
        const std::uint64_t handle =
            r.formatVersion >= harken::kTraceFormatVersionV4
                ? RemapEmitterHandle(emitterHandles, recorded)
                : recorded;
        const std::uint32_t fadeMs = r.U32();
        result = Vn_StopEmitter(handle, fadeMs);
        break;
    }
    case harken::kTraceCallSetEmitterValue: {
        const std::uint64_t recordedHandle = r.U64();
        const std::uint64_t handle =
            r.formatVersion >= harken::kTraceFormatVersionV4
                ? RemapEmitterHandle(emitterHandles, recordedHandle)
                : recordedHandle;
        const VnObjectId variableId = r.ObjectId();
        const float value = r.F32();
        if (r.formatVersion < harken::kTraceFormatVersionV4) {
            result = Vn_SetEmitterValue(handle, variableId, value);
            break;
        }
        const std::uint32_t baseReserved = r.U32();
        const std::uint64_t recordedWorld = r.U64();
        const std::uint64_t recordedTransport = r.U64();
        const std::uint64_t recordedFrontier = r.U64();
        const std::uint64_t recordedRequested = r.U64();
        const std::uint64_t recordedTicket = r.U64();
        const VnResult recordedResult = static_cast<VnResult>(
            static_cast<std::int32_t>(r.U32()));
        const std::uint32_t tailReserved = r.U32();
        const harken::VariableCallObservation observation =
            ObserveReplayVariableCall(engine);
        const harken::VariableAcceptResult accepted =
            engine.variables.AcceptEmitter(
                handle, variableId, value, observation, engine.emitters,
                engine.params);
        result = accepted.result;
        if (baseReserved != 0 || tailReserved != 0 ||
            result != recordedResult ||
            !RememberWorldGeneration(worldGenerations, recordedWorld,
                                    accepted.observation.world) ||
            recordedTransport != accepted.observation.transport ||
            recordedFrontier !=
                accepted.observation.publicationFrontierFrame ||
            recordedRequested != accepted.observation.requestedEffectiveFrame ||
            !RecordLogicTicket(logicTickets, recordedTicket,
                               accepted.ticket.sequence)) {
            return 1;
        }
        break;
    }
    case harken::kTraceCallClearEmitterValue:
        return 1;
    case harken::kTraceCallSetValues:
    case harken::kTraceCallResetValues: {
        ReplayedVariableBatch batch;
        ReadVariableBatch(r, batch);
        harken::VariableAcceptResult accepted{};
        if (batch.desc.structSize != sizeof(VnVariableBatchDescV1) ||
            batch.desc.reserved0 != 0 || batch.desc.reserved1 != 0) {
            accepted.result = VN_INVALID_ARG;
        } else if (callId == harken::kTraceCallResetValues) {
            accepted = engine.variables.AcceptGlobalResetBatch(
                batch.desc.writes, batch.desc.writeCount,
                ObserveReplayVariableCall(engine), engine.params,
                engine.bindings, engine.busTargets);
        } else {
            accepted = engine.variables.AcceptGlobalBatch(
                batch.desc.writes, batch.desc.writeCount,
                ObserveReplayVariableCall(engine), engine.params,
                engine.bindings, engine.busTargets);
        }
        if (!MatchVariableAcceptTail(r, logicTickets, worldGenerations,
                                     accepted, &result)) {
            return 1;
        }
        break;
    }
    case harken::kTraceCallSetEmitterValues: {
        const std::uint64_t handle =
            RemapEmitterHandle(emitterHandles, r.U64());
        ReplayedVariableBatch batch;
        ReadVariableBatch(r, batch);
        harken::VariableAcceptResult accepted{};
        if (batch.desc.structSize != sizeof(VnVariableBatchDescV1) ||
            batch.desc.reserved0 != 0 || batch.desc.reserved1 != 0) {
            accepted.result = VN_INVALID_ARG;
        } else {
            accepted = engine.variables.AcceptEmitterBatch(
                handle, batch.desc.writes, batch.desc.writeCount,
                ObserveReplayVariableCall(engine), engine.emitters,
                engine.params);
        }
        if (!MatchVariableAcceptTail(r, logicTickets, worldGenerations,
                                     accepted, &result)) {
            return 1;
        }
        break;
    }
    case harken::kTraceCallClearEmitterValues:
        return 1;
    case harken::kTraceCallQueryVariableTicket: {
        VnVariableTicketQueryDescV1 desc{};
        desc.structSize = r.U32();
        desc.reserved0 = r.U32();
        desc.ticket.sequence = r.U64();
        desc.ticket.worldGeneration = r.U64();
        desc.ticket.transportGeneration = r.U64();
        const VnResult expected = static_cast<VnResult>(
            static_cast<std::int32_t>(r.U32()));
        const std::uint32_t recordedOutSize = r.U32();
        const std::uint32_t recordedStatus = r.U32();
        const std::uint32_t recordedOutcome = r.U32();
        const std::uint32_t recordedReserved = r.U32();
        if (expected == VN_INVALID_ARG) {
            result = VN_INVALID_ARG;
            break;
        }
        const std::uint64_t replayedSequence =
            RemapLogicTicket(logicTickets, desc.ticket.sequence);
        // An unissued sequence stays unknown even if its raw number happens
        // to name a replay-side ticket. Transport epochs replay identically.
        desc.ticket.sequence =
            replayedSequence == UINT64_MAX ? 0 : replayedSequence;
        const auto world = std::find_if(
            worldGenerations.begin(), worldGenerations.end(),
            [&](const auto& mapping) {
                return mapping.first == desc.ticket.worldGeneration;
            });
        if (world == worldGenerations.end()) {
            desc.ticket.sequence = 0;
        } else {
            desc.ticket.worldGeneration = world->second;
        }
        VnVariableTicketQueryResultV1 out{};
        out.structSize = sizeof(out);
        result = Vn_QueryVariableTicket(&desc, &out);
        if (result != expected || recordedOutSize != sizeof(out) ||
            recordedReserved != 0 || out.status != recordedStatus ||
            out.outcome != recordedOutcome) {
            return 1;
        }
        break;
    }
    case harken::kTraceCallQueryVariable: {
        VnVariableQueryDescV1 desc{};
        desc.structSize = r.U32();
        desc.flags = r.U32();
        desc.variableId = r.ObjectId();
        const std::uint64_t recordedEmitter = r.U64();
        desc.reserved0 = r.U64();
        desc.emitter = RemapEmitterHandle(emitterHandles, recordedEmitter);
        const VnResult expected = static_cast<VnResult>(
            static_cast<std::int32_t>(r.U32()));
        const std::uint32_t recordedOutSize = r.U32();
        const std::uint32_t recordedKind = r.U32();
        const std::uint32_t recordedScope = r.U32();
        const std::uint32_t recordedWriters = r.U32();
        const float recordedMin = r.F32();
        const float recordedMax = r.F32();
        const float recordedDefault = r.F32();
        const float recordedCurrent = r.F32();
        const std::uint32_t recordedOverride = r.U32();
        const std::uint32_t recordedPending = r.U32();
        const std::uint64_t recordedFingerprint = r.U64();
        if (expected == VN_INVALID_ARG) {
            result = VN_INVALID_ARG;
            break;
        }
        VnVariableQueryResultV1 out{};
        out.structSize = sizeof(out);
        result = Vn_QueryVariable(&desc, &out);
        if (result != expected || recordedOutSize != sizeof(out) ||
            (result == VN_OK &&
             (out.kind != recordedKind || out.scope != recordedScope ||
              out.writers != recordedWriters ||
              out.rangeMin != recordedMin || out.rangeMax != recordedMax ||
              out.defaultValue != recordedDefault ||
              out.currentValue != recordedCurrent ||
              out.hasEmitterOverride != recordedOverride ||
              out.pendingCount != recordedPending ||
              out.executionFingerprint != recordedFingerprint))) {
            return 1;
        }
        break;
    }
    case harken::kTraceCallReservedLegacyContainerRegister:
    case harken::kTraceCallReservedLegacyContainerUnregister:
    case harken::kTraceCallReservedLegacyContainerReplace:
    case harken::kTraceCallReservedLegacyDirectPlay:
    case harken::kTraceCallReservedLegacyActionRegister:
    case harken::kTraceCallReservedLegacyActionUnregister:
    case harken::kTraceCallReservedLegacyActionTrigger:
    case harken::kTraceCallReservedLegacyActionFindByName:
    case harken::kTraceCallReservedLegacyActionTriggerByName:
    case harken::kTraceCallReservedLegacyActionReplace:
    case harken::kTraceCallReservedLegacyActionTriggerEx:
    case harken::kTraceCallReservedLegacyActionStop:
    case harken::kTraceCallReservedLegacyActionInstanceState:
        return 1;
    case harken::kTraceCallPostAction: {
        const auto version = r.U32();
        if (version != 2) {
            return 1;
        }
        VnPostActionDesc desc{};
        desc.structSize = r.U32();
        desc.flags = r.U32();
        desc.actionId = r.ObjectId();
        desc.emitter = RemapEmitterHandle(emitterHandles, r.U64());
        desc.callerSeed = r.U64();
        desc.reserved0 = r.U64();
        const VnResult expected = static_cast<VnResult>(
            static_cast<std::int32_t>(r.U32()));
        const VnActionFiringHandle recordedFiring = r.U64();
        if (r.failed || desc.structSize != sizeof(VnPostActionDesc) ||
            desc.flags != 0 || harken::IsNull(desc.actionId) ||
            desc.reserved0 != 0) {
            return 1;
        }
        VnActionFiringHandle replayedFiring = 0;
        result = Vn_PostAction(&desc, &replayedFiring);
        if (result != expected) {
            return 1;
        }
        if (result == VN_OK) {
            if (version == 2 && !RememberActionCommitIdentity(r, engine, worldGenerations, logicTickets))
                return 1;
            if (!RememberActionFiringHandle(actionFiringHandles,
                                            recordedFiring,
                                            replayedFiring)) {
                return 1;
            }
        } else if (recordedFiring != 0 || replayedFiring != 0) {
            return 1;
        }
        break;
    }
    case harken::kTraceCallPostActionByName: {
        const auto version = r.U32();
        if (version != 2) {
            return 1;
        }
        VnPostActionByNameDesc desc{};
        desc.structSize = r.U32();
        desc.flags = r.U32();
        std::string name;
        const bool nameValid = r.Str(&name);
        const std::uint8_t* projectGuid = r.Bytes(16);
        if (projectGuid == nullptr) {
            return 1;
        }
        std::memcpy(desc.projectGuid, projectGuid, 16);
        desc.emitter = RemapEmitterHandle(emitterHandles, r.U64());
        desc.callerSeed = r.U64();
        desc.reserved0 = r.U64();
        desc.utf8Name = nameValid ? name.c_str() : nullptr;
        const VnResult expected = static_cast<VnResult>(
            static_cast<std::int32_t>(r.U32()));
        const VnActionFiringHandle recordedFiring = r.U64();
        if (r.failed || desc.structSize != sizeof(desc) || desc.flags != 0 ||
            desc.reserved0 != 0) {
            return 1;
        }
        VnActionFiringHandle replayedFiring = 0;
        result = Vn_PostActionByName(&desc, &replayedFiring);
        if (result != expected) {
            return 1;
        }
        if (result == VN_OK) {
            if (version == 2 && !RememberActionCommitIdentity(r, engine, worldGenerations, logicTickets))
                return 1;
            if (!RememberActionFiringHandle(actionFiringHandles,
                                            recordedFiring,
                                            replayedFiring)) {
                return 1;
            }
        } else if (recordedFiring != 0 || replayedFiring != 0) {
            return 1;
        }
        break;
    }
    case harken::kTraceCallResolveActionByName: {
        if (r.U32() != 1) {
            return 1;
        }
        VnPostActionByNameDesc desc{};
        desc.structSize = r.U32();
        desc.flags = r.U32();
        std::string name;
        const bool nameValid = r.Str(&name);
        const std::uint8_t* projectGuid = r.Bytes(16);
        if (projectGuid == nullptr) {
            return 1;
        }
        std::memcpy(desc.projectGuid, projectGuid, 16);
        desc.emitter = RemapEmitterHandle(emitterHandles, r.U64());
        desc.callerSeed = r.U64();
        desc.reserved0 = r.U64();
        desc.utf8Name = nameValid ? name.c_str() : nullptr;
        const VnResult expected = static_cast<VnResult>(
            static_cast<std::int32_t>(r.U32()));
        if (r.failed || desc.structSize != sizeof(desc) || desc.flags != 0 ||
            desc.reserved0 != 0) {
            return 1;
        }
        VnActionResolved resolved{};
        resolved.structSize = sizeof(resolved);
        result = Vn_ResolveActionByName(&desc, &resolved);
        if (result != expected) {
            return 1;
        }
        if (expected == VN_OK) {
            const VnObjectId recordedActionId = r.ObjectId();
            if (r.failed || resolved.actionId != recordedActionId) {
                return 1;
            }
        }
        break;
    }
    case harken::kTraceCallPostActionPinned: {
        const auto version = r.U32();
        if (version != 2) {
            return 1;
        }
        VnActionPinnedDesc desc{};
        desc.structSize = r.U32();
        desc.flags = r.U32();
        const std::uint8_t* projectGuid = r.Bytes(16);
        if (projectGuid == nullptr) {
            return 1;
        }
        std::memcpy(desc.projectGuid, projectGuid, 16);
        desc.actionId = r.ObjectId();
        std::string name;
        const bool nameValid = r.Str(&name);
        desc.nameBindingRevision = r.U64();
        desc.callableFingerprint = r.U64();
        desc.emitter = RemapEmitterHandle(emitterHandles, r.U64());
        desc.callerSeed = r.U64();
        desc.reserved0 = r.U64();
        desc.utf8ExpectedName = nameValid ? name.c_str() : nullptr;
        const VnResult expected = static_cast<VnResult>(
            static_cast<std::int32_t>(r.U32()));
        const VnActionFiringHandle recordedFiring = r.U64();
        if (r.failed || desc.structSize != sizeof(desc) || desc.flags != 0 ||
            desc.reserved0 != 0 || harken::IsNull(desc.actionId)) {
            return 1;
        }
        VnActionFiringHandle replayedFiring = 0;
        result = Vn_PostActionPinned(&desc, &replayedFiring);
        if (result != expected) {
            return 1;
        }
        if (result == VN_OK) {
            if (version == 2 && !RememberActionCommitIdentity(r, engine, worldGenerations, logicTickets))
                return 1;
            if (!RememberActionFiringHandle(actionFiringHandles,
                                            recordedFiring,
                                            replayedFiring)) {
                return 1;
            }
        } else if (recordedFiring != 0 || replayedFiring != 0) {
            return 1;
        }
        break;
    }
    case harken::kTraceCallPostActionResolved: {
        const std::uint32_t version = r.U32();
        if (version != 2) {
            return 1;
        }
        VnActionResolved resolved{};
        resolved.structSize = r.U32();
        resolved.reserved0 = r.U32();
        resolved.actionId = r.ObjectId();
        resolved.nameBindingRevision = r.U64();
        resolved.callableFingerprint = r.U64();
        resolved.publicationGeneration = r.U64();
        const std::uint8_t* projectGuid = r.Bytes(16);
        if (projectGuid == nullptr) {
            return 1;
        }
        std::memcpy(resolved.projectGuid, projectGuid, 16);
        const VnEmitterHandle emitter =
            RemapEmitterHandle(emitterHandles, r.U64());
        const std::uint64_t callerSeed = r.U64();
        if (version == 2) {
            const std::uint64_t recordedGeneration = r.U64();
            // Preserve whether the token was current, independently of earlier sessions.
            resolved.publicationGeneration = recordedGeneration != 0 &&
                    resolved.publicationGeneration == recordedGeneration
                ? engine.packComposition.CompositeGeneration() : 0;
        }
        const VnResult expected = static_cast<VnResult>(
            static_cast<std::int32_t>(r.U32()));
        const VnActionFiringHandle recordedFiring = r.U64();
        if (r.failed || resolved.structSize != sizeof(resolved) ||
            resolved.reserved0 != 0 || harken::IsNull(resolved.actionId)) {
            return 1;
        }
        VnActionFiringHandle replayedFiring = 0;
        result = Vn_PostActionResolved(&resolved, emitter, callerSeed,
                                       &replayedFiring);
        if (result != expected) {
            return 1;
        }
        if (result == VN_OK) {
            if (version == 2 && !RememberActionCommitIdentity(r, engine, worldGenerations, logicTickets))
                return 1;
            if (!RememberActionFiringHandle(actionFiringHandles,
                                            recordedFiring,
                                            replayedFiring)) {
                return 1;
            }
        } else if (recordedFiring != 0 || replayedFiring != 0) {
            return 1;
        }
        break;
    }
    case harken::kTraceCallSeekActionFiring: {
        if (r.U32() != 1) {
            return 1;
        }
        VnActionSeekDescV1 desc{};
        desc.structSize = r.U32();
        desc.flags = r.U32();
        const VnActionFiringHandle recordedFiring = r.U64();
        desc.positionMs = r.F64();
        desc.mode = r.U32();
        desc.reserved0 = r.U32();
        const VnResult expected = static_cast<VnResult>(
            static_cast<std::int32_t>(r.U32()));
        const VnActionSeekTicket recordedTicket = r.U64();
        if (r.failed || desc.structSize != sizeof(desc) || desc.flags != 0 ||
            desc.reserved0 != 0 ||
            desc.mode > VN_ACTION_SEEK_RELATIVE ||
            !TryRemapActionFiringHandle(actionFiringHandles, recordedFiring,
                                        &desc.firing)) {
            return 1;
        }
        VnActionSeekTicket replayedTicket = 0;
        result = Vn_SeekActionFiring(&desc, &replayedTicket);
        if (result != expected) {
            return 1;
        }
        if (result == VN_OK) {
            if (!RememberActionSeekTicket(actionSeekTickets, recordedTicket,
                                          replayedTicket)) {
                return 1;
            }
        } else if (recordedTicket != 0 || replayedTicket != 0) {
            return 1;
        }
        break;
    }
    case harken::kTraceCallQueryActionSeek: {
        if (r.U32() != 1) {
            return 1;
        }
        const VnActionSeekTicket recordedInputTicket = r.U64();
        const std::uint32_t outputStructSize = r.U32();
        const VnResult expected = static_cast<VnResult>(
            static_cast<std::int32_t>(r.U32()));
        VnActionSeekInfoV1 recorded{};
        recorded.structSize = r.U32();
        recorded.state = r.U32();
        recorded.result = static_cast<VnResult>(
            static_cast<std::int32_t>(r.U32()));
        recorded.reserved0 = r.U32();
        recorded.firing = r.U64();
        recorded.targetCount = r.U32();
        recorded.appliedCount = r.U32();
        recorded.cancelledCount = r.U32();
        recorded.reserved1 = r.U32();
        recorded.acceptedLogicSequence = r.U64();
        recorded.appliedAudioBlockSequence = r.U64();
        recorded.minimumAppliedSourceFrame = r.U64();
        recorded.maximumAppliedSourceFrame = r.U64();
        VnActionSeekTicket replayedInputTicket = 0;
        if (r.failed || outputStructSize != sizeof(VnActionSeekInfoV1) ||
            recorded.structSize != sizeof(VnActionSeekInfoV1) ||
            recorded.state > VN_ACTION_SEEK_CANCELLED ||
            recorded.reserved0 != 0 || recorded.reserved1 != 0 ||
            !TryRemapActionSeekTicket(actionSeekTickets, recordedInputTicket,
                                      &replayedInputTicket)) {
            return 1;
        }
        VnActionSeekInfoV1 replayed{};
        replayed.structSize = sizeof(replayed);
        result = Vn_QueryActionSeek(replayedInputTicket, &replayed);
        VnActionFiringHandle replayedFiring = 0;
        if (result != expected ||
            !TryRemapActionFiringHandle(actionFiringHandles, recorded.firing,
                                        &replayedFiring) ||
            recorded.structSize != replayed.structSize ||
            recorded.state != replayed.state ||
            recorded.result != replayed.result ||
            replayedFiring != replayed.firing ||
            recorded.targetCount != replayed.targetCount ||
            recorded.appliedCount != replayed.appliedCount ||
            recorded.cancelledCount != replayed.cancelledCount ||
            recorded.acceptedLogicSequence != replayed.acceptedLogicSequence ||
            recorded.appliedAudioBlockSequence !=
                replayed.appliedAudioBlockSequence ||
            recorded.minimumAppliedSourceFrame !=
                replayed.minimumAppliedSourceFrame ||
            recorded.maximumAppliedSourceFrame !=
                replayed.maximumAppliedSourceFrame) {
            return 1;
        }
        break;
    }
    case harken::kTraceCallReleaseActionSeek: {
        if (r.U32() != 1) {
            return 1;
        }
        const VnActionSeekTicket recordedTicket = r.U64();
        const VnResult expected = static_cast<VnResult>(
            static_cast<std::int32_t>(r.U32()));
        VnActionSeekTicket replayedTicket = 0;
        if (r.failed || !TryRemapActionSeekTicket(
                            actionSeekTickets, recordedTicket,
                            &replayedTicket)) {
            return 1;
        }
        result = Vn_ReleaseActionSeek(replayedTicket);
        if (result != expected) {
            return 1;
        }
        break;
    }
    case harken::kTraceCallStopActionFiring: {
        if (r.U32() != 1) {
            return 1;
        }
        const VnActionFiringHandle recordedFiring = r.U64();
        const std::uint32_t fadeMs = r.U32();
        const VnResult expected = static_cast<VnResult>(
            static_cast<std::int32_t>(r.U32()));
        VnActionFiringHandle replayedFiring = 0;
        if (r.failed || !TryRemapActionFiringHandle(
                            actionFiringHandles, recordedFiring,
                            &replayedFiring)) {
            return 1;
        }
        result = Vn_StopActionFiring(replayedFiring, fadeMs);
        if (result != expected) {
            return 1;
        }
        break;
    }
    case harken::kTraceCallQueryActionFiring: {
        if (r.U32() != 1) {
            return 1;
        }
        const VnActionFiringHandle recordedInputFiring = r.U64();
        const std::uint32_t outputStructSize = r.U32();
        const VnResult expected = static_cast<VnResult>(
            static_cast<std::int32_t>(r.U32()));
        VnActionFiringInfo recorded{};
        recorded.structSize = r.U32();
        recorded.state = r.U32();
        recorded.outcome = r.U32();
        recorded.terminalReason = r.U32();
        recorded.actionId = r.ObjectId();
        recorded.worldGeneration = r.U64();
        recorded.acceptedLogicSequence = r.U64();
        recorded.parentFiring = r.U64();
        recorded.directStepCount = r.U32();
        recorded.directStepTerminalCount = r.U32();
        recorded.childFiringCount = r.U32();
        recorded.livePlayableCount = r.U32();
        VnActionFiringHandle replayedInputFiring = 0;
        if (r.failed || outputStructSize != sizeof(VnActionFiringInfo) ||
            recorded.structSize != sizeof(VnActionFiringInfo) ||
            recorded.state > VN_ACTION_FIRING_TERMINAL_RETAINED ||
            recorded.outcome > VN_ACTION_OUTCOME_CANCELLED ||
            recorded.terminalReason > VN_ACTION_TERMINAL_RUNTIME_ERROR ||
            !TryRemapActionFiringHandle(
                actionFiringHandles, recordedInputFiring,
                &replayedInputFiring)) {
            return 1;
        }
        VnActionFiringInfo replayed{};
        replayed.structSize = sizeof(replayed);
        result = Vn_QueryActionFiring(replayedInputFiring, &replayed);
        if (result != expected) {
            return 1;
        }
        VnActionFiringHandle replayedParent = 0;
        if (!TryRemapActionFiringHandle(actionFiringHandles,
                                        recorded.parentFiring,
                                        &replayedParent) ||
            recorded.structSize != replayed.structSize ||
            recorded.state != replayed.state ||
            recorded.outcome != replayed.outcome ||
            recorded.terminalReason != replayed.terminalReason ||
            recorded.actionId != replayed.actionId ||
            recorded.worldGeneration != replayed.worldGeneration ||
            recorded.acceptedLogicSequence !=
                replayed.acceptedLogicSequence ||
            replayedParent != replayed.parentFiring ||
            recorded.directStepCount != replayed.directStepCount ||
            recorded.directStepTerminalCount !=
                replayed.directStepTerminalCount ||
            recorded.childFiringCount != replayed.childFiringCount ||
            recorded.livePlayableCount != replayed.livePlayableCount) {
            return 1;
        }
        break;
    }
    case harken::kTraceCallQueryActionStep: {
        if (r.U32() != 1) {
            return 1;
        }
        const VnActionFiringHandle recordedInputFiring = r.U64();
        const std::uint32_t authoredEventIndex = r.U32();
        const std::uint32_t outputStructSize = r.U32();
        const VnResult expected = static_cast<VnResult>(
            static_cast<std::int32_t>(r.U32()));
        VnActionStepResult recorded{};
        recorded.structSize = r.U32();
        recorded.state = r.U32();
        recorded.result = static_cast<VnResult>(
            static_cast<std::int32_t>(r.U32()));
        recorded.opcode = r.U32();
        recorded.ownerActionId = r.ObjectId();
        recorded.targetStableId = r.ObjectId();
        recorded.logicCommitSequence = r.U64();
        recorded.childFiring = r.U64();
        recorded.producedPlayableInstance = r.U64();
        recorded.authoredEventIndex = r.U32();
        recorded.executionOrdinal = r.U32();
        recorded.depth = r.U32();
        recorded.targetKind = r.U32();
        recorded.capturedPlayableCount = r.U32();
        recorded.remainingPlayableCount = r.U32();
        VnActionFiringHandle replayedInputFiring = 0;
        if (r.failed || outputStructSize != sizeof(VnActionStepResult) ||
            recorded.structSize != sizeof(VnActionStepResult) ||
            recorded.state > VN_ACTION_STEP_AWAITING_MATERIALIZATION ||
            recorded.opcode > VN_ACTION_OPCODE_RESET ||
            recorded.targetKind > VN_ACTION_TARGET_BUS ||
            !TryRemapActionFiringHandle(
                actionFiringHandles, recordedInputFiring,
                &replayedInputFiring)) {
            return 1;
        }
        VnActionStepResult replayed{};
        replayed.structSize = sizeof(replayed);
        result = Vn_QueryActionStep(replayedInputFiring,
                                    authoredEventIndex, &replayed);
        if (result != expected ||
            !ObserveIdentityMapping(actionFiringHandles,
                                    recorded.childFiring,
                                    replayed.childFiring) ||
            !ObserveIdentityMapping(playableObservations,
                                    recorded.producedPlayableInstance,
                                    replayed.producedPlayableInstance) ||
            recorded.structSize != replayed.structSize ||
            recorded.state != replayed.state ||
            recorded.result != replayed.result ||
            recorded.opcode != replayed.opcode ||
            recorded.ownerActionId != replayed.ownerActionId ||
            recorded.targetStableId != replayed.targetStableId ||
            recorded.logicCommitSequence != replayed.logicCommitSequence ||
            recorded.authoredEventIndex != replayed.authoredEventIndex ||
            recorded.executionOrdinal != replayed.executionOrdinal ||
            recorded.depth != replayed.depth ||
            recorded.targetKind != replayed.targetKind ||
            recorded.capturedPlayableCount !=
                replayed.capturedPlayableCount ||
            recorded.remainingPlayableCount !=
                replayed.remainingPlayableCount) {
            return 1;
        }
        break;
    }
    case harken::kTraceCallReleaseActionFiring: {
        if (r.U32() != 1) {
            return 1;
        }
        const VnActionFiringHandle recordedFiring = r.U64();
        const VnResult expected = static_cast<VnResult>(
            static_cast<std::int32_t>(r.U32()));
        VnActionFiringHandle replayedFiring = 0;
        if (r.failed || !TryRemapActionFiringHandle(
                            actionFiringHandles, recordedFiring,
                            &replayedFiring)) {
            return 1;
        }
        result = Vn_ReleaseActionFiring(replayedFiring);
        if (result != expected) {
            return 1;
        }
        break;
    }
    case harken::kTraceCallStopAll:
        result = Vn_StopAll(r.U32());
        break;
    case harken::kTraceCallConnectBusSend: {
        const VnObjectId fromBus = r.ObjectId();
        const VnObjectId toBus = r.ObjectId();
        const float levelDb = r.F32();
        result = Vn_ConnectBusSend(fromBus, toBus, levelDb);
        break;
    }
    case harken::kTraceCallDisconnectBusSend: {
        const VnObjectId fromBus = r.ObjectId();
        const VnObjectId toBus = r.ObjectId();
        result = Vn_DisconnectBusSend(fromBus, toBus);
        break;
    }
    case harken::kTraceCallSetBusSendLevel: {
        const VnObjectId fromBus = r.ObjectId();
        const VnObjectId toBus = r.ObjectId();
        const float levelDb = r.F32();
        const std::uint32_t rampMs = r.U32();
        result = Vn_SetBusSendLevel(fromBus, toBus, levelDb, rampMs);
        break;
    }
    case harken::kTraceCallSetVoiceSend: {
        const std::uint64_t handle =
            RemapVoiceHandle(voiceHandles, r.U64());
        const std::uint32_t sendSlot = r.U32();
        const VnObjectId toBus = r.ObjectId();
        const float levelDb = r.F32();
        result = Vn_SetVoiceSend(handle, sendSlot, toBus, levelDb);
        break;
    }
    case harken::kTraceCallClearVoiceSend: {
        const std::uint64_t handle =
            RemapVoiceHandle(voiceHandles, r.U64());
        const std::uint32_t sendSlot = r.U32();
        result = Vn_ClearVoiceSend(handle, sendSlot);
        break;
    }
    case harken::kTraceCallSetVoiceSendLevel: {
        const std::uint64_t handle =
            RemapVoiceHandle(voiceHandles, r.U64());
        const std::uint32_t sendSlot = r.U32();
        const float levelDb = r.F32();
        const std::uint32_t rampMs = r.U32();
        result = Vn_SetVoiceSendLevel(handle, sendSlot, levelDb, rampMs);
        break;
    }
    case harken::kTraceCallClearBusEffect: {
        const VnObjectId busId = r.ObjectId();
        const std::uint32_t slot = r.U32();
        result = Vn_ClearBusEffect(busId, slot);
        break;
    }
    case harken::kTraceCallSetBusEffectParam: {
        const VnObjectId busId = r.ObjectId();
        const std::uint32_t slot = r.U32();
        const std::uint32_t paramIndex = r.U32();
        const float value = r.F32();
        result = Vn_SetBusEffectParam(busId, slot, paramIndex, value);
        break;
    }
    case harken::kTraceCallSetBusEffectBypass: {
        const VnObjectId busId = r.ObjectId();
        const std::uint32_t slot = r.U32();
        const std::uint32_t bypass01 = r.U32();
        result = Vn_SetBusEffectBypass(busId, slot, bypass01);
        break;
    }
    case harken::kTraceCallSetBusEffectDynamicRoute: {
        const VnObjectId targetBusId = r.ObjectId();
        const std::uint32_t effectSlot = r.U32();
        const std::uint32_t routeSlot = r.U32();
        VnEffectDynamicRouteDesc desc{};
        if (!ReadDescInto(r, &desc)) {
            return 1;
        }
        if (desc.sourceKind == VN_DYNAMIC_SOURCE_VOICE) {
            VnVoiceHandle replayedSource = 0;
            if (!TryRemapVoiceHandle(
                    voiceHandles,
                    desc.sourceVoiceHandle,
                    &replayedSource)) {
                std::fprintf(
                    stderr,
                    "trace-replay: Dynamic Route voice source handle "
                    "%llu has no recorded-to-replayed mapping - replay "
                    "refused\n",
                    static_cast<unsigned long long>(desc.sourceVoiceHandle));
                return 1;
            }
            desc.sourceVoiceHandle = replayedSource;
        }
        result = Vn_SetBusEffectDynamicRoute(
            targetBusId, effectSlot, routeSlot, &desc);
        break;
    }
    case harken::kTraceCallClearBusEffectDynamicRoute: {
        const VnObjectId targetBusId = r.ObjectId();
        const std::uint32_t effectSlot = r.U32();
        const std::uint32_t routeSlot = r.U32();
        const std::uint32_t expectedEffectType = r.U32();
        result = Vn_ClearBusEffectDynamicRoute(
            targetBusId, effectSlot, routeSlot, expectedEffectType);
        break;
    }
    case harken::kTraceCallResetVoiceEffect: {
        const VnVoiceHandle handle =
            RemapVoiceHandle(voiceHandles, r.U64());
        const std::uint32_t effectSlot = r.U32();
        const std::uint32_t expectedEffectType = r.U32();
        result = Vn_ResetVoiceEffect(handle, effectSlot,
                                     expectedEffectType);
        break;
    }
    case harken::kTraceCallResetBusEffect: {
        const VnObjectId busId = r.ObjectId();
        const std::uint32_t effectSlot = r.U32();
        const std::uint32_t expectedEffectType = r.U32();
        result = Vn_ResetBusEffect(busId, effectSlot,
                                   expectedEffectType);
        break;
    }
    case harken::kTraceCallSetBusFaderPoint: {
        const VnObjectId busId = r.ObjectId();
        const std::uint32_t faderAfterSlot = r.U32();
        result = Vn_SetBusFaderPoint(busId, faderAfterSlot);
        break;
    }
    case harken::kTraceCallSetBusOutputLayout: {
        const VnObjectId busId = r.ObjectId();
        const std::uint32_t layout = r.U32();
        result = Vn_SetBusOutputLayout(busId, layout);
        break;
    }
    case harken::kTraceCallSetBusDownmixMatrix: {
        const VnObjectId busId = r.ObjectId();
        VnDownmixMatrixDesc desc{};
        if (!ReadDescInto(r, &desc)) {
            return 1;
        }
        result = Vn_SetBusDownmixMatrix(busId, &desc);
        break;
    }
    case harken::kTraceCallResetBusDownmixMatrix: {
        const VnObjectId busId = r.ObjectId();
        result = Vn_ResetBusDownmixMatrix(busId);
        break;
    }
    case harken::kTraceCallRegisterBinding:
    case harken::kTraceCallReplaceBinding: {
        const VnObjectId bindingId = r.ObjectId();
        VnBindingDesc desc{};
        if (!ReadDescInto(r, &desc)) {
            return 1;
        }
        result = (callId == harken::kTraceCallRegisterBinding)
                     ? Vn_RegisterBinding(bindingId, &desc)
                     : Vn_ReplaceBinding(bindingId, &desc);
        break;
    }
    case harken::kTraceCallUnregisterBinding:
        result = Vn_UnregisterBinding(r.ObjectId());
        break;
    case harken::kTraceCallAttachBinding:
    case harken::kTraceCallDetachBinding: {
        const VnObjectId bindingId = r.ObjectId();
        VnBindingAttachDesc desc{};
        if (!ReadDescInto(r, &desc)) {
            return 1;
        }
        result = (callId == harken::kTraceCallAttachBinding)
                     ? Vn_AttachBinding(bindingId, &desc)
                     : Vn_DetachBinding(bindingId, &desc);
        break;
    }
    case harken::kTraceCallApplyVoiceBinding:
    case harken::kTraceCallClearVoiceBinding: {
        const std::uint64_t handle =
            RemapVoiceHandle(voiceHandles, r.U64());
        const VnObjectId bindingId = r.ObjectId();
        result = (callId == harken::kTraceCallApplyVoiceBinding)
                     ? Vn_ApplyVoiceBinding(handle, bindingId)
                     : Vn_ClearVoiceBinding(handle, bindingId);
        break;
    }
    case harken::kTraceCallRemoved65:
    case harken::kTraceCallRemoved66:
    case harken::kTraceCallRemoved67:
    case harken::kTraceCallRemoved74:
        return 1;
    case harken::kTraceCallCapturePropertyTargets:
    case harken::kTraceCallCapturePropertyBindings:
    case harken::kTraceCallCaptureEffectCapabilities:
    case harken::kTraceCallCaptureEffectParameterCapabilities:
    case harken::kTraceCallCaptureEffectDynamicCapabilities:
    case harken::kTraceCallCaptureEffectDynamicParameterCapabilities:
    case harken::kTraceCallCaptureEffectDynamicRoutes:
    case harken::kTraceCallCaptureCompressorRuntimeInfo: {
        // Queries re-execute so their validation and any observable query
        // side effects are reproduced. Out-rows are recomputed rather than
        // journaled; tagged-vintage records compare VnResult and the
        // normalized requiredCount unconditionally. The journaled shape
        // reconstructs refused calls exactly — a zero rowStructSize under a nonzero
        // capacity reproduces the buffer refusal, a zero hasRequiredCount
        // the NULL-requiredCount refusal — and the buffer sizing never
        // trusts the journaled capacity (see ReplayCaptureQuery).
        const std::uint32_t capacity = r.U32();
        const std::uint32_t rowStructSize = r.U32();
        const std::uint32_t hasRequiredCount = r.U32();
        std::uint32_t replayedRequired =
            harken::kTraceRequiredCountUnavailable;
        if (callId == harken::kTraceCallCapturePropertyTargets) {
            result = ReplayCaptureQuery<VnPropertyTargetInfo>(
                &Vn_CapturePropertyTargets, capacity, rowStructSize,
                hasRequiredCount, &replayedRequired);
        } else if (callId ==
                   harken::kTraceCallCapturePropertyBindings) {
            result = ReplayCaptureQuery<VnPropertyBindingInfo>(
                &Vn_CapturePropertyBindings, capacity, rowStructSize,
                hasRequiredCount, &replayedRequired);
        } else if (callId ==
                   harken::kTraceCallCaptureEffectCapabilities) {
            result = ReplayCaptureQuery<VnEffectCapability>(
                &Vn_CaptureEffectCapabilities, capacity, rowStructSize,
                hasRequiredCount, &replayedRequired);
        } else if (callId ==
                   harken::kTraceCallCaptureEffectParameterCapabilities) {
            result = ReplayCaptureQuery<VnEffectParameterCapability>(
                &Vn_CaptureEffectParameterCapabilities, capacity,
                rowStructSize, hasRequiredCount, &replayedRequired);
        } else if (callId ==
                   harken::kTraceCallCaptureEffectDynamicCapabilities) {
            result = ReplayCaptureQuery<VnEffectDynamicCapability>(
                &Vn_CaptureEffectDynamicCapabilities, capacity,
                rowStructSize, hasRequiredCount, &replayedRequired);
        } else if (
            callId ==
            harken::kTraceCallCaptureEffectDynamicParameterCapabilities) {
            result =
                ReplayCaptureQuery<VnEffectDynamicParameterCapability>(
                    &Vn_CaptureEffectDynamicParameterCapabilities,
                    capacity, rowStructSize, hasRequiredCount,
                    &replayedRequired);
        } else if (callId ==
                   harken::kTraceCallCaptureEffectDynamicRoutes) {
            result = ReplayCaptureQuery<VnEffectDynamicRouteInfo>(
                &Vn_CaptureEffectDynamicRoutes, capacity, rowStructSize,
                hasRequiredCount, &replayedRequired);
        } else {
            result = ReplayCaptureQuery<VnCompressorRuntimeInfo>(
                &Vn_CaptureCompressorRuntimeInfo, capacity,
                rowStructSize, hasRequiredCount, &replayedRequired);
        }
        const bool resultTailRequired =
            callId ==
                harken::kTraceCallCaptureEffectDynamicCapabilities ||
            callId ==
                harken::kTraceCallCaptureEffectDynamicParameterCapabilities ||
            callId == harken::kTraceCallCaptureEffectDynamicRoutes ||
            callId == harken::kTraceCallCaptureCompressorRuntimeInfo;
        if (r.Remaining() == 0 && resultTailRequired) {
            std::fprintf(
                stderr,
                "trace-replay: missing mandatory capture-result tail for "
                "call %u - replay refused\n",
                static_cast<unsigned>(callId));
            return 1;
        }
        if (r.Remaining() != 0) {
            if (r.Remaining() !=
                harken::kTraceCaptureResultTailBytes) {
                std::fprintf(
                    stderr,
                    "trace-replay: malformed capture-result tail for "
                    "call %u - replay refused\n",
                    static_cast<unsigned>(callId));
                return 1;
            }
            const std::uint32_t feature = r.U32();
            const std::uint32_t vintage = r.U32();
            const VnResult recordedResult =
                static_cast<VnResult>(
                    static_cast<std::int32_t>(r.U32()));
            const std::uint32_t recordedRequired = r.U32();
            if (feature != harken::kTraceCaptureResultFeature ||
                vintage != harken::kTraceCaptureResultVintage) {
                std::fprintf(
                    stderr,
                    "trace-replay: unknown capture-result feature/"
                    "vintage for call %u - replay refused\n",
                    static_cast<unsigned>(callId));
                return 1;
            }
            if (result != recordedResult) {
                std::fprintf(
                    stderr,
                    "trace-replay: capture result mismatch for call %u "
                    "(recorded %d, replayed %d) - replay refused\n",
                    static_cast<unsigned>(callId),
                    static_cast<int>(recordedResult),
                    static_cast<int>(result));
                return 1;
            }
            if (replayedRequired != recordedRequired) {
                std::fprintf(
                    stderr,
                    "trace-replay: capture requiredCount mismatch for "
                    "call %u (recorded %u, replayed %u) - replay "
                    "refused\n",
                    static_cast<unsigned>(callId),
                    static_cast<unsigned>(recordedRequired),
                    static_cast<unsigned>(replayedRequired));
                return 1;
            }
        }
        break;
    }
    default:
        // A future recorder vintage: this replayer cannot interpret the
        // call, and skipping it would silently change the render.
        std::fprintf(stderr,
                     "trace-replay: unknown call id %u in journal - replay "
                     "refused (journal is from a newer recorder)\n",
                     static_cast<unsigned>(callId));
        return 1;
    }
    if (r.failed) {
        return 1;
    }
    if (r.Remaining() != 0) {
        const bool pairedCall =
            callId == harken::kTraceCallPreviewPlayPlayable ||
            callId == harken::kTraceCallPostAction;
        if (!pairedCall ||
            r.Remaining() != harken::kTracePreviewPlayPairedTailBytes) {
            return 1;
        }
        (void)r.U64();
        (void)r.U64();
        (void)r.U64();
        (void)r.U64();
        (void)r.U64();
        (void)r.U64();
        (void)r.U32();
        if (r.failed || r.Remaining() != 0) {
            return 1;
        }
    }
    ++stats.calls;
    if (result != VN_OK) {
        ++stats.callsNotOk;
    }
    return 0;
}

void PumpOneBlock(harken::Engine* engine, RenderCapture& capture) {
    engine->nullBackend.Pump(1);
    const float* block = engine->nullBackend.LastBlock();
    const std::uint32_t frames = engine->nullBackend.BlockFrames();
    const std::uint32_t channels = engine->nullBackend.Channels();
    const std::size_t bytes =
        static_cast<std::size_t>(frames) * channels * sizeof(float);
    capture.hash = harken::Fnv1a64(block, bytes, capture.hash);
    capture.frames += frames;
    for (std::size_t i = 0;
         i < static_cast<std::size_t>(frames) * channels; ++i) {
        const float a = block[i] < 0.0f ? -block[i] : block[i];
        if (a > capture.peak) {
            capture.peak = a;
        }
    }
    if (capture.toFile) {
        capture.file.write(reinterpret_cast<const char*>(block),
                           static_cast<std::streamsize>(bytes));
    }
}

} // namespace

void PrintTraceReplayUsage() {
    std::fprintf(
        stderr,
        "\ntrace-replay re-executes a session journal recorded by\n"
        "Vn_TraceStart on the null backend with the recorded config, seed\n"
        "and block/tick schedule, and reports an FNV-1a 64 hash of the\n"
        "rendered output. The same journal renders byte-identically on\n"
        "every replay.\n"
        "  --capture <file>  also write the raw render (interleaved f32,\n"
        "                    little-endian) for byte comparison\n"
        "  --null            accepted for symmetry; replay always runs the\n"
        "                    null backend\n"
        "exit codes: 0 = ordinary deterministic replay; 1 = usage/file/\n"
        "journal error; 2 = engine init failure; 3 = LIMITED deterministic\n"
        "replay (the journal carries callback-context API calls, replayed\n"
        "after their enclosing update returns - render captured, but not\n"
        "ordinary proof of the recorded interleaving); 4 = refused, a\n"
        "content file's hash no longer matches the journal (the report\n"
        "names the file). Exit 0 may be complete or a crash-safe partial\n"
        "prefix; read the completion= summary line. Replay execution and\n"
        "device-format identity are independent; read the\n"
        "device_format_identity= summary line before treating a real-device\n"
        "trace as render-identity evidence.\n");
}

int RunTraceReplay(int argc, char** argv) {
    const char* tracePath = nullptr;
    const char* capturePath = nullptr;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--null") == 0) {
            continue; // replay is null-backend by definition
        }
        if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            capturePath = argv[++i];
            continue;
        }
        if (argv[i][0] != '-' && tracePath == nullptr) {
            tracePath = argv[i];
            continue;
        }
        std::fprintf(stderr, "error: unrecognized trace-replay argument "
                             "'%s'\n", argv[i]);
        PrintTraceReplayUsage();
        return 1;
    }
    if (tracePath == nullptr) {
        std::fprintf(stderr, "error: trace-replay needs a journal file\n");
        PrintTraceReplayUsage();
        return 1;
    }

    // Load the whole journal; traces are logic-side API journals, small
    // next to the content they reference.
    std::filesystem::path traceFsPath;
    if (!harken::TryMakeUtf8Path(tracePath, &traceFsPath)) {
        std::fprintf(stderr, "error: trace path '%s' is not valid UTF-8\n",
                     tracePath);
        return 1;
    }
    std::ifstream in(traceFsPath, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "error: cannot open trace '%s'\n", tracePath);
        return 1;
    }
    std::vector<std::uint8_t> journal(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    ByteReader r;
    r.data = journal.data();
    r.size = journal.size();

    char magic[4] = {};
    r.Take(magic, sizeof magic);
    if (r.failed || std::memcmp(magic, harken::kTraceMagic, 4) != 0) {
        std::fprintf(stderr, "error: '%s' is not a VTRC trace\n", tracePath);
        return 1;
    }
    const std::uint32_t version = r.U32();
    if (version != harken::kTraceFormatVersion) {
        std::fprintf(stderr,
                     "error: trace format version %u is not the supported "
                     "version %u\n",
                     static_cast<unsigned>(version),
                     static_cast<unsigned>(harken::kTraceFormatVersion));
        return 1;
    }
    r.formatVersion = version;
    const std::uint32_t headerBytes = r.U32();
    const std::uint32_t configVocabulary = r.U32();
    const std::uint64_t baseBlocks = r.U64();
    (void)r.U64(); // recorder QPC rate: analysis metadata only
    if (r.failed || headerBytes < harken::kTraceFixedHeaderBytes ||
        headerBytes > journal.size()) {
        std::fprintf(stderr, "error: malformed trace header\n");
        return 1;
    }
    const std::size_t configImageBytes =
        headerBytes - harken::kTraceFixedHeaderBytes;
    const std::uint8_t* configImage = r.Bytes(configImageBytes);
    if (configImage == nullptr) {
        std::fprintf(stderr, "error: malformed trace header\n");
        return 1;
    }
    VnConfig config{};
    std::uint32_t configStructSize = 0;
    std::uint32_t recordedResolvedSampleRate = 0;
    std::uint32_t recordedResolvedDeviceLayout =
        VN_CHANNEL_LAYOUT_DEFAULT;
    if (version == harken::kTraceFormatVersionV2) {
        if (sizeof(void*) != 8) {
            std::fprintf(
                stderr,
                "error: historical VTRC v2 journals use the frozen "
                "64-bit native ABI and require a 64-bit replay host\n");
            return 1;
        }
        const bool knownConfigImage =
            configImageBytes == harken::kTraceConfigImageLegacyBytes ||
            configImageBytes == harken::kTraceConfigImageCurrentBytes;
        if (!knownConfigImage || configVocabulary > configImageBytes) {
            std::fprintf(
                stderr,
                "error: unsupported VTRC v2 config image/declaration "
                "(image=%zu, declared=%u)\n",
                configImageBytes,
                static_cast<unsigned>(configVocabulary));
            return 1;
        }
        std::memcpy(&config, configImage,
                    configImageBytes < sizeof(VnConfig) ? configImageBytes
                                                        : sizeof(VnConfig));
        config.pendingActionCapacity = 0;
        configStructSize = configVocabulary;
    } else {
        std::uint32_t semanticVintage = 0;
        if (!harken::DecodeTraceConfig(configImage, configImageBytes,
                                      &config, &semanticVintage,
                                      &recordedResolvedSampleRate,
                                      &recordedResolvedDeviceLayout) ||
            semanticVintage != configVocabulary) {
            std::fprintf(stderr,
                         "error: malformed or unsupported VTRC v3 config "
                         "schema\n");
            return 1;
        }
        configStructSize = sizeof(VnConfig);
    }
    const std::uint32_t recordedDeviceMode = config.deviceMode;
    const bool recordedNullDevice =
        recordedDeviceMode == VN_DEVICE_NULL;
    const bool recordedRealDevice =
        recordedDeviceMode == VN_DEVICE_REAL;
    const bool rateIdentityExact =
        version >= harken::kTraceFormatVersionV3 || recordedNullDevice ||
        (recordedRealDevice && config.sampleRate != 0);
    const bool deviceLayoutFieldPresent =
        version >= harken::kTraceFormatVersionV3
            ? configVocabulary >= 8
            : configStructSize >= offsetof(VnConfig, maxBindings);
    const bool deviceLayoutIdentityExact =
        version >= harken::kTraceFormatVersionV3 || recordedNullDevice ||
        (recordedRealDevice && deviceLayoutFieldPresent &&
         config.deviceChannelLayout != VN_CHANNEL_LAYOUT_DEFAULT);
    const bool deviceFormatIdentityExact =
        rateIdentityExact && deviceLayoutIdentityExact;
    // The recorded declared vintage drives config resolution exactly as at
    // record time; the callback pointers were nulled at capture. Replay is
    // null-backend by definition (deviceMode is journal metadata here).
    config.structSize = configStructSize;
    config.deviceMode = VN_DEVICE_NULL;
    if (version >= harken::kTraceFormatVersionV3) {
        config.sampleRate = recordedResolvedSampleRate;
        config.deviceChannelLayout = recordedResolvedDeviceLayout;
    }
    config.onNotification = nullptr;
    config.notificationUserData = nullptr;
    config.logCallback = nullptr;
    config.logUserData = nullptr;

    const VnResult initResult = Vn_Init(&config);
    if (initResult != VN_OK) {
        std::fprintf(stderr, "error: Vn_Init failed (%d) replaying '%s'\n",
                     static_cast<int>(initResult), tracePath);
        return 2;
    }
    harken::Engine* engine = harken::detail::ActiveEngine();
    const std::uint32_t replayRateHz =
        engine->config.mixRate.SampleRate();
    const std::uint32_t replayDeviceLayout =
        engine->config.deviceChannelLayout;

    RenderCapture capture;
    if (capturePath != nullptr) {
        std::filesystem::path captureFsPath;
        if (!harken::TryMakeUtf8Path(capturePath, &captureFsPath)) {
            std::fprintf(stderr,
                         "error: capture path '%s' is not valid UTF-8\n",
                         capturePath);
            Vn_Term();
            return 1;
        }
        capture.file.open(captureFsPath, std::ios::binary | std::ios::trunc);
        if (!capture.file) {
            std::fprintf(stderr, "error: cannot create capture file '%s'\n",
                         capturePath);
            Vn_Term();
            return 1;
        }
        capture.toFile = true;
        capture.capturePath = capturePath;
    }

    // Pre-journal blocks: rendered before Vn_TraceStart, provably silent
    // (the start gate admits no stateful call before them). Pumped so the
    // absolute block clock matches, but not part of the journaled capture.
    for (std::uint64_t i = 0; i < baseBlocks; ++i) {
        if (Vn_Update() != VN_OK) {
            std::fprintf(stderr, "error: pre-journal timeline publication failed\n");
            Vn_Term();
            return 2;
        }
        engine->nullBackend.Pump(1);
    }

    ReplayStats stats;
    BlobStore blobs;
    VoiceHandleMap voiceHandles;
    PackHandleMap packHandles;
    ActionFiringHandleMap actionFiringHandles;
    ActionSeekTicketMap actionSeekTickets;
    PlayableObservationMap playableObservations;
    EmitterHandleMap emitterHandles;
    PreviewHandleMap previewHandles;
    WorldGenerationMap worldGenerations;
    ProbeGenerationMap probeGenerations;
    struct RecordedPreparedTopology {
        std::vector<VnBusId> busIds;
        std::vector<std::uint32_t> generations;
        std::vector<std::uint32_t> parents;
        std::vector<std::uint8_t> alive;
        std::vector<std::uint32_t> sendTargets;
        std::vector<float> sendLevels;
        std::vector<std::uint8_t> sendLevelApply;
        std::vector<std::uint32_t> feederCount;
        std::vector<std::uint32_t> emittedFeeders;
        std::vector<std::uint8_t> emitted;
        harken::ProspectiveTopologyView view{};
        bool valid = false;
        std::uint64_t ordinal = 0;
    };
    RecordedPreparedTopology recordedPreparedTopology;
    LogicTicketMap logicTickets;
    VariableOutcomeQueue variableOutcomes(engine->variables.RevisionStorageCapacity());
    engine->variableOutcomeSinkContext = &variableOutcomes;
    engine->variableOutcomeSink = &VariableOutcomeQueue::Capture;
    bool cleanEnd = false;
    bool warnedCallbackContext = false;
    bool haveOrdinal = false;
    std::uint64_t lastOrdinal = 0;
    std::uint64_t nextBlockIndex = baseBlocks;
    bool updateOpen = false;
    std::uint64_t openUpdateIndex = 0;
    std::uint64_t nextUpdateIndex = 0;
    // Either condition downgrades the run to exit 3 so automation cannot
    // mistake diagnostic coverage for ordinary deterministic replay.
    bool limitedCallbackContext = false;
    bool limitedHistoricalCurrentWorld = false;
    bool sessionTerminated = false;
    int exitCode = 0;

    while (!r.failed && r.offset < r.size && exitCode == 0) {
        const std::uint32_t payloadBytes = r.U32();
        const std::uint16_t recordType = r.U16();
        const std::uint16_t flags = r.U16();
        const std::uint64_t ordinal = r.U64();
        (void)r.U64(); // wall-clock timestamp: analysis only
        if (r.failed) {
            break; // truncated inside a record header
        }
        if (haveOrdinal && ordinal <= lastOrdinal) {
            std::fprintf(
                stderr,
                "error: trace record ordinal %llu is not greater than "
                "the previous ordinal %llu\n",
                static_cast<unsigned long long>(ordinal),
                static_cast<unsigned long long>(lastOrdinal));
            exitCode = 1;
            break;
        }
        haveOrdinal = true;
        lastOrdinal = ordinal;
        const std::uint8_t* payload = r.Bytes(payloadBytes);
        if (payload == nullptr) {
            r.failed = true;
            break; // truncated inside a payload
        }
        ByteReader rec;
        rec.data = payload;
        rec.size = payloadBytes;
        rec.formatVersion = version;
        if (recordedPreparedTopology.valid &&
            recordType != harken::kTraceRecordCurrentWorldInstall) {
            std::fprintf(
                stderr,
                "error: prepared topology plan was not immediately followed by "
                "its paired CurrentWorld install\n");
            exitCode = 1;
            break;
        }
        const bool variableBoundary =
            recordType == harken::kTraceRecordApiCall ||
            recordType == harken::kTraceRecordUpdateBegin ||
            recordType == harken::kTraceRecordUpdateEnd ||
            recordType == harken::kTraceRecordCurrentWorldInstall ||
            recordType == harken::kTraceRecordPreparedTopologyPlan ||
            recordType == harken::kTraceRecordSessionTerminate ||
            recordType == harken::kTraceRecordEnd ||
            (recordType >= harken::kTraceRecordMeterProbeInstall &&
             recordType <= harken::kTraceRecordMeterProbeBindingDetach);
        if (variableBoundary && !variableOutcomes.Empty() &&
            !limitedCallbackContext &&
            (flags & harken::kTraceFlagCallbackContext) == 0) {
            std::fprintf(stderr, "error: missing variable outcome before the next operation\n");
            exitCode = 1;
            break;
        }
        if (sessionTerminated && recordType != harken::kTraceRecordVariableOutcome &&
            recordType != harken::kTraceRecordBlockBarrier && recordType != harken::kTraceRecordEnd) {
            std::fprintf(stderr, "error: operation follows session termination\n");
            exitCode = 1;
            break;
        }
        switch (recordType) {
        case harken::kTraceRecordPackBytes: {
            const auto id = rec.U64();
            const auto hash = rec.U64();
            const auto count = rec.U64();
            if (rec.failed || id != blobs.packs.size() + 1 || count == 0 || count != rec.Remaining()) {
                exitCode = 1;
                break;
            }
            const auto* bytes = rec.Bytes(static_cast<std::size_t>(count));
            if (rec.failed || harken::Fnv1a64(bytes, static_cast<std::size_t>(count)) != hash) {
                exitCode = 1;
                break;
            }
            blobs.packs.push_back({sizeof(VnPackByteSpan), 0, bytes, count});
            break;
        }
        case harken::kTraceRecordApiCall: {
            const std::uint16_t callId = rec.U16();
            const std::uint16_t reserved = rec.U16();
            if (rec.failed) {
                std::fprintf(
                    stderr,
                    "error: API call payload is shorter than its "
                    "mandatory header\n");
                exitCode = 1;
                break;
            }
            if (((callId >= harken::kTraceCallPostAction &&
                  callId <= harken::kTraceCallReleaseActionFiring) ||
                 (callId >= harken::kTraceCallPostActionByName &&
                  callId <= harken::kTraceCallReleaseActionSeek)) &&
                reserved != 0) {
                std::fprintf(stderr,
                             "error: malformed argument stream in call "
                             "record (id %u)\n",
                             static_cast<unsigned>(callId));
                exitCode = 1;
                break;
            }
            if ((flags & harken::kTraceFlagCallbackContext) != 0) {
                limitedCallbackContext = true;
                if (!warnedCallbackContext) {
                    // Never silent: this replayer runs Vn_Update without
                    // user callbacks, so a call the live session issued
                    // mid-update executes here after that update returns.
                    std::fprintf(
                        stderr,
                        "warning: trace contains callback-context API "
                        "calls; replay executes them after the enclosing "
                        "update returns and may reorder relative to the "
                        "live session\n");
                    warnedCallbackContext = true;
                }
            }
            if (version < harken::kTraceFormatVersionV4 &&
                (callId == harken::kTraceCallSetValue ||
                 callId == harken::kTraceCallSetEmitterValue ||
                 callId == harken::kTraceCallClearEmitterValue)) {
                std::fprintf(
                    stderr,
                    "error: historical trace uses a Variable call replaced "
                    "by the current authority (id %u)\n",
                    static_cast<unsigned>(callId));
                exitCode = 1;
                break;
            }
            if ((flags & harken::kTraceFlagArgsUnreplayable) != 0) {
                // The recorded call was refused by the engine with zero
                // side effects, so skipping reproduces its (absent) effect.
                ++stats.skippedUnreplayable;
                break;
            }
            const int callExit = ExecuteCall(
                callId, rec, blobs, *engine, voiceHandles,
                actionFiringHandles, actionSeekTickets,
                playableObservations, emitterHandles,
                previewHandles, worldGenerations,
                logicTickets, packHandles, baseBlocks, stats);
            if (callExit == 1) {
                std::fprintf(stderr,
                             "error: malformed argument stream in call "
                             "record (id %u)\n",
                             static_cast<unsigned>(callId));
                exitCode = 1;
            } else if (callExit != 0) {
                exitCode = callExit;
            }
            break;
        }
        case harken::kTraceRecordBlockBarrier: {
            const std::uint64_t blockIndex = rec.U64();
            if (rec.failed || rec.Remaining() != 0 ||
                blockIndex != nextBlockIndex) {
                std::fprintf(
                    stderr,
                    "error: malformed block barrier (recorded=%llu, "
                    "expected=%llu, payload=%u)\n",
                    static_cast<unsigned long long>(blockIndex),
                    static_cast<unsigned long long>(nextBlockIndex),
                    static_cast<unsigned>(payloadBytes));
                exitCode = 1;
                break;
            }
            PumpOneBlock(engine, capture);
            ++stats.blocks;
            ++nextBlockIndex;
            break;
        }
        case harken::kTraceRecordUpdateBegin: {
            const std::uint64_t updateIndex = rec.U64();
            if (rec.failed || rec.Remaining() != 0 || updateOpen ||
                updateIndex != nextUpdateIndex) {
                std::fprintf(
                    stderr,
                    "error: malformed update-begin barrier "
                    "(recorded=%llu, expected=%llu, payload=%u)\n",
                    static_cast<unsigned long long>(updateIndex),
                    static_cast<unsigned long long>(nextUpdateIndex),
                    static_cast<unsigned>(payloadBytes));
                exitCode = 1;
                break;
            }
            updateOpen = true;
            openUpdateIndex = updateIndex;
            (void)Vn_Update();
            ++stats.updates;
            break;
        }
        case harken::kTraceRecordUpdateEnd: {
            const std::uint64_t updateIndex = rec.U64();
            if (rec.failed || rec.Remaining() != 0 || !updateOpen ||
                updateIndex != openUpdateIndex) {
                std::fprintf(
                    stderr,
                    "error: malformed update-end barrier "
                    "(recorded=%llu, expected=%llu, payload=%u)\n",
                    static_cast<unsigned long long>(updateIndex),
                    static_cast<unsigned long long>(
                        updateOpen ? openUpdateIndex : nextUpdateIndex),
                    static_cast<unsigned>(payloadBytes));
                exitCode = 1;
                break;
            }
            updateOpen = false;
            ++nextUpdateIndex;
            break; // the matching begin already ran the update
        }
        case harken::kTraceRecordCurrentWorldInstall: {
            if (version < harken::kTraceFormatVersionV4) {
                std::fprintf(stderr,
                             "error: historical trace contains a current-world "
                             "record\n");
                exitCode = 1;
                break;
            }
            if ((flags & harken::kTraceFlagArgsUnreplayable) != 0) {
                ++stats.skippedUnreplayable;
                break;
            }
            const std::uint32_t worldVintage = rec.U32();
            const std::uint32_t encoding = rec.U32();
            const auto expected =
                static_cast<harken::WorldInstallStatus>(rec.U32());
            const std::uint32_t prefixReserved = rec.U32();
            const std::uint64_t recordedPrior = rec.U64();
            const std::uint64_t recordedResult = rec.U64();
            harken::HistoricalContainerWorldViewV1 vintageOneInstall{};
            std::vector<harken::VariableDefinitionV1> vintageOneDefinitions;
            harken::TraceCurrentWorldImage currentWorldImage;
            const harken::ContainerWorldInstallViewV1* install = nullptr;
            const harken::HistoricalContainerWorldViewV1* replay = nullptr;
            bool paired = false;
            harken::RuntimeProjectGenerationTokenV1 pairedToken{};
            std::uint64_t pairedPublishedBusFingerprint = 0;
            std::uint64_t pairedCompileTargetProfileFingerprint = 0;
            std::uint32_t pairedPublicationVintage = 0;
            if (worldVintage == 1) {
                auto& decoded = vintageOneInstall;
                decoded.identity.format.schema = rec.U32();
                decoded.identity.format.compiler = rec.U32();
                decoded.identity.format.selectionAlgorithm = rec.U32();
                decoded.identity.format.gridAlgorithm = rec.U32();
                decoded.identity.format.transitionAlgorithm = rec.U32();
                decoded.identity.format.publicationProtocol = rec.U32();
                decoded.identity.runtimeSemanticFingerprint = rec.U64();
                decoded.identity.capabilityMask = rec.U64();
                decoded.variableCount = rec.U32();
                decoded.containerCount = rec.U32();
                decoded.transitionCount = rec.U32();
                decoded.capacity.maxContainerDepth = rec.U32();
                decoded.capacity.maxChildrenPerContainer = rec.U32();
                decoded.capacity.maxAvoidLastN = rec.U32();
                decoded.capacity.maxDiscreteAxes = rec.U32();
                decoded.capacity.maxDiscreteRows = rec.U32();
                decoded.capacity.maxDiscreteTrieNodes = rec.U32();
                decoded.capacity.maxContinuousAxes = rec.U32();
                decoded.capacity.maxContinuousRows = rec.U32();
                decoded.capacity.maxContinuousCandidates = rec.U32();
                decoded.capacity.maxContinuousActiveRows = rec.U32();
                decoded.capacity.maxContinuousLiveRows = rec.U32();
                decoded.capacity.maxTransitionRules = rec.U32();
                decoded.capacity.maxCurvePoints = rec.U32();
                decoded.capacity.maxKnownScheduleLeaves = rec.U32();
                if (decoded.variableCount > harken::kHardVariableDefinitionCapacity ||
                    decoded.containerCount != 0 || decoded.transitionCount != 0) {
                    rec.failed = true;
                } else {
                    vintageOneDefinitions.resize(decoded.variableCount);
                }
                bool reservedOk = true;
                for (auto& definition : vintageOneDefinitions) {
                    definition.variableId = rec.ObjectId();
                    definition.kind =
                        static_cast<harken::VariableKindV1>(rec.U32());
                    reservedOk = rec.U32() == 0 && reservedOk;
                    definition.rangeMin = rec.F32();
                    definition.rangeMax = rec.F32();
                    definition.defaultValue = rec.F32();
                    reservedOk = rec.U32() == 0 && reservedOk;
                }
                decoded.variables = vintageOneDefinitions.empty()
                    ? nullptr : vintageOneDefinitions.data();
                if (!reservedOk || decoded.identity.capabilityMask != 0) {
                    rec.failed = true;
                } else {
                    decoded.identity.capabilityMask =
                        decoded.variableCount == 0
                            ? 0 : harken::kWorldCapabilityVariableDefinitions;
                    decoded.identity.runtimeSemanticFingerprint =
                        harken::ComputeCurrentWorldFingerprintV3(decoded);
                }
                replay = &decoded;
            } else if (worldVintage >= 2 &&
                       worldVintage <= harken::kCurrentWorldBodyVintageV8) {
                // Historical journals stay diagnostic-only: ReplayRawAttempt
                // may inspect them, but they never install a runtime world.
                const std::size_t bodyBytes = rec.Remaining();
                const std::uint8_t* body = rec.Bytes(bodyBytes);
                if (body == nullptr ||
                    !currentWorldImage.Decode(
                        body, bodyBytes, worldVintage,
                        harken::CurrentWorldDecodeMode::ReplayRawAttempt)) {
                    rec.failed = true;
                } else {
                    replay = currentWorldImage.ReplayView();
                    if (replay == nullptr) {
                        rec.failed = true;
                    }
                }
            } else if (worldVintage == harken::kCurrentWorldShippingBodyVintage) {
                const std::size_t rest = rec.Remaining();
                const std::uint8_t* restBytes = rec.Bytes(rest);
                const auto mode = encoding == 0
                    ? harken::CurrentWorldDecodeMode::StrictArtifact
                    : harken::CurrentWorldDecodeMode::ReplayCurrentAttempt;
                if (restBytes == nullptr) {
                    rec.failed = true;
                } else if (rest >= harken::kTraceCurrentWorldInstallPairedTailBytes) {
                    const std::size_t prefixBytes =
                        rest - harken::kTraceCurrentWorldInstallPairedTailBytes;
                    const auto prefixResult = currentWorldImage.DecodeWithResult(
                        restBytes, prefixBytes, worldVintage, mode);
                    const auto fullResult = currentWorldImage.DecodeWithResult(
                        restBytes, rest, worldVintage, mode);
                    const bool prefixOk =
                        prefixResult == harken::CurrentWorldBodyDecodeResult::Ok;
                    const bool fullOk =
                        fullResult == harken::CurrentWorldBodyDecodeResult::Ok;
                    if (prefixOk && !fullOk) {
                        paired = true;
                        install = currentWorldImage.ProductionView();
                    } else if (fullOk) {
                        install = currentWorldImage.ProductionView();
                    } else {
                        rec.failed = true;
                    }
                    if (paired) {
                        ByteReader tailReader{};
                        tailReader.data = restBytes + prefixBytes;
                        tailReader.size =
                            harken::kTraceCurrentWorldInstallPairedTailBytes;
                        tailReader.formatVersion = rec.formatVersion;
                        pairedToken.projectSessionId = tailReader.U64();
                        pairedToken.runtimeEpoch = tailReader.U64();
                        pairedToken.authoringRevision = tailReader.U64();
                        pairedToken.submissionSerial = tailReader.U64();
                        pairedPublishedBusFingerprint = tailReader.U64();
                        pairedCompileTargetProfileFingerprint = tailReader.U64();
                        pairedPublicationVintage = tailReader.U32();
                        if (tailReader.failed || tailReader.Remaining() != 0 ||
                            !harken::TokenIsPaired(pairedToken) ||
                            pairedPublicationVintage != 1u) {
                            rec.failed = true;
                            paired = false;
                        }
                    }
                } else if (!currentWorldImage.Decode(
                               restBytes, rest, worldVintage, mode)) {
                    rec.failed = true;
                } else {
                    install = currentWorldImage.ProductionView();
                }
            } else {
                rec.failed = true;
            }
            if (rec.failed || rec.Remaining() != 0 || prefixReserved != 0 ||
                encoding > 1 ||
                (install == nullptr && replay == nullptr)) {
                exitCode = 1;
                break;
            }
            if (worldVintage != harken::kCurrentWorldShippingBodyVintage) {
                if (replay == nullptr) {
                    exitCode = 1;
                    break;
                }
                const harken::WorldInstallStatus diagnosed = worldVintage == 2
                    ? harken::ValidateCurrentWorldV2Replay(*replay)
                    : worldVintage == 3
                    ? harken::ValidateCurrentWorldV3Replay(*replay)
                    : worldVintage <= 5
                    ? harken::ValidateCurrentWorldV4Replay(*replay)
                    : harken::ValidateCurrentWorldV7Replay(*replay);
                if (diagnosed != expected) {
                    exitCode = 1;
                    break;
                }
                std::fprintf(
                    stderr,
                    "warning: historical CurrentWorld body vintage %u "
                    "was validated for diagnostics only; no runtime world "
                    "was installed or published\n",
                    static_cast<unsigned>(worldVintage));
                limitedHistoricalCurrentWorld = true;
                break;
            }
            if (recordedPrior != 0) {
                bool matched = false;
                for (const auto& mapping : worldGenerations) {
                    if (mapping.first == recordedPrior &&
                        mapping.second == engine->variables.CurrentWorldGeneration()) {
                        matched = true;
                    }
                }
                if (!matched) {
                    exitCode = 1;
                    break;
                }
            }
            if (install == nullptr) {
                exitCode = 1;
                break;
            }
            if (paired) {
                if (!recordedPreparedTopology.valid ||
                    recordedPreparedTopology.ordinal + 1u != ordinal) {
                    std::fprintf(
                        stderr,
                        "error: paired CurrentWorld install is missing the "
                        "immediately preceding prepared topology plan\n");
                    exitCode = 1;
                    break;
                }
                harken::ProjectGenerationInstallViewV1 view{};
                view.token = pairedToken;
                view.publicationVintage = pairedPublicationVintage;
                view.publishedBusFingerprint = pairedPublishedBusFingerprint;
                view.compileTargetProfileFingerprint =
                    pairedCompileTargetProfileFingerprint;
                view.ownerId = pairedToken.projectSessionId;
                view.ownerRevision = pairedToken.authoringRevision;
                std::uint64_t liveRevision = 0;
                if (harken::QueryLiveTopologyOwnerRevisionV1(
                        *engine, view.ownerId, &liveRevision)) {
                    view.ownerBaseRevision = liveRevision;
                }
                view.compiledWorld = *install;
                view.prospective = &recordedPreparedTopology.view;
                harken::PreparedProjectGenerationV1 prepared;
                const harken::ProjectGenerationPrepareResultV1 preparedResult =
                    harken::PrepareProjectGenerationV1(*engine, view, prepared);
                if (preparedResult.status !=
                    harken::ProjectGenerationPrepareStatusV1::Ok) {
                    exitCode = 1;
                    break;
                }
                const harken::ProjectGenerationCommitResultV1 committed =
                    harken::CommitPreparedProjectGenerationV1(*engine, prepared);
                if (committed.status != harken::ProjectGenerationCommitStatusV1::Ok) {
                    harken::CancelPreparedProjectGenerationV1(
                        *engine, std::move(prepared));
                    exitCode = 1;
                    break;
                }
                if (expected != harken::WorldInstallStatus::Ok) {
                    exitCode = 1;
                    break;
                }
                if (recordedResult != 0) {
                    worldGenerations.emplace_back(
                        recordedResult, committed.installedWorldGeneration);
                }
                recordedPreparedTopology = RecordedPreparedTopology{};
            } else {
                if (recordedPreparedTopology.valid) {
                    std::fprintf(
                        stderr,
                        "error: prepared topology plan was not immediately "
                        "followed by its paired CurrentWorld install\n");
                    exitCode = 1;
                    break;
                }
                const harken::WorldInstallResult replayed =
                    harken::InstallContainerWorldV1(*engine, *install);
                if (replayed.status != expected) {
                    exitCode = 1;
                    break;
                }
                if (recordedResult != 0) {
                    worldGenerations.emplace_back(recordedResult,
                                                  replayed.resultGeneration);
                }
            }
            break;
        }
        case harken::kTraceRecordMeterProbeInstall: {
            if (version < harken::kTraceFormatVersionV4) {
                std::fprintf(stderr,
                             "error: historical trace contains a MeterProbe "
                             "record\n");
                exitCode = 1;
                break;
            }
            if (rec.U32() != 1) { exitCode = 1; break; }
            const VnResult expected = static_cast<VnResult>(
                static_cast<std::int32_t>(rec.U32()));
            const VnObjectId stableId = rec.ObjectId();
            const VnBusId busId = rec.ObjectId();
            const std::uint32_t tap = rec.U32();
            const std::uint32_t attack = rec.U32();
            const std::uint32_t release = rec.U32();
            const std::uint32_t recordedGeneration = rec.U32();
            std::uint32_t replayedGeneration = 0;
            const VnResult replayed = harken::InstallMeterProbe(
                *engine, stableId, busId, tap, attack, release,
                &replayedGeneration);
            if (rec.failed || rec.Remaining() != 0 || replayed != expected) {
                exitCode = 1;
                break;
            }
            if (recordedGeneration != 0 && replayed == VN_OK) {
                probeGenerations.push_back(
                    {stableId, recordedGeneration, replayedGeneration});
            }
            break;
        }
        case harken::kTraceRecordMeterProbeUninstall: {
            if (version < harken::kTraceFormatVersionV4) {
                exitCode = 1;
                break;
            }
            if (rec.U32() != 1) { exitCode = 1; break; }
            const VnResult expected = static_cast<VnResult>(
                static_cast<std::int32_t>(rec.U32()));
            const VnObjectId stableId = rec.ObjectId();
            const std::uint32_t recordedGeneration = rec.U32();
            const VnResult replayed = harken::UninstallMeterProbe(
                *engine, stableId,
                RemapProbeGeneration(probeGenerations, stableId,
                                     recordedGeneration));
            if (rec.failed || rec.Remaining() != 0 || replayed != expected) {
                exitCode = 1;
            }
            break;
        }
        case harken::kTraceRecordMeterProbeBindingDefine: {
            if (version < harken::kTraceFormatVersionV4) {
                exitCode = 1;
                break;
            }
            if (rec.U32() != 1) { exitCode = 1; break; }
            const VnResult expected = static_cast<VnResult>(
                static_cast<std::int32_t>(rec.U32()));
            const VnObjectId bindingId = rec.ObjectId();
            const VnObjectId stableId = rec.ObjectId();
            const std::uint32_t recordedGeneration = rec.U32();
            const std::uint32_t scope = rec.U32();
            const std::uint32_t targetId = rec.U32();
            const std::uint32_t transitionMs = rec.U32();
            const std::uint32_t curveLocalId = rec.U32();
            const std::uint32_t pointCount = rec.U32();
            if (curveLocalId != 0 || pointCount == 0 ||
                pointCount > harken::kMaxCurvePoints) {
                exitCode = 1;
                break;
            }
            std::vector<harken::CurvePoint> points(pointCount);
            std::vector<harken::CurveHandlePair> handles(
                pointCount > 0 ? pointCount - 1 : 0);
            for (harken::CurvePoint& point : points) {
                point.x = rec.F32();
                point.y = rec.F32();
            }
            for (harken::CurveHandlePair& handle : handles) {
                handle.h1x = rec.F32();
                handle.h1y = rec.F32();
                handle.h2x = rec.F32();
                handle.h2y = rec.F32();
            }
            const VnResult replayed = harken::DefineMeterProbeBinding(
                *engine, bindingId, stableId,
                RemapProbeGeneration(probeGenerations, stableId,
                                     recordedGeneration),
                scope, targetId, points.data(), handles.data(), pointCount,
                transitionMs);
            if (rec.failed || rec.Remaining() != 0 || replayed != expected) {
                exitCode = 1;
            }
            break;
        }
        case harken::kTraceRecordMeterProbeBindingUndefine: {
            if (version < harken::kTraceFormatVersionV4) {
                exitCode = 1;
                break;
            }
            if (rec.U32() != 1) { exitCode = 1; break; }
            const VnResult expected = static_cast<VnResult>(
                static_cast<std::int32_t>(rec.U32()));
            const VnResult replayed =
                harken::UndefineMeterProbeBinding(*engine, rec.ObjectId());
            if (rec.failed || rec.Remaining() != 0 || replayed != expected) {
                exitCode = 1;
            }
            break;
        }
        case harken::kTraceRecordMeterProbeBindingAttach:
        case harken::kTraceRecordMeterProbeBindingDetach: {
            if (version < harken::kTraceFormatVersionV4) {
                exitCode = 1;
                break;
            }
            if (rec.U32() != 1) { exitCode = 1; break; }
            const VnResult expected = static_cast<VnResult>(
                static_cast<std::int32_t>(rec.U32()));
            const VnObjectId bindingId = rec.ObjectId();
            const std::uint32_t scope = rec.U32();
            const auto target = rec.ObjectId();
            const auto recordedHandle = rec.U64();
            auto targetHandle = recordedHandle;
            std::uint32_t generation = rec.U32();
            if (scope == VN_TARGET_SCOPE_VOICE ||
                scope == VN_TARGET_SCOPE_VOICE_EFFECT_PARAM) {
                targetHandle = RemapVoiceHandle(voiceHandles, recordedHandle);
                if (expected != VN_STALE_HANDLE) {
                    const VnVoiceHandle render = engine->ledger.ToRender(targetHandle);
                    if (render != 0) {
                        generation = static_cast<std::uint32_t>(render >> 32);
                    }
                }
            }
            const VnResult replayed =
                recordType == harken::kTraceRecordMeterProbeBindingAttach
                    ? harken::AttachMeterProbeBinding(
                          *engine, bindingId, scope, target, targetHandle, generation)
                    : harken::DetachMeterProbeBinding(
                          *engine, bindingId, scope, target, targetHandle, generation);
            if (rec.failed || rec.Remaining() != 0 || replayed != expected) {
                exitCode = 1;
            }
            break;
        }
        case harken::kTraceRecordVariableOutcome: {
            if (rec.U32() != 1 || rec.Remaining() != 76) {
                exitCode = 1;
                break;
            }
            const std::uint32_t outcome = rec.U32();
            const std::uint64_t acceptedTicket = rec.U64();
            const std::uint64_t relatedTicket = rec.U64();
            const std::uint64_t actualFrame = rec.U64();
            const std::uint64_t bankOrdinal = rec.U64();
            const std::uint64_t recordedWorld = rec.U64();
            const std::uint64_t transport = rec.U64();
            const VnObjectId variableId = rec.ObjectId();
            const std::uint32_t scope = rec.U32();
            const std::uint32_t emitterGeneration = rec.U32();
            harken::VariableOutcomeRecord replayed{};
            const bool haveReplayed = variableOutcomes.Pop(&replayed);
            if (rec.failed || rec.Remaining() != 0 || outcome < 1 ||
                outcome > VN_VARIABLE_OUTCOME_REJECTED_TARGET || scope > 1 ||
                !haveReplayed ||
                replayed.outcome !=
                    static_cast<harken::VariableOutcomeKind>(outcome) ||
                replayed.acceptedTicket.sequence !=
                    RemapLogicTicket(logicTickets, acceptedTicket) ||
                replayed.relatedTicket.sequence !=
                    RemapLogicTicket(logicTickets, relatedTicket) ||
                replayed.actualFrame != actualFrame ||
                replayed.bankVersionStableOrdinal != bankOrdinal ||
                !WorldGenerationMatches(worldGenerations, recordedWorld,
                                        replayed.world) ||
                replayed.transport != transport ||
                replayed.variableId != variableId ||
                static_cast<std::uint32_t>(replayed.scope) != scope ||
                replayed.emitterGeneration != emitterGeneration) {
                std::fprintf(stderr,
                    "error: variable outcome mismatch: kind=%u/%u ticket=%llu/%llu related=%llu/%llu world=%llu/%llu\n",
                    outcome, static_cast<unsigned>(replayed.outcome),
                    static_cast<unsigned long long>(acceptedTicket),
                    static_cast<unsigned long long>(replayed.acceptedTicket.sequence),
                    static_cast<unsigned long long>(relatedTicket),
                    static_cast<unsigned long long>(replayed.relatedTicket.sequence),
                    static_cast<unsigned long long>(recordedWorld),
                    static_cast<unsigned long long>(replayed.world));
                exitCode = 1;
            }
            break;
        }
        case harken::kTraceRecordPreparedTopologyPlan: {
            if (recordedPreparedTopology.valid) {
                std::fprintf(
                    stderr,
                    "error: prepared topology plan was not consumed by the "
                    "next paired CurrentWorld install\n");
                exitCode = 1;
                break;
            }
            const std::uint32_t vintage = rec.U32();
            const std::uint32_t n = rec.U32();
            const std::uint32_t slots = rec.U32();
            const std::uint32_t reserved = rec.U32();
            if (vintage != harken::kTracePreparedTopologyPlanVintage ||
                reserved != 0 || n == 0 || slots == 0) {
                exitCode = 1;
                break;
            }
            const std::uint64_t sendCount =
                static_cast<std::uint64_t>(n) * slots;
            if (sendCount > (1u << 20)) {
                exitCode = 1;
                break;
            }
            RecordedPreparedTopology decoded;
            decoded.busIds.resize(n);
            decoded.generations.resize(n);
            decoded.parents.resize(n);
            decoded.alive.resize(n);
            decoded.sendTargets.resize(static_cast<std::size_t>(sendCount));
            decoded.sendLevels.resize(static_cast<std::size_t>(sendCount));
            decoded.sendLevelApply.resize(static_cast<std::size_t>(sendCount));
            decoded.feederCount.assign(n, 0);
            decoded.emittedFeeders.assign(n, 0);
            decoded.emitted.assign(n, 0);
            for (std::uint32_t i = 0; i < n; ++i) {
                decoded.busIds[i] = rec.ObjectId();
                decoded.generations[i] = rec.U32();
                decoded.parents[i] = rec.U32();
                decoded.alive[i] = static_cast<std::uint8_t>(rec.U32());
                const std::uint32_t pad = rec.U32();
                if (pad != 0)
                    rec.failed = true;
            }
            for (std::uint64_t i = 0; i < sendCount; ++i) {
                decoded.sendTargets[static_cast<std::size_t>(i)] = rec.U32();
                const std::uint32_t bits = rec.U32();
                std::memcpy(&decoded.sendLevels[static_cast<std::size_t>(i)],
                            &bits, sizeof(bits));
                decoded.sendLevelApply[static_cast<std::size_t>(i)] =
                    static_cast<std::uint8_t>(rec.U32());
            }
            if (rec.failed || rec.Remaining() != 0) {
                exitCode = 1;
                break;
            }
            decoded.view.busCount = n;
            decoded.view.busIds = decoded.busIds.data();
            decoded.view.generations = decoded.generations.data();
            decoded.view.parents = decoded.parents.data();
            decoded.view.alive = decoded.alive.data();
            decoded.view.sendTargets = decoded.sendTargets.data();
            decoded.view.sendLevels = decoded.sendLevels.data();
            decoded.view.sendLevelApply = decoded.sendLevelApply.data();
            decoded.view.feederCount = decoded.feederCount.data();
            decoded.view.emittedFeeders = decoded.emittedFeeders.data();
            decoded.view.emitted = decoded.emitted.data();
            decoded.valid = true;
            decoded.ordinal = ordinal;
            recordedPreparedTopology = std::move(decoded);
            recordedPreparedTopology.view.busIds =
                recordedPreparedTopology.busIds.data();
            recordedPreparedTopology.view.generations =
                recordedPreparedTopology.generations.data();
            recordedPreparedTopology.view.parents =
                recordedPreparedTopology.parents.data();
            recordedPreparedTopology.view.alive =
                recordedPreparedTopology.alive.data();
            recordedPreparedTopology.view.sendTargets =
                recordedPreparedTopology.sendTargets.data();
            recordedPreparedTopology.view.sendLevels =
                recordedPreparedTopology.sendLevels.data();
            recordedPreparedTopology.view.sendLevelApply =
                recordedPreparedTopology.sendLevelApply.data();
            recordedPreparedTopology.view.feederCount =
                recordedPreparedTopology.feederCount.data();
            recordedPreparedTopology.view.emittedFeeders =
                recordedPreparedTopology.emittedFeeders.data();
            recordedPreparedTopology.view.emitted =
                recordedPreparedTopology.emitted.data();
            break;
        }
        case harken::kTraceRecordProjectGenerationInstallAck: {
            if (rec.Remaining() != 40) {
                exitCode = 1;
                break;
            }
            harken::ProjectGenerationInstallAckV1 ack{};
            ack.planIndex = rec.U32();
            ack.zero = rec.U32();
            ack.slotGeneration = rec.U64();
            ack.planSequence = rec.U64();
            ack.planFingerprint = rec.U64();
            ack.claimSetFingerprint = rec.U64();
            if (rec.failed || rec.Remaining() != 0 || ack.zero != 0) {
                exitCode = 1;
                break;
            }
            harken::Engine* engine = harken::ActiveEngineForProjectGenerationV1();
            if (engine == nullptr) {
                std::fprintf(stderr,
                             "warning: project-generation install ack skipped "
                             "(no paired generation)\n");
                break;
            }
            bool paired = false;
            for (const auto& row : engine->currentWorldBusAdmission) {
                if (row.occupied &&
                    row.state == harken::BusAdmissionClaimState::Pending &&
                    row.key.fields[1] == ack.planFingerprint &&
                    row.key.fields[3] == ack.claimSetFingerprint) {
                    paired = true;
                    break;
                }
            }
            if (!paired) {
                std::fprintf(stderr,
                             "warning: project-generation install ack skipped "
                             "(no paired generation)\n");
                break;
            }
            engine->projectGenerationInstallAckRing.Push(ack);
            (void)harken::DrainProjectGenerationInstallAcksV1(*engine);
            break;
        }
        case harken::kTraceRecordSessionTerminate:
            if (payloadBytes != 0 || flags != 0) {
                std::fprintf(stderr, "error: invalid session termination record\n");
                exitCode = 1;
                break;
            }
            sessionTerminated = true;
            engine->variables.TerminateSession();
            break;
        case harken::kTraceRecordEnd:
            if (recordedPreparedTopology.valid) {
                std::fprintf(
                    stderr,
                    "error: prepared topology plan was not consumed by a "
                    "paired CurrentWorld install\n");
                exitCode = 1;
                break;
            }
            if (rec.Remaining() != 0) {
                std::fprintf(stderr,
                             "error: TraceEnd payload must be empty\n");
                exitCode = 1;
                break;
            }
            if ((flags & harken::kTraceFlagCallbackContext) != 0) {
                // The recorder's aggregate marker; normally redundant with
                // the per-call flags above, but it keeps the limited
                // verdict even if every flagged call was itself skipped.
                limitedCallbackContext = true;
            }
            cleanEnd = true;
            break;
        default:
            std::fprintf(stderr,
                         "error: unknown record type %u - replay refused "
                         "(journal is from a newer recorder)\n",
                         static_cast<unsigned>(recordType));
            exitCode = 1;
            break;
        }
        engine->CompleteVariableOperation();
        if (variableOutcomes.Overflowed()) {
            std::fprintf(stderr, "error: variable outcome operation exceeded its bounded capacity\n");
            exitCode = 1;
        }
        if (exitCode != 0) {
            std::fprintf(stderr, "error: replay failed at record type %u, offset %zu\n",
                         static_cast<unsigned>(recordType), r.offset);
        }
        if (cleanEnd) {
            if (r.offset != r.size) {
                std::fprintf(
                    stderr,
                    "error: bytes follow the terminal TraceEnd record\n");
                exitCode = 1;
                cleanEnd = false;
            }
            break;
        }
    }

    if (exitCode == 0 && !variableOutcomes.Empty() && !limitedCallbackContext) {
        std::fprintf(stderr, "error: missing variable outcome at end of journal\n");
        exitCode = 1;
    }

    const char* completion = "complete";
    const char* completionReason = "none";
    if (exitCode == 0 && !cleanEnd) {
        completion = "partial";
        if (r.failed) {
            completionReason = "truncated-record";
            std::fprintf(stderr,
                         "warning: journal truncated mid-record (a crash "
                         "capture?); replayed up to the last complete "
                         "record\n");
        } else {
            completionReason = "missing-end";
            std::fprintf(stderr,
                         "warning: journal carries no end marker (the trace "
                         "was not stopped cleanly); replayed everything "
                         "present\n");
        }
    }

    const std::uint64_t replayCaptureTruncationCount =
        engine->captureTruncationCount;
    engine->variableOutcomeSink = nullptr;
    engine->variableOutcomeSinkContext = nullptr;
    Vn_Term();

    if (capture.toFile) {
        capture.file.close();
        if (exitCode != 0) {
            // A refused replay must not leave a plausible-looking capture. The
            // path was validated when the capture file opened, so this
            // reconstruction succeeds; the guarded call keeps the naked
            // char8_t spell out of the tree.
            std::filesystem::path captureFsPath;
            if (harken::TryMakeUtf8Path(capture.capturePath.c_str(),
                                       &captureFsPath)) {
                std::error_code ec;
                std::filesystem::remove(captureFsPath, ec);
            }
        }
    }

    if (exitCode != 0) {
        return exitCode;
    }

    std::printf("trace-replay: %llu calls (%llu non-OK), %llu blocks, %llu "
                "updates, %llu unreplayable skipped, "
                "capture_truncations=%llu;\n",
                static_cast<unsigned long long>(stats.calls),
                static_cast<unsigned long long>(stats.callsNotOk),
                static_cast<unsigned long long>(stats.blocks),
                static_cast<unsigned long long>(stats.updates),
                static_cast<unsigned long long>(stats.skippedUnreplayable),
                static_cast<unsigned long long>(
                    replayCaptureTruncationCount));
    std::printf("trace-replay: render fnv1a64=%016llx frames=%llu "
                "peak=%.6f\n",
                static_cast<unsigned long long>(capture.hash),
                static_cast<unsigned long long>(capture.frames),
                static_cast<double>(capture.peak));
    std::printf("trace-replay: completion=%s reason=%s;\n",
                completion, completionReason);
    std::printf(
        "trace-replay: device_format_identity=%s "
        "rate_identity=%s device_layout_identity=%s "
        "replay_rate_hz=%u replay_device_layout=%u;\n",
        deviceFormatIdentityExact ? "exact" : "not-provable",
        rateIdentityExact ? "exact" : "not-recorded",
        deviceLayoutIdentityExact ? "exact" : "not-recorded",
        static_cast<unsigned>(replayRateHz),
        static_cast<unsigned>(replayDeviceLayout));
    if (!deviceFormatIdentityExact) {
        std::fprintf(
            stderr,
            "warning: the VTRC v2 journal does not record the resolved "
            "real-device sample rate and/or device layout; this NullBackend "
            "render is not proof of device-format identity\n");
    }
    if (limitedCallbackContext || limitedHistoricalCurrentWorld) {
        std::fprintf(stderr, "trace-replay: limited deterministic replay:");
        if (limitedCallbackContext) {
            std::fprintf(
                stderr,
                " callback-context API calls execute after their enclosing "
                "update returns;");
        }
        if (limitedHistoricalCurrentWorld) {
            std::fprintf(
                stderr,
                " historical CurrentWorld bodies were validated for "
                "diagnostics only and were not installed or published;");
        }
        std::fprintf(stderr, " NOT ordinary replay proof\n");
        return 3;
    }
    return 0;
}

} // namespace harkenhost
