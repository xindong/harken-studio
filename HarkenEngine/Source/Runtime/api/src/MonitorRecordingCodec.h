#ifndef HARKEN_API_MONITOR_RECORDING_CODEC_H
#define HARKEN_API_MONITOR_RECORDING_CODEC_H

#include "MonitorIdentityCut.h"
#include "MonitorLifecycleProjection.h"
#include <utility>

namespace harken::monitor {

inline constexpr std::uint32_t kRecordingDescriptorBytes = 256;
inline constexpr std::uint32_t kRecordingEndBytes = 408;
inline constexpr std::uint32_t kRecordingPositionBytes = 24;

enum class RecordingSource : std::uint32_t {
    Typed = 0, LogicRing = 1, AudioRing = 2, AudioLog = 3, LogicLog = 4, Control = 5
};

struct RecordingPosition {
    RecordingSource source = RecordingSource::Typed;
    std::uint32_t reserved = 0;
    std::uint64_t ticket = 0;
    std::uint64_t recordBytes = 0;
};

struct RecordingDescriptor {
    std::uint32_t version = 1, bytes = kRecordingDescriptorBytes;
    std::uint64_t engine = 0, epoch = 0;
    std::uint64_t owner = 0, ownerGeneration = 0, ownerEpoch = 0, leaseGeneration = 0;
    std::uint32_t schema = 1, streams = 0, permissions = 0, requestSymbols = 0;
    std::uint64_t supported[2]{};
    std::uint64_t startedNs = 0;
    std::uint64_t starts[4]{};
    std::uint64_t ringStarts[2]{}, rawDropStarts[2]{};
    std::uint64_t cutId = 0, cutRevision = 0, cutFirst = 0, cutLast = 0;
    std::uint64_t recordingEpoch = 0, controlStart = 0;
    std::uint64_t unavailableStarts[4]{};
    std::uint64_t logStarts[2]{};
};

struct RecordingEnd {
    std::uint32_t version = 1, bytes = kRecordingEndBytes;
    std::uint64_t engine = 0, epoch = 0, stoppedNs = 0;
    std::uint64_t ends[4]{}, completed[4]{};
    std::uint64_t ringEnds[2]{}, ringCompleted[2]{};
    std::uint64_t sourceDrops[6]{}, deliveryDrops[6]{}, rawDrops[2]{};
    std::uint64_t unavailable[4]{};
    std::uint32_t baselineAvailability = 0, symbolAvailability = 0;
    std::uint32_t boundaryComplete = 0, stopReason = 0;
    std::uint64_t logEnds[2]{}, logCompleted[2]{};
    std::uint64_t recordCount = 0, checksum = kIdentityChecksumSeed;
    std::uint64_t eventSourceDrops = 0, logDropCount = 0;
    std::uint64_t nativeLogDrops[2]{};
    std::uint64_t recordingEpoch = 0, logFiltered[2]{};
    std::uint64_t controlEnd = 0, controlDrops = 0;
};

template<class Visitor, class Value>
inline void VisitRecordingDescriptor(Visitor& visit, Value& value) noexcept {
    visit(value.version); visit(value.bytes); visit(value.engine); visit(value.epoch);
    visit(value.owner); visit(value.ownerGeneration); visit(value.ownerEpoch); visit(value.leaseGeneration);
    visit(value.schema); visit(value.streams); visit(value.permissions); visit(value.requestSymbols);
    for (auto& word : value.supported) visit(word);
    visit(value.startedNs);
    for (auto& word : value.starts) visit(word);
    for (auto& word : value.ringStarts) visit(word);
    for (auto& word : value.rawDropStarts) visit(word);
    visit(value.cutId); visit(value.cutRevision); visit(value.cutFirst); visit(value.cutLast);
    visit(value.recordingEpoch); visit(value.controlStart);
    for (auto& word : value.unavailableStarts) visit(word);
    for (auto& word : value.logStarts) visit(word);
}

template<class Visitor, class Value>
inline void VisitRecordingEnd(Visitor& visit, Value& value) noexcept {
    visit(value.version); visit(value.bytes); visit(value.engine); visit(value.epoch); visit(value.stoppedNs);
    for (auto& word : value.ends) visit(word);
    for (auto& word : value.completed) visit(word);
    for (auto& word : value.ringEnds) visit(word);
    for (auto& word : value.ringCompleted) visit(word);
    for (auto& word : value.sourceDrops) visit(word);
    for (auto& word : value.deliveryDrops) visit(word);
    for (auto& word : value.rawDrops) visit(word);
    for (auto& word : value.unavailable) visit(word);
    visit(value.baselineAvailability); visit(value.symbolAvailability);
    visit(value.boundaryComplete); visit(value.stopReason);
    for (auto& word : value.logEnds) visit(word);
    for (auto& word : value.logCompleted) visit(word);
    visit(value.recordCount); visit(value.checksum); visit(value.eventSourceDrops); visit(value.logDropCount);
    for (auto& word : value.nativeLogDrops) visit(word);
    visit(value.recordingEpoch);
    for (auto& word : value.logFiltered) visit(word);
    visit(value.controlEnd); visit(value.controlDrops);
}

inline bool RecordingHasSymbols(const RecordingDescriptor& value) noexcept {
    return value.requestSymbols != 0 && (value.permissions & VN_MONITOR_PERM_SYMBOLS) != 0;
}

inline bool RecordingKeepsLegacy(std::uint32_t kind, std::uint32_t category = 0, std::uint32_t code = 0) noexcept {
    if ((kind >= VN_MONITOR_RECORD_STATE_CUT_BEGIN && kind <= VN_MONITOR_RECORD_EVENT)) return false;
    return kind != VN_MONITOR_RECORD_EVENT_DETAIL || !ExecutionReplacesLegacyEvent(category, code);
}

inline bool ValidRecordingDescriptor(const RecordingDescriptor& value) noexcept {
    constexpr auto streams = VN_MONITOR_STREAM_STATE | VN_MONITOR_STREAM_EVENT |
        VN_MONITOR_STREAM_TIME_SERIES | VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    constexpr auto permissions = VN_MONITOR_PERM_OBSERVE_STATE | VN_MONITOR_PERM_OBSERVE_EVENTS |
        VN_MONITOR_PERM_OBSERVE_MEASUREMENTS | VN_MONITOR_PERM_CAPTURE_CONTROL | VN_MONITOR_PERM_RECORD |
        VN_MONITOR_PERM_SYMBOLS | VN_MONITOR_PERM_RAW_AUDIO;
    if (value.version != 1 || value.bytes != kRecordingDescriptorBytes || value.engine == 0 || value.epoch == 0 ||
        value.owner == 0 || value.ownerGeneration == 0 || value.ownerEpoch == 0 || value.leaseGeneration == 0 ||
        value.ownerGeneration != (value.owner >> 8) || (value.schema == 0 || value.schema > 2) || value.startedNs == 0 ||
        value.requestSymbols > 1 || value.streams == 0 || (value.streams & ~streams) != 0 ||
        (value.permissions & ~permissions) != 0 || (value.permissions & VN_MONITOR_PERM_RECORD) == 0 ||
        value.recordingEpoch == 0 || value.recordingEpoch == value.epoch) return false;
    for (const auto pair : {std::pair{VN_MONITOR_STREAM_STATE, VN_MONITOR_PERM_OBSERVE_STATE},
        std::pair{VN_MONITOR_STREAM_EVENT, VN_MONITOR_PERM_OBSERVE_EVENTS},
        std::pair{VN_MONITOR_STREAM_TIME_SERIES, VN_MONITOR_PERM_OBSERVE_MEASUREMENTS},
        std::pair{VN_MONITOR_STREAM_OPTIONAL_AUDIO, VN_MONITOR_PERM_RAW_AUDIO}})
        if ((value.streams & pair.first) != 0 && (value.permissions & pair.second) == 0) return false;
    const bool state = (value.streams & VN_MONITOR_STREAM_STATE) != 0;
    return state ? value.cutId != 0 && value.cutRevision != 0 && value.cutFirst != 0 && value.cutLast > value.cutFirst
        : value.cutId == 0 && value.cutRevision == 0 && value.cutFirst == 0 && value.cutLast == 0;
}

inline bool DecodeRecordingDescriptor(const std::uint8_t* bytes, std::uint64_t size,
    RecordingDescriptor& value) noexcept {
    if (!bytes || size != kRecordingDescriptorBytes) return false;
    ExecutionPayloadReader reader{bytes};
    VisitRecordingDescriptor(reader, value);
    return reader.offset == size && ValidRecordingDescriptor(value);
}

inline bool DecodeRecordingEnd(const std::uint8_t* bytes, std::uint64_t size, RecordingEnd& value) noexcept {
    if (!bytes || size != kRecordingEndBytes) return false;
    ExecutionPayloadReader reader{bytes};
    VisitRecordingEnd(reader, value);
    return reader.offset == size && value.version == 1 && value.bytes == kRecordingEndBytes && value.boundaryComplete <= 1;
}

// Required framing preserves raw ring/log provenance without changing the legacy envelope.
inline void PackRecordingPosition(std::uint8_t* bytes, const RecordingPosition& value) noexcept {
    ExecutionPayloadWriter write{bytes};
    write(static_cast<std::uint32_t>(value.source)); write(value.reserved);
    write(value.ticket); write(value.recordBytes);
}

class MonitorRecordingVerifier {
public:
    bool Begin(const RecordingDescriptor& descriptor) noexcept {
        *this = {};
        descriptor_ = descriptor;
        begun_ = ValidRecordingDescriptor(descriptor);
        std::copy(std::begin(descriptor.starts), std::end(descriptor.starts), std::begin(lastSources_));
        std::copy(std::begin(descriptor.ringStarts), std::end(descriptor.ringStarts), std::begin(lastRaw_));
        std::copy(std::begin(descriptor.logStarts), std::end(descriptor.logStarts), std::begin(lastLog_));
        lastBaseline_ = descriptor.cutFirst == 0 ? 0 : descriptor.cutFirst - 1;
        lastControl_ = descriptor.controlStart;
        return begun_;
    }

    bool Record(const std::uint8_t* frame, std::uint64_t size) noexcept {
        if (!begun_ || ended_ || !frame || size < kRecordingPositionBytes + kPackedEnvelopeBytes ||
            size > VN_MONITOR_HARD_CHUNK_PAYLOAD_BYTES) return Fail();
        ExecutionPayloadReader read{frame};
        std::uint32_t source = 0, reserved = 0;
        std::uint64_t ticket = 0, recordBytes = 0;
        read(source); read(reserved); read(ticket); read(recordBytes);
        if (source > static_cast<unsigned>(RecordingSource::Control) || reserved != 0 ||
            recordBytes != size - kRecordingPositionBytes || (source == 0 ? ticket != 0 : ticket == 0)) return Fail();
        const auto* bytes = frame + kRecordingPositionBytes;
        VnMonitorRecordHeader header{};
        if (!ReadIdentityHeader(bytes, recordBytes, header) || header.monitorSessionEpoch != descriptor_.epoch ||
            header.deliverySequence != records_ + 1 || header.streamKind == 0 ||
            (header.streamKind & (header.streamKind - 1)) != 0 ||
            (descriptor_.streams & header.streamKind) == 0 || recordBytes != PackedRecordBytes(static_cast<std::uint32_t>(header.payloadBytes)) ||
            (header.flags != VN_MONITOR_RECORD_FLAG_REQUIRED && header.flags != VN_MONITOR_RECORD_FLAG_OPTIONAL)) return Fail();
        for (auto at = kPackedEnvelopeBytes + header.payloadBytes; at < recordBytes; ++at)
            if (bytes[at] != 0) return Fail();
        const auto* payload = bytes + kPackedEnvelopeBytes;
        bool valid = true;
        const auto observe = [&](unsigned index) {
            if (header.sourceSequence <= lastSources_[index]) return false;
            lastSources_[index] = header.sourceSequence;
            ++sourceRecords_[index];
            return true;
        };
        if (source == 1 || source == 2) {
            auto& last = lastRaw_[source - 1];
            if (ticket <= descriptor_.ringStarts[source - 1] || ticket < last) return Fail();
            last = ticket;
        } else if (source == 3 || source == 4) {
            const auto index = source - 3;
            if (ticket <= lastLog_[index] || header.recordKind != VN_MONITOR_RECORD_EVENT_DETAIL) return Fail();
            lastLog_[index] = ticket;
            ++logRecords_[index];
        } else if (source == static_cast<unsigned>(RecordingSource::Control)) {
            if (ticket <= lastControl_ || header.recordKind != VN_MONITOR_RECORD_CAPTURE_PLAN_ACTIVATED) return Fail();
            lastControl_ = ticket;
            ++controlRecords_;
        }
        const bool baseline = header.streamId == kIdentityBaselineStream;
        if (baseline) {
            if (header.sourceSequence <= lastBaseline_ || header.sourceSequence > descriptor_.cutLast) return Fail();
            lastBaseline_ = header.sourceSequence;
        }
        switch (header.recordKind) {
        case VN_MONITOR_RECORD_EXECUTION_FACT: {
            VnMonitorExecutionFactV2 value{};
            valid = source == 0 && header.recordVersion <= descriptor_.schema &&
                DecodeExecutionFact(header, payload, header.payloadBytes, value) &&
                value.fact.factKind < 128 && (descriptor_.supported[value.fact.factKind / 64] & (UINT64_C(1) << (value.fact.factKind % 64))) != 0;
            if (valid) valid = observe(header.producerId == kExecutionLogicProducer ? 0 : 1);
            break;
        }
        case VN_MONITOR_RECORD_IDENTITY_CUT_BEGIN: {
            VnMonitorIdentityCutBeginV1 value{};
            valid = source == 0 && !cutBegun_ && DecodeIdentityBegin(header, payload, header.payloadBytes, value) &&
                value.stateCutId == descriptor_.cutId && value.identityRevision == descriptor_.cutRevision &&
                header.sourceSequence == descriptor_.cutFirst && value.deltaSourceSequence == descriptor_.starts[2] &&
                (RecordingHasSymbols(descriptor_) ? value.symbolProducerId == kHostSymbolProducer &&
                    value.symbolSourceSequence == descriptor_.starts[3] : value.symbolProducerId == 0) && cut_.Begin(header, value);
            cutBegun_ = true;
            break;
        }
        case VN_MONITOR_RECORD_IDENTITY_FACT: {
            VnMonitorIdentityFactV1 value{};
            valid = source == 0 && DecodeIdentityFact(header, payload, header.payloadBytes, value);
            if (valid && value.stateCutId != 0) {
                valid = value.stateCutId == descriptor_.cutId && value.identityRevision == descriptor_.cutRevision;
                if (valid) cut_.Row(header, value);
            } else if (valid) valid = observe(2);
            break;
        }
        case VN_MONITOR_RECORD_IDENTITY_CUT_END: {
            VnMonitorIdentityCutEndV1 value{};
            valid = source == 0 && DecodeIdentityEnd(header, payload, header.payloadBytes, value) &&
                header.sourceSequence == descriptor_.cutLast && value.stateCutId == descriptor_.cutId &&
                value.identityRevision == descriptor_.cutRevision;
            if (valid) cut_.End(header, value);
            break;
        }
        case VN_MONITOR_RECORD_HOST_OBJECT_SYMBOL: {
            VnMonitorHostObjectSymbolV1 value{};
            valid = source == 0 && RecordingHasSymbols(descriptor_) &&
                DecodeHostSymbol(header, payload, header.payloadBytes, value);
            if (valid && value.stateCutId != 0) {
                valid = value.stateCutId == descriptor_.cutId && value.cutRevision == descriptor_.cutRevision;
                if (valid) cut_.Row(header, value);
            } else if (valid) valid = observe(3);
            break;
        }
        default:
            if (header.recordKind == VN_MONITOR_RECORD_EVENT_DETAIL) {
                valid = header.recordVersion == 2 && header.payloadBytes == sizeof(VnMonitorEventDetailV2) &&
                    RecordingKeepsLegacy(header.recordKind, ReadWord(payload + 4), ReadWord(payload + 8));
            } else valid = RecordingKeepsLegacy(header.recordKind) && ValidLegacyPayload(header, payload);
            if (header.recordKind == VN_MONITOR_RECORD_AUDIO_CHUNK &&
                (descriptor_.permissions & VN_MONITOR_PERM_RAW_AUDIO) == 0) valid = false;
            if (header.streamKind == VN_MONITOR_STREAM_STATE && header.recordKind != VN_MONITOR_RECORD_DROP_SUMMARY) valid = false;
            if (source == 0 && header.recordKind != VN_MONITOR_RECORD_DROP_SUMMARY) valid = false;
            if (header.recordKind > VN_MONITOR_RECORD_HOST_OBJECT_SYMBOL &&
                (header.flags & VN_MONITOR_RECORD_FLAG_REQUIRED) != 0) valid = false;
            break;
        }
        if (header.recordKind >= VN_MONITOR_RECORD_EXECUTION_FACT &&
            header.recordKind <= VN_MONITOR_RECORD_HOST_OBJECT_SYMBOL && header.engineMonitorInstanceId != descriptor_.engine) valid = false;
        if (!valid) return Fail();
        ++records_;
        checksum_ = IdentityChecksum(checksum_, frame, static_cast<std::uint32_t>(size));
        return true;
    }

    bool End(const RecordingEnd& value) noexcept {
        if (!begun_ || ended_ || value.version != 1 || value.bytes != kRecordingEndBytes ||
            value.engine != descriptor_.engine || value.epoch != descriptor_.epoch ||
            value.recordingEpoch != descriptor_.recordingEpoch || value.stoppedNs < descriptor_.startedNs ||
            value.boundaryComplete > 1 || value.recordCount != records_ || value.checksum != checksum_) return Fail();
        ended_ = true;
        bool complete = !failed_ && value.boundaryComplete != 0 && value.eventSourceDrops == 0 && value.logDropCount == 0;
        const auto subscribed = [&](unsigned i) {
            return i < 2 ? (descriptor_.streams & VN_MONITOR_STREAM_EVENT) != 0 :
                i == 2 ? (descriptor_.streams & VN_MONITOR_STREAM_STATE) != 0 : RecordingHasSymbols(descriptor_);
        };
        for (unsigned i = 0; i != 4; ++i) {
            if (value.ends[i] < descriptor_.starts[i] || lastSources_[i] > value.ends[i]) return Fail();
            if (subscribed(i)) complete &= (value.ends[i] == descriptor_.starts[i] || value.completed[i] >= value.ends[i]) && value.unavailable[i] == 0 &&
                sourceRecords_[i] == value.ends[i] - descriptor_.starts[i];
            else if (sourceRecords_[i] != 0) return Fail();
        }
        for (unsigned i = 0; i != 2; ++i) {
            if (value.ringEnds[i] < descriptor_.ringStarts[i] || lastRaw_[i] > value.ringEnds[i] ||
                value.logEnds[i] < descriptor_.logStarts[i] || lastLog_[i] > value.logEnds[i]) return Fail();
            complete &= (value.ringEnds[i] == descriptor_.ringStarts[i] || value.ringCompleted[i] >= value.ringEnds[i]) && value.rawDrops[i] == 0;
            if ((descriptor_.streams & VN_MONITOR_STREAM_EVENT) != 0)
                complete &= (value.logEnds[i] == descriptor_.logStarts[i] || value.logCompleted[i] >= value.logEnds[i]) && value.nativeLogDrops[i] == 0 &&
                    value.logFiltered[i] <= value.logEnds[i] - descriptor_.logStarts[i] &&
                    logRecords_[i] == value.logEnds[i] - descriptor_.logStarts[i] - value.logFiltered[i];
            else if (logRecords_[i] != 0) return Fail();
        }
        for (unsigned i = 0; i != 6; ++i) complete &= value.sourceDrops[i] == 0 && value.deliveryDrops[i] == 0;
        if (value.controlEnd < descriptor_.controlStart || lastControl_ > value.controlEnd) return Fail();
        complete &= value.controlDrops == 0 && controlRecords_ == value.controlEnd - descriptor_.controlStart;
        if ((descriptor_.streams & VN_MONITOR_STREAM_STATE) != 0)
            complete &= cut_.Complete() && value.baselineAvailability == VN_MONITOR_OBSERVATION_AVAILABLE;
        if (RecordingHasSymbols(descriptor_)) complete &= value.symbolAvailability == VN_MONITOR_OBSERVATION_AVAILABLE;
        complete_ = complete;
        return true;
    }

    std::uint64_t Records() const noexcept { return records_; }
    std::uint64_t Checksum() const noexcept { return checksum_; }
    bool Complete() const noexcept { return ended_ && complete_ && !failed_; }

private:
    static bool ValidLegacyPayload(const VnMonitorRecordHeader& header, const std::uint8_t* payload) noexcept {
        const auto fixed = [&](std::uint32_t bytes, std::uint32_t structSize) {
            return header.recordVersion == 1 && header.payloadBytes == bytes && ReadWord(payload) == structSize;
        };
        switch (header.recordKind) {
        case VN_MONITOR_RECORD_TIME_SERIES: return fixed(sizeof(VnMonitorTimeSeriesSample), sizeof(VnMonitorTimeSeriesSample));
        case VN_MONITOR_RECORD_DROP_SUMMARY: return fixed(sizeof(VnMonitorDropSummary), sizeof(VnMonitorDropSummary));
        // Legacy measurement headers declare sizes independently of their packed payloads.
        case VN_MONITOR_RECORD_WAVEFORM_BUCKET:
        case VN_MONITOR_RECORD_PARTIAL_BUCKET_DISCARDED: return fixed(96, 88);
        case VN_MONITOR_RECORD_SCOPE_RESULT: return fixed(144, 144);
        case VN_MONITOR_RECORD_METER: return fixed(64, 64);
        case VN_MONITOR_RECORD_GAIN_REDUCTION: return fixed(48, 40);
        case VN_MONITOR_RECORD_AGGREGATE: return fixed(40, 40);
        case VN_MONITOR_RECORD_AUDIO_CHUNK: {
            if (header.recordVersion != 1 || header.payloadBytes < sizeof(VnMonitorAudioChunk) ||
                ReadWord(payload) != sizeof(VnMonitorAudioChunk)) return false;
            ExecutionPayloadReader read{payload + 8};
            std::uint64_t begin = 0, end = 0;
            read(begin); read(end);
            const auto channels = ReadWord(payload + 44);
            return end >= begin && end - begin <= VN_MONITOR_HARD_AUDIO_CHUNK_FRAMES &&
                channels != 0 && channels <= kPackedAudioMaxChannels &&
                header.payloadBytes == sizeof(VnMonitorAudioChunk) + (end - begin) * channels * sizeof(float);
        }
        case VN_MONITOR_RECORD_SPECTRUM:
            return header.recordVersion == 1 && header.payloadBytes >= kPackedSpectrumPayloadBytes &&
                ReadWord(payload) == sizeof(VnMonitorSpectrumPayload) &&
                header.payloadBytes == kPackedSpectrumPayloadBytes + UINT64_C(4) * ReadWord(payload + 20);
        case VN_MONITOR_RECORD_STREAM_BASELINE:
        case VN_MONITOR_RECORD_CAPTURE_PLAN_ACTIVATED:
        case VN_MONITOR_RECORD_TAP_BINDING_CHANGED:
        case VN_MONITOR_RECORD_MEASUREMENT_SEGMENT_RESET:
        case VN_MONITOR_RECORD_CLOCK_ANCHOR:
        case VN_MONITOR_RECORD_REGISTRY_REVISION:
            return header.recordVersion == 1 && header.payloadBytes == 0;
        default:
            return header.recordKind > VN_MONITOR_RECORD_HOST_OBJECT_SYMBOL &&
                header.flags == VN_MONITOR_RECORD_FLAG_OPTIONAL;
        }
    }

    static std::uint32_t ReadWord(const std::uint8_t* bytes) noexcept {
        ExecutionPayloadReader read{bytes};
        std::uint32_t value = 0;
        read(value);
        return value;
    }
    bool Fail() noexcept { failed_ = true; return false; }
    RecordingDescriptor descriptor_{};
    MonitorIdentityCutVerifier cut_;
    std::uint64_t records_ = 0, checksum_ = kIdentityChecksumSeed;
    std::uint64_t lastSources_[4]{}, sourceRecords_[4]{}, lastRaw_[2]{}, lastLog_[2]{}, logRecords_[2]{};
    std::uint64_t lastBaseline_ = 0;
    std::uint64_t lastControl_ = 0, controlRecords_ = 0;
    bool begun_ = false, ended_ = false, failed_ = false, complete_ = false, cutBegun_ = false;
};

} // namespace harken::monitor

#endif
