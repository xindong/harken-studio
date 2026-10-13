#include "Common/ObjectIdTestSupport.h"
#include "monitor_fixture_decode.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifndef HARKEN_MONITOR_FIXTURE_DIR
#define HARKEN_MONITOR_FIXTURE_DIR "."
#endif

namespace {

void WriteU32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
    p[2] = static_cast<uint8_t>(value >> 16);
    p[3] = static_cast<uint8_t>(value >> 24);
}

void WriteU64(uint8_t* p, uint64_t value) {
    WriteU32(p, static_cast<uint32_t>(value));
    WriteU32(p + 4, static_cast<uint32_t>(value >> 32));
}

std::vector<uint8_t> PackRecord(const VnMonitorRecordHeader& in,
                                const uint8_t* payload,
                                uint64_t payloadBytes) {
    const uint64_t headerBytes = VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES;
    uint64_t recordBytes = headerBytes + payloadBytes;
    if ((recordBytes & 7ull) != 0)
        recordBytes += 8u - (recordBytes & 7ull);
    std::vector<uint8_t> out(static_cast<size_t>(recordBytes), 0);
    uint8_t* p = out.data();
    size_t o = 0;
    WriteU32(p + o, in.recordKind); o += 4;
    WriteU32(p + o, in.recordVersion); o += 4;
    WriteU32(p + o, in.flags); o += 4;
    WriteU32(p + o, in.streamKind); o += 4;
    WriteU64(p + o, recordBytes); o += 8;
    WriteU64(p + o, in.streamId); o += 8;
    WriteU64(p + o, in.producerId); o += 8;
    WriteU64(p + o, in.sourceSequence); o += 8;
    WriteU64(p + o, in.deliverySequence); o += 8;
    WriteU64(p + o, in.engineMonitorInstanceId); o += 8;
    WriteU64(p + o, in.monitorSessionEpoch); o += 8;
    WriteU64(p + o, in.worldGeneration); o += 8;
    WriteU64(p + o, in.capturePlanGeneration); o += 8;
    WriteU64(p + o, in.renderPlanGeneration); o += 8;
    WriteU64(p + o, in.transportEpoch); o += 8;
    WriteU64(p + o, in.deviceEpoch); o += 8;
    WriteU64(p + o, in.clockAnchorGeneration); o += 8;
    WriteU64(p + o, in.measurementSegmentGeneration); o += 8;
    WriteU64(p + o, in.executeFrame); o += 8;
    WriteU64(p + o, in.graphFrame); o += 8;
    WriteU64(p + o, in.devicePresentationEstimate); o += 8;
    WriteU32(p + o, in.deviceUncertaintyFrames); o += 4;
    WriteU32(p + o, in.entityKind); o += 4;
    WriteU64(p + o, in.programSemanticFingerprint); o += 8;
    WriteU64(p + o, in.runtimeLineageFingerprint); o += 8;
    WriteU64(p + o, in.optionalSymbolFingerprint); o += 8;
    std::memcpy(p + o, harken::ObjectIdBytes(in.stableId).data(), 16); o += 16;
    WriteU64(p + o, in.instanceHandle); o += 8;
    WriteU64(p + o, in.instanceGeneration); o += 8;
    WriteU64(p + o, in.actionInstanceHandle); o += 8;
    WriteU64(p + o, in.playableInstanceHandle); o += 8;
    WriteU64(p + o, in.parentActionInstanceHandle); o += 8;
    WriteU64(p + o, in.traceCorrelationId); o += 8;
    WriteU32(p + o, in.reason); o += 4;
    WriteU32(p + o, in.validityFlags); o += 4;
    WriteU64(p + o, payloadBytes); o += 8;
    WriteU32(p + o, in.telemetry.reserved0); o += 4;
    WriteU64(p + o, in.telemetry.arrangingInstance); o += 8;
    std::memcpy(p + o, harken::ObjectIdBytes(in.telemetry.clipId).data(), 16); o += 16;
    WriteU32(p + o, in.telemetry.loopIteration); o += 4;
    WriteU32(p + o, in.telemetry.occurrenceOrdinal); o += 4;
    WriteU64(p + o, in.telemetry.logicCommitSequence); o += 8;
    WriteU64(p + o, in.telemetry.executeFrame); o += 8;
    EXPECT_EQ(o, 316u);
    if (payload != nullptr && payloadBytes > 0)
        std::memcpy(p + o, payload, static_cast<size_t>(payloadBytes));
    return out;
}

VnMonitorRecordHeader LegalHeader(uint32_t kind, uint32_t streamKind) {
    VnMonitorRecordHeader h{};
    h.recordKind = kind;
    h.recordVersion = 1;
    h.flags = VN_MONITOR_RECORD_FLAG_REQUIRED;
    h.streamKind = streamKind;
    h.streamId = 1;
    h.producerId = 1;
    h.sourceSequence = 1;
    h.deliverySequence = 0;
    h.engineMonitorInstanceId = 1;
    h.monitorSessionEpoch = 1;
    h.worldGeneration = 1;
    h.capturePlanGeneration = 1;
    h.renderPlanGeneration = 1;
    h.transportEpoch = 1;
    h.deviceEpoch = 1;
    h.clockAnchorGeneration = 1;
    h.measurementSegmentGeneration = 1;
    h.executeFrame = 1;
    h.validityFlags = VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC;
    return h;
}

std::vector<uint8_t> PackEvent(const VnMonitorRecordHeader& in) {
    VnMonitorEventPayload event{};
    event.structSize = sizeof(event);
    event.eventClass = VN_MONITOR_EVENT_START;
    return PackRecord(in, reinterpret_cast<const uint8_t*>(&event), sizeof(event));
}

std::vector<uint8_t> PackLegalEvent() {
    return PackEvent(LegalHeader(VN_MONITOR_RECORD_EVENT, VN_MONITOR_STREAM_EVENT));
}

} // namespace

TEST(MonitorFixture, RecordCheckedInLegalEvent) {
    const char* dir = HARKEN_MONITOR_FIXTURE_DIR;
    std::ifstream in(std::string(dir) + "/legal_event.bin", std::ios::binary);
    ASSERT_TRUE(in) << dir;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    ASSERT_GE(bytes.size(), 8u);
    uint64_t count = 0;
    std::memcpy(&count, bytes.data(), 8);
    ASSERT_EQ(count, 1u);
    VnMonitorDecodedRecord out{};
    uint8_t skip[8]{};
    auto r = vn_monitor_decode_records(bytes.data() + 8, bytes.size() - 8, &out, 1,
                                       skip, sizeof(skip));
    EXPECT_EQ(r.status, VN_OK);
    EXPECT_EQ(r.recordsWritten, 1u);
    EXPECT_EQ(out.header.recordKind, VN_MONITOR_RECORD_EVENT);
    EXPECT_EQ(out.header.deliverySequence, 0u);
    EXPECT_EQ(out.header.payloadBytes, sizeof(VnMonitorEventPayload));
    EXPECT_EQ(out.header.recordBytes % 8u, 0u);
    EXPECT_GE(out.header.recordBytes, VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES);
    EXPECT_EQ(out.numericReadable, 1u);
}

TEST(MonitorFixture, RecordLegalFourSurfaces) {
    VnMonitorStateUpsert upsert{};
    upsert.structSize = sizeof(upsert);
    upsert.entityKind = VN_MONITOR_ENTITY_VOICE;
    upsert.stateCutId = 9;
    upsert.stableId = harkentest::ObjectId(11);
    upsert.volumeDb = -6.0f;
    VnMonitorEventPayload event{};
    event.structSize = sizeof(event);
    event.eventClass = VN_MONITOR_EVENT_START;
    event.reason = 2;
    VnMonitorTimeSeriesSample series{};
    series.structSize = sizeof(series);
    series.metricId = 1;
    series.valueBits = 0;
    VnMonitorAudioChunk audio{};
    audio.structSize = sizeof(audio);
    audio.sampleRate = 48000;
    audio.channelCount = 2;

    auto a = PackRecord(LegalHeader(VN_MONITOR_RECORD_STATE_UPSERT,
                                    VN_MONITOR_STREAM_STATE),
                        reinterpret_cast<const uint8_t*>(&upsert),
                        sizeof(upsert));
    auto b = PackRecord(LegalHeader(VN_MONITOR_RECORD_EVENT,
                                    VN_MONITOR_STREAM_EVENT),
                        reinterpret_cast<const uint8_t*>(&event),
                        sizeof(event));
    auto c = PackRecord(LegalHeader(VN_MONITOR_RECORD_TIME_SERIES,
                                    VN_MONITOR_STREAM_TIME_SERIES),
                        reinterpret_cast<const uint8_t*>(&series),
                        sizeof(series));
    auto d = PackRecord(LegalHeader(VN_MONITOR_RECORD_AUDIO_CHUNK,
                                    VN_MONITOR_STREAM_OPTIONAL_AUDIO),
                        reinterpret_cast<const uint8_t*>(&audio),
                        sizeof(audio));
    std::vector<uint8_t> batch;
    batch.insert(batch.end(), a.begin(), a.end());
    batch.insert(batch.end(), b.begin(), b.end());
    batch.insert(batch.end(), c.begin(), c.end());
    batch.insert(batch.end(), d.begin(), d.end());
    VnMonitorDecodedRecord recs[4]{};
    uint8_t skip[8]{};
    auto r = vn_monitor_decode_records(batch.data(), batch.size(), recs, 4, skip,
                                       sizeof(skip));
    EXPECT_EQ(r.status, VN_OK);
    EXPECT_EQ(r.recordsWritten, 4u);
    EXPECT_EQ(recs[0].header.recordKind, VN_MONITOR_RECORD_STATE_UPSERT);
    EXPECT_EQ(recs[1].header.recordKind, VN_MONITOR_RECORD_EVENT);
    EXPECT_EQ(recs[2].header.recordKind, VN_MONITOR_RECORD_TIME_SERIES);
    EXPECT_EQ(recs[3].header.recordKind, VN_MONITOR_RECORD_AUDIO_CHUNK);
    EXPECT_EQ(recs[0].numericReadable, 1u);
}

TEST(MonitorFixture, RecordRequiredUnknownKind) {
    auto rec = PackRecord(LegalHeader(200, VN_MONITOR_STREAM_STATE), nullptr, 0);
    VnMonitorDecodedRecord out{};
    uint8_t skip[8]{};
    auto r = vn_monitor_decode_records(rec.data(), rec.size(), &out, 1, skip,
                                       sizeof(skip));
    EXPECT_EQ(r.status, VN_MONITOR_INCOMPATIBLE);
    EXPECT_EQ(r.recordsWritten, 0u);
    EXPECT_EQ(r.bytesConsumed, 0u);
}

TEST(MonitorFixture, RecordRequiredUnknownVersion) {
    auto h = LegalHeader(VN_MONITOR_RECORD_EVENT, VN_MONITOR_STREAM_EVENT);
    h.recordVersion = 99;
    auto rec = PackEvent(h);
    VnMonitorDecodedRecord out{};
    uint8_t skip[8]{};
    auto r = vn_monitor_decode_records(rec.data(), rec.size(), &out, 1, skip,
                                       sizeof(skip));
    EXPECT_EQ(r.status, VN_MONITOR_INCOMPATIBLE);
    EXPECT_EQ(r.recordsWritten, 0u);
}

TEST(MonitorFixture, RecordOptionalUnknownSkipped) {
    auto unknown = LegalHeader(200, VN_MONITOR_STREAM_EVENT);
    unknown.flags = VN_MONITOR_RECORD_FLAG_OPTIONAL;
    auto mid = PackRecord(unknown, nullptr, 0);
    auto legal = PackLegalEvent();
    std::vector<uint8_t> batch;
    batch.insert(batch.end(), mid.begin(), mid.end());
    batch.insert(batch.end(), legal.begin(), legal.end());
    VnMonitorDecodedRecord recs[2]{};
    std::array<uint8_t, 1024> skip{};
    auto r = vn_monitor_decode_records(batch.data(), batch.size(), recs, 2,
                                       skip.data(), skip.size());
    EXPECT_EQ(r.status, VN_OK);
    EXPECT_EQ(r.recordsWritten, 2u);
    EXPECT_EQ(recs[0].skippedBytesCount, mid.size());
    EXPECT_EQ(recs[1].header.recordKind, VN_MONITOR_RECORD_EVENT);
}

TEST(MonitorFixture, RecordNativeMemcpyIncompatible) {
    VnMonitorRecordHeader native{};
    native.structSize = sizeof(native);
    native.recordKind = VN_MONITOR_RECORD_EVENT;
    native.recordVersion = 1;
    native.flags = VN_MONITOR_RECORD_FLAG_REQUIRED;
    native.recordBytes = sizeof(native);
    native.sourceSequence = 1;
    std::array<uint8_t, sizeof(VnMonitorRecordHeader) + 16> buf{};
    std::memcpy(buf.data(), &native, sizeof(native));
    VnMonitorDecodedRecord out{};
    uint8_t skip[8]{};
    auto r = vn_monitor_decode_records(buf.data(), buf.size(), &out, 1, skip,
                                       sizeof(skip));
    EXPECT_EQ(r.status, VN_MONITOR_INCOMPATIBLE);
}

TEST(MonitorFixture, RecordUnalignedOrBadSize) {
    auto rec = PackLegalEvent();
    rec[16] = 1; /* corrupt recordBytes low byte to odd */
    VnMonitorDecodedRecord out{};
    uint8_t skip[8]{};
    auto r = vn_monitor_decode_records(rec.data(), rec.size(), &out, 1, skip,
                                       sizeof(skip));
    EXPECT_EQ(r.status, VN_MONITOR_INCOMPATIBLE);
    EXPECT_EQ(r.bytesConsumed, 0u);
}

TEST(MonitorFixture, RecordSourceSequenceZero) {
    auto h = LegalHeader(VN_MONITOR_RECORD_EVENT, VN_MONITOR_STREAM_EVENT);
    h.sourceSequence = 0;
    auto rec = PackEvent(h);
    VnMonitorDecodedRecord out{};
    uint8_t skip[8]{};
    auto r = vn_monitor_decode_records(rec.data(), rec.size(), &out, 1, skip,
                                       sizeof(skip));
    EXPECT_EQ(r.status, VN_INVALID_ARG);
    EXPECT_EQ(r.bytesConsumed, 0u);
}

TEST(MonitorFixture, RecordSequenceExhausted) {
    auto h = LegalHeader(VN_MONITOR_RECORD_EVENT, VN_MONITOR_STREAM_EVENT);
    h.sourceSequence = UINT64_MAX;
    auto rec = PackEvent(h);
    VnMonitorDecodedRecord out{};
    uint8_t skip[8]{};
    auto r = vn_monitor_decode_records(rec.data(), rec.size(), &out, 1, skip,
                                       sizeof(skip));
    EXPECT_EQ(r.status, VN_OK);
    EXPECT_EQ(out.header.sourceSequence, UINT64_MAX);
}

TEST(MonitorFixture, RecordGenerationFieldsDistinct) {
    auto h = LegalHeader(VN_MONITOR_RECORD_EVENT, VN_MONITOR_STREAM_EVENT);
    h.worldGeneration = 2;
    h.capturePlanGeneration = 3;
    h.clockAnchorGeneration = 4;
    auto rec = PackEvent(h);
    VnMonitorDecodedRecord out{};
    uint8_t skip[8]{};
    auto r = vn_monitor_decode_records(rec.data(), rec.size(), &out, 1, skip,
                                       sizeof(skip));
    EXPECT_EQ(r.status, VN_OK);
    EXPECT_EQ(out.header.worldGeneration, 2u);
    EXPECT_EQ(out.header.capturePlanGeneration, 3u);
    EXPECT_EQ(out.header.clockAnchorGeneration, 4u);
}

TEST(MonitorFixture, RecordDropStagesIndependent) {
    VnMonitorDropSummary drop{};
    drop.structSize = sizeof(drop);
    drop.stage = VN_MONITOR_DROP_SOURCE_PRODUCER;
    drop.streamId = 1;
    drop.firstSourceSequence = 2;
    drop.lastSourceSequence = 4;
    auto rec = PackRecord(LegalHeader(VN_MONITOR_RECORD_DROP_SUMMARY,
                                      VN_MONITOR_STREAM_EVENT),
                          reinterpret_cast<const uint8_t*>(&drop),
                          sizeof(drop));
    VnMonitorDecodedRecord out{};
    uint8_t skip[8]{};
    auto r = vn_monitor_decode_records(rec.data(), rec.size(), &out, 1, skip,
                                       sizeof(skip));
    EXPECT_EQ(r.status, VN_OK);
    const auto* decoded = reinterpret_cast<const VnMonitorDropSummary*>(out.payload);
    EXPECT_EQ(decoded->stage, VN_MONITOR_DROP_SOURCE_PRODUCER);
    EXPECT_EQ(out.header.deliverySequence, 0u);
}

TEST(MonitorFixture, RecordStateCutCompleteView) {
    VnMonitorStateCutBegin begin{};
    begin.structSize = sizeof(begin);
    begin.stateCutId = 7;
    begin.entityTrueCount[VN_MONITOR_ENTITY_VOICE] = 1;
    VnMonitorStateUpsert upsert{};
    upsert.structSize = sizeof(upsert);
    upsert.stateCutId = 7;
    upsert.entityKind = VN_MONITOR_ENTITY_VOICE;
    upsert.stableId = harkentest::ObjectId(11);
    VnMonitorStateCutEnd end{};
    end.structSize = sizeof(end);
    end.stateCutId = 7;
    end.result = VN_MONITOR_STATE_CUT_COMPLETE;
    end.checksumAlgorithm = VN_MONITOR_CHECKSUM_CRC32C;
    end.emittedCount[VN_MONITOR_ENTITY_VOICE] = 1;
    auto b = PackRecord(LegalHeader(VN_MONITOR_RECORD_STATE_CUT_BEGIN,
                                    VN_MONITOR_STREAM_STATE),
                        reinterpret_cast<const uint8_t*>(&begin), sizeof(begin));
    auto u = PackRecord(LegalHeader(VN_MONITOR_RECORD_STATE_UPSERT,
                                    VN_MONITOR_STREAM_STATE),
                        reinterpret_cast<const uint8_t*>(&upsert), sizeof(upsert));
    end.checksum = vn_monitor_crc32c(u.data() + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES,
                                     sizeof(upsert));
    auto e = PackRecord(LegalHeader(VN_MONITOR_RECORD_STATE_CUT_END,
                                    VN_MONITOR_STREAM_STATE),
                        reinterpret_cast<const uint8_t*>(&end), sizeof(end));
    std::vector<uint8_t> batch;
    batch.insert(batch.end(), b.begin(), b.end());
    batch.insert(batch.end(), u.begin(), u.end());
    batch.insert(batch.end(), e.begin(), e.end());
    VnMonitorDecodedRecord recs[3]{};
    uint8_t skip[8]{};
    auto r = vn_monitor_decode_records(batch.data(), batch.size(), recs, 3, skip,
                                       sizeof(skip));
    ASSERT_EQ(r.status, VN_OK);
    ASSERT_EQ(r.recordsWritten, 3u);
    const auto* decodedEnd =
        reinterpret_cast<const VnMonitorStateCutEnd*>(recs[2].payload);
    EXPECT_EQ(decodedEnd->result, VN_MONITOR_STATE_CUT_COMPLETE);
    EXPECT_EQ(decodedEnd->checksum,
              vn_monitor_crc32c(recs[1].payload, recs[1].payloadBytes));
}

TEST(MonitorFixture, RecordStateCutIncompleteNotDeletion) {
    VnMonitorStateCutEnd end{};
    end.structSize = sizeof(end);
    end.result = VN_MONITOR_STATE_CUT_TRUNCATED;
    end.checksumAlgorithm = VN_MONITOR_CHECKSUM_CRC32C;
    auto rec = PackRecord(LegalHeader(VN_MONITOR_RECORD_STATE_CUT_END,
                                      VN_MONITOR_STREAM_STATE),
                          reinterpret_cast<const uint8_t*>(&end), sizeof(end));
    VnMonitorDecodedRecord out{};
    uint8_t skip[8]{};
    auto r = vn_monitor_decode_records(rec.data(), rec.size(), &out, 1, skip,
                                       sizeof(skip));
    EXPECT_EQ(r.status, VN_OK);
    const auto* decoded = reinterpret_cast<const VnMonitorStateCutEnd*>(out.payload);
    EXPECT_EQ(decoded->result, VN_MONITOR_STATE_CUT_TRUNCATED);
}

TEST(MonitorFixture, RecordTombstoneIsDelete) {
    VnMonitorStateTombstone tomb{};
    tomb.structSize = sizeof(tomb);
    tomb.entityKind = VN_MONITOR_ENTITY_VOICE;
    tomb.stableId = harkentest::ObjectId(11);
    tomb.reason = 1;
    auto rec = PackRecord(LegalHeader(VN_MONITOR_RECORD_STATE_TOMBSTONE,
                                      VN_MONITOR_STREAM_STATE),
                          reinterpret_cast<const uint8_t*>(&tomb), sizeof(tomb));
    VnMonitorDecodedRecord out{};
    uint8_t skip[8]{};
    auto r = vn_monitor_decode_records(rec.data(), rec.size(), &out, 1, skip,
                                       sizeof(skip));
    EXPECT_EQ(r.status, VN_OK);
    EXPECT_EQ(out.header.recordKind, VN_MONITOR_RECORD_STATE_TOMBSTONE);
}

TEST(MonitorFixture, RecordWaveformFields) {
    VnMonitorWaveformBucket bucket{};
    bucket.structSize = sizeof(bucket);
    bucket.bucketFrames = 64;
    bucket.channelMode = VN_MONITOR_CHANNEL_PER_CHANNEL;
    bucket.partialDiscard = 1;
    auto rec = PackRecord(LegalHeader(VN_MONITOR_RECORD_WAVEFORM_BUCKET,
                                      VN_MONITOR_STREAM_OPTIONAL_AUDIO),
                          reinterpret_cast<const uint8_t*>(&bucket),
                          sizeof(bucket));
    VnMonitorDecodedRecord out{};
    uint8_t skip[8]{};
    auto r = vn_monitor_decode_records(rec.data(), rec.size(), &out, 1, skip,
                                       sizeof(skip));
    EXPECT_EQ(r.status, VN_OK);
    const auto* decoded = reinterpret_cast<const VnMonitorWaveformBucket*>(out.payload);
    EXPECT_EQ(decoded->bucketFrames, 64u);
    EXPECT_EQ(decoded->partialDiscard, 1u);
}

TEST(MonitorFixture, RecordScopeFieldsNoRecompute) {
    VnMonitorScopeResult scope{};
    scope.structSize = sizeof(scope);
    scope.preTriggerFrames = 8;
    scope.postTriggerFrames = 8;
    scope.maxSearchFrames = 64;
    scope.executedPreTriggerFrames = 8;
    scope.noTrigger = 1;
    auto rec = PackRecord(LegalHeader(VN_MONITOR_RECORD_SCOPE_RESULT,
                                      VN_MONITOR_STREAM_OPTIONAL_AUDIO),
                          reinterpret_cast<const uint8_t*>(&scope),
                          sizeof(scope));
    VnMonitorDecodedRecord out{};
    uint8_t skip[8]{};
    auto r = vn_monitor_decode_records(rec.data(), rec.size(), &out, 1, skip,
                                       sizeof(skip));
    EXPECT_EQ(r.status, VN_OK);
    const auto* decoded = reinterpret_cast<const VnMonitorScopeResult*>(out.payload);
    EXPECT_EQ(decoded->preTriggerFrames, 8u);
    EXPECT_EQ(decoded->maxSearchFrames, 64u);
    EXPECT_EQ(decoded->noTrigger, 1u);
}

TEST(MonitorFixture, RecordMeterSpectrumGr) {
    VnMonitorMeterPayload meter{};
    meter.structSize = sizeof(meter);
    meter.lufsSupported = 0;
    VnMonitorSpectrumPayload spec{};
    spec.structSize = sizeof(spec);
    spec.fftSize = 1024;
    VnMonitorGainReductionPayload gr{};
    gr.structSize = sizeof(gr);
    gr.reductionDbBits = 0;
    auto a = PackRecord(LegalHeader(VN_MONITOR_RECORD_METER,
                                    VN_MONITOR_STREAM_TIME_SERIES),
                        reinterpret_cast<const uint8_t*>(&meter), sizeof(meter));
    auto b = PackRecord(LegalHeader(VN_MONITOR_RECORD_SPECTRUM,
                                    VN_MONITOR_STREAM_OPTIONAL_AUDIO),
                        reinterpret_cast<const uint8_t*>(&spec), sizeof(spec));
    auto c = PackRecord(LegalHeader(VN_MONITOR_RECORD_GAIN_REDUCTION,
                                    VN_MONITOR_STREAM_TIME_SERIES),
                        reinterpret_cast<const uint8_t*>(&gr), sizeof(gr));
    std::vector<uint8_t> batch;
    batch.insert(batch.end(), a.begin(), a.end());
    batch.insert(batch.end(), b.begin(), b.end());
    batch.insert(batch.end(), c.begin(), c.end());
    VnMonitorDecodedRecord recs[3]{};
    uint8_t skip[8]{};
    auto r = vn_monitor_decode_records(batch.data(), batch.size(), recs, 3, skip,
                                       sizeof(skip));
    EXPECT_EQ(r.status, VN_OK);
    EXPECT_EQ(reinterpret_cast<const VnMonitorMeterPayload*>(recs[0].payload)
                  ->lufsSupported,
              0u);
}

TEST(MonitorFixture, RecordNumericNotReadable) {
    auto h = LegalHeader(VN_MONITOR_RECORD_TIME_SERIES,
                         VN_MONITOR_STREAM_TIME_SERIES);
    h.validityFlags = VN_MONITOR_VALID_NUMERIC | VN_MONITOR_VALID_UNSUPPORTED;
    VnMonitorTimeSeriesSample series{};
    series.structSize = sizeof(series);
    auto rec = PackRecord(h, reinterpret_cast<const uint8_t*>(&series),
                          sizeof(series));
    VnMonitorDecodedRecord out{};
    uint8_t skip[8]{};
    auto r = vn_monitor_decode_records(rec.data(), rec.size(), &out, 1, skip,
                                       sizeof(skip));
    EXPECT_EQ(r.status, VN_OK);
    EXPECT_EQ(out.numericReadable, 0u);
}

TEST(MonitorFixture, RecordArmedNumericZero) {
    auto h = LegalHeader(VN_MONITOR_RECORD_TIME_SERIES,
                         VN_MONITOR_STREAM_TIME_SERIES);
    VnMonitorTimeSeriesSample series{};
    series.structSize = sizeof(series);
    series.valueBits = 0;
    auto rec = PackRecord(h, reinterpret_cast<const uint8_t*>(&series),
                          sizeof(series));
    VnMonitorDecodedRecord out{};
    uint8_t skip[8]{};
    auto r = vn_monitor_decode_records(rec.data(), rec.size(), &out, 1, skip,
                                       sizeof(skip));
    EXPECT_EQ(r.status, VN_OK);
    EXPECT_EQ(out.numericReadable, 1u);
    const auto* decoded =
        reinterpret_cast<const VnMonitorTimeSeriesSample*>(out.payload);
    EXPECT_EQ(decoded->valueBits, 0u);
}

TEST(MonitorFixture, RecordSourceDeliveryMustBeZero) {
    auto h = LegalHeader(VN_MONITOR_RECORD_EVENT, VN_MONITOR_STREAM_EVENT);
    h.deliverySequence = 9;
    auto rec = PackEvent(h);
    VnMonitorDecodedRecord out{};
    uint8_t skip[8]{};
    auto r = vn_monitor_decode_records(rec.data(), rec.size(), &out, 1, skip,
                                       sizeof(skip));
    EXPECT_EQ(r.status, VN_INVALID_ARG);
    EXPECT_EQ(r.recordsWritten, 0u);
    EXPECT_EQ(r.bytesConsumed, 0u);
}

TEST(MonitorFixture, RecordRequiredInvalidKind) {
    auto rec = PackRecord(LegalHeader(VN_MONITOR_RECORD_INVALID,
                                      VN_MONITOR_STREAM_EVENT),
                          nullptr, 0);
    VnMonitorDecodedRecord out{};
    uint8_t skip[8]{};
    auto r = vn_monitor_decode_records(rec.data(), rec.size(), &out, 1, skip,
                                       sizeof(skip));
    EXPECT_EQ(r.status, VN_MONITOR_INCOMPATIBLE);
    EXPECT_EQ(r.recordsWritten, 0u);
    EXPECT_EQ(r.bytesConsumed, 0u);
}

TEST(MonitorFixture, RecordCrc32cRfc3720) {
    const uint8_t zeros[32]{};
    EXPECT_EQ(vn_monitor_crc32c(zeros, sizeof(zeros)), 0x8A9136AAu);
}

TEST(MonitorFixture, RecordPayloadStructSizeFirst) {
    EXPECT_EQ(offsetof(VnMonitorStateCutBegin, structSize), 0u);
    EXPECT_EQ(offsetof(VnMonitorStateUpsert, structSize), 0u);
    EXPECT_EQ(offsetof(VnMonitorStateTombstone, structSize), 0u);
    EXPECT_EQ(offsetof(VnMonitorStateCutEnd, structSize), 0u);
    EXPECT_EQ(offsetof(VnMonitorEventPayload, structSize), 0u);
    EXPECT_EQ(offsetof(VnMonitorTimeSeriesSample, structSize), 0u);
    EXPECT_EQ(offsetof(VnMonitorAudioChunk, structSize), 0u);
    EXPECT_EQ(offsetof(VnMonitorWaveformBucket, structSize), 0u);
    EXPECT_EQ(offsetof(VnMonitorScopeResult, structSize), 0u);
    EXPECT_EQ(offsetof(VnMonitorSpectrumPayload, structSize), 0u);
    EXPECT_EQ(offsetof(VnMonitorMeterPayload, structSize), 0u);
    EXPECT_EQ(offsetof(VnMonitorGainReductionPayload, structSize), 0u);
    EXPECT_EQ(offsetof(VnMonitorAggregatePayload, structSize), 0u);
    EXPECT_EQ(VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES, 316u);
    EXPECT_NE(sizeof(VnMonitorRecordHeader),
              static_cast<size_t>(VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES));
}

TEST(MonitorFixture, RecordPayloadBytesOverflowRejected) {
    auto rec = PackLegalEvent();
    WriteU64(rec.data() + 256, UINT64_MAX);
    VnMonitorDecodedRecord out{};
    uint8_t skip[8]{};
    auto r = vn_monitor_decode_records(rec.data(), rec.size(), &out, 1, skip,
                                       sizeof(skip));
    EXPECT_EQ(r.status, VN_MONITOR_INCOMPATIBLE);
    EXPECT_EQ(r.recordsWritten, 0u);
    EXPECT_EQ(r.bytesConsumed, 0u);
}
