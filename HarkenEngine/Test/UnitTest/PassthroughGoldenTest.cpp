// Golden cases: the insert-effect seam. A voice's chain slots start empty,
// which is a zero-cost identity (no process call); these cases prove that
// default is bit-transparent on a non-silent signal, and that an installed
// slot effect really runs over every produced frame.
//
// Tolerances: identity
// comparisons are exact (tolerance 0) — an empty slot performs no
// arithmetic and this mono-to-mono cascade is the unity-gain identity path.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/render/VoicePool.h"

namespace {

using harkentest::CaptureMulti;
using harkentest::EngineSession;
using harkentest::MakeNullConfigLayout;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlockFrames = 256;

// Counting identity effect: proves invocation without touching audio.
struct CountingState {
    std::uint64_t calls = 0;
    std::uint64_t framesSeen = 0;
};

void CountingProcess(void* state, float* const* /*channels*/,
                     std::uint32_t /*channelCount*/, std::uint32_t /*offset*/,
                     std::uint32_t frames) {
    auto* counting = static_cast<CountingState*>(state);
    counting->calls += 1;
    counting->framesSeen += frames;
}

std::uint32_t CountingTail(const void* /*state*/) {
    return 0;
}

TEST(PassthroughGolden, DefaultPassthroughIsIdentity) {
    EngineSession session(MakeNullConfigLayout(
        0, kBlockFrames, 48000, VN_CHANNEL_LAYOUT_MONO, VN_CHANNEL_LAYOUT_MONO));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> sine =
        harkentest::MakeSine(kBlockFrames, 750.0, 48000.0, 0.7f);
    HARKENTEST_ASSERT_NON_SILENT(sine);
    const VnPCMDesc desc = MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    CaptureMulti out;
    harkentest::PumpAndCaptureMulti(session.Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.channels[0]);

    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        ASSERT_EQ(sine[i], out.channels[0][i]) << "sample " << i;
    }
}

TEST(PassthroughGolden, SeamProcessRunsOverEveryProducedFrame) {
    EngineSession session(MakeNullConfigLayout(
        0, kBlockFrames, 48000, VN_CHANNEL_LAYOUT_MONO, VN_CHANNEL_LAYOUT_MONO));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> sine =
        harkentest::MakeSine(300, 750.0, 48000.0, 0.7f);
    HARKENTEST_ASSERT_NON_SILENT(sine);
    const VnPCMDesc desc = MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &desc));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(2));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    // Install the counting identity into the voice's first chain slot
    // through the internal seam; state lives on this (logic) thread's stack
    // for the test's duration.
    CountingState counting;
    harken::VoiceRenderContext* voice =
        session.Engine()->voices.Resolve(harkentest::RenderHandle(session.Engine(
            ), handle));
    ASSERT_NE(nullptr, voice);
    voice->fxSlots[0].process = CountingProcess;
    voice->fxSlots[0].tailFrames = CountingTail;
    voice->fxSlots[0].state = &counting;

    CaptureMulti out;
    harkentest::PumpAndCaptureMulti(session.Engine(), 2, out);
    HARKENTEST_ASSERT_NON_SILENT(out.channels[0]);

    // Block 1 carries 256 frames, block 2 the remaining 44: one process
    // call per block with data, every produced frame seen, audio identical.
    EXPECT_EQ(2u, counting.calls);
    EXPECT_EQ(sine.size(), counting.framesSeen);
    for (std::size_t i = 0; i < sine.size(); ++i) {
        ASSERT_EQ(sine[i], out.channels[0][i]) << "sample " << i;
    }
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

} // namespace

#if defined(HARKEN_MONITOR_HUB_ABSENT)
TEST(MonitorHubAbsent, TypedEventAndStateRequestsCannotClaimUnavailableProducers) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    for (const auto stream : {VN_MONITOR_STREAM_EVENT, VN_MONITOR_STREAM_STATE}) {
        VnMonitorSessionDescV2 desc{};
        desc.structSize = sizeof(desc);
        desc.requestedExecutionFactVersion = 1;
        desc.requestedStreamKindMask = stream;
        desc.requestedPermissionMask = stream == VN_MONITOR_STREAM_EVENT ?
            VN_MONITOR_PERM_OBSERVE_EVENTS : VN_MONITOR_PERM_OBSERVE_STATE;
        VnMonitorSessionHandle handle = 91;
        EXPECT_EQ(VN_MONITOR_UNSUPPORTED,
            Vn_MonitorOpenSession(reinterpret_cast<const VnMonitorSessionDesc*>(&desc), &handle));
        EXPECT_EQ(91u, handle);
    }
}
#endif
