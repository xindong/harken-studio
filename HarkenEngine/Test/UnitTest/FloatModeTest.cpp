#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/render/VoicePool.h"

#include <gtest/gtest.h>

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) ||              \
    defined(__x86_64__)
#include <pmmintrin.h>
#include <xmmintrin.h>
#endif

namespace {

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) ||              \
    defined(__x86_64__)

constexpr unsigned int kDenormalModeMask =
    _MM_FLUSH_ZERO_MASK | _MM_DENORMALS_ZERO_MASK;

class MxcsrRestore final {
public:
    explicit MxcsrRestore(unsigned int value) noexcept : m_value(value) {}
    ~MxcsrRestore() { _mm_setcsr(m_value); }

    MxcsrRestore(const MxcsrRestore&) = delete;
    MxcsrRestore& operator=(const MxcsrRestore&) = delete;

private:
    unsigned int m_value;
};

struct RenderFloatModeObservation {
    unsigned int mxcsr = 0;
    bool called = false;
};

void ObserveRenderFloatMode(void* state, float* const* /*channels*/,
                            std::uint32_t /*channelCount*/,
                            std::uint32_t /*offset*/,
                            std::uint32_t /*frames*/) {
    auto* observation = static_cast<RenderFloatModeObservation*>(state);
    observation->mxcsr = _mm_getcsr();
    observation->called = true;
}

std::uint32_t NoEffectTail(const void* /*state*/) {
    return 0;
}

TEST(FloatMode, NullRenderEnablesFtzDazAndRestoresCallerState) {
    constexpr std::uint32_t kBlockFrames = 64;

    harkentest::EngineSession session(
        harkentest::MakeNullConfig(1, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> signal =
        harkentest::MakeDC(kBlockFrames, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(signal, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));

    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    harken::VoiceRenderContext* voice = session.Engine()->voices.Resolve(
        harkentest::RenderHandle(session.Engine(), handle));
    ASSERT_NE(nullptr, voice);

    RenderFloatModeObservation observation;
    voice->fxSlots[0].process = &ObserveRenderFloatMode;
    voice->fxSlots[0].tailFrames = &NoEffectTail;
    voice->fxSlots[0].state = &observation;

    const unsigned int originalMxcsr = _mm_getcsr();
    MxcsrRestore restore(originalMxcsr);
    const unsigned int callerMxcsr = originalMxcsr & ~kDenormalModeMask;
    _mm_setcsr(callerMxcsr);

    session.Engine()->nullBackend.Pump(1);

    ASSERT_TRUE(observation.called);
    EXPECT_EQ(kDenormalModeMask, observation.mxcsr & kDenormalModeMask);
    EXPECT_EQ(callerMxcsr, _mm_getcsr());
}

#endif

} // namespace
