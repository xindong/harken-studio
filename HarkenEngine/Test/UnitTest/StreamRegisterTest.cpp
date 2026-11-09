#include "Common/VorbisFixture.h"
#include "harken/media/VorbisDecoder.h"
#include "Common/PcmStreamFixture.h"
// Vn_RegisterStreamFile / Vn_UnregisterStreamFile validation: the
// streaming thread performs the open and the header/TOC/checksum-table
// verification during the (blocking) call, so every refusal here is
// observable synchronously and must leave no trace behind. Also covers
// the cross-namespace exclusivity with Vn_RegisterPCM — one VnSoundId
// names one asset, resident or streamed, never both.
#include "Common/ObjectIdTestSupport.h"
#include "Common/StreamBuilder.h"
#include "Common/TestUtil.h"
#include "Common/MonitorExecutionTestSupport.h"

#include "harken/media/StreamFormat.h"
#include "harken/media/StreamManager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#undef ReplaceFile
#endif

#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif

namespace harken::detail {
struct StreamManagerTestAccess {
    static std::size_t MetadataRequests(StreamManager& manager) {
        std::lock_guard<std::mutex> lock(manager.m_mutex);
        return std::count_if(manager.m_sourceRequests.begin(), manager.m_sourceRequests.end(),
            [](const auto& request) { return request && request->metadataFile; });
    }
    static VnResult MetadataResult(StreamManager& manager, std::uint32_t index) {
        std::lock_guard<std::mutex> lock(manager.m_mutex);
        return manager.m_files[index]->metadataResult;
    }
    static VnMonitorStreamFactV1 Observation(StreamManager& manager, std::uint32_t slot) {
        std::lock_guard<std::mutex> lock(manager.m_mutex);
        return manager.m_slots[slot].observation;
    }
    static void SetCommitPageLimit(StreamManager& manager, std::size_t pages) {
        manager.m_ringStorage.SetCommitPageLimitForTests(pages);
    }
    static std::size_t CommittedBytes(const StreamManager& manager) {
        return manager.m_ringStorage.CommittedBytes();
    }
    static std::shared_ptr<const StreamByteSource>& Input(StreamManager& manager, std::uint32_t file) {
        return manager.m_files[file]->source;
    }
    static void ServeSlots(StreamManager& manager) {
        StreamManager::Request request;
        request.kind = StreamManager::Request::Kind::Serve;
        request.forceServe = true;
        EXPECT_EQ(VN_OK, manager.PostAndWait(request));
    }
    static VnResult ServeAndWait(StreamManager& manager) {
        StreamManager::Request request;
        request.kind = StreamManager::Request::Kind::Serve;
        return manager.PostAndWait(request);
    }
    static void ExhaustSourceGenerations(StreamManager& manager) {
        std::lock_guard<std::mutex> lock(manager.m_mutex);
        manager.m_lastSourceGeneration = UINT64_MAX;
    }
    static bool StagedDataMatchesVerifiedChunk(const StreamManager& manager,
                                               std::uint32_t slot) {
        const auto& source = manager.m_slots[slot];
        if (source.stagedChunk == StreamManager::kNoChunk) return true;
        const auto& file = *manager.m_files[source.fileIndex];
        const auto bytes = StreamChunkBytes(file.chunkFrames, file.toc.channelCount);
        return source.stagedChunk < file.chunkChecksums.size() &&
            source.staging.size() * sizeof(float) == bytes &&
            Fnv1a64(source.staging.data(), static_cast<std::size_t>(bytes)) ==
                file.chunkChecksums[source.stagedChunk];
    }
};
} // namespace harken::detail

namespace {

using harkentest::BuildStreamBytes;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::StreamSpec;
using harkentest::WriteFileBytes;

constexpr std::uint32_t kRate = 48000;

struct StreamRequestFaultGuard {
    explicit StreamRequestFaultGuard(harken::StreamRequestFault fault) {
        harken::SetStreamRequestFaultForTests(fault);
    }
    ~StreamRequestFaultGuard() {
        harken::SetStreamRequestFaultForTests(
            harken::StreamRequestFault::None);
    }
    StreamRequestFaultGuard(const StreamRequestFaultGuard&) = delete;
    StreamRequestFaultGuard& operator=(const StreamRequestFaultGuard&) =
        delete;
};

std::string TempPath(const std::string& name) {
    return std::string(HARKEN_TEMP_DIR "/") + name;
}

// Recomputes the header's tocChecksum from the (possibly patched) TOC
// entry and chunk-checksum table, so a test can alter a validated TOC
// field and prove the refusal comes from field validation rather than
// from the checksum tripping first.
void FixTocChecksum(std::vector<std::uint8_t>* bytes) {
    harken::StreamHeader header;
    ASSERT_TRUE(harken::ParseStreamHeader(bytes->data(), &header));
    const harken::StreamTocEntry entry = harken::ParseStreamTocEntry(
        bytes->data() + header.tocOffset);
    std::uint64_t checksum =
        harken::Fnv1a64(bytes->data() + header.tocOffset,
                       harken::kStreamTocEntryBytes);
    checksum = harken::Fnv1a64(
        bytes->data() + static_cast<std::size_t>(entry.chunkChecksumOffset),
        static_cast<std::size_t>(entry.chunkCount) *
            harken::kStreamChunkChecksumBytes,
        checksum);
    header.tocChecksum = checksum;
    harken::SerializeStreamHeader(header, bytes->data());
}

std::vector<std::uint8_t> MakeValidImage(VnObjectId soundId,
                                         std::size_t frames = 1000) {
    StreamSpec spec;
    spec.soundId = soundId;
    spec.sampleRate = kRate;
    spec.channelCount = 1;
    spec.chunkFrames = 256;
    const std::vector<float> tone =
        harkentest::MakeSine(frames, 500.0, kRate, 0.5f);
    return BuildStreamBytes(spec, tone);
}

class ReadObserver final {
    class Source final : public harken::StreamByteSource {
    public:
        std::shared_ptr<const harken::StreamByteSource> original;
        std::function<void()> callback;
        const bool* failReads = nullptr;
        std::size_t maxRead = SIZE_MAX;
        bool afterRead = false;
        std::uint64_t ByteCount() const noexcept override { return original->ByteCount(); }
        harken::StreamReadResult ReadAt(std::uint64_t offset, void* data, std::size_t size,
                                       harken::StreamReadCancellation cancel) const override {
            if (!afterRead) callback();
            if (failReads && *failReads) return {};
            const auto result = original->ReadAt(offset, data, std::min(size, maxRead), cancel);
            if (afterRead) callback();
            return result;
        }
    };
public:
    ReadObserver(std::shared_ptr<const harken::StreamByteSource>& input,
                 std::function<void()> callback, const bool* failReads = nullptr,
                 bool afterRead = false, std::size_t maxRead = SIZE_MAX)
        : input_(input), original_(input) {
        auto observed = std::make_shared<Source>();
        observed->original = original_;
        observed->callback = std::move(callback);
        observed->failReads = failReads;
        observed->maxRead = maxRead;
        observed->afterRead = afterRead;
        input_ = std::move(observed);
    }
    ~ReadObserver() { Restore(); }
    void Restore() {
        if (!restored_) { input_ = original_; restored_ = true; }
    }
private:
    std::shared_ptr<const harken::StreamByteSource>& input_;
    std::shared_ptr<const harken::StreamByteSource> original_;
    bool restored_ = false;
};

std::vector<float> ServiceSamples(std::size_t frames, unsigned bias) {
    std::vector<float> samples(frames);
    for (std::size_t i = 0; i < frames; ++i)
        samples[i] = static_cast<float>(bias + i + 1) * 0x1p-10f;
    return samples;
}

void ExpectAndConsume(harken::StreamRing& ring, const std::vector<float>& expected,
                       std::size_t& position, std::uint32_t count) {
    ASSERT_LE(position + count, expected.size());
    ASSERT_GE(ring.BufferedFrames(), count);
    while (count != 0) {
        const auto span = std::min(count, ring.BufferedFrames());
        ASSERT_GT(span, 0u);
        for (std::uint32_t i = 0; i < span; ++i)
            EXPECT_EQ(expected[position + i], harkentest::PeekPcmSample(ring, i)) << position + i;
        harkentest::ConsumePcmFrames(ring, span);
        position += span;
        count -= span;
    }
}

harken::StreamManager::ReplacementRequest ReplacementAt(
    const harken::StreamManager::StreamFacts& facts, std::uint64_t frame = 0) {
    harken::StreamManager::ReplacementRequest request;
    request.fileIndex = facts.fileIndex;
    request.sourceGeneration = facts.sourceGeneration;
    request.contentRevision = facts.contentRevision;
    request.targetFrame = frame;
    request.loopEndFrame = facts.frameCount;
    return request;
}

template<class Predicate> bool WaitUntil(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return predicate();
}

struct ReadGate {
    std::atomic<bool> entered{false}, release{false};
    std::atomic<std::uint64_t> offset{UINT64_MAX};
};
struct ReleaseReadGate {
    ReadGate& gate;
    ~ReleaseReadGate() { gate.release.store(true, std::memory_order_release); }
};
class ControlledSource : public harken::StreamByteSource {
public:
    std::vector<std::uint8_t> bytes;
    std::shared_ptr<ReadGate> gate = std::make_shared<ReadGate>();
    std::shared_ptr<std::atomic<unsigned>> destroyed = std::make_shared<std::atomic<unsigned>>(0);
    std::size_t maxRead = SIZE_MAX;
    mutable std::atomic<unsigned> reads{0};
    bool cooperate = false;
    ~ControlledSource() override { destroyed->fetch_add(1); }
    std::uint64_t ByteCount() const noexcept override { return bytes.size(); }
    harken::StreamReadResult ReadAt(std::uint64_t offset, void* destination, std::size_t count,
                                    harken::StreamReadCancellation cancellation) const override {
        reads.fetch_add(1, std::memory_order_relaxed);
        if (offset == gate->offset.load(std::memory_order_acquire)) {
            gate->entered.store(true, std::memory_order_release);
            while (!gate->release.load(std::memory_order_acquire)) {
                if (cooperate && cancellation.IsCancelled()) return {harken::StreamReadStatus::Cancelled, 0};
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (cancellation.IsCancelled()) return {harken::StreamReadStatus::Cancelled, 0};
        if (offset > bytes.size() || count > bytes.size() - offset) return {};
        count = std::min(count, maxRead);
        std::memcpy(destination, bytes.data() + offset, count);
        return {harken::StreamReadStatus::Ok, count};
    }
};

TEST(StreamRegister, DefaultByteRingSurvivesBoundedMultichannelIoJitter) {
    class JitterSource final : public ControlledSource {
    public:
        std::atomic<bool> enabled{false};
        mutable std::atomic<unsigned> delayedReads{0};
        mutable std::atomic<std::uint64_t> maxDelayUs{0};
        harken::StreamReadResult ReadAt(std::uint64_t offset, void* destination, std::size_t count,
                                        harken::StreamReadCancellation cancel) const override {
            if (enabled.load(std::memory_order_acquire)) {
                constexpr unsigned delays[] = {10, 35, 75, 90};
                const auto index = delayedReads.fetch_add(1);
                const auto begin = std::chrono::steady_clock::now();
                std::this_thread::sleep_for(std::chrono::milliseconds(delays[index % 4]));
                const auto us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - begin).count());
                auto maximum = maxDelayUs.load();
                while (maximum < us && !maxDelayUs.compare_exchange_weak(maximum, us)) {}
            }
            return ControlledSource::ReadAt(offset, destination, count, cancel);
        }
    };
    constexpr unsigned blockFrames = 256, channels = 6, totalFrames = 48000 * 3;
    auto config = harkentest::MakeNullConfigLayout(2, blockFrames, kRate,
        VN_CHANNEL_LAYOUT_5_1, VN_CHANNEL_LAYOUT_5_1);
    config.maxStreams = 1;
    config.renderWorkerCount = 0;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    auto source = std::make_shared<JitterSource>();
    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(17501);
    spec.sampleRate = kRate; spec.channelCount = channels; spec.chunkFrames = 4096;
    std::vector<float> planar(totalFrames * channels);
    for (unsigned ch = 0; ch < channels; ++ch)
        std::fill_n(planar.data() + ch * totalFrames, totalFrames, .04f * (ch + 1));
    source->bytes = BuildStreamBytes(spec, planar);
    harken::StreamManager::SourceToken token;
    ASSERT_EQ(VN_OK, engine->streams.BeginSource(spec.soundId, source, false, 0, &token));
    ASSERT_TRUE(WaitUntil([&] { return engine->streams.QuerySource(token) == harken::StreamManager::PreparationState::Ready; }));
    ASSERT_EQ(VN_OK, engine->streams.TakeSource(token));
    auto play = harkentest::MakePlayDesc(spec.soundId);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    auto* ring = engine->streams.RingOf(0);
    ASSERT_EQ(256u * 1024, ring->capacity);
    ASSERT_EQ(10922u, ring->FrameCapacity());
    auto* render = engine->voices.Resolve(harkentest::RenderHandle(engine, voice));
    ASSERT_NE(nullptr, render);
    source->enabled.store(true, std::memory_order_release);
    const auto begin = std::chrono::steady_clock::now();
    unsigned starved = 0;
    for (unsigned block = 0; block < 375; ++block) {
        std::this_thread::sleep_until(begin + std::chrono::microseconds(
            static_cast<std::uint64_t>(block) * blockFrames * 1000000 / kRate));
        engine->nullBackend.Pump(1);
        if (render->starvedThisBlock) ++starved;
        const auto* output = engine->nullBackend.LastBlock();
        for (unsigned ch = 0; ch < channels; ++ch)
            EXPECT_NEAR(.04f * (ch + 1), output[(blockFrames - 1) * channels + ch], 1e-6f)
                << "block=" << block << " channel=" << ch;
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    EXPECT_EQ(0u, starved);
    EXPECT_GE(source->delayedReads.load(), 8u);
    RecordProperty("ring_bytes", ring->capacity);
    RecordProperty("ring_frames", ring->FrameCapacity());
    RecordProperty("delayed_reads", source->delayedReads.load());
    RecordProperty("max_io_delay_us", std::to_string(source->maxDelayUs.load()));
    RecordProperty("starved_blocks", starved);
}

struct NativeSourceState {
    std::vector<std::uint8_t> bytes;
    ReadGate gate;
    std::atomic<unsigned> reads{0}, releases{0};
    bool cooperate = true;
    std::size_t maxRead = SIZE_MAX;
    std::thread::id releaseThread;
    static std::int32_t Read(void* context, std::uint64_t offset, void* destination,
        std::uint32_t count, std::uint32_t* outRead, const VnReadCancellationViewV1* cancel) {
        auto& self = *static_cast<NativeSourceState*>(context);
        self.reads.fetch_add(1);
        *outRead = 0;
        if (offset == self.gate.offset.load()) {
            self.gate.entered.store(true);
            while (!self.gate.release.load()) {
                if (self.cooperate && cancel->isCancelled(cancel->context)) return VN_BYTE_READ_CANCELLED;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (cancel->isCancelled(cancel->context)) return VN_BYTE_READ_CANCELLED;
        if (offset > self.bytes.size() || count > self.bytes.size() - offset) return VN_BYTE_READ_IO_ERROR;
        *outRead = static_cast<std::uint32_t>(std::min<std::size_t>(count, self.maxRead));
        std::memcpy(destination, self.bytes.data() + offset, *outRead);
        return VN_BYTE_READ_OK;
    }
    static void Release(void* context) {
        auto& self = *static_cast<NativeSourceState*>(context);
        self.releaseThread = std::this_thread::get_id();
        self.releases.fetch_add(1, std::memory_order_release);
    }
    VnByteSourceDescV1 Descriptor() {
        return {sizeof(VnByteSourceDescV1), 0, bytes.size(), this, &Read, &Release};
    }
};

TEST(StreamRegister, PublicSourceAdmissionCancelAndNamespaceStayResponsive) {
    NativeSourceState blocked, refused;
    const auto id = harkentest::ObjectId(17330);
    blocked.bytes = MakeValidImage(id);
    refused.bytes = blocked.bytes;
    blocked.gate.offset.store(0);
    blocked.cooperate = false;
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ReleaseReadGate release{blocked.gate};
    const auto healthyId = harkentest::ObjectId(17331);
    const auto path = TempPath("stream_sdk_source.stream");
    WriteFileBytes(path, MakeValidImage(healthyId));
    VnStreamSourceRequest healthy = 0;
    ASSERT_EQ(VN_OK, Vn_BeginStreamFile(healthyId, VN_STREAM_SOURCE_REGISTER, path.c_str(), &healthy));
    VnStreamSourceInfoV1 info{sizeof(info)};
    ASSERT_TRUE(WaitUntil([&] { return Vn_QueryStreamSource(healthy, &info) == VN_OK && info.state == VN_STREAM_SOURCE_READY; }));
    ASSERT_EQ(VN_OK, Vn_CommitStreamSource(healthy));
    VnPlayDesc play = harkentest::MakePlayDesc(healthyId);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    auto desc = blocked.Descriptor();
    VnStreamSourceRequest pending = 0;
    ASSERT_EQ(VN_OK, Vn_BeginStreamSource(id, VN_STREAM_SOURCE_REGISTER, &desc, &pending));
    ASSERT_TRUE(WaitUntil([&] { return blocked.gate.entered.load(); }));
    EXPECT_EQ(VN_OK, Vn_Update());
    session.Engine()->nullBackend.Pump(1);
    EXPECT_EQ(VN_BACKPRESSURE, Vn_CommitStreamSource(pending));
    auto other = refused.Descriptor();
    VnStreamSourceRequest rejected = 99;
    EXPECT_EQ(VN_IN_USE, Vn_BeginStreamSource(id, VN_STREAM_SOURCE_REGISTER, &other, &rejected));
    EXPECT_EQ(0u, rejected);
    EXPECT_EQ(0u, refused.reads.load());
    EXPECT_EQ(0u, refused.releases.load());
    const std::vector<float> tone(64, 0.25f);
    auto pcm = harkentest::MonoDesc(tone, kRate);
    EXPECT_EQ(VN_IN_USE, Vn_RegisterPCM(id, &pcm));
    EXPECT_EQ(VN_OK, Vn_CancelStreamSource(pending));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_QueryStreamSource(pending, &info));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_CancelStreamSource(pending));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_CommitStreamSource(pending));
    EXPECT_EQ(0u, blocked.releases.load());
    EXPECT_EQ(VN_OK, Vn_RegisterPCM(id, &pcm));
    blocked.gate.release.store(true);
    ASSERT_TRUE(WaitUntil([&] { return blocked.releases.load(std::memory_order_acquire) == 1; }));
    EXPECT_NE(std::this_thread::get_id(), blocked.releaseThread);
    other.flags = 1;
    rejected = 99;
    EXPECT_EQ(VN_INVALID_ARG, Vn_BeginStreamSource(harkentest::ObjectId(17332), 0, &other, &rejected));
    EXPECT_EQ(0u, rejected);
    EXPECT_EQ(0u, refused.reads.load());
    EXPECT_EQ(0u, refused.releases.load());
}

TEST(StreamRegister, PublicReadyFailureAndReplacementKeepIndependentOwnership) {
    NativeSourceState first, second, failed, stale;
    const auto a = harkentest::ObjectId(17333), b = harkentest::ObjectId(17334);
    first.bytes = MakeValidImage(a); first.maxRead = 7;
    second.bytes = MakeValidImage(b);
    failed.bytes = {0, 1, 2, 3};
    stale.bytes = first.bytes;
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto d1 = first.Descriptor(), d2 = second.Descriptor(), bad = failed.Descriptor();
    VnStreamSourceRequest one = 0, two = 0, replacement = 0;
    ASSERT_EQ(VN_OK, Vn_BeginStreamSource(a, 0, &d1, &one));
    ASSERT_EQ(VN_OK, Vn_BeginStreamSource(b, 0, &d2, &two));
    VnStreamSourceInfoV1 i1{sizeof(i1)}, i2{sizeof(i2)};
    ASSERT_TRUE(WaitUntil([&] { return Vn_QueryStreamSource(one, &i1) == VN_OK &&
        Vn_QueryStreamSource(two, &i2) == VN_OK && i1.state == VN_STREAM_SOURCE_READY && i2.state == VN_STREAM_SOURCE_READY; }));
    ASSERT_EQ(VN_OK, Vn_CommitStreamSource(one));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_CommitStreamSource(one));
    ASSERT_EQ(VN_OK, Vn_BeginStreamSource(a, VN_STREAM_SOURCE_REPLACE, &bad, &replacement));
    ASSERT_TRUE(WaitUntil([&] { return Vn_QueryStreamSource(replacement, &i1) == VN_OK && i1.state == VN_STREAM_SOURCE_FAILED; }));
    EXPECT_EQ(VN_BAD_STREAM_FILE, Vn_CommitStreamSource(replacement));
    EXPECT_EQ(0u, failed.releases.load());
    harken::StreamManager::StreamFacts facts;
    EXPECT_TRUE(session.Engine()->streams.FindFile(a, &facts));
    EXPECT_EQ(1000u, facts.frameCount);
    EXPECT_EQ(VN_OK, Vn_CancelStreamSource(replacement));
    EXPECT_EQ(VN_OK, Vn_CancelStreamSource(two));
    ASSERT_TRUE(WaitUntil([&] { return failed.releases.load() == 1 && second.releases.load() == 1; }));
    EXPECT_EQ(0u, first.releases.load());
    auto staleDesc = stale.Descriptor();
    ASSERT_EQ(VN_OK, Vn_BeginStreamSource(a, VN_STREAM_SOURCE_REPLACE, &staleDesc, &replacement));
    ASSERT_TRUE(WaitUntil([&] { return Vn_QueryStreamSource(replacement, &i1) == VN_OK && i1.state == VN_STREAM_SOURCE_READY; }));
    EXPECT_EQ(VN_OK, Vn_UnregisterStreamFile(a));
    ASSERT_TRUE(WaitUntil([&] { return first.releases.load() == 1; }));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_CommitStreamSource(replacement));
    EXPECT_EQ(VN_OK, Vn_QueryStreamSource(replacement, &i1));
    EXPECT_EQ(VN_STREAM_SOURCE_FAILED, i1.state);
    EXPECT_EQ(VN_STALE_HANDLE, i1.result);
    EXPECT_EQ(0u, stale.releases.load());
    EXPECT_EQ(VN_OK, Vn_CancelStreamSource(replacement));
    ASSERT_TRUE(WaitUntil([&] { return stale.releases.load() == 1; }));
}

TEST(StreamRegister, PublicFailedWorkerAndTermDiscardWithoutTokenReuse) {
    NativeSourceState old, current;
    const auto id = harkentest::ObjectId(17335);
    old.bytes = MakeValidImage(id); current.bytes = old.bytes;
    VnStreamSourceRequest oldToken = 0;
    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        auto desc = old.Descriptor();
        old.gate.offset.store(0);
        ReleaseReadGate release{old.gate};
        ASSERT_EQ(VN_OK, Vn_BeginStreamSource(id, 0, &desc, &oldToken));
        ASSERT_TRUE(WaitUntil([&] { return old.gate.entered.load(); }));
        harken::SetStreamRequestFaultForTests(harken::StreamRequestFault::WorkerLoop);
        const bool failed = WaitUntil([&] { return session.Engine()->streams.WorkerState() == harken::StreamWorkerState::Failed; });
        harken::SetStreamRequestFaultForTests(harken::StreamRequestFault::None);
        ASSERT_TRUE(failed);
        VnStreamSourceInfoV1 info{sizeof(info)};
        EXPECT_EQ(VN_OK, Vn_QueryStreamSource(oldToken, &info));
        EXPECT_EQ(VN_STREAM_SOURCE_FAILED, info.state);
        EXPECT_EQ(VN_DEVICE_ERROR, info.result);
        EXPECT_EQ(VN_OK, Vn_CancelStreamSource(oldToken));
        EXPECT_EQ(VN_STALE_HANDLE, Vn_QueryStreamSource(oldToken, &info));
    }
    EXPECT_EQ(1u, old.releases.load());
    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        auto desc = current.Descriptor();
        current.gate.offset.store(0);
        ReleaseReadGate release{current.gate};
        VnStreamSourceRequest token = 0;
        ASSERT_EQ(VN_OK, Vn_BeginStreamSource(id, 0, &desc, &token));
        EXPECT_NE(oldToken, token);
        EXPECT_EQ(VN_STALE_HANDLE, Vn_CancelStreamSource(oldToken));
        ASSERT_TRUE(WaitUntil([&] { return current.gate.entered.load(); }));
        // Term owns the accepted request even when the caller never cancels.
        EXPECT_EQ(VN_OK, Vn_Term());
        EXPECT_EQ(1u, current.releases.load());
    }
}

TEST(StreamRegister, SourceShortReadsAndIndependentReadyResults) {
    using Manager = harken::StreamManager;
    harken::EngineConfig config{};
    config.maxStreams = 2;
    config.streamRingBytes = (64) * sizeof(float);
    Manager streams(config);
    auto first = std::make_shared<ControlledSource>();
    auto second = std::make_shared<ControlledSource>();
    const auto id = harkentest::ObjectId(17300);
    first->bytes = MakeValidImage(id);
    first->maxRead = 3;
    second->bytes = MakeValidImage(harkentest::ObjectId(17301));
    Manager::SourceToken a, b, refused;
    ASSERT_EQ(VN_OK, streams.BeginSource(id, first, false, 0, &a));
    EXPECT_EQ(VN_IN_USE, streams.BeginSource(id, first, false, 0, &refused));
    ASSERT_EQ(VN_OK, streams.BeginSource(harkentest::ObjectId(17301), second, false, 0, &b));
    ASSERT_TRUE(WaitUntil([&] { return streams.QuerySource(a) == Manager::PreparationState::Ready &&
        streams.QuerySource(b) == Manager::PreparationState::Ready; }));
    const auto thirdId = harkentest::ObjectId(17302);
    const auto path = TempPath("stream_source_while_ready.stream");
    WriteFileBytes(path, MakeValidImage(thirdId));
    ASSERT_EQ(VN_OK, streams.RegisterFile(thirdId, path.c_str()));
    Manager::StreamFacts facts;
    ASSERT_EQ(VN_OK, streams.TakeSource(a, &facts));
    EXPECT_EQ(VN_STALE_HANDLE, streams.TakeSource(a));
    EXPECT_EQ(Manager::PreparationState::Invalid, streams.QuerySource(a));
    EXPECT_TRUE(streams.CancelSource(b));
    EXPECT_TRUE(WaitUntil([&] { return streams.QuerySource(b) == Manager::PreparationState::Invalid; }));
    EXPECT_FALSE(streams.HasPendingSource(harkentest::ObjectId(17301)));
    EXPECT_TRUE(streams.PrepareSlotWorkingSet(0, 1));
    std::uint64_t ringToken;
    ASSERT_EQ(VN_OK, streams.ActivateSlot(0, facts.fileIndex, 0, 0, facts.frameCount, 1, &ringToken));
    EXPECT_EQ(64u, streams.RingOf(0)->BufferedFrames());
    EXPECT_TRUE(streams.RetirePreparedSlot(streams.RingOf(0), ringToken));
    streams.SyncServe();
    EXPECT_EQ(VN_OK, streams.UnregisterFile(id));
}

TEST(StreamRegister, SourceReadAndReleaseDoNotBlockRegisteredSlotActivation) {
    using Manager = harken::StreamManager;
    class ReleaseSource final : public ControlledSource {
    public:
        std::shared_ptr<ReadGate> releaseGate;
        ~ReleaseSource() override {
            if (!releaseGate) return;
            releaseGate->entered.store(true, std::memory_order_release);
            while (!releaseGate->release.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    for (const bool blockRelease : {false, true}) {
        SCOPED_TRACE(blockRelease);
        harken::EngineConfig config{};
        config.maxStreams = 2;
        config.streamRingBytes = (64) * sizeof(float);
        Manager streams(config);
        auto healthy = std::make_shared<ControlledSource>();
        StreamSpec spec;
        spec.soundId = harkentest::ObjectId(17336);
        spec.chunkFrames = 16;
        healthy->maxRead = 16 * sizeof(float);
        const auto samples = ServiceSamples(4096, 0);
        healthy->bytes = BuildStreamBytes(spec, samples);
        Manager::SourceToken ready;
        ASSERT_EQ(VN_OK, streams.BeginSource(spec.soundId, healthy, false, 0, &ready));
        ASSERT_TRUE(WaitUntil([&] { return streams.QuerySource(ready) == Manager::PreparationState::Ready; }));
        Manager::StreamFacts facts;
        ASSERT_EQ(VN_OK, streams.TakeSource(ready, &facts));
        std::uint64_t tokens[2]{};
        ASSERT_TRUE(streams.PrepareSlotWorkingSet(0, 1));
        ASSERT_EQ(VN_OK, streams.ActivateSlot(0, facts.fileIndex, 0, 0, 4096, 1, &tokens[0]));

        auto blocked = std::make_shared<ReleaseSource>();
        const auto blockedId = harkentest::ObjectId(17337);
        blocked->bytes = MakeValidImage(blockedId);
        auto gate = blockRelease ? std::make_shared<ReadGate>() : blocked->gate;
        auto destroyed = blocked->destroyed;
        ReleaseReadGate release{*gate};
        if (blockRelease) blocked->releaseGate = gate;
        else gate->offset.store(0, std::memory_order_release);
        Manager::SourceToken pending;
        ASSERT_EQ(VN_OK, streams.BeginSource(blockedId, blocked, false, 0, &pending));
        if (blockRelease) {
            ASSERT_TRUE(WaitUntil([&] { return streams.QuerySource(pending) == Manager::PreparationState::Ready; }));
            ASSERT_EQ(VN_OK, streams.TakeSource(pending));
            blocked.reset();
            ASSERT_EQ(VN_OK, streams.UnregisterFile(blockedId));
        } else blocked.reset();
        ASSERT_TRUE(WaitUntil([&] { return gate->entered.load(std::memory_order_acquire); }));
        EXPECT_EQ(0u, destroyed->load());

        ASSERT_TRUE(streams.PrepareSlotWorkingSet(1, 1));
        std::atomic<bool> activated{false};
        VnResult result = VN_DEVICE_ERROR;
        std::thread caller([&] {
            result = streams.ActivateSlot(1, facts.fileIndex, 1024, 0, 4096, 1, &tokens[1]);
            activated.store(true, std::memory_order_release);
        });
        harkentest::ConsumePcmFrames(*streams.RingOf(0), 64);
        const bool refill = WaitUntil([&] { return streams.RingOf(0)->BufferedFrames() == 64; });
        const bool independent = WaitUntil([&] { return activated.load(std::memory_order_acquire); });
        // Release the deliberately blocked callback before joining on failure.
        if (!independent) gate->release.store(true, std::memory_order_release);
        caller.join();
        EXPECT_TRUE(refill);
        EXPECT_TRUE(independent);
        ASSERT_EQ(VN_OK, result);
        std::size_t position = 1024;
        ExpectAndConsume(*streams.RingOf(1), samples, position, 64);
        if (!blockRelease) EXPECT_TRUE(streams.CancelSource(pending));
        gate->release.store(true, std::memory_order_release);
        EXPECT_TRUE(WaitUntil([&] { return destroyed->load() == 1; }));
        for (unsigned i = 0; i < 2; ++i)
            EXPECT_TRUE(streams.RetirePreparedSlot(streams.RingOf(i), tokens[i]));
        streams.SyncServe();
        EXPECT_EQ(VN_OK, streams.UnregisterFile(spec.soundId));
    }
}

TEST(StreamRegister, SlowSameSourceActiveReadDoesNotBlockHealthyInstanceOrReleaseEarly) {
    using Manager = harken::StreamManager;
    for (const bool failWorker : {false, true}) {
    SCOPED_TRACE(failWorker);
    harken::EngineConfig config{};
    config.maxStreams = 3;
    config.streamRingBytes = (64) * sizeof(float);
    Manager streams(config);
    auto source = std::make_shared<ControlledSource>();
    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(17303);
    spec.chunkFrames = 16;
    source->maxRead = 16 * sizeof(float);
    const auto samples = ServiceSamples(4096, 0);
    source->bytes = BuildStreamBytes(spec, samples);
    const auto toc = harken::ParseStreamTocEntry(source->bytes.data() + harken::kStreamHeaderBytes);
    Manager::SourceToken prepared;
    ASSERT_EQ(VN_OK, streams.BeginSource(spec.soundId, source, false, 0, &prepared));
    ASSERT_TRUE(WaitUntil([&] { return streams.QuerySource(prepared) == Manager::PreparationState::Ready; }));
    Manager::StreamFacts facts;
    ASSERT_EQ(VN_OK, streams.TakeSource(prepared, &facts));
    std::uint64_t tokens[3]{};
    for (unsigned i = 0; i < 2; ++i) {
        ASSERT_TRUE(streams.PrepareSlotWorkingSet(i, 1));
        ASSERT_EQ(VN_OK, streams.ActivateSlot(i, facts.fileIndex, i * 1024, 0, 4096, 1, &tokens[i]));
    }
    source->gate->offset.store(toc.payloadOffset + 1088 * sizeof(float));
    ReleaseReadGate release{*source->gate};
    harkentest::ConsumePcmFrames(*streams.RingOf(1), 64);
    ASSERT_TRUE(WaitUntil([&] { return source->gate->entered.load(); }));
    ASSERT_TRUE(streams.PrepareSlotWorkingSet(2, 1));
    std::atomic<bool> activated{false};
    VnResult activation = VN_DEVICE_ERROR;
    std::thread activate([&] {
        activation = streams.ActivateSlot(2, facts.fileIndex, 2048, 0, 4096, 1, &tokens[2]);
        activated.store(true, std::memory_order_release);
    });
    const bool independent = WaitUntil([&] { return activated.load(std::memory_order_acquire); });
    if (!independent) source->gate->release.store(true);
    activate.join();
    ASSERT_TRUE(independent);
    ASSERT_EQ(VN_OK, activation);
    std::size_t newPosition = 2048;
    ExpectAndConsume(*streams.RingOf(2), samples, newPosition, 64);
    std::size_t consumed = 0;
    for (unsigned pass = 0; pass < 4; ++pass) {
        ASSERT_TRUE(WaitUntil([&] { return streams.RingOf(0)->BufferedFrames() == 64; }));
        ExpectAndConsume(*streams.RingOf(0), samples, consumed, 64);
    }
    if (failWorker) {
        harken::SetStreamRequestFaultForTests(harken::StreamRequestFault::WorkerLoop);
        const bool failed = WaitUntil([&] { return streams.WorkerState() == harken::StreamWorkerState::Failed; });
        harken::SetStreamRequestFaultForTests(harken::StreamRequestFault::None);
        EXPECT_TRUE(failed);
    }
    for (unsigned i = 0; i < 3; ++i)
        EXPECT_TRUE(streams.RetirePreparedSlot(streams.RingOf(i), tokens[i]));
    streams.SyncServe();
    streams.ReclaimAfterWorkerFailure();
    EXPECT_LT(streams.FreeSlotCount(), 3u);
    EXPECT_EQ(failWorker ? VN_DEVICE_ERROR : VN_IN_USE, streams.UnregisterFile(spec.soundId));
    source->gate->release.store(true);
    ASSERT_TRUE(WaitUntil([&] { streams.ReclaimAfterWorkerFailure(); return streams.FreeSlotCount() == 3; }));
    if (!failWorker) EXPECT_EQ(VN_OK, streams.UnregisterFile(spec.soundId));
    }
}

TEST(StreamRegister, PrivateReadWindowsReduceReadsAndDiscardReboundSourceBytes) {
    using Manager = harken::StreamManager;
    for (const auto ringFrames : {256u, 8192u}) {
    SCOPED_TRACE(ringFrames);
    const auto totalFrames = std::max(8192u, ringFrames * 8);
    harken::EngineConfig config{};
    config.maxStreams = 2;
    config.streamRingBytes = (ringFrames) * sizeof(float);
    Manager streams(config);
    EXPECT_EQ(0u, streams.ReadWindowBytes());
    std::array<std::shared_ptr<ControlledSource>, 2> sources;
    std::array<std::vector<float>, 2> samples;
    Manager::StreamFacts facts[2]{};
    std::uint64_t tokens[2]{};
    for (unsigned i = 0; i < 2; ++i) {
        sources[i] = std::make_shared<ControlledSource>();
        StreamSpec spec;
        spec.soundId = harkentest::ObjectId(17320 + i);
        spec.chunkFrames = 64;
        samples[i] = ServiceSamples(totalFrames, i * 16384);
        sources[i]->bytes = BuildStreamBytes(spec, samples[i]);
        Manager::SourceToken sourceToken;
        ASSERT_EQ(VN_OK, streams.BeginSource(spec.soundId, sources[i], false, 0, &sourceToken));
        ASSERT_TRUE(WaitUntil([&] { return streams.QuerySource(sourceToken) == Manager::PreparationState::Ready; }));
        ASSERT_EQ(VN_OK, streams.TakeSource(sourceToken, &facts[i]));
        sources[i]->reads.store(0);
        ASSERT_TRUE(streams.PrepareSlotWorkingSet(i, 1));
        ASSERT_EQ(VN_OK, streams.ActivateSlot(i, facts[i].fileIndex, 0, 0, facts[i].frameCount, 1, &tokens[i]));
    }
    std::size_t positions[2]{};
    for (unsigned round = 0; round < 4; ++round) {
        for (unsigned i = 0; i < 2; ++i) ExpectAndConsume(*streams.RingOf(i), samples[i], positions[i], ringFrames);
        streams.SyncServe();
    }
    for (unsigned i = 0; i < 2; ++i) EXPECT_LT(sources[i]->reads.load(), positions[i] / 64);
    EXPECT_GT(streams.ReadWindowBytes(), 0u);
    ASSERT_TRUE(streams.RetirePreparedSlot(streams.RingOf(0), tokens[0]));
    streams.SyncServe();
    auto replacement = std::make_shared<ControlledSource>();
    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(17320);
    spec.chunkFrames = 64;
    auto changed = ServiceSamples(totalFrames, 32768);
    replacement->bytes = BuildStreamBytes(spec, changed);
    Manager::SourceToken sourceToken;
    ASSERT_EQ(VN_OK, streams.BeginSource(spec.soundId, replacement, true, facts[0].sourceGeneration, &sourceToken));
    ASSERT_TRUE(WaitUntil([&] { return streams.QuerySource(sourceToken) == Manager::PreparationState::Ready; }));
    ASSERT_EQ(VN_OK, streams.TakeSource(sourceToken, &facts[0]));
    ASSERT_EQ(VN_OK, streams.ActivateSlot(0, facts[0].fileIndex, 0, 0, facts[0].frameCount, 1, &tokens[0]));
    std::size_t changedPosition = 0;
    ExpectAndConsume(*streams.RingOf(0), changed, changedPosition, ringFrames);
    for (unsigned i = 0; i < 2; ++i) EXPECT_TRUE(streams.RetirePreparedSlot(streams.RingOf(i), tokens[i]));
    streams.SyncServe();
    EXPECT_TRUE(WaitUntil([&] { return streams.ReadWindowBytes() == 0; }));
    }
}

TEST(StreamRegister, OptionalReadAheadNeverRequiresFutureBytesOrRetriesCancellation) {
    enum class Behavior { Short, Reject, PartialError, Zero, Cancel, RequiredFailure };
    class OptionalSource final : public ControlledSource {
    public:
        Behavior behavior = Behavior::Short;
        std::uint64_t payload = 0;
        mutable std::atomic<unsigned> payloadCalls{0};
        harken::StreamReadResult ReadAt(std::uint64_t offset, void* destination, std::size_t count,
                                       harken::StreamReadCancellation cancellation) const override {
            if (offset >= payload) {
                payloadCalls.fetch_add(1);
                if (behavior == Behavior::Cancel) return {harken::StreamReadStatus::Cancelled, 0};
                if (count > 64 * sizeof(float) || behavior == Behavior::RequiredFailure) {
                    if (behavior == Behavior::Reject || behavior == Behavior::RequiredFailure) return {};
                    if (behavior == Behavior::PartialError) {
                        std::memset(destination, 0xff, 64 * sizeof(float));
                        return {harken::StreamReadStatus::IoError, 64 * sizeof(float)};
                    }
                    if (behavior == Behavior::Zero) return {harken::StreamReadStatus::Ok, 0};
                }
                if (behavior == Behavior::Short && offset != payload) return {};
                count = std::min(count, 64 * sizeof(float));
            }
            return ControlledSource::ReadAt(offset, destination, count, cancellation);
        }
    };
    for (auto behavior : {Behavior::Short, Behavior::Reject, Behavior::PartialError,
                          Behavior::Zero, Behavior::Cancel, Behavior::RequiredFailure}) {
        SCOPED_TRACE(static_cast<unsigned>(behavior));
        harken::EngineConfig config{};
        config.maxStreams = 1;
        config.streamRingBytes = (64) * sizeof(float);
        harken::StreamManager streams(config);
        auto source = std::make_shared<OptionalSource>();
        StreamSpec spec;
        spec.soundId = harkentest::ObjectId(17322);
        spec.chunkFrames = 64;
        auto samples = ServiceSamples(4096, 0);
        source->bytes = BuildStreamBytes(spec, samples);
        source->payload = harken::ParseStreamTocEntry(source->bytes.data() + harken::kStreamHeaderBytes).payloadOffset;
        source->behavior = behavior;
        harken::StreamManager::SourceToken sourceToken;
        ASSERT_EQ(VN_OK, streams.BeginSource(spec.soundId, source, false, 0, &sourceToken));
        ASSERT_TRUE(WaitUntil([&] { return streams.QuerySource(sourceToken) == harken::StreamManager::PreparationState::Ready; }));
        harken::StreamManager::StreamFacts facts;
        ASSERT_EQ(VN_OK, streams.TakeSource(sourceToken, &facts));
        ASSERT_TRUE(streams.PrepareSlotWorkingSet(0, 1));
        std::uint64_t token;
        ASSERT_EQ(VN_OK, streams.ActivateSlot(0, facts.fileIndex, 0, 0, facts.frameCount, 1, &token));
        EXPECT_EQ(behavior == Behavior::Short || behavior == Behavior::Cancel ? 1u : 2u,
                  source->payloadCalls.load());
        if (behavior == Behavior::Cancel || behavior == Behavior::RequiredFailure) {
            EXPECT_EQ(0u, streams.RingOf(0)->BufferedFrames());
            EXPECT_EQ(harken::StreamTerminalReason::IntegrityAbort, streams.RingOf(0)->TerminalReason());
        } else {
            std::size_t position = 0;
            ExpectAndConsume(*streams.RingOf(0), samples, position, 64);
        }
        EXPECT_TRUE(streams.RetirePreparedSlot(streams.RingOf(0), token));
        streams.SyncServe();
    }
}

TEST(StreamRegister, CancelledSourceValidationRetainsLeaseUntilReadReturns) {
    using Manager = harken::StreamManager;
    harken::EngineConfig config{};
    config.maxStreams = 1;
    config.streamRingBytes = (64) * sizeof(float);
    Manager streams(config);
    const auto healthyId = harkentest::ObjectId(17305);
    const auto healthyPath = TempPath("stream_source_metadata_neighbor.stream");
    WriteFileBytes(healthyPath, MakeValidImage(healthyId));
    ASSERT_EQ(VN_OK, streams.RegisterFile(healthyId, healthyPath.c_str()));
    Manager::StreamFacts healthy;
    ASSERT_TRUE(streams.FindFile(healthyId, &healthy));
    ASSERT_TRUE(streams.PrepareSlotWorkingSet(0, 1));
    std::uint64_t ringToken;
    ASSERT_EQ(VN_OK, streams.ActivateSlot(0, healthy.fileIndex, 0, 0, healthy.frameCount, 1, &ringToken));
    auto source = std::make_shared<ControlledSource>();
    const auto id = harkentest::ObjectId(17304);
    source->bytes = MakeValidImage(id);
    auto gate = source->gate;
    auto destroyed = source->destroyed;
    gate->offset.store(0);
    ReleaseReadGate release{*gate};
    Manager::SourceToken token;
    ASSERT_EQ(VN_OK, streams.BeginSource(id, source, false, 0, &token));
    source.reset();
    ASSERT_TRUE(WaitUntil([&] { return gate->entered.load(); }));
    harkentest::ConsumePcmFrames(*streams.RingOf(0), 64);
    EXPECT_TRUE(WaitUntil([&] { return streams.RingOf(0)->BufferedFrames() == 64; }));
    EXPECT_TRUE(streams.CancelSource(token));
    EXPECT_FALSE(streams.CancelSource(token));
    EXPECT_EQ(Manager::PreparationState::Cancelled, streams.QuerySource(token));
    EXPECT_EQ(VN_STALE_HANDLE, streams.TakeSource(token));
    EXPECT_EQ(0u, destroyed->load());
    gate->release.store(true);
    ASSERT_TRUE(WaitUntil([&] { return destroyed->load() == 1; }));
    EXPECT_EQ(Manager::PreparationState::Invalid, streams.QuerySource(token));
    EXPECT_FALSE(streams.HasPendingSource(id));
    EXPECT_TRUE(streams.RetirePreparedSlot(streams.RingOf(0), ringToken));
    streams.SyncServe();
    EXPECT_EQ(VN_OK, streams.UnregisterFile(healthyId));
}

TEST(StreamRegister, ReadAtUses64BitOffsetsAndRejectsPrematureZero) {
    struct SparseSource : harken::StreamByteSource {
        std::uint64_t ByteCount() const noexcept override { return (1ull << 33) + 16; }
        harken::StreamReadResult ReadAt(std::uint64_t offset, void* destination, std::size_t count,
                                       harken::StreamReadCancellation) const override {
            if (offset < (1ull << 33)) return {harken::StreamReadStatus::Ok, 0};
            count = std::min<std::size_t>(count, 3);
            std::memset(destination, 0x5a, count);
            return {harken::StreamReadStatus::Ok, count};
        }
    } source;
    std::array<std::uint8_t, 16> output{};
    EXPECT_EQ(harken::StreamReadStatus::Ok, harken::ReadStreamBytes(source, 1ull << 33,
        output.data(), output.size(), {}));
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](auto value) { return value == 0x5a; }));
    EXPECT_EQ(harken::StreamReadStatus::IoError, harken::ReadStreamBytes(source, 0,
        output.data(), output.size(), {}));
    EXPECT_EQ(harken::StreamReadStatus::IoError, harken::ReadStreamBytes(source, UINT64_MAX,
        output.data(), output.size(), {}));
}

TEST(StreamRegister, SourceFailureAndTermBalanceOwnershipAndNeverReuseRequestIdentity) {
    using Manager = harken::StreamManager;
    harken::EngineConfig config{};
    config.maxStreams = 1;
    config.streamRingBytes = (64) * sizeof(float);
    const auto id = harkentest::ObjectId(17306);
    Manager::SourceToken cancelled;
    auto source = std::make_shared<ControlledSource>();
    source->bytes = MakeValidImage(id);
    source->maxRead = 0;
    auto destroyed = source->destroyed;
    {
        Manager streams(config);
        Manager::SourceToken failed;
        ASSERT_EQ(VN_OK, streams.BeginSource(id, source, false, 0, &failed));
        source.reset();
        ASSERT_TRUE(WaitUntil([&] { return streams.QuerySource(failed) == Manager::PreparationState::Failed; }));
        VnResult error = VN_OK;
        EXPECT_EQ(Manager::PreparationState::Failed, streams.QuerySource(failed, &error));
        EXPECT_EQ(VN_BAD_STREAM_FILE, error);
        EXPECT_EQ(0u, destroyed->load());
        EXPECT_TRUE(streams.CancelSource(failed));
        ASSERT_TRUE(WaitUntil([&] { return destroyed->load() == 1; }));
        source = std::make_shared<ControlledSource>();
        source->bytes = MakeValidImage(id);
        source->cooperate = true;
        source->gate->offset.store(0);
        destroyed = source->destroyed;
        auto gate = source->gate;
        ASSERT_EQ(VN_OK, streams.BeginSource(id, source, false, 0, &cancelled));
        source.reset();
        ASSERT_TRUE(WaitUntil([&] { return gate->entered.load(); }));
        // Term must request cancellation and join before releasing the source.
    }
    EXPECT_EQ(1u, destroyed->load());
    Manager streams(config);
    source = std::make_shared<ControlledSource>();
    source->bytes = MakeValidImage(id);
    Manager::SourceToken current;
    ASSERT_EQ(VN_OK, streams.BeginSource(id, source, false, 0, &current));
    EXPECT_NE(cancelled.value, current.value);
    EXPECT_EQ(Manager::PreparationState::Invalid, streams.QuerySource(cancelled));
    EXPECT_FALSE(streams.CancelSource(cancelled));
    EXPECT_TRUE(streams.CancelSource(current));
}

TEST(StreamRegister, FileSourceRetainsOpenedIdentityAndReadsSparse64BitRange) {
    const auto path = TempPath("stream_source_sparse.bin");
    const auto retained = TempPath("stream_source_sparse_retained.bin");
    constexpr std::uint64_t offset = (1ull << 32) + 17;
    {
#ifdef _WIN32
        const auto wide = std::filesystem::path(path).wstring();
        const auto handle = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(INVALID_HANDLE_VALUE, handle);
        DWORD returned = 0;
        const auto sparse = DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0,
            nullptr, 0, &returned, nullptr);
        CloseHandle(handle);
        ASSERT_TRUE(sparse);
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
#else
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
#endif
        file.seekp(static_cast<std::streamoff>(offset));
        file.put('A');
        ASSERT_TRUE(file.good());
    }
    auto opened = harken::OpenStreamByteSource(path.c_str());
    ASSERT_TRUE(opened);
    EXPECT_EQ(offset + 1, opened->ByteCount());
    std::error_code error;
    std::filesystem::remove(retained, error);
    std::filesystem::rename(path, retained);
    { std::ofstream file(path, std::ios::binary | std::ios::trunc); file.put('B'); }
    char sample = 0;
    EXPECT_EQ(harken::StreamReadStatus::Ok,
        harken::ReadStreamBytes(*opened, offset, &sample, 1, {}));
    EXPECT_EQ('A', sample);
    auto fresh = harken::OpenStreamByteSource(path.c_str());
    ASSERT_TRUE(fresh);
    EXPECT_EQ(harken::StreamReadStatus::Ok,
        harken::ReadStreamBytes(*fresh, 0, &sample, 1, {}));
    EXPECT_EQ('B', sample);
    fresh.reset();
    opened.reset();
    std::filesystem::remove(path);
    std::filesystem::remove(retained);
}

TEST(StreamRegister, ReplacementWaitsForPrebufferAndTransfersOwnershipOnce) {
    using Manager = harken::StreamManager;
    using State = Manager::PreparationState;
    using Access = harken::detail::StreamManagerTestAccess;
    harken::EngineConfig config{};
    config.maxStreams = 2;
    config.streamRingBytes = (16) * sizeof(float);
    Manager streams(config);
    streams.SetThrottled(true);
    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(17200);
    spec.chunkFrames = 8;
    const auto samples = ServiceSamples(64, 0);
    const auto path = TempPath("stream_replacement_ready.stream");
    WriteFileBytes(path, BuildStreamBytes(spec, samples));
    ASSERT_EQ(VN_OK, streams.RegisterFile(spec.soundId, path.c_str()));
    Manager::StreamFacts facts{};
    ASSERT_TRUE(streams.FindFile(spec.soundId, &facts));
    auto request = ReplacementAt(facts, 3);
    request.prebufferTargetFrames = 12;
    request.pendingSlotReservations = 2;
    Manager::PreparedToken token{};
    EXPECT_EQ(VN_BACKPRESSURE, streams.BeginReplacement(request, &token));
    EXPECT_EQ(2u, streams.FreeSlotCount());
    request.pendingSlotReservations = 1;
    ASSERT_EQ(VN_OK, streams.BeginReplacement(request, &token));
    EXPECT_EQ(State::Preparing, streams.QueryReplacement(token));
    Manager::PreparedToken refused{};
    EXPECT_EQ(VN_BACKPRESSURE, streams.BeginReplacement(request, &refused));
    Manager::PreparedStream taken{};
    EXPECT_EQ(VN_BACKPRESSURE, streams.TakeReplacement(token, &taken));
    EXPECT_EQ(nullptr, taken.ring);
    streams.SetMaxServeFramesForTest(4);
    streams.SyncServe();
    Access::ServeSlots(streams);
    EXPECT_EQ(State::Preparing, streams.QueryReplacement(token));
    Access::ServeSlots(streams);
    EXPECT_EQ(State::Preparing, streams.QueryReplacement(token));
    Access::ServeSlots(streams);
    ASSERT_EQ(State::Ready, streams.QueryReplacement(token));
    EXPECT_EQ(State::Ready, streams.QueryReplacement(token));
    ASSERT_EQ(VN_OK, streams.TakeReplacement(token, &taken));
    EXPECT_EQ(State::Invalid, streams.QueryReplacement(token));
    EXPECT_FALSE(streams.CancelReplacement(token));
    Manager::PreparedStream duplicate{};
    EXPECT_EQ(VN_STALE_HANDLE, streams.TakeReplacement(token, &duplicate));
    std::size_t position = 3;
    ExpectAndConsume(*taken.ring, samples, position, 12);
    EXPECT_EQ(VN_IN_USE, streams.UnregisterFile(spec.soundId));
    ASSERT_TRUE(streams.RetirePreparedSlot(taken.ring, taken.ringToken));
    streams.SyncServe();

    request = ReplacementAt(facts);
    ASSERT_EQ(VN_OK, streams.BeginReplacement(request, &token));
    EXPECT_EQ(State::Preparing, streams.QueryReplacement(token))
        << "zero requested prebuffer does not make an empty source ready";
    Access::ServeSlots(streams);
    EXPECT_EQ(State::Ready, streams.QueryReplacement(token));
    EXPECT_TRUE(streams.CancelReplacement(token));
    EXPECT_EQ(State::Cancelled, streams.QueryReplacement(token));
    EXPECT_FALSE(streams.CancelReplacement(token));
    streams.SyncServe();
    EXPECT_EQ(VN_OK, streams.UnregisterFile(spec.soundId));
}

TEST(StreamRegister, PrivateTransferKeepsMediaPendingAndRejectsOldOwnershipAfterReuse) {
    using Manager = harken::StreamManager;
    using State = Manager::PreparationState;
    using Access = harken::detail::StreamManagerTestAccess;
    harken::EngineConfig config{};
    config.maxStreams = 1;
    config.streamRingBytes = (16) * sizeof(float);
    Manager streams(config);
    streams.SetThrottled(true);
    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(17208);
    spec.chunkFrames = 8;
    const auto samples = ServiceSamples(64, 0);
    const auto path = TempPath("stream_private_transfer.stream");
    WriteFileBytes(path, BuildStreamBytes(spec, samples));
    ASSERT_EQ(VN_OK, streams.RegisterFile(spec.soundId, path.c_str()));
    Manager::StreamFacts facts{};
    ASSERT_TRUE(streams.FindFile(spec.soundId, &facts));
    auto request = ReplacementAt(facts, 3);
    request.prebufferTargetFrames = 12;
    Manager::PreparedToken token{};
    ASSERT_EQ(VN_OK, streams.BeginReplacement(request, &token));
    Manager::PreparedStream owned{};
    EXPECT_EQ(VN_BACKPRESSURE, streams.TakeReplacement(token, &owned));
    ASSERT_EQ(VN_OK, streams.TransferReplacement(token, &owned));
    EXPECT_EQ(State::Invalid, streams.QueryReplacement(token));
    EXPECT_FALSE(streams.CancelReplacement(token));
    Manager::PreparedStream duplicate{};
    EXPECT_EQ(VN_STALE_HANDLE, streams.TransferReplacement(token, &duplicate));
    EXPECT_EQ(State::Preparing, streams.PromoteTransferredStream(owned));
    EXPECT_EQ(VN_IN_USE, streams.UnregisterFile(spec.soundId));
    streams.SetMaxServeFramesForTest(4);
    streams.SyncServe();
    Access::ServeSlots(streams);
    EXPECT_EQ(State::Preparing, streams.PromoteTransferredStream(owned));
    Access::ServeSlots(streams);
    EXPECT_EQ(State::Preparing, streams.PromoteTransferredStream(owned));
    Access::ServeSlots(streams);
    ASSERT_EQ(State::Ready, streams.PromoteTransferredStream(owned));
    EXPECT_EQ(State::Ready, streams.PromoteTransferredStream(owned));
    std::size_t position = 3;
    ExpectAndConsume(*owned.ring, samples, position, 12);
    ASSERT_TRUE(streams.RetirePreparedSlot(owned.ring, owned.ringToken));
    streams.SyncServe();
    Manager::PreparedToken next{};
    ASSERT_EQ(VN_OK, streams.BeginReplacement(request, &next));
    Manager::PreparedStream replacement{};
    ASSERT_EQ(VN_OK, streams.TransferReplacement(next, &replacement));
    EXPECT_EQ(owned.slot, replacement.slot);
    EXPECT_NE(owned.ringToken, replacement.ringToken);
    EXPECT_EQ(State::Invalid, streams.PromoteTransferredStream(owned));
    EXPECT_FALSE(streams.RetirePreparedSlot(owned.ring, owned.ringToken));
    EXPECT_EQ(State::Preparing, streams.PromoteTransferredStream(replacement));
    ASSERT_TRUE(streams.RetirePreparedSlot(replacement.ring, replacement.ringToken));
    streams.SyncServe();
    EXPECT_EQ(VN_OK, streams.UnregisterFile(spec.soundId));
}

TEST(StreamRegister, ReplacementPreservesSourcePositionLoopCountAndOutro) {
    using Manager = harken::StreamManager;
    using Access = harken::detail::StreamManagerTestAccess;
    harken::EngineConfig config{};
    config.maxStreams = 1;
    config.streamRingBytes = (64) * sizeof(float);
    Manager streams(config);
    streams.SetThrottled(true);
    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(17201);
    spec.chunkFrames = 4;
    const auto samples = ServiceSamples(11, 0);
    const auto path = TempPath("stream_replacement_loops.stream");
    WriteFileBytes(path, BuildStreamBytes(spec, samples));
    ASSERT_EQ(VN_OK, streams.RegisterFile(spec.soundId, path.c_str()));
    Manager::StreamFacts facts{};
    ASSERT_TRUE(streams.FindFile(spec.soundId, &facts));
    for (const auto count : {1u, 3u, 0u}) {
        SCOPED_TRACE(count);
        auto request = ReplacementAt(facts, 3);
        request.loopStartFrame = 2;
        request.loopEndFrame = 7;
        request.loopCount = count;
        request.prebufferTargetFrames = 1000;
        Manager::PreparedToken token{};
        ASSERT_EQ(VN_OK, streams.BeginReplacement(request, &token));
        streams.SyncServe();
        Access::ServeSlots(streams);
        ASSERT_EQ(Manager::PreparationState::Ready, streams.QueryReplacement(token));
        Manager::PreparedStream taken{};
        ASSERT_EQ(VN_OK, streams.TakeReplacement(token, &taken));
        std::vector<float> expected;
        std::uint64_t frame = 3;
        auto remaining = count;
        while (expected.size() < taken.ring->FrameCapacity()) {
            const bool loop = remaining == 0 || remaining > 1;
            if (frame == (loop ? 7u : samples.size())) {
                if (!loop) break;
                frame = 2;
                if (remaining != 0) --remaining;
            }
            expected.push_back(samples[frame++]);
        }
        EXPECT_EQ(count == 0 ? harken::StreamTerminalReason::None
                             : harken::StreamTerminalReason::NaturalEnd,
                  taken.ring->TerminalReason());
        std::size_t consumed = 0;
        ExpectAndConsume(*taken.ring, expected, consumed,
                         static_cast<std::uint32_t>(expected.size()));
        ASSERT_TRUE(streams.RetirePreparedSlot(taken.ring, taken.ringToken));
        streams.SyncServe();
    }
    auto request = ReplacementAt(facts, 9);
    request.loopStartFrame = 2;
    request.loopEndFrame = 7;
    request.loopCount = 2;
    Manager::PreparedToken token{};
    EXPECT_EQ(VN_INVALID_ARG, streams.BeginReplacement(request, &token));
    request.loopCount = 0;
    request.targetFrame = facts.frameCount;
    EXPECT_EQ(VN_INVALID_ARG, streams.BeginReplacement(request, &token));
    request.loopCount = 1;
    request.targetFrame = facts.frameCount + 1;
    EXPECT_EQ(VN_INVALID_ARG, streams.BeginReplacement(request, &token));
    request.targetFrame = 9;
    ASSERT_EQ(VN_OK, streams.BeginReplacement(request, &token));
    streams.SyncServe();
    Access::ServeSlots(streams);
    Manager::PreparedStream taken{};
    ASSERT_EQ(VN_OK, streams.TakeReplacement(token, &taken));
    std::size_t position = 9;
    ExpectAndConsume(*taken.ring, samples, position, 2);
    ASSERT_TRUE(streams.RetirePreparedSlot(taken.ring, taken.ringToken));
    streams.SyncServe();
    request.targetFrame = facts.frameCount;
    ASSERT_EQ(VN_OK, streams.BeginReplacement(request, &token));
    streams.SyncServe();
    Access::ServeSlots(streams);
    EXPECT_EQ(Manager::PreparationState::Ready, streams.QueryReplacement(token));
    EXPECT_TRUE(streams.CancelReplacement(token));
    streams.SyncServe();
    EXPECT_EQ(VN_OK, streams.UnregisterFile(spec.soundId));
}

TEST(StreamRegister, ReplacementPinsRetiringSourceAndRejectsSourceAndRingReuse) {
    using Manager = harken::StreamManager;
    harken::EngineConfig config{};
    config.maxStreams = 2;
    config.streamRingBytes = (64) * sizeof(float);
    Manager streams(config);
    streams.SetThrottled(true);
    const auto id = harkentest::ObjectId(17202);
    const auto path = TempPath("stream_replacement_generations.stream");
    WriteFileBytes(path, MakeValidImage(id));
    ASSERT_EQ(VN_OK, streams.RegisterFile(id, path.c_str()));
    Manager::StreamFacts original{}, current{};
    ASSERT_TRUE(streams.FindFile(id, &original));
    auto request = ReplacementAt(original);
    Manager::PreparedToken first{}, retiring{}, reused{};
    ASSERT_EQ(VN_OK, streams.BeginReplacement(request, &first));
    ASSERT_EQ(VN_OK, streams.ReplaceFile(id, path.c_str()));
    ASSERT_TRUE(streams.FindFile(id, &current));
    EXPECT_EQ(original.contentRevision, current.contentRevision);
    EXPECT_NE(original.sourceGeneration, current.sourceGeneration);
    ASSERT_EQ(VN_OK, streams.BeginReplacement(request, &retiring));
    EXPECT_EQ(1u, streams.RetiringFileCount());
    ASSERT_TRUE(streams.CancelReplacement(first));
    streams.SyncServe();
    EXPECT_EQ(1u, streams.RetiringFileCount());
    ASSERT_TRUE(streams.CancelReplacement(retiring));
    streams.SyncServe();
    EXPECT_EQ(0u, streams.RetiringFileCount());
    ASSERT_EQ(VN_OK, streams.UnregisterFile(id));
    ASSERT_EQ(VN_OK, streams.RegisterFile(id, path.c_str()));
    ASSERT_TRUE(streams.FindFile(id, &current));
    ASSERT_EQ(original.fileIndex, current.fileIndex);
    ASSERT_EQ(original.contentRevision, current.contentRevision);
    EXPECT_EQ(VN_STALE_HANDLE, streams.BeginReplacement(request, &reused));
    request = ReplacementAt(current);
    // Even a wrapped ring generation cannot revive a preparation owner.
    streams.RingOf(first.slot)->generation.store(UINT32_MAX, std::memory_order_relaxed);
    ASSERT_EQ(VN_OK, streams.BeginReplacement(request, &reused));
    EXPECT_EQ(first.slot, reused.slot);
    EXPECT_NE(first.generation, reused.generation);
    EXPECT_EQ(Manager::PreparationState::Invalid, streams.QueryReplacement(first));
    EXPECT_FALSE(streams.CancelReplacement(first));
    EXPECT_EQ(Manager::PreparationState::Preparing, streams.QueryReplacement(reused));
    EXPECT_TRUE(streams.CancelReplacement(reused));
    streams.SyncServe();
    EXPECT_EQ(VN_OK, streams.UnregisterFile(id));
}

TEST(StreamRegister, ReplacementIntegrityFailureWinsOverBufferedFrames) {
    using Manager = harken::StreamManager;
    using Access = harken::detail::StreamManagerTestAccess;
    harken::EngineConfig config{};
    config.maxStreams = 1;
    config.streamRingBytes = (64) * sizeof(float);
    Manager streams(config);
    streams.SetThrottled(true);
    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(17203);
    spec.chunkFrames = 8;
    auto bytes = BuildStreamBytes(spec, ServiceSamples(64, 0));
    const auto toc = harken::ParseStreamTocEntry(bytes.data() + harken::kStreamHeaderBytes);
    bytes[static_cast<std::size_t>(toc.payloadOffset + harken::StreamChunkBytes(8, 1))] ^= 1u;
    const auto path = TempPath("stream_replacement_bad_chunk.stream");
    WriteFileBytes(path, bytes);
    ASSERT_EQ(VN_OK, streams.RegisterFile(spec.soundId, path.c_str()));
    Manager::StreamFacts facts{};
    ASSERT_TRUE(streams.FindFile(spec.soundId, &facts));
    auto request = ReplacementAt(facts);
    request.prebufferTargetFrames = 1;
    Manager::PreparedToken token{};
    ASSERT_EQ(VN_OK, streams.BeginReplacement(request, &token));
    streams.SyncServe();
    Access::ServeSlots(streams);
    EXPECT_EQ(8u, streams.RingOf(token.slot)->BufferedFrames());
    EXPECT_EQ(Manager::PreparationState::Failed, streams.QueryReplacement(token));
    Manager::PreparedStream taken{};
    EXPECT_EQ(VN_DEVICE_ERROR, streams.TakeReplacement(token, &taken));
    EXPECT_TRUE(streams.CancelReplacement(token));
    streams.SyncServe();
    EXPECT_EQ(VN_OK, streams.UnregisterFile(spec.soundId));
}

TEST(StreamRegister, ReplacementPrivateFailureRollsBackSourcePin) {
    using Manager = harken::StreamManager;
    using Access = harken::detail::StreamManagerTestAccess;
    harken::EngineConfig config{};
    config.maxStreams = 1;
    config.streamRingBytes = (1024) * sizeof(float);
    Manager streams(config);
    const auto id = harkentest::ObjectId(17204);
    const auto path = TempPath("stream_replacement_capacity.stream");
    WriteFileBytes(path, MakeValidImage(id));
    ASSERT_EQ(VN_OK, streams.RegisterFile(id, path.c_str()));
    Manager::StreamFacts facts{};
    ASSERT_TRUE(streams.FindFile(id, &facts));
    Access::SetCommitPageLimit(streams, 0);
    Manager::PreparedToken token{};
    EXPECT_EQ(VN_BACKPRESSURE, streams.BeginReplacement(ReplacementAt(facts), &token));
    EXPECT_EQ(Manager::PreparationState::Invalid, streams.QueryReplacement(token));
    EXPECT_EQ(1u, streams.FreeSlotCount());
    Access::ExhaustSourceGenerations(streams);
    EXPECT_EQ(VN_BACKPRESSURE, streams.ReplaceFile(id, path.c_str()));
    Manager::StreamFacts unchanged{};
    ASSERT_TRUE(streams.FindFile(id, &unchanged));
    EXPECT_EQ(facts.sourceGeneration, unchanged.sourceGeneration);
    EXPECT_EQ(VN_OK, streams.UnregisterFile(id));
    EXPECT_EQ(VN_BACKPRESSURE, streams.RegisterFile(id, path.c_str()));
    EXPECT_FALSE(streams.FindFile(id, &unchanged));
}

TEST(StreamRegister, ContinuousConsumptionDoesNotStarveAnotherStream) {
    auto config = MakeNullConfig(2, 64, kRate);
    config.maxStreams = 2;
    config.streamRingBytes = (8) * sizeof(float);
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& streams = session.Engine()->streams;
    using Access = harken::detail::StreamManagerTestAccess;
    streams.SetThrottled(true);
    std::uint32_t files[2]{};
    std::uint64_t tokens[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        StreamSpec spec;
        spec.soundId = harkentest::ObjectId(7090 + i);
        spec.sampleRate = kRate;
        spec.channelCount = 1;
        spec.chunkFrames = 1;
        const auto path = TempPath("stream_service_fairness_" + std::to_string(i) + ".stream");
        WriteFileBytes(path, BuildStreamBytes(spec, {0.25f, 0.25f}));
        ASSERT_EQ(VN_OK, streams.RegisterFile(spec.soundId, path.c_str()));
        harken::StreamManager::StreamFacts facts;
        ASSERT_TRUE(streams.FindFile(spec.soundId, &facts));
        files[i] = facts.fileIndex;
        ASSERT_TRUE(streams.PrepareSlotWorkingSet(i, 1));
        ASSERT_EQ(VN_OK, streams.ActivateSlot(i, files[i], 0, 0, 2, 0, &tokens[i]));
    }
    streams.SyncServe();

    std::atomic<std::uint32_t> firstReads{0};
    std::uint32_t firstReadsWhenSecondServed = UINT32_MAX;
    {
        ReadObserver first(Access::Input(streams, files[0]), [&] {
            if (firstReads.fetch_add(1) + 1 < 64) {
                auto* ring = streams.RingOf(0);
                harkentest::ConsumePcmFrames(*ring, ring->BufferedFrames());
            }
        }, nullptr, false, sizeof(float));
        ReadObserver second(Access::Input(streams, files[1]), [&] {
            if (firstReadsWhenSecondServed == UINT32_MAX)
                firstReadsWhenSecondServed = firstReads.load();
        }, nullptr, false, sizeof(float));
        Access::ServeSlots(streams);
    }
    EXPECT_LT(firstReadsWhenSecondServed, 64u);
    for (std::uint32_t i = 0; i < 2; ++i) {
        auto* ring = streams.RingOf(i);
        ASSERT_GT(ring->BufferedFrames(), 0u);
        EXPECT_EQ(0.25f, harkentest::PeekPcmSample(*ring, 0));
        ASSERT_TRUE(streams.RetirePreparedSlot(ring, tokens[i]));
    }
    streams.SyncServe();
    EXPECT_EQ(2u, streams.FreeSlotCount());
    EXPECT_EQ(0u, streams.PendingRequestCountForTest());
    for (std::uint32_t i = 0; i < 2; ++i)
        EXPECT_EQ(VN_OK, streams.UnregisterFile(harkentest::ObjectId(7090 + i)));
}

TEST(StreamRegister, AdjacentStreamServicePreservesSamplesAcrossTailAndLoop) {
    using Access = harken::detail::StreamManagerTestAccess;
    for (bool sharedFile : {false, true}) {
        SCOPED_TRACE(sharedFile);
        auto config = MakeNullConfig(3, 64, kRate);
        config.maxStreams = 3;
        config.streamRingBytes = (128) * sizeof(float);
        config.prebufferMs = 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& streams = session.Engine()->streams;
        streams.SetThrottled(true);
        streams.SyncServe();
        const unsigned assetCount = sharedFile ? 2 : 3;
        const unsigned lengths[] = {sharedFile ? 253u : 237u,
                                    sharedFile ? 229u : 251u, 229u};
        std::array<std::vector<float>, 3> sources;
        std::uint32_t files[3]{};
        std::uint64_t tokens[3]{};
        for (unsigned asset = 0; asset < assetCount; ++asset) {
            StreamSpec spec;
            spec.soundId = harkentest::ObjectId(17090 + asset);
            spec.chunkFrames = asset == 1 ? 32 : 16;
            spec.loopable = sharedFile && asset == 0;
            spec.loopStartFrame = spec.loopable ? 48 : 0;
            spec.loopEndFrame = spec.loopable ? 176 : 0;
            sources[asset] = ServiceSamples(lengths[asset], asset * 256);
            const auto path = TempPath("stream_service_samples_" +
                std::to_string(sharedFile) + "_" + std::to_string(asset) + ".stream");
            WriteFileBytes(path, BuildStreamBytes(spec, sources[asset]));
            ASSERT_EQ(VN_OK, streams.RegisterFile(spec.soundId, path.c_str()));
            harken::StreamManager::StreamFacts facts;
            ASSERT_TRUE(streams.FindFile(spec.soundId, &facts));
            files[asset] = facts.fileIndex;
        }
        std::array<std::vector<float>, 3> expected;
        for (unsigned slot = 0; slot < 3; ++slot) {
            const unsigned asset = sharedFile ? (slot == 2 ? 1 : 0) : slot;
            const unsigned start = sharedFile && slot == 1 ? 32 : 0;
            const bool loop = sharedFile && slot < 2;
            const auto& source = sources[asset];
            if (loop) {
                expected[slot].insert(expected[slot].end(), source.begin() + start,
                                      source.begin() + 176);
                expected[slot].insert(expected[slot].end(), source.begin() + 48,
                                      source.end());
            } else {
                expected[slot].assign(source.begin() + start, source.end());
            }
            ASSERT_TRUE(streams.PrepareSlotWorkingSet(slot, 1));
            ASSERT_EQ(VN_OK, streams.ActivateSlot(slot, files[asset], start,
                loop ? 48 : 0, loop ? 176 : source.size(), loop ? 2 : 1, &tokens[slot]));
        }
        Access::ServeSlots(streams);
        for (unsigned slot = 0; slot < 3; ++slot)
            ASSERT_EQ(128u, streams.RingOf(slot)->BufferedFrames());
        std::size_t positions[3]{};
        for (unsigned round = 0; round < 16; ++round) {
            bool remaining = false;
            for (unsigned slot = 0; slot < 3; ++slot) {
                if (positions[slot] == expected[slot].size()) continue;
                remaining = true;
                auto& ring = *streams.RingOf(slot);
                const auto count = std::min(64u, ring.BufferedFrames());
                ASSERT_GT(count, 0u);
                ExpectAndConsume(ring, expected[slot], positions[slot], count);
                ASSERT_FALSE(::testing::Test::HasFatalFailure());
            }
            if (!remaining) break;
            Access::ServeSlots(streams);
        }
        for (unsigned slot = 0; slot < 3; ++slot) {
            EXPECT_EQ(expected[slot].size(), positions[slot]);
            EXPECT_EQ(0u, streams.RingOf(slot)->BufferedFrames());
            EXPECT_EQ(harken::StreamTerminalReason::NaturalEnd,
                      streams.RingOf(slot)->TerminalReason());
            ASSERT_TRUE(streams.RetirePreparedSlot(streams.RingOf(slot), tokens[slot]));
        }
        streams.SyncServe();
        for (unsigned asset = 0; asset < assetCount; ++asset)
            EXPECT_EQ(VN_OK, streams.UnregisterFile(harkentest::ObjectId(17090 + asset)));
    }
}

TEST(StreamRegister, AdjacentStreamIntegrityFailureDoesNotStopHealthyNeighbor) {
    using Access = harken::detail::StreamManagerTestAccess;
    enum class Fault { ShortRead, Checksum, NonFinite };
    for (unsigned badSlot : {0u, 1u})
    for (const auto fault : {Fault::ShortRead, Fault::Checksum, Fault::NonFinite}) {
        SCOPED_TRACE(badSlot);
        SCOPED_TRACE(static_cast<unsigned>(fault));
        auto config = MakeNullConfig(2, 64, kRate);
        config.maxStreams = 2;
        config.streamRingBytes = (128) * sizeof(float);
        config.prebufferMs = 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& streams = session.Engine()->streams;
        streams.SetThrottled(true);
        streams.SyncServe();
        std::array<std::vector<float>, 2> sources;
        std::uint32_t files[2]{};
        std::uint64_t tokens[2]{};
        for (unsigned slot = 0; slot < 2; ++slot) {
            StreamSpec spec;
            spec.soundId = harkentest::ObjectId(17100 + slot);
            spec.chunkFrames = slot == 0 ? 16 : 32;
            sources[slot] = ServiceSamples(512, slot * 512);
            auto bytes = BuildStreamBytes(spec, sources[slot]);
            if (slot == badSlot && fault != Fault::ShortRead) {
                const auto toc = harken::ParseStreamTocEntry(bytes.data() + harken::kStreamHeaderBytes);
                const auto chunkIndex = 128 / spec.chunkFrames;
                const auto chunkBytes = harken::StreamChunkBytes(spec.chunkFrames, 1);
                auto* chunk = bytes.data() + harken::StreamChunkFileOffset(
                    toc.payloadOffset, chunkIndex, chunkBytes);
                if (fault == Fault::Checksum) {
                    chunk[0] ^= 1u;
                } else {
                    const std::uint32_t nonfinite = 0x7fc12345u;
                    std::memcpy(chunk, &nonfinite, sizeof(nonfinite));
                    harken::StoreLE64(bytes.data() + toc.chunkChecksumOffset +
                        chunkIndex * harken::kStreamChunkChecksumBytes,
                        harken::Fnv1a64(chunk, static_cast<std::size_t>(chunkBytes)));
                    FixTocChecksum(&bytes);
                    ASSERT_FALSE(::testing::Test::HasFatalFailure());
                }
            }
            const auto path = TempPath("stream_service_fault_" + std::to_string(slot) + ".stream");
            WriteFileBytes(path, bytes);
            ASSERT_EQ(VN_OK, streams.RegisterFile(spec.soundId, path.c_str()));
            harken::StreamManager::StreamFacts facts;
            ASSERT_TRUE(streams.FindFile(spec.soundId, &facts));
            files[slot] = facts.fileIndex;
            ASSERT_TRUE(streams.PrepareSlotWorkingSet(slot, 1));
            ASSERT_EQ(VN_OK, streams.ActivateSlot(slot, files[slot], 0, 0, 512, 1, &tokens[slot]));
        }
        bool failReads = false;
        unsigned reads = 0;
        ReadObserver observer(Access::Input(streams, files[badSlot]), [&] { ++reads; }, &failReads,
                              false, fault == Fault::ShortRead ? (badSlot == 0 ? 16 : 32) * sizeof(float) : SIZE_MAX);
        Access::ServeSlots(streams);
        std::size_t positions[2]{};
        for (unsigned slot = 0; slot < 2; ++slot) {
            ASSERT_EQ(128u, streams.RingOf(slot)->BufferedFrames());
            ExpectAndConsume(*streams.RingOf(slot), sources[slot], positions[slot], 64);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
        }
        // Cached invalid bytes must fail without retrying the provider.
        reads = 0;
        failReads = true;
        Access::ServeSlots(streams);
        for (unsigned slot = 0; slot < 2; ++slot) {
            auto& ring = *streams.RingOf(slot);
            const bool bad = slot == badSlot;
            EXPECT_EQ(bad ? harken::StreamTerminalReason::IntegrityAbort
                          : harken::StreamTerminalReason::None, ring.TerminalReason());
            EXPECT_EQ(bad ? 128u : 192u, ring.writeFrames.load(std::memory_order_acquire));
            ExpectAndConsume(ring, sources[slot], positions[slot], bad ? 64 : 128);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
        }
        Access::ServeSlots(streams);
        if (fault != Fault::ShortRead) EXPECT_EQ(0u, reads);
        EXPECT_EQ(0u, streams.RingOf(badSlot)->BufferedFrames());
        const unsigned healthy = 1 - badSlot;
        ExpectAndConsume(*streams.RingOf(healthy), sources[healthy], positions[healthy], 128);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        for (unsigned slot = 0; slot < 2; ++slot)
            ASSERT_TRUE(streams.RetirePreparedSlot(streams.RingOf(slot), tokens[slot]));
        streams.SyncServe();
        observer.Restore();
        for (unsigned slot = 0; slot < 2; ++slot)
            EXPECT_EQ(VN_OK, streams.UnregisterFile(harkentest::ObjectId(17100 + slot)));

    }
}

TEST(StreamRegister, StreamReadExceptionFailsServeAndRetriesVerifiedData) {
    using Access = harken::detail::StreamManagerTestAccess;
    auto config = MakeNullConfig(2, 64, kRate);
    config.maxStreams = 2;
    config.streamRingBytes = (128) * sizeof(float);
    config.prebufferMs = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& streams = session.Engine()->streams;
    streams.SetThrottled(true);
    streams.SyncServe();
    std::array<std::vector<float>, 2> sources;
    std::uint32_t files[2]{};
    std::uint64_t tokens[2]{};
    for (unsigned slot = 0; slot < 2; ++slot) {
        StreamSpec spec;
        spec.soundId = harkentest::ObjectId(17110 + slot);
        spec.chunkFrames = slot == 0 ? 16 : 32;
        sources[slot] = ServiceSamples(512, slot * 512);
        const auto path = TempPath("stream_service_throw_" + std::to_string(slot) + ".stream");
        WriteFileBytes(path, BuildStreamBytes(spec, sources[slot]));
        ASSERT_EQ(VN_OK, streams.RegisterFile(spec.soundId, path.c_str()));
        harken::StreamManager::StreamFacts facts;
        ASSERT_TRUE(streams.FindFile(spec.soundId, &facts));
        files[slot] = facts.fileIndex;
        ASSERT_TRUE(streams.PrepareSlotWorkingSet(slot, 1));
        ASSERT_EQ(VN_OK, streams.ActivateSlot(slot, files[slot], 0, 0, 512, 1, &tokens[slot]));
    }
    bool throwReads = false;
    ReadObserver observer(Access::Input(streams, files[1]), [&] {
        if (throwReads) throw std::ios_base::failure("injected read failure");
    }, nullptr, true, 32 * sizeof(float));
    Access::ServeSlots(streams);
    std::size_t positions[2]{};
    for (unsigned slot = 0; slot < 2; ++slot) {
        ASSERT_EQ(128u, streams.RingOf(slot)->BufferedFrames());
        ExpectAndConsume(*streams.RingOf(slot), sources[slot], positions[slot], 64);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
    }
    {
        // The source overwrites private read bytes before this failure escapes.
        throwReads = true;
        streams.SyncServe();
        // Every attempted read fails until quiescence, including an automatic
        // pass that may run before this explicit request reaches the worker.
        streams.SetThrottled(false);
        EXPECT_EQ(VN_BAD_STREAM_FILE, Access::ServeAndWait(streams));
        streams.SetThrottled(true);
        streams.SyncServe();
        EXPECT_EQ(harken::StreamWorkerState::Ready, streams.WorkerState());
        for (unsigned slot = 0; slot < 2; ++slot) {
            EXPECT_EQ(harken::StreamTerminalReason::None, streams.RingOf(slot)->TerminalReason());
            EXPECT_TRUE(Access::StagedDataMatchesVerifiedChunk(streams, slot));
        }
        EXPECT_EQ(128u, streams.RingOf(1)->writeFrames.load(std::memory_order_acquire));
    }
    throwReads = false;
    Access::ServeSlots(streams);
    for (unsigned slot = 0; slot < 2; ++slot) {
        ExpectAndConsume(*streams.RingOf(slot), sources[slot], positions[slot], 128);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
    }
    Access::ServeSlots(streams);
    for (unsigned slot = 0; slot < 2; ++slot) {
        ExpectAndConsume(*streams.RingOf(slot), sources[slot], positions[slot], 128);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        EXPECT_EQ(320u, positions[slot]);
        ASSERT_TRUE(streams.RetirePreparedSlot(streams.RingOf(slot), tokens[slot]));
    }
    streams.SyncServe();
    observer.Restore();
    for (unsigned slot = 0; slot < 2; ++slot)
        EXPECT_EQ(VN_OK, streams.UnregisterFile(harkentest::ObjectId(17110 + slot)));

}

TEST(StreamRegister, ConsumptionDuringReadKeepsServiceBudgetBounded) {
    using Access = harken::detail::StreamManagerTestAccess;
    auto config = MakeNullConfig(2, 64, kRate);
    config.maxStreams = 2;
    config.streamRingBytes = (128) * sizeof(float);
    config.prebufferMs = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& streams = session.Engine()->streams;
    streams.SetThrottled(true);
    streams.SyncServe();
    std::array<std::vector<float>, 2> sources;
    std::uint32_t files[2]{};
    std::uint64_t tokens[2]{};
    for (unsigned slot = 0; slot < 2; ++slot) {
        StreamSpec spec;
        spec.soundId = harkentest::ObjectId(17120 + slot);
        spec.chunkFrames = slot == 0 ? 16 : 32;
        sources[slot] = ServiceSamples(512, slot * 512);
        const auto path = TempPath("stream_service_budget_" + std::to_string(slot) + ".stream");
        WriteFileBytes(path, BuildStreamBytes(spec, sources[slot]));
        ASSERT_EQ(VN_OK, streams.RegisterFile(spec.soundId, path.c_str()));
        harken::StreamManager::StreamFacts facts;
        ASSERT_TRUE(streams.FindFile(spec.soundId, &facts));
        files[slot] = facts.fileIndex;
        ASSERT_TRUE(streams.PrepareSlotWorkingSet(slot, 1));
        ASSERT_EQ(VN_OK, streams.ActivateSlot(slot, files[slot], 0, 0, 512, 1, &tokens[slot]));
    }
    std::size_t positions[2]{};
    bool consumeOnRead = false;
    ReadObserver observer(Access::Input(streams, files[0]), [&] {
        if (consumeOnRead) {
            auto& ring = *streams.RingOf(0);
            ExpectAndConsume(ring, sources[0], positions[0], ring.BufferedFrames());
        }
    }, nullptr, false, 16 * sizeof(float));
    Access::ServeSlots(streams);
    std::uint64_t beforeWrite[2]{};
    std::uint32_t budgets[2]{};
    for (unsigned slot = 0; slot < 2; ++slot) {
        auto& ring = *streams.RingOf(slot);
        ASSERT_EQ(128u, ring.BufferedFrames());
        ExpectAndConsume(ring, sources[slot], positions[slot], 64);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        beforeWrite[slot] = ring.writeFrames.load(std::memory_order_acquire);
        budgets[slot] = ring.WriteAvailableBytes() / ring.bytesPerFrame;
    }
    consumeOnRead = true;
    Access::ServeSlots(streams);
    consumeOnRead = false;
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    for (unsigned slot = 0; slot < 2; ++slot) {
        auto& ring = *streams.RingOf(slot);
        const auto published = ring.writeFrames.load(std::memory_order_acquire) - beforeWrite[slot];
        EXPECT_GT(published, 0u);
        EXPECT_LE(published, budgets[slot]);
        EXPECT_EQ(harken::StreamTerminalReason::None, ring.TerminalReason());
        ExpectAndConsume(ring, sources[slot], positions[slot], ring.BufferedFrames());
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        EXPECT_EQ(beforeWrite[slot] + published, positions[slot]);
        ASSERT_TRUE(streams.RetirePreparedSlot(streams.RingOf(slot), tokens[slot]));
    }
    streams.SyncServe();
    observer.Restore();
    for (unsigned slot = 0; slot < 2; ++slot)
        EXPECT_EQ(VN_OK, streams.UnregisterFile(harkentest::ObjectId(17120 + slot)));

}

TEST(StreamRegister, RegisterExposesTocFactsThroughFindFile) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& streams = session.Engine()->streams;
    using RingAccess = harken::detail::StreamManagerTestAccess;
    EXPECT_EQ(0u, RingAccess::CommittedBytes(streams));

    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(7001);
    spec.sampleRate = 44100;
    spec.channelCount = 2;
    spec.chunkFrames = 128;
    spec.loopable = true;
    spec.loopStartFrame = 100;
    spec.loopEndFrame = 700;
    std::vector<float> planar(800 * 2);
    const std::vector<float> tone =
        harkentest::MakeSine(800, 500.0, 44100.0, 0.5f);
    for (std::size_t n = 0; n < 800; ++n) {
        planar[n] = tone[n];
        planar[800 + n] = -tone[n];
    }
    const std::string path = TempPath("stream_reg_facts.stream");
    WriteFileBytes(path, BuildStreamBytes(spec, planar));

    harken::StreamManager::StreamFacts facts;
    RingAccess::SetCommitPageLimit(streams, 1);
    ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(spec.soundId, path.c_str()));
    EXPECT_EQ(0u, RingAccess::CommittedBytes(streams));
    auto play = harkentest::MakePlayDesc(spec.soundId);
    VnVoiceHandle voice = 0;
    const auto freeSlots = streams.FreeSlotCount();
    EXPECT_EQ(VN_NO_STREAM, harkentest::PlayRaw(&play, &voice));
    EXPECT_EQ(0u, voice);
    EXPECT_EQ(freeSlots, streams.FreeSlotCount());
    EXPECT_EQ(0u, session.Engine()->voices.ActiveCount());
    EXPECT_GT(RingAccess::CommittedBytes(streams), 0u);
    RingAccess::SetCommitPageLimit(streams, SIZE_MAX);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));

    ASSERT_TRUE(session.Engine()->streams.FindFile(harkentest::ObjectId(7001), &facts));
    EXPECT_EQ(44100u, facts.sampleRate);
    EXPECT_EQ(2u, facts.channelCount);
    EXPECT_EQ(800u, facts.frameCount);
    EXPECT_EQ(harken::kStreamFlagLoopable, facts.flags);
    EXPECT_EQ(100u, facts.loopStartFrame);
    EXPECT_EQ(700u, facts.loopEndFrame);
    EXPECT_NE(0u, facts.contentRevision);

    const auto originalRevision = facts.contentRevision;
    spec.channelCount = 3;
    planar.resize(800 * 3, 0.125f);
    const auto widerPath = TempPath("stream_reg_wider.stream");
    WriteFileBytes(widerPath, BuildStreamBytes(spec, planar));
    const auto ringBytes = RingAccess::CommittedBytes(streams);
    RingAccess::SetCommitPageLimit(streams, 0);
    ASSERT_EQ(VN_OK, Vn_ReplaceStreamFile(spec.soundId, widerPath.c_str()));
    ASSERT_TRUE(streams.FindFile(spec.soundId, &facts));
    EXPECT_EQ(3u, facts.channelCount);
    EXPECT_NE(originalRevision, facts.contentRevision);
    EXPECT_EQ(ringBytes, RingAccess::CommittedBytes(streams));
}

TEST(StreamRegister, PendingReferencePreservesWideBackingThroughNarrowStartsAndRollback) {
    auto config = MakeNullConfig(4, 256, kRate);
    config.maxStreams = 4;
    config.streamRingBytes = (1024) * sizeof(float);
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& streams = session.Engine()->streams;
    using RingAccess = harken::detail::StreamManagerTestAccess;
    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(7011);
    spec.sampleRate = kRate;
    spec.channelCount = 2;
    spec.chunkFrames = 256;
    const auto widePath = TempPath("stream_pending_wide.stream");
    WriteFileBytes(widePath, BuildStreamBytes(spec, std::vector<float>(8192, 0.25f)));
    auto monoSource = std::make_shared<ControlledSource>();
    monoSource->bytes = MakeValidImage(harkentest::ObjectId(7012), 4096);
    monoSource->maxRead = 256 * sizeof(float);
    const auto monoToc = harken::ParseStreamTocEntry(monoSource->bytes.data() + harken::kStreamHeaderBytes);
    ASSERT_EQ(VN_OK, streams.RegisterFile(spec.soundId, widePath.c_str()));
    harken::StreamManager::SourceToken prepared;
    ASSERT_EQ(VN_OK, streams.BeginSource(harkentest::ObjectId(7012), monoSource, false, 0, &prepared));
    ASSERT_TRUE(WaitUntil([&] { return streams.QuerySource(prepared) == harken::StreamManager::PreparationState::Ready; }));
    ASSERT_EQ(VN_OK, streams.TakeSource(prepared));
    harken::StreamManager::StreamFacts wide{}, mono{};
    ASSERT_TRUE(streams.FindFile(spec.soundId, &wide));
    ASSERT_TRUE(streams.FindFile(harkentest::ObjectId(7012), &mono));
    ASSERT_TRUE(streams.AcquirePendingRef(wide.fileIndex));
    for (std::uint32_t slot = 0; slot < 2; ++slot) {
        ASSERT_TRUE(streams.PrepareSlotWorkingSet(slot, 1));
        ASSERT_EQ(VN_OK, streams.ActivateSlot(slot, mono.fileIndex, 0, 0, 4096, 1));
    }
    const auto bytes = RingAccess::CommittedBytes(streams);
    RingAccess::SetCommitPageLimit(streams, 0);
    for (std::uint32_t retry = 0; retry < 8; ++retry) {
        std::uint32_t slot = UINT32_MAX;
        ASSERT_TRUE(streams.PeekPreparedFreeSlot(2, &slot));
        EXPECT_EQ(2u, slot);
        std::uint64_t token = 0;
        ASSERT_EQ(VN_OK, streams.ActivateSlot(slot, wide.fileIndex, 0, 0, 4096, 1, &token));
        ASSERT_TRUE(streams.RetirePreparedSlot(streams.RingOf(slot), token));
        EXPECT_EQ(bytes, RingAccess::CommittedBytes(streams));
    }
    std::uint32_t rollbackSlot = UINT32_MAX;
    ASSERT_TRUE(streams.PeekPreparedFreeSlot(2, &rollbackSlot));
    ASSERT_EQ(2u, rollbackSlot);
    std::uint64_t rollbackToken = 0;
    ASSERT_EQ(VN_OK, streams.ActivateSlot(rollbackSlot, wide.fileIndex, 0, 0, 4096, 1, &rollbackToken));
    monoSource->gate->offset.store(monoToc.payloadOffset + 1024 * sizeof(float));
    ReleaseReadGate release{*monoSource->gate};
    harkentest::ConsumePcmFrames(*streams.RingOf(0), 1024);
    ASSERT_TRUE(WaitUntil([&] { return monoSource->gate->entered.load(); }));
    harken::SetStreamWorkerStallReleaseForTests(false);
    const auto stalls = harken::StreamWorkerStallEntriesForTests();
    harken::SetStreamRequestFaultForTests(harken::StreamRequestFault::WorkerStall);
    const bool stalled = WaitUntil([&] { return harken::StreamWorkerStallEntriesForTests() > stalls; });
    if (!stalled) {
        harken::SetStreamRequestFaultForTests(harken::StreamRequestFault::None);
        harken::SetStreamWorkerStallReleaseForTests(true);
    }
    ASSERT_TRUE(stalled);
    EXPECT_TRUE(streams.RetirePreparedSlot(streams.RingOf(rollbackSlot), rollbackToken));
    std::atomic<bool> reaped{false};
    bool available = false;
    std::uint32_t readySlot = UINT32_MAX;
    std::thread peek([&] {
        available = streams.PeekPreparedFreeSlot(2, &readySlot);
        reaped.store(true, std::memory_order_release);
    });
    const bool queued = WaitUntil([&] { return streams.PendingRequestCountForTest() != 0; });
    harken::SetStreamRequestFaultForTests(harken::StreamRequestFault::None);
    harken::SetStreamWorkerStallReleaseForTests(true);
    const bool independent = WaitUntil([&] { return reaped.load(std::memory_order_acquire); });
    if (!independent) monoSource->gate->release.store(true);
    peek.join();
    EXPECT_TRUE(queued);
    EXPECT_TRUE(independent);
    EXPECT_TRUE(available);
    EXPECT_EQ(2u, readySlot);
    monoSource->gate->release.store(true);
    streams.ReleasePendingRef(wide.fileIndex);
    streams.SyncServe();
    EXPECT_EQ(VN_OK, streams.UnregisterFile(spec.soundId));
}

TEST(StreamRegister, ContentRevisionUsesSemanticHeaderAndChunkChecksums) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(7201);
    spec.sampleRate = 44100;
    spec.channelCount = 1;
    spec.chunkFrames = 128;
    std::vector<float> samples =
        harkentest::MakeSine(600, 500.0, 44100.0, 0.5f);

    const std::string firstPath =
        TempPath("stream_content_revision_first.stream");
    WriteFileBytes(firstPath, BuildStreamBytes(spec, samples));
    ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(spec.soundId, firstPath.c_str()));
    harken::StreamManager::StreamFacts first;
    ASSERT_TRUE(session.Engine()->streams.FindFile(spec.soundId, &first));
    ASSERT_NE(0u, first.contentRevision);

    spec.soundId = harkentest::ObjectId(7202);
    const std::string samePath =
        TempPath("stream_content_revision_same.stream");
    WriteFileBytes(samePath, BuildStreamBytes(spec, samples));
    ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(spec.soundId, samePath.c_str()));
    harken::StreamManager::StreamFacts same;
    ASSERT_TRUE(session.Engine()->streams.FindFile(spec.soundId, &same));
    EXPECT_EQ(first.contentRevision, same.contentRevision);

    spec.soundId = harkentest::ObjectId(7203);
    samples[257] += 0.001f;
    const std::string changedPath =
        TempPath("stream_content_revision_changed.stream");
    WriteFileBytes(changedPath, BuildStreamBytes(spec, samples));
    ASSERT_EQ(VN_OK,
              Vn_RegisterStreamFile(spec.soundId, changedPath.c_str()));
    harken::StreamManager::StreamFacts changed;
    ASSERT_TRUE(session.Engine()->streams.FindFile(spec.soundId, &changed));
    EXPECT_NE(first.contentRevision, changed.contentRevision);

    spec.soundId = harkentest::ObjectId(7204);
    spec.chunkFrames = 256;
    const std::string rechunkedPath =
        TempPath("stream_content_revision_rechunked.stream");
    WriteFileBytes(rechunkedPath, BuildStreamBytes(spec, samples));
    ASSERT_EQ(VN_OK,
              Vn_RegisterStreamFile(spec.soundId, rechunkedPath.c_str()));
    harken::StreamManager::StreamFacts rechunked;
    ASSERT_TRUE(session.Engine()->streams.FindFile(spec.soundId, &rechunked));
    EXPECT_NE(changed.contentRevision, rechunked.contentRevision);
}

TEST(StreamRegister, LoopReplacementChangesRevisionAndPreservesPinnedGeneration) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& streams = session.Engine()->streams;
    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(7205);
    spec.sampleRate = kRate;
    spec.channelCount = 1;
    spec.chunkFrames = 128;
    const auto samples = harkentest::MakeSine(600, 500.0, kRate, 0.5f);
    const auto firstPath = TempPath("stream_loop_revision_first.stream");
    WriteFileBytes(firstPath, BuildStreamBytes(spec, samples));
    ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(spec.soundId, firstPath.c_str()));
    harken::StreamManager::StreamFacts original{};
    ASSERT_TRUE(streams.FindFile(spec.soundId, &original));
    ASSERT_TRUE(streams.AcquirePendingRef(original.fileIndex));

    spec.loopable = true;
    spec.loopStartFrame = 128;
    spec.loopEndFrame = 512;
    const auto replacementPath = TempPath("stream_loop_revision_next.stream");
    WriteFileBytes(replacementPath, BuildStreamBytes(spec, samples));
    ASSERT_EQ(VN_OK, Vn_ReplaceStreamFile(spec.soundId, replacementPath.c_str()));
    harken::StreamManager::StreamFacts current{}, pinned{};
    ASSERT_TRUE(streams.FindFile(spec.soundId, &current));
    EXPECT_NE(original.contentRevision, current.contentRevision);
    ASSERT_TRUE(streams.FactsForFileIndex(original.fileIndex, &pinned));
    EXPECT_EQ(original.contentRevision, pinned.contentRevision);
    EXPECT_EQ(original.flags, pinned.flags);
    EXPECT_EQ(original.loopStartFrame, pinned.loopStartFrame);
    EXPECT_EQ(original.loopEndFrame, pinned.loopEndFrame);
    streams.ReleasePendingRef(original.fileIndex);
    streams.SyncServe();
    EXPECT_EQ(0u, streams.RetiringFileCount());

    auto previousRevision = current.contentRevision;
    for (const auto region : {std::pair{192u, 512u}, std::pair{192u, 576u},
                              std::pair{0u, 600u}, std::pair{0u, 0u}}) {
        spec.loopStartFrame = region.first;
        spec.loopEndFrame = region.second;
        const auto path = TempPath("stream_loop_revision_" +
            std::to_string(region.first) + "_" + std::to_string(region.second) + ".stream");
        WriteFileBytes(path, BuildStreamBytes(spec, samples));
        ASSERT_EQ(VN_OK, Vn_ReplaceStreamFile(spec.soundId, path.c_str()));
        ASSERT_TRUE(streams.FindFile(spec.soundId, &current));
        if (region.second == 0) EXPECT_EQ(previousRevision, current.contentRevision);
        else EXPECT_NE(previousRevision, current.contentRevision);
        previousRevision = current.contentRevision;
        streams.SyncServe();
    }
    EXPECT_EQ(VN_BAD_STREAM_FILE,
              Vn_ReplaceStreamFile(spec.soundId, "missing_loop_revision.stream"));
    ASSERT_TRUE(streams.FindFile(spec.soundId, &current));
    EXPECT_EQ(previousRevision, current.contentRevision);
    EXPECT_EQ(VN_OK, Vn_UnregisterStreamFile(spec.soundId));
    EXPECT_EQ(0u, streams.RetiringFileCount());
}

TEST(StreamRegister, EnqueueFailureReturnsBadFileAndCanRetry) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::string path = TempPath("stream_reg_enqueue_fault.stream");
    WriteFileBytes(path, MakeValidImage(harkentest::ObjectId(7001)));

    {
        StreamRequestFaultGuard fault(harken::StreamRequestFault::Enqueue);
        EXPECT_EQ(VN_BAD_STREAM_FILE,
                  Vn_RegisterStreamFile(harkentest::ObjectId(7001), path.c_str()));
    }

    harken::StreamManager::StreamFacts facts;
    EXPECT_FALSE(session.Engine()->streams.FindFile(harkentest::ObjectId(7001), &facts));
    EXPECT_EQ(VN_OK, Vn_RegisterStreamFile(harkentest::ObjectId(7001), path.c_str()));
    EXPECT_EQ(VN_OK, Vn_UnregisterStreamFile(harkentest::ObjectId(7001)));
}

TEST(StreamRegister, ChunkFramesAcceptanceMatchesSharedCeiling) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> oneFrame = {0.25f};
    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(7010);
    spec.sampleRate = kRate;
    spec.channelCount = 1;
    spec.chunkFrames = harken::kMaxStreamChunkFrames;
    const std::string acceptedPath =
        TempPath("stream_reg_chunk_ceiling.stream");
    WriteFileBytes(acceptedPath, BuildStreamBytes(spec, oneFrame));
    EXPECT_EQ(VN_OK,
              Vn_RegisterStreamFile(spec.soundId, acceptedPath.c_str()));

    spec.soundId = harkentest::ObjectId(7011);
    spec.chunkFrames = harken::kMaxStreamChunkFrames + 1;
    const std::string refusedPath =
        TempPath("stream_reg_chunk_over_ceiling.stream");
    WriteFileBytes(refusedPath, BuildStreamBytes(spec, oneFrame));
    EXPECT_EQ(VN_BAD_STREAM_FILE,
              Vn_RegisterStreamFile(spec.soundId, refusedPath.c_str()));
}

TEST(StreamRegister, ArgumentAndLifecycleRefusals) {
    // No engine: shape checks cannot even run.
    EXPECT_EQ(VN_NOT_INITIALIZED,
              Vn_RegisterStreamFile(harkentest::ObjectId(7002), "irrelevant"));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_UnregisterStreamFile(harkentest::ObjectId(7002)));

    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterStreamFile(harkentest::ObjectId(7002), nullptr));

    // A missing file is a bad stream file, not a bad argument.
    const std::string missing = TempPath("stream_reg_missing.stream");
    std::filesystem::remove(missing);
    EXPECT_EQ(VN_BAD_STREAM_FILE,
              Vn_RegisterStreamFile(harkentest::ObjectId(7002), missing.c_str()));

    // Unregistering an id that names no stream.
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_UnregisterStreamFile(harkentest::ObjectId(7002)));
}

TEST(StreamRegister, CorruptContainersRefusedWithoutSideEffects) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<std::uint8_t> valid = MakeValidImage(harkentest::ObjectId(7003));
    const std::string path = TempPath("stream_reg_corrupt.stream");

    struct Corruption {
        const char* name;
        void (*mutate)(std::vector<std::uint8_t>*);
    };
    const Corruption corruptions[] = {
        {"wrong magic",
         [](std::vector<std::uint8_t>* b) { (*b)[0] = 'X'; }},
        {"unknown format version",
         [](std::vector<std::uint8_t>* b) {
             harken::StoreLE32(b->data() + 4, 999);
         }},
        {"entry count not one",
         [](std::vector<std::uint8_t>* b) {
             harken::StoreLE32(b->data() + 20, 2);
         }},
        {"TOC entry byte flipped (tocChecksum mismatch)",
         [](std::vector<std::uint8_t>* b) {
             (*b)[harken::kStreamHeaderBytes + 16] ^= 0x01;
         }},
        {"chunk-checksum table byte flipped (tocChecksum mismatch)",
         [](std::vector<std::uint8_t>* b) {
             (*b)[harken::kStreamHeaderBytes +
                  harken::kStreamTocEntryBytes] ^= 0x01;
         }},
        {"file truncated mid-payload",
         [](std::vector<std::uint8_t>* b) { b->resize(b->size() / 2); }},
        {"chunk count off by one (validated field, checksum refit)",
         [](std::vector<std::uint8_t>* b) {
             harken::StreamTocEntry entry = harken::ParseStreamTocEntry(
                 b->data() + harken::kStreamHeaderBytes);
             entry.chunkCount += 1;
             harken::SerializeStreamTocEntry(
                 entry, b->data() + harken::kStreamHeaderBytes);
             FixTocChecksum(b);
         }},
        {"loop fields on a non-loopable entry (checksum refit)",
         [](std::vector<std::uint8_t>* b) {
             harken::StreamTocEntry entry = harken::ParseStreamTocEntry(
                 b->data() + harken::kStreamHeaderBytes);
             entry.loopStartFrame = 10;
             entry.loopEndFrame = 20;
             harken::SerializeStreamTocEntry(
                 entry, b->data() + harken::kStreamHeaderBytes);
             FixTocChecksum(b);
         }},
    };

    for (const Corruption& corruption : corruptions) {
        std::vector<std::uint8_t> bytes = valid;
        corruption.mutate(&bytes);
        WriteFileBytes(path, bytes);
        EXPECT_EQ(VN_BAD_STREAM_FILE,
                  Vn_RegisterStreamFile(harkentest::ObjectId(7003), path.c_str()))
            << corruption.name;
        harken::StreamManager::StreamFacts facts;
        EXPECT_FALSE(session.Engine()->streams.FindFile(harkentest::ObjectId(7003), &facts))
            << corruption.name << ": refusal must leave no registration";
    }

    // The untouched image proves the fixture itself is valid — the
    // refusals above measured the corruption, not the builder.
    WriteFileBytes(path, valid);
    EXPECT_EQ(VN_OK, Vn_RegisterStreamFile(harkentest::ObjectId(7003), path.c_str()));
}

TEST(StreamRegister, TocSoundIdMustMatchRegisteredId) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    // The file's TOC names the id it was compiled for; registering it
    // under a different id is a packaging error and must refuse instead
    // of silently serving the wrong content at playback.
    const std::string path = TempPath("stream_reg_id_mismatch.stream");
    WriteFileBytes(path, MakeValidImage(harkentest::ObjectId(7006)));

    EXPECT_EQ(VN_BAD_STREAM_FILE, Vn_RegisterStreamFile(harkentest::ObjectId(7007), path.c_str()));
    harken::StreamManager::StreamFacts facts;
    EXPECT_FALSE(session.Engine()->streams.FindFile(harkentest::ObjectId(7007), &facts));

    // The identical file under its own id registers normally, proving the
    // refusal measured the mismatch and not the fixture.
    EXPECT_EQ(VN_OK, Vn_RegisterStreamFile(harkentest::ObjectId(7006), path.c_str()));
}

TEST(StreamRegister, SoundIdNamespaceIsSharedWithPCM) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::string path = TempPath("stream_reg_shared.stream");
    WriteFileBytes(path, MakeValidImage(harkentest::ObjectId(7004)));

    ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(harkentest::ObjectId(7004), path.c_str()));
    // Same id again as a stream: taken.
    EXPECT_EQ(VN_IN_USE, Vn_RegisterStreamFile(harkentest::ObjectId(7004), path.c_str()));
    // Same id as resident PCM: also taken — one namespace.
    const std::vector<float> tone = harkentest::MakeSine(64, 500.0, kRate,
                                                        0.5f);
    VnPCMDesc desc = harkentest::MonoDesc(tone, kRate);
    EXPECT_EQ(VN_IN_USE, Vn_RegisterPCM(harkentest::ObjectId(7004), &desc));

    // And the mirror image: a PCM-registered id refuses stream
    // registration.
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(7005), &desc));
    EXPECT_EQ(VN_IN_USE, Vn_RegisterStreamFile(harkentest::ObjectId(7005), path.c_str()));

    // Unregistration frees the name for either namespace.
    ASSERT_EQ(VN_OK, Vn_UnregisterStreamFile(harkentest::ObjectId(7004)));
    harken::StreamManager::StreamFacts facts;
    EXPECT_FALSE(session.Engine()->streams.FindFile(harkentest::ObjectId(7004), &facts));
    EXPECT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(7004), &desc));
}

// Illegal UTF-8 in the path is refused end to end, never undefined
// behavior: the streaming thread converts the path during the blocking
// register, so a byte sequence that is not valid UTF-8 must surface as
// VN_BAD_STREAM_FILE rather than escaping the conversion — the thread must
// not terminate and PostAndWait must not hang. Each literal terminates its
// hex escape at the quote so the following ASCII is a separate token.
constexpr const char* kIllegalUtf8Paths[] = {
    "\xFF\xFE\x80" "stream",  // bytes that are never UTF-8 lead bytes
    "\x80" "orphan",          // an isolated continuation byte, no lead
    "\xC0\xAF" "overlong",    // overlong two-byte encoding of '/'
};

TEST(StreamRegister, IllegalUtf8PathRefusedAndStreamThreadSurvives) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    for (const char* illegal : kIllegalUtf8Paths) {
        EXPECT_EQ(VN_BAD_STREAM_FILE,
                  Vn_RegisterStreamFile(harkentest::ObjectId(7101), illegal)) << illegal;
        harken::StreamManager::StreamFacts facts;
        EXPECT_FALSE(session.Engine()->streams.FindFile(harkentest::ObjectId(7101), &facts))
            << illegal << ": refusal must leave no registration";
    }

    // Proof the streaming thread outlived every refusal and PostAndWait was
    // signalled each time: a well-formed file still registers on the same
    // session and the update pump completes.
    const std::string path = TempPath("stream_reg_after_illegal.stream");
    WriteFileBytes(path, MakeValidImage(harkentest::ObjectId(7101)));
    EXPECT_EQ(VN_OK, Vn_RegisterStreamFile(harkentest::ObjectId(7101), path.c_str()));
    EXPECT_EQ(VN_OK, Vn_Update());
}

TEST(StreamRegister, IllegalUtf8ReplacePathRefusedOldFileKept) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::string path = TempPath("stream_reg_replace_base.stream");
    WriteFileBytes(path, MakeValidImage(harkentest::ObjectId(7102)));
    ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(harkentest::ObjectId(7102), path.c_str()));

    // An illegal-UTF-8 replacement path is a bad stream file with the old
    // registration untouched, never an escaping conversion.
    EXPECT_EQ(VN_BAD_STREAM_FILE,
              Vn_ReplaceStreamFile(harkentest::ObjectId(7102), kIllegalUtf8Paths[0]));

    // The original file is still registered and serving its own facts.
    harken::StreamManager::StreamFacts facts;
    ASSERT_TRUE(session.Engine()->streams.FindFile(harkentest::ObjectId(7102), &facts));
    EXPECT_EQ(1000u, facts.frameCount);
    EXPECT_EQ(kRate, facts.sampleRate);
}

// A well-formed multibyte UTF-8 path registers end to end. The UTF-8 funnel
// wrapped around the streaming thread's open must accept legal non-ASCII, not
// reject everything past 7-bit — the mirror of the refusals above, proving
// they measured ill-formed bytes rather than the mere presence of multibyte
// ones. The filename carries Chinese plus an emoji. The file is created
// through a path decoded from the same UTF-8 bytes the API is handed, so the
// test's write and the engine's UTF-8 -> native transcode resolve to one file;
// a narrow WriteFileBytes would decode those bytes in the ANSI codepage on
// Windows and name a different file. The hex escapes seal at the quote so the
// ASCII tail is a separate token.
TEST(StreamRegister, LegalUtf8PathRegistersEndToEnd) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    // The filename contains two CJK code points and a musical-note emoji.
    const std::string utf8Path = TempPath(
        "stream_reg_" "\xE6\xB5\x8B\xE8\xAF\x95" "\xF0\x9F\x8E\xB5" ".stream");
    const std::filesystem::path filePath(
        reinterpret_cast<const char8_t*>(utf8Path.c_str()));

    std::filesystem::create_directories(filePath.parent_path());
    {
        const std::vector<std::uint8_t> image = MakeValidImage(harkentest::ObjectId(7103));
        std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good()) << "cannot create UTF-8-named stream file";
        out.write(reinterpret_cast<const char*>(image.data()),
                  static_cast<std::streamsize>(image.size()));
        ASSERT_TRUE(out.good());
    }

    EXPECT_EQ(VN_OK, Vn_RegisterStreamFile(harkentest::ObjectId(7103), utf8Path.c_str()));
    harken::StreamManager::StreamFacts facts;
    EXPECT_TRUE(session.Engine()->streams.FindFile(harkentest::ObjectId(7103), &facts));

    // Leave nothing behind: drop the registration, then the file.
    EXPECT_EQ(VN_OK, Vn_UnregisterStreamFile(harkentest::ObjectId(7103)));
    std::error_code ec;
    std::filesystem::remove(filePath, ec);
}

} // namespace


TEST(StreamRegister, StreamObservationsMeasureProviderCallsCacheFailuresAndReuse) {
    using Manager = harken::StreamManager;
    class ObservedSource final : public ControlledSource {
    public:
        unsigned failure = 0;
        mutable std::atomic<std::uint64_t> lastOffset{0}, lastRequest{0}, lastBytes{0}, totalBytes{0};
        harken::StreamReadResult ReadAt(std::uint64_t offset, void* data, std::size_t count,
            harken::StreamReadCancellation cancel) const override {
            lastOffset = offset; lastRequest = count; lastBytes = 0;
            if (failure == 2) { ++reads; return {}; }
            if (failure == 4) { ++reads; throw std::runtime_error("provider failed"); }
            const auto result = ControlledSource::ReadAt(offset, data, count, cancel);
            lastBytes = result.bytes; totalBytes += result.bytes;
            return result;
        }
    };
    for (const unsigned failure : {0u, 2u, 3u, 4u}) {
        SCOPED_TRACE(failure);
        auto config = harkentest::MakeNullConfig(1, 64, 48000);
        config.maxStreams = 1;
        config.streamRingBytes = (64) * sizeof(float);
        config.prebufferMs = 1;
        harkentest::EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& streams = session.Engine()->streams;
        streams.SetThrottled(true);
        auto source = std::make_shared<ObservedSource>();
        StreamSpec spec;
        spec.soundId = harkentest::ObjectId(17901);
        spec.chunkFrames = 64;
        source->bytes = BuildStreamBytes(spec, ServiceSamples(8192, 0));
        source->maxRead = failure == 0 ? SIZE_MAX : 17;
        Manager::SourceToken request;
        ASSERT_EQ(VN_OK, streams.BeginSource(spec.soundId, source, false, 0, &request));
        ASSERT_TRUE(WaitUntil([&] { return streams.QuerySource(request) == Manager::PreparationState::Ready; }));
        Manager::StreamFacts facts{};
        ASSERT_EQ(VN_OK, streams.TakeSource(request, &facts));
        source->reads = 0; source->totalBytes = 0; source->failure = failure;
        if (failure == 3) {
            const auto toc = harken::ParseStreamTocEntry(source->bytes.data() + harken::kStreamHeaderBytes);
            source->bytes[static_cast<std::size_t>(toc.payloadOffset)] ^= 1;
        }
        ASSERT_TRUE(streams.PrepareSlotWorkingSet(0, 1));
        std::uint64_t token = 0;
        streams.SetThrottled(false);
        EXPECT_EQ(failure == 4 ? VN_BAD_STREAM_FILE : VN_OK,
            streams.ActivateSlot(0, facts.fileIndex, 0, 0, facts.frameCount, 1, &token));
        streams.SyncServe();
        streams.SetThrottled(true);
        streams.SyncServe();
        // Late subscribers receive the completed job and whole-generation
        // totals, including provider work performed while no session existed.
        const auto monitor = harkentest::monitor::OpenIdentityEvents(4);
        streams.PublishPlaybackObservations();
        auto rows = harkentest::monitor::ReadExecutionEvents<VnMonitorStreamFactV1>(monitor);
        ASSERT_FALSE(rows.empty());
        const auto fact = rows.back().fact;
        EXPECT_EQ(token, fact.ringToken);
        EXPECT_EQ(facts.sourceGeneration, fact.sourceGeneration);
        EXPECT_EQ(source->reads.load(), fact.totalReadCalls);
        EXPECT_EQ(source->totalBytes.load(), fact.totalReadBytes);
        EXPECT_EQ(source->lastOffset.load(), fact.readOffset);
        EXPECT_EQ(source->lastRequest.load(), fact.readRequestedBytes);
        EXPECT_EQ(source->lastBytes.load(), fact.lastReadBytes);
        EXPECT_GT(fact.readCalls, 0u);
        EXPECT_GE(fact.readEndNs - fact.readBeginNs, fact.readCostNs);
        EXPECT_EQ(failure ? failure : 1u, fact.readStatus);
        EXPECT_EQ(0u, fact.audioBlock);
        Manager::PlaybackObservation state{};
        ASSERT_TRUE(streams.CapturePlayback(token, &state));
        EXPECT_EQ(failure == 4 ? Manager::PreparationState::Preparing :
            failure ? Manager::PreparationState::Failed : Manager::PreparationState::Ready, state.state);
        if (!failure) {
            auto* ring = streams.RingOf(0);
            const auto before = fact.totalReadCalls;
            harkentest::ConsumePcmFrames(*ring, ring->BufferedFrames());
            streams.SetThrottled(false);
            streams.SyncServe();
            streams.SetThrottled(true);
            streams.PublishPlaybackObservations();
            rows = harkentest::monitor::ReadExecutionEvents<VnMonitorStreamFactV1>(monitor);
            ASSERT_FALSE(rows.empty());
            EXPECT_GT(rows.back().fact.readSequence, fact.readSequence);
            EXPECT_EQ(0u, rows.back().fact.readCalls);
            EXPECT_EQ(0u, rows.back().fact.readCostNs);
            EXPECT_EQ(0u, rows.back().fact.readStatus);
            EXPECT_EQ(before, rows.back().fact.totalReadCalls);
            ASSERT_TRUE(ring->MarkEndOfData(harken::StreamTerminalReason::ProducerFailure));
            ASSERT_TRUE(streams.CapturePlayback(token, &state));
            EXPECT_EQ(Manager::PreparationState::Failed, state.state);
            EXPECT_EQ(harken::StreamTerminalReason::ProducerFailure, state.terminal);
        }
        ASSERT_TRUE(streams.RetirePreparedSlot(streams.RingOf(0), token));
        streams.SyncServe();
        EXPECT_FALSE(streams.CapturePlayback(token, &state));
        if (!failure) {
            std::uint64_t replacement = 0;
            ASSERT_EQ(VN_OK, streams.ActivateSlot(0, facts.fileIndex, 0, 0, facts.frameCount, 1, &replacement));
            EXPECT_NE(token, replacement);
            EXPECT_FALSE(streams.CapturePlayback(token, &state));
            ASSERT_TRUE(streams.CapturePlayback(replacement, &state));
            streams.PublishPlaybackObservations();
            rows = harkentest::monitor::ReadExecutionEvents<VnMonitorStreamFactV1>(monitor);
            ASSERT_FALSE(rows.empty());
            EXPECT_EQ(replacement, rows.back().fact.ringToken);
            EXPECT_EQ(0u, rows.back().fact.totalReadCalls);
            EXPECT_EQ(0u, rows.back().fact.readSequence);
            ASSERT_TRUE(streams.RetirePreparedSlot(streams.RingOf(0), replacement));
            streams.SyncServe();
        }
        EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(monitor));
    }
}

TEST(StreamRegister, VorbisPackRangeFeedsVerifiedPacketsAcrossSeekAndLoop) {
    for (const unsigned channels : {1u, 2u, 4u, 6u, 8u}) {
        SCOPED_TRACE(channels);
        harken::EngineConfig config;
        config.maxStreams = 2;
        config.streamRingBytes = 8192;
        harken::VorbisCodec codec(2 * 1024 * 1024, 0);
        harken::StreamManager streams(config);
        auto fixture = harken::test::MakeVorbisFixture(channels, true, 257, 1023);
        harken::VorbisMedia media;
        ASSERT_TRUE(harken::ParseVorbisTestMedia(fixture.media.data(), fixture.media.size(), media));
        std::vector<std::uint8_t> prefix(fixture.media.begin(), fixture.media.begin() + media.packetDataOffset);
        harken::VorbisPrepareResult result;
        auto encoded = harken::VorbisStreamMedia::Prepare(codec, std::move(prefix), fixture.media.size(),
                                                          4 * 1024 * 1024, result);
        ASSERT_NE(encoded, nullptr);
        auto source = std::make_shared<ControlledSource>();
        source->bytes = std::move(fixture.media);
        auto range = std::make_shared<harken::PackMediaRange>();
        range->source = source; range->vorbis = encoded;
        range->sampleRate = media.sampleRate; range->channelCount = channels;
        range->frameCount = media.frames; range->contentRevision = media.revision;
        range->loop = {media.loopStart, media.loopEnd};
        harken::StreamManager::StreamFacts facts;
        ASSERT_EQ(VN_OK, streams.ResolvePackSource(range, &facts));
        EXPECT_EQ(source->reads.load(), 0u);
        ASSERT_EQ(facts.encoded, encoded.get());
        ASSERT_TRUE(streams.PrepareSlotWorkingSet(0, channels));
        ASSERT_EQ(VN_OK, streams.ActivateSlot(0, facts.fileIndex, 713, 257, 1023, 3));
        auto* ring = streams.RingOf(0);
        auto* decoder = facts.encoded->Acquire();
        ASSERT_NE(decoder, nullptr);
        std::uint64_t expectedFrame = 713;
        unsigned passes = 3;
        const auto total = (1023 - 713) + (1023 - 257) + media.frames - 257;
        std::uint64_t read = 0;
        static constexpr unsigned map6[] = {0, 2, 1, 5, 3, 4};
        static constexpr unsigned map8[] = {0, 2, 1, 7, 5, 6, 3, 4};
        for (unsigned attempt = 0; read < total && attempt < 10000; ++attempt) {
            if (!ring->BufferedFrames()) streams.SyncServe();
            harken::PCMView output;
            ASSERT_EQ(harken::StreamDecodeResult::Ready, decoder->ReadStream(*ring, 257, 0, output));
            ASSERT_GT(output.frames, 0u);
            for (unsigned frame = 0; frame < output.frames; ++frame) {
                if (passes > 1 && expectedFrame == 1023) { --passes; expectedFrame = 257; }
                for (unsigned ch = 0; ch < channels; ++ch) {
                    const auto referenceChannel = channels == 6 ? map6[ch] : channels == 8 ? map8[ch] : ch;
                    ASSERT_FLOAT_EQ(static_cast<const float*>(output.channels[ch])[frame],
                        fixture.reference[expectedFrame * channels + referenceChannel]);
                }
                ++expectedFrame;
            }
            ring->DeliverFrames(static_cast<std::uint32_t>(output.frames));
            read += output.frames;
        }
        EXPECT_EQ(read, total);
        streams.SyncServe();
        EXPECT_EQ(ring->TerminalReason(), harken::StreamTerminalReason::NaturalEnd);
        decoder->Retire(); codec.states.Collect();
        ring->state.store(harken::StreamSlotState::Retired, std::memory_order_release);
        streams.SyncServe();
        EXPECT_EQ(streams.FreeSlotCount(), 2u);
        EXPECT_EQ(codec.states.ActiveBytes(), 0u);
    }
}

TEST(StreamRegister, VorbisStreamRejectsChangedPositionMetadataBeforePublishingFrames) {
    for (unsigned mutation = 0; mutation < 4; ++mutation) {
        SCOPED_TRACE(mutation);
        harken::EngineConfig config;
        config.maxStreams = 1;
        config.streamRingBytes = 8192;
        harken::VorbisCodec codec(2 * 1024 * 1024, 0);
        harken::StreamManager streams(config);
        auto fixture = harken::test::MakeVorbisFixture(1, true, 257, 1023);
        harken::VorbisMedia media;
        ASSERT_TRUE(harken::ParseVorbisTestMedia(fixture.media.data(), fixture.media.size(), media));
        ASSERT_GT(media.seek.size(), 1u);
        const auto discardOffset = harken::vorbis_test_wire::kHeaderBytes + media.identBytes + media.setupBytes +
            harken::vorbis_test_wire::kSeekBytes + 16;
        const auto offset = mutation == 0 ? discardOffset : mutation == 1 ? 68u : mutation == 2 ? 80u : 16u;
        const auto changed = mutation == 0 ? media.seek[1].discardFrames + 1 : mutation == 1 ? media.loopDiscard + 1 :
            mutation == 2 ? media.loopTrim + 1 : media.frames - 1;
        for (unsigned byte = 0; byte < 4; ++byte)
            fixture.media[offset + byte] = static_cast<std::uint8_t>(changed >> (8 * byte));
        std::vector<std::uint8_t> prefix(fixture.media.begin(), fixture.media.begin() + media.packetDataOffset);
        harken::VorbisPrepareResult result;
        auto encoded = harken::VorbisStreamMedia::Prepare(codec, std::move(prefix), fixture.media.size(),
            4 * 1024 * 1024, result);
        if (!encoded) { EXPECT_EQ(harken::VorbisPrepareResult::Invalid, result); continue; }
        auto source = std::make_shared<ControlledSource>();
        source->bytes = std::move(fixture.media);
        auto range = std::make_shared<harken::PackMediaRange>();
        range->source = source; range->vorbis = encoded;
        range->sampleRate = media.sampleRate; range->channelCount = 1;
        range->frameCount = encoded->Media().frames; range->contentRevision = media.revision;
        range->loop = {media.loopStart, media.loopEnd};
        harken::StreamManager::StreamFacts facts;
        ASSERT_EQ(VN_OK, streams.ResolvePackSource(range, &facts));
        ASSERT_TRUE(streams.PrepareSlotWorkingSet(0, 1));
        ASSERT_EQ(VN_OK, streams.ActivateSlot(0, facts.fileIndex, 4096, 0, range->frameCount, 1));
        streams.SyncServe();
        auto* ring = streams.RingOf(0);
        EXPECT_EQ(0u, ring->BufferedFrames());
        EXPECT_EQ(harken::StreamTerminalReason::IntegrityAbort, ring->TerminalReason());
        ring->state.store(harken::StreamSlotState::Retired, std::memory_order_release);
        streams.SyncServe();
    }
}

TEST(StreamRegister, FullVorbisByteRingDoesNotBlockAnotherStreamAndTinyRingFails) {
    auto fixture = harken::test::MakeVorbisFixture(1, true);
    harken::VorbisMedia media;
    ASSERT_TRUE(harken::ParseVorbisTestMedia(fixture.media.data(), fixture.media.size(), media));
    harken::VorbisCodec codec(2 * 1024 * 1024, 0);
    harken::VorbisPrepareResult prepared;
    std::vector<std::uint8_t> prefix(fixture.media.begin(), fixture.media.begin() + media.packetDataOffset);
    auto encoded = harken::VorbisStreamMedia::Prepare(codec, std::move(prefix), fixture.media.size(),
        4 * 1024 * 1024, prepared);
    ASSERT_NE(encoded, nullptr);
    auto source = std::make_shared<ControlledSource>();
    source->bytes = fixture.media;
    auto range = std::make_shared<harken::PackMediaRange>();
    range->source = source; range->vorbis = encoded;
    range->sampleRate = media.sampleRate; range->channelCount = 1;
    range->frameCount = media.frames; range->contentRevision = media.revision;
    for (unsigned capacity : {64u, 1024u}) {
        SCOPED_TRACE(capacity);
        harken::EngineConfig config;
        config.maxStreams = 2; config.streamRingBytes = capacity;
        harken::StreamManager streams(config);
        harken::StreamManager::StreamFacts facts;
        ASSERT_EQ(VN_OK, streams.ResolvePackSource(range, &facts));
        ASSERT_TRUE(streams.PrepareSlotWorkingSet(0, 1));
        const auto start = streams.ActivateSlot(0, facts.fileIndex, 0, 0, media.frames, 1);
        auto* paused = streams.RingOf(0);
        if (capacity == 64) {
            EXPECT_EQ(VN_NO_STREAM, start);
            EXPECT_EQ(VN_NO_STREAM, paused->preparationResult.load());
            EXPECT_EQ(0u, paused->BufferedFrames());
        } else {
            ASSERT_EQ(VN_OK, start);
            const auto initialPausedBytes = paused->ReadAvailableBytes();
            ASSERT_GT(paused->BufferedFrames(), 0u);
            ASSERT_TRUE(streams.PrepareSlotWorkingSet(1, 1));
            ASSERT_EQ(VN_OK, streams.ActivateSlot(1, facts.fileIndex, 0, 0, media.frames, 1));
            auto* playing = streams.RingOf(1);
            auto* decoder = encoded->Acquire();
            ASSERT_NE(decoder, nullptr);
            const auto initialReads = source->reads.load();
            std::uint64_t delivered = 0;
            for (unsigned attempt = 0; delivered < 48000 && attempt < 1000; ++attempt) {
                ASSERT_TRUE(WaitUntil([&] { return playing->BufferedFrames() != 0 || playing->SourceEnded(); }));
                harken::PCMView output;
                ASSERT_EQ(harken::StreamDecodeResult::Ready, decoder->ReadStream(*playing, 512, 0, output));
                ASSERT_GT(output.frames, 0u);
                for (unsigned i = 0; i < output.frames; ++i)
                    ASSERT_FLOAT_EQ(fixture.reference[delivered + i], static_cast<const float*>(output.channels[0])[i]);
                playing->DeliverFrames(static_cast<std::uint32_t>(output.frames));
                delivered += output.frames;
            }
            EXPECT_GE(delivered, 48000u);
            EXPECT_GT(source->reads.load(), initialReads);
            EXPECT_EQ(0u, paused->readFrames.load());
            EXPECT_EQ(initialPausedBytes, paused->ReadAvailableBytes());
            EXPECT_EQ(VN_OK, paused->preparationResult.load());
            decoder->Retire(); codec.states.Collect();
            playing->state.store(harken::StreamSlotState::Retired, std::memory_order_release);
        }
        paused->state.store(harken::StreamSlotState::Retired, std::memory_order_release);
        streams.SyncServe();
        EXPECT_EQ(2u, streams.FreeSlotCount());
    }
    auto changedSource = std::make_shared<ControlledSource>();
    changedSource->bytes = source->bytes;
    changedSource->bytes.back() ^= 1;
    harken::PackMediaRange changed = *range;
    changed.source = changedSource;
    EXPECT_EQ(harken::MediaContentMatch::Equal, harken::MatchMediaContent({}, range.get(), {}, range.get()));
    EXPECT_EQ(harken::MediaContentMatch::Different, harken::MatchMediaContent({}, range.get(), {}, &changed));
    EXPECT_EQ(0u, changedSource->reads.load());
}

namespace {
    struct ExternalVorbisCounts {
        std::atomic<unsigned> opens{0}, closes{0}, wrongThread{0}, reads{0};
        std::atomic<std::uint64_t> furthestRead{0}, largestRead{0};
        std::atomic<std::uint64_t> firstPayloadRead{UINT64_MAX};
    };
    struct ExternalVorbisLocator final : harken::StreamByteLocator {
        struct Source final : harken::StreamByteSource {
            std::shared_ptr<ExternalVorbisCounts> counts;
            std::shared_ptr<const std::vector<std::uint8_t>> bytes;
            std::thread::id caller;
            std::uint64_t failOffset = UINT64_MAX;
            std::uint64_t payloadOffset = UINT64_MAX;
            ~Source() override {
                if (std::this_thread::get_id() == caller) ++counts->wrongThread;
                ++counts->closes;
            }
            std::uint64_t ByteCount() const noexcept override { return bytes->size(); }
            harken::StreamReadResult ReadAt(std::uint64_t offset, void* data, std::size_t count,
                harken::StreamReadCancellation cancel) const override {
                if (std::this_thread::get_id() == caller) ++counts->wrongThread;
                ++counts->reads;
                if (offset >= payloadOffset) {
                    auto unset = UINT64_MAX;
                    counts->firstPayloadRead.compare_exchange_strong(unset, offset);
                }
                const auto recordMax = [](auto& value, std::uint64_t next) {
                    auto prior = value.load();
                    while (prior < next && !value.compare_exchange_weak(prior, next)) {}
                };
                recordMax(counts->largestRead, count);
                recordMax(counts->furthestRead, offset + count);
                if (cancel.IsCancelled()) return {harken::StreamReadStatus::Cancelled, 0};
                if (offset == failOffset) return {};
                if (offset > bytes->size() || count > bytes->size() - offset) return {};
                std::memcpy(data, bytes->data() + offset, count);
                return {harken::StreamReadStatus::Ok, count};
            }
        };
        std::shared_ptr<ExternalVorbisCounts> counts = std::make_shared<ExternalVorbisCounts>();
        std::shared_ptr<const std::vector<std::uint8_t>> bytes;
        std::thread::id caller = std::this_thread::get_id();
        std::uint64_t failOffset = UINT64_MAX;
        std::uint64_t payloadOffset = UINT64_MAX;
        mutable std::atomic<bool> blockNextOpen{false}, enteredOpen{false}, releaseOpen{false};
        std::shared_ptr<const harken::StreamByteSource> Open(harken::StreamReadCancellation cancel) const override {
            if (std::this_thread::get_id() == caller) ++counts->wrongThread;
            ++counts->opens;
            if (blockNextOpen.exchange(false)) {
                enteredOpen.store(true, std::memory_order_release);
                while (!cancel.IsCancelled() && !releaseOpen.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!bytes || cancel.IsCancelled()) return {};
            auto source = std::make_shared<Source>();
            source->counts = counts; source->bytes = bytes; source->caller = caller;
            source->failOffset = failOffset;
            source->payloadOffset = payloadOffset;
            return source;
        }
    };
}

TEST(StreamRegister, ExternalVorbisOpensOnDemandClosesAfterReferencesAndValidatesEveryReopen) {
    using Locator = ExternalVorbisLocator;
    const auto fixture = harken::test::MakeVorbisFixture(2, true, 257, 1023);
    const auto image = harken::test::WrapVorbisStream(fixture.media);
    harken::VorbisMedia media;
    ASSERT_TRUE(harken::ParseVorbisTestMedia(fixture.media.data(), fixture.media.size(), media));
    harken::VorbisCodec codec(2 * 1024 * 1024, 0);
    harken::VorbisPrepareResult prepared;
    std::vector<std::uint8_t> prefix(fixture.media.begin(), fixture.media.begin() + media.packetDataOffset);
    auto encoded = harken::VorbisStreamMedia::Prepare(codec, std::move(prefix), fixture.media.size(),
        4 * 1024 * 1024, prepared);
    ASSERT_NE(encoded, nullptr);
    auto locator = std::make_shared<Locator>();
    locator->bytes = std::make_shared<const std::vector<std::uint8_t>>(image);
    auto external = std::make_shared<harken::PackExternalStream>();
    external->locator = locator; external->fileBytes = image.size();
    auto range = std::make_shared<harken::PackMediaRange>();
    range->external = external; range->vorbis = encoded;
    range->blobOffset = harken::vorbis_test_wire::kStreamHeaderBytes;
    range->sampleRate = media.sampleRate; range->channelCount = media.channels;
    range->frameCount = media.frames; range->contentRevision = media.revision;
    range->loop = {media.loopStart, media.loopEnd};
    harken::PCMRegistry registry;
    const auto sound = harkentest::ObjectId(17509);
    harken::PCMRegistry::PreparedPackSamples samples;
    ASSERT_EQ(VN_OK, registry.PreparePackRangeSamples(sound, range, &samples));
    harken::PCMRegistry::PackReferenceRegistration registration;
    registration.soundId = sound; registration.samples = &samples;
    harken::PCMRegistry::PreparedPackMutation mutation;
    ASSERT_EQ(VN_OK, registry.PreparePackMutation(&registration, 1, 0, &mutation));
    registry.CommitPreparedPackMutation(&mutation);
    const auto reference = registry.ReferenceOf(sound, registry.CookieOf(sound));
    ASSERT_TRUE(reference.Valid());
    EXPECT_EQ(range, reference.Range());
    EXPECT_FALSE(reference.View().Valid());
    harken::PCMRegistry::PreparedResidentUpgrade upgrade;
    EXPECT_EQ(VN_CAPABILITY_UNAVAILABLE, registry.PrepareResidentUpgrade(reference, &upgrade));
    EXPECT_FALSE(upgrade.IsPrepared());
    harken::EngineConfig config;
    config.maxStreams = 2; config.streamRingBytes = 1024;
    harken::StreamManager streams(config);
    harken::StreamManager::StreamFacts facts;
    ASSERT_EQ(VN_OK, streams.ResolvePackSource(range, &facts));
    EXPECT_EQ(0u, locator->counts->opens.load());
    EXPECT_EQ(0u, locator->counts->reads.load());
    ASSERT_TRUE(streams.AcquirePendingRef(facts.fileIndex));
    for (unsigned slot = 0; slot < 2; ++slot) {
        ASSERT_TRUE(streams.PrepareSlotWorkingSet(slot, media.channels));
        ASSERT_EQ(VN_OK, streams.ActivateSlot(slot, facts.fileIndex, 0, 0, media.frames, 1));
        ASSERT_GT(streams.RingOf(slot)->BufferedFrames(), 0u);
    }
    EXPECT_EQ(1u, locator->counts->opens.load());
    EXPECT_LT(locator->counts->furthestRead.load(), image.size());
    EXPECT_LE(locator->counts->largestRead.load(), 16384u);
    for (unsigned slot = 0; slot < 2; ++slot)
        streams.RingOf(slot)->state.store(harken::StreamSlotState::Retired, std::memory_order_release);
    streams.SyncServe();
    EXPECT_EQ(2u, streams.FreeSlotCount());
    EXPECT_EQ(0u, locator->counts->closes.load());
    streams.ReleasePendingRef(facts.fileIndex);
    ASSERT_TRUE(WaitUntil([&] { return locator->counts->closes.load() == 1; }));
    for (unsigned variant = 0; variant < 7; ++variant) {
        SCOPED_TRACE(variant);
        auto changed = image;
        if (variant == 2) changed.pop_back();
        if (variant == 3) changed[range->blobOffset + 92] ^= 1;
        if (variant == 4) changed[range->blobOffset + media.packetDataOffset + 2] ^= 1;
        locator->bytes = variant == 1 ? nullptr : std::make_shared<const std::vector<std::uint8_t>>(std::move(changed));
        locator->failOffset = variant == 5 ? 0 : variant == 6 ? range->blobOffset : UINT64_MAX;
        harken::StreamManager::StreamFacts again;
        ASSERT_EQ(VN_OK, streams.ResolvePackSource(range, &again));
        EXPECT_EQ(facts.fileIndex, again.fileIndex);
        EXPECT_EQ(facts.sourceGeneration, again.sourceGeneration);
        const auto closedBefore = locator->counts->closes.load();
        ASSERT_TRUE(streams.PrepareSlotWorkingSet(0, media.channels));
        ASSERT_EQ(VN_OK, streams.ActivateSlot(0, facts.fileIndex, 0, 0, media.frames, 1));
        auto* ring = streams.RingOf(0);
        EXPECT_EQ(variant == 0 ? harken::StreamTerminalReason::None :
            harken::StreamTerminalReason::IntegrityAbort, ring->TerminalReason());
        if (variant == 0) EXPECT_GT(ring->BufferedFrames(), 0u);
        else EXPECT_EQ(0u, ring->BufferedFrames());
        if (variant >= 3) {
            const auto observation = harken::detail::StreamManagerTestAccess::Observation(streams, 0);
            EXPECT_EQ(std::uint64_t(ring->generation.load()) << 32, observation.ringToken);
            EXPECT_EQ(facts.sourceGeneration, observation.sourceGeneration);
            EXPECT_GT(observation.readSequence, 0u);
            EXPECT_EQ(variant >= 5 ? 2u : 3u, observation.readStatus);
        }
        ring->state.store(harken::StreamSlotState::Retired, std::memory_order_release);
        streams.SyncServe();
        ASSERT_TRUE(WaitUntil([&] { return locator->counts->closes.load() == closedBefore + (variant != 1); }));
    }
    EXPECT_EQ(8u, locator->counts->opens.load());
    locator->failOffset = UINT64_MAX;
    locator->bytes = std::make_shared<const std::vector<std::uint8_t>>(image);
    locator->blockNextOpen.store(true);
    harken::StreamManager::PreparedToken cancelled, survivor;
    auto request = ReplacementAt(facts);
    request.prebufferTargetFrames = 2;
    ASSERT_EQ(VN_OK, streams.BeginReplacement(request, &cancelled));
    ASSERT_TRUE(WaitUntil([&] { return locator->enteredOpen.load(std::memory_order_acquire); }));
    ASSERT_EQ(VN_OK, streams.BeginReplacement(request, &survivor));
    ASSERT_TRUE(streams.CancelReplacement(cancelled));
    ASSERT_TRUE(WaitUntil([&] { return streams.QueryReplacement(survivor) ==
        harken::StreamManager::PreparationState::Ready; }));
    const auto closedBefore = locator->counts->closes.load();
    harken::StreamManager::PreparedStream owned;
    ASSERT_EQ(VN_OK, streams.TakeReplacement(survivor, &owned));
    EXPECT_GT(owned.ring->BufferedFrames(), 0u);
    ASSERT_TRUE(streams.RetirePreparedSlot(owned.ring, owned.ringToken));
    streams.SyncServe();
    ASSERT_TRUE(WaitUntil([&] { return locator->counts->closes.load() == closedBefore + 1; }));
    EXPECT_EQ(10u, locator->counts->opens.load());
    EXPECT_EQ(2u, streams.FreeSlotCount());
    EXPECT_EQ(0u, locator->counts->wrongThread.load());
}

TEST(StreamRegister, ExternalVorbisDiscoversOnlyMetadataAndSharesPendingPreparation) {
    const auto fixture = harken::test::MakeVorbisFixture(2, true, 257, 1023);
    const auto image = harken::test::WrapVorbisStream(fixture.media);
    harken::VorbisMedia media;
    ASSERT_TRUE(harken::ParseVorbisTestMedia(fixture.media.data(), fixture.media.size(), media));
    harken::VorbisCodec codec(2 * 1024 * 1024, 0);
    harken::EngineConfig config;
    config.maxStreams = 2; config.streamRingBytes = 16384;
    harken::StreamManager streams(config);
    using State = harken::StreamManager::PreparationState;
    for (unsigned variant = 0; variant < 8; ++variant) {
        SCOPED_TRACE(variant);
        auto locator = std::make_shared<ExternalVorbisLocator>();
        auto bytes = image;
        if (variant == 2) bytes[0] ^= 1;
        locator->bytes = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
        if (variant == 3) locator->failOffset = harken::vorbis_test_wire::kStreamHeaderBytes;
        auto external = std::make_shared<harken::PackExternalStream>();
        external->locator = locator; external->fileBytes = image.size();
        auto range = std::make_shared<harken::PackMediaRange>();
        range->external = external; range->blobOffset = harken::vorbis_test_wire::kStreamHeaderBytes;
        range->sampleRate = media.sampleRate; range->channelCount = media.channels;
        range->frameCount = media.frames; range->contentRevision = media.revision + (variant == 4);
        range->loop = {media.loopStart, media.loopEnd};
        harken::StreamManager::StreamFacts facts;
        ASSERT_EQ(VN_OK, streams.ResolvePackSource(range, &facts));
        EXPECT_TRUE(facts.encodedPending);
        EXPECT_EQ(nullptr, facts.encoded);
        EXPECT_EQ(0u, locator->counts->opens.load());
        ASSERT_TRUE(streams.AcquirePendingRef(facts.fileIndex));
        ASSERT_TRUE(streams.AcquirePendingRef(facts.fileIndex));
        locator->blockNextOpen.store(variant >= 6);
        const std::size_t budget = variant == 1 ? 1 : variant == 5 ? media.packetDataOffset : 4 * 1024 * 1024;
        VnResult error = VN_OK;
        const auto poll = [&] { return streams.PreparePackSource(facts.fileIndex, facts.sourceGeneration, codec, budget, &error); };
        ASSERT_EQ(State::Preparing, poll());
        if (variant >= 6) {
            ASSERT_TRUE(WaitUntil([&] { return locator->enteredOpen.load(std::memory_order_acquire); }));
            streams.ReleasePendingRef(facts.fileIndex);
            EXPECT_EQ(State::Preparing, poll());
            EXPECT_EQ(1u, locator->counts->opens.load());
            if (variant == 7) {
                streams.ReleasePendingRef(facts.fileIndex);
                ASSERT_TRUE(WaitUntil([&] {
                    return harken::detail::StreamManagerTestAccess::MetadataRequests(streams) == 0;
                }));
                EXPECT_EQ(0u, locator->counts->closes.load());
                continue;
            }
            locator->releaseOpen.store(true, std::memory_order_release);
        } else streams.ReleasePendingRef(facts.fileIndex);
        State result = State::Preparing;
        ASSERT_TRUE(WaitUntil([&] { result = poll(); return result != State::Preparing; }));
        const bool valid = variant == 0 || variant == 6;
        ASSERT_EQ(valid ? State::Ready : State::Failed, result);
        EXPECT_EQ(valid ? VN_OK : variant == 1 || variant == 5 ? VN_BACKPRESSURE : VN_BAD_STREAM_FILE, error);
        EXPECT_LE(locator->counts->largestRead.load(), 16384u);
        EXPECT_LE(locator->counts->furthestRead.load(), range->blobOffset + media.packetDataOffset);
        EXPECT_EQ(1u, locator->counts->opens.load());
        ASSERT_TRUE(streams.FactsForFileIndex(facts.fileIndex, &facts));
        EXPECT_EQ(!valid, facts.encodedPending);
        if (valid) {
            ASSERT_NE(nullptr, facts.encoded);
            EXPECT_EQ(range->blobOffset + media.packetDataOffset, locator->counts->furthestRead.load());
            ASSERT_TRUE(streams.PrepareSlotWorkingSet(0, media.channels));
            ASSERT_EQ(VN_OK, streams.ActivateSlot(0, facts.fileIndex, 0, 0, media.frames, 1));
            auto* ring = streams.RingOf(0);
            auto* decoder = facts.encoded->Acquire();
            ASSERT_NE(nullptr, decoder);
            harken::PCMView output;
            ASSERT_EQ(harken::StreamDecodeResult::Ready, decoder->ReadStream(*ring, 128, 0, output));
            ASSERT_GT(output.frames, 0u);
            for (unsigned ch = 0; ch < output.channelCount; ++ch)
                for (unsigned i = 0; i < output.frames; ++i)
                    EXPECT_FLOAT_EQ(fixture.reference[i * media.channels + ch], static_cast<const float*>(output.channels[ch])[i]);
            decoder->Retire(); codec.states.Collect();
            ring->state.store(harken::StreamSlotState::Retired, std::memory_order_release);
            streams.SyncServe();
        }
        if (variant == 1) {
            const auto entries = harken::StreamWorkerStallEntriesForTests();
            harken::SetStreamWorkerStallReleaseForTests(false);
            harken::SetStreamRequestFaultForTests(harken::StreamRequestFault::WorkerStall);
            const bool stalled = WaitUntil([&] { return harken::StreamWorkerStallEntriesForTests() > entries; });
            streams.ReleasePendingRef(facts.fileIndex);
            ASSERT_TRUE(streams.AcquirePendingRef(facts.fileIndex));
            EXPECT_EQ(VN_OK, harken::detail::StreamManagerTestAccess::MetadataResult(streams, facts.fileIndex));
            harken::SetStreamRequestFaultForTests(harken::StreamRequestFault::None);
            harken::SetStreamWorkerStallReleaseForTests(true);
            ASSERT_TRUE(stalled);
            State retry = State::Preparing;
            ASSERT_TRUE(WaitUntil([&] {
                retry = streams.PreparePackSource(facts.fileIndex, facts.sourceGeneration, codec, 4 * 1024 * 1024, &error);
                return retry != State::Preparing;
            }));
            EXPECT_EQ(State::Ready, retry);
            EXPECT_EQ(VN_OK, error);
            streams.ReleasePendingRef(facts.fileIndex);
        } else streams.ReleasePendingRef(facts.fileIndex);
        ASSERT_TRUE(WaitUntil([&] { return locator->counts->closes.load() == (variant == 1 ? 2u : 1u); }));
        EXPECT_EQ(0u, locator->counts->wrongThread.load());
    }
}

TEST(StreamRegister, LazyVorbisDecoderTransfersOnceAndRetiresAcrossCancellationAndReuse) {
    const auto fixture = harken::test::MakeVorbisFixture(1, true);
    const auto image = harken::test::WrapVorbisStream(fixture.media);
    harken::VorbisMedia media;
    ASSERT_TRUE(harken::ParseVorbisTestMedia(fixture.media.data(), fixture.media.size(), media));
    auto config = harkentest::MakeNullConfig(2, 256, 48000);
    config.maxTriggerDepth = 2; config.maxStreams = 2; config.renderWorkerCount = 0;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    using State = harken::StreamManager::PreparationState;
    for (unsigned variant = 0; variant < 4; ++variant) {
        SCOPED_TRACE(variant);
        auto locator = std::make_shared<ExternalVorbisLocator>();
        locator->bytes = std::make_shared<const std::vector<std::uint8_t>>(image);
        auto external = std::make_shared<harken::PackExternalStream>();
        external->locator = locator; external->fileBytes = image.size();
        auto range = std::make_shared<harken::PackMediaRange>();
        range->external = external; range->blobOffset = harken::vorbis_test_wire::kStreamHeaderBytes;
        range->sampleRate = media.sampleRate; range->channelCount = media.channels;
        range->frameCount = media.frames; range->contentRevision = media.revision;
        harken::StreamManager::StreamFacts facts;
        ASSERT_EQ(VN_OK, engine.streams.ResolvePackSource(range, &facts));
        auto request = ReplacementAt(facts);
        request.prebufferTargetFrames = 256;
        harken::StreamManager::PreparedToken token;
        ASSERT_EQ(VN_OK, engine.streams.BeginReplacement(request, &token));
        harken::StreamManager::PreparedStream owned;
        ASSERT_EQ(VN_OK, engine.streams.TransferReplacement(token, &owned));
        ASSERT_TRUE(owned.ring->packetEncoded);
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, engine.voices.PlayStream(owned.ring, 1, 48000, 256, 0, 1,
            0, false, 0, 0, 0, harken::ResolvePanGains(config.mixChannelLayout, 0),
            0, media.frames, 0, media.frames, 1, harken::SelectResampleProfile(48000, 48000), &voice));
        ASSERT_TRUE(engine.voices.BindStreamRingToken(voice, owned.ring, owned.ringToken));
        engine.voices.ApplyPlay(voice, voice, 0, engine.mixer.BlocksRendered());
        engine.nullBackend.Pump(1);
        ASSERT_NE(nullptr, engine.voices.Resolve(voice));
        EXPECT_EQ(0u, owned.ring->readFrames.load());
        EXPECT_EQ(harken::VariableReadCutStatus::Waiting, engine.voices.TimelineStreamStartStatus(
            static_cast<std::uint32_t>(voice), static_cast<std::uint32_t>(voice >> 32)));
        if (variant == 1) {
            locator->blockNextOpen.store(true);
            engine.streams.ServicePackSources(engine.vorbis, 4 * 1024 * 1024);
            ASSERT_TRUE(WaitUntil([&] { return locator->enteredOpen.load(std::memory_order_acquire); }));
        } else {
            ASSERT_TRUE(WaitUntil([&] {
                engine.streams.ServicePackSources(engine.vorbis, variant == 3 ? 1 : 4 * 1024 * 1024);
                engine.streams.ServiceTransferredPreparations();
                const auto status = engine.voices.TimelineStreamStartStatus(
                    static_cast<std::uint32_t>(voice), static_cast<std::uint32_t>(voice >> 32));
                return status != harken::VariableReadCutStatus::Waiting;
            }));
            if (variant == 3) EXPECT_EQ(VN_BACKPRESSURE, owned.ring->preparationResult.load());
            else {
                ASSERT_NE(nullptr, owned.ring->pendingDecoder.load());
                EXPECT_GT(engine.vorbis.states.ActiveBytes(), 0u);
                const auto preparedBytes = engine.vorbis.states.ActiveBytes();
                for (unsigned i = 0; i < 3; ++i) engine.streams.ServicePackSources(engine.vorbis, 4 * 1024 * 1024);
                EXPECT_EQ(preparedBytes, engine.vorbis.states.ActiveBytes());
            }
        }
        if (variant == 0) {
            engine.nullBackend.Pump(1);
            auto* rendered = engine.voices.Resolve(voice);
            ASSERT_NE(nullptr, rendered);
            EXPECT_NE(nullptr, rendered->mediaDecoder);
            EXPECT_EQ(nullptr, owned.ring->pendingDecoder.load());
            EXPECT_GT(owned.ring->readFrames.load(), 0u);
        }
        engine.voices.ApplyStop(voice, 0, 0, engine.notifications, engine.mixer.BlocksRendered());
        engine.nullBackend.Pump(1);
        ASSERT_TRUE(WaitUntil([&] {
            engine.streams.ReclaimAfterWorkerFailure();
            engine.vorbis.states.Collect();
            return engine.streams.FreeSlotCount() == 2 && engine.vorbis.states.ActiveBytes() == 0 &&
                harken::detail::StreamManagerTestAccess::MetadataRequests(engine.streams) == 0;
        }));
        EXPECT_EQ(nullptr, owned.ring->pendingDecoder.load());
        if (variant != 0) EXPECT_EQ(0u, owned.ring->readFrames.load());
        EXPECT_EQ(0u, harken::memguard::HitCount());
    }
}

TEST(StreamRegister, VorbisPrefetchStartsBeforeOpenAndVerifiesAcrossItsPacketBoundary) {
    const auto fixture = harken::test::MakeVorbisFixture(1, true, 257, 8191);
    const auto image = harken::test::WrapVorbisStream(fixture.media);
    harken::VorbisMedia media;
    ASSERT_TRUE(harken::ParseVorbisTestMedia(fixture.media.data(), fixture.media.size(), media));
    ASSERT_GT(media.seek.size(), 2u);
    const std::uint8_t* packet = nullptr;
    std::uint16_t count = 0;
    ASSERT_TRUE(media.Packet(media.seek[1].packetOffset, packet, count));
    const auto cut = media.seek[1].packetOffset + count + 2;
    ASSERT_LT(cut, media.seek[2].packetOffset);
    const auto prefixBytes = media.packetDataOffset + cut;
    const std::vector<std::uint8_t> prefix(fixture.media.begin(), fixture.media.begin() + prefixBytes);
    harken::VorbisCodec codec(2 * 1024 * 1024, 0);
    harken::VorbisPrepareResult prepared;
    EXPECT_EQ(nullptr, harken::VorbisStreamMedia::Prepare(codec, prefix, fixture.media.size(),
        4 * 1024 * 1024, prepared));
    EXPECT_EQ(harken::VorbisPrepareResult::Invalid, prepared);
    auto incomplete = prefix;
    incomplete.pop_back();
    EXPECT_EQ(nullptr, harken::VorbisStreamMedia::Prepare(codec, incomplete, fixture.media.size(),
        4 * 1024 * 1024, prepared, true));
    auto encoded = harken::VorbisStreamMedia::Prepare(codec, prefix, fixture.media.size(),
        4 * 1024 * 1024, prepared, true);
    ASSERT_NE(nullptr, encoded);
    EXPECT_GE(encoded->ResidentBytes(), prefixBytes);
    harken::EngineConfig config;
    config.maxStreams = 2; config.streamRingBytes = 32768;
    harken::StreamManager streams(config);
    for (bool corrupt : {false, true}) {
        SCOPED_TRACE(corrupt);
        auto locator = std::make_shared<ExternalVorbisLocator>();
        auto bytes = image;
        if (corrupt) bytes[harken::vorbis_test_wire::kStreamHeaderBytes + prefixBytes + 2] ^= 1;
        locator->bytes = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
        locator->payloadOffset = harken::vorbis_test_wire::kStreamHeaderBytes + media.packetDataOffset;
        locator->blockNextOpen.store(true);
        auto external = std::make_shared<harken::PackExternalStream>();
        external->locator = locator; external->fileBytes = image.size();
        auto range = std::make_shared<harken::PackMediaRange>();
        range->external = external; range->vorbis = encoded;
        range->blobOffset = harken::vorbis_test_wire::kStreamHeaderBytes;
        range->sampleRate = media.sampleRate; range->channelCount = media.channels;
        range->frameCount = media.frames; range->contentRevision = media.revision;
        range->loop = {media.loopStart, media.loopEnd};
        harken::StreamManager::StreamFacts facts;
        ASSERT_EQ(VN_OK, streams.ResolvePackSource(range, &facts));
        EXPECT_EQ(0u, locator->counts->opens.load());
        auto request = ReplacementAt(facts);
        request.prebufferTargetFrames = 48000;
        harken::StreamManager::PreparedToken token;
        ASSERT_EQ(VN_OK, streams.BeginReplacement(request, &token));
        EXPECT_EQ(harken::StreamManager::PreparationState::Ready, streams.QueryReplacement(token));
        harken::StreamManager::PreparedStream owned;
        ASSERT_EQ(VN_OK, streams.TransferReplacement(token, &owned));
        ASSERT_TRUE(owned.ring->startPromoted.load());
        auto* decoder = encoded->Acquire();
        ASSERT_NE(nullptr, decoder);
        harken::PCMView output;
        ASSERT_EQ(harken::StreamDecodeResult::Ready, decoder->ReadStream(*owned.ring, 256, 0, output));
        ASSERT_GT(output.frames, 0u);
        std::uint64_t delivered = 0;
        const auto verify = [&] {
            for (unsigned i = 0; i < output.frames; ++i)
                EXPECT_FLOAT_EQ(fixture.reference[delivered + i], static_cast<const float*>(output.channels[0])[i]);
            owned.ring->DeliverFrames(static_cast<std::uint32_t>(output.frames));
            delivered += output.frames;
        };
        verify();
        EXPECT_EQ(UINT64_MAX, locator->counts->firstPayloadRead.load());
        locator->releaseOpen.store(true, std::memory_order_release);
        if (corrupt) {
            ASSERT_TRUE(WaitUntil([&] { return owned.ring->TerminalReason() == harken::StreamTerminalReason::IntegrityAbort; }));
        } else {
            while (delivered < 16000) {
                ASSERT_TRUE(WaitUntil([&] { return owned.ring->BufferedFrames() > 0 || owned.ring->SourceEnded(); }));
                ASSERT_EQ(harken::StreamDecodeResult::Ready, decoder->ReadStream(*owned.ring, 256, 0, output));
                ASSERT_GT(output.frames, 0u);
                verify();
            }
        }
        EXPECT_EQ(range->blobOffset + prefixBytes, locator->counts->firstPayloadRead.load());
        decoder->Retire(); codec.states.Collect();
        ASSERT_TRUE(streams.RetirePreparedSlot(owned.ring, owned.ringToken));
        streams.SyncServe();
        ASSERT_TRUE(WaitUntil([&] { return locator->counts->closes.load() == 1; }));
        EXPECT_EQ(0u, locator->counts->wrongThread.load());
    }
}
