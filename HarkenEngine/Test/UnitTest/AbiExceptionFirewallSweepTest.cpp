#include "Common/ObjectIdTestSupport.h"
#include "harken/core/LogicAllocCheckpoint.h"
#include "Common/TestUtil.h"

#include <gtest/gtest.h>

namespace {

using harken::detail::ArmLogicAllocFaultForTests;
using harken::detail::DisarmLogicAllocFaultForTests;
using harken::detail::LogicAllocSite;

class AbiExceptionFirewallSweep : public ::testing::Test {
protected:
    void TearDown() override { DisarmLogicAllocFaultForTests(); }
};

struct LogicAllocLogCapture {
    bool sawFailure = false;

    static void Callback(void* userData, uint32_t, uint32_t code,
                         const char*) {
        auto* capture = static_cast<LogicAllocLogCapture*>(userData);
        capture->sawFailure |= code == harken::kLogCodeLogicAllocFailure;
    }
};

TEST_F(AbiExceptionFirewallSweep, DestroyBusDetachesCheckpoint) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));
    VnBindingDesc binding{};
    binding.structSize = sizeof(VnBindingDesc);
    binding.variableId = harkentest::ObjectId(1);
    binding.targetScopeKind = VN_TARGET_SCOPE_BUS;
    binding.targetId = 0;
    binding.curve.structSize = sizeof(VnCurveDesc);
    binding.curve.pointCount = 2;
    binding.curve.points[0] = {0.0f, 0.0f};
    binding.curve.points[1] = {1.0f, -6.0f};
    binding.curve.handles[0] = harkentest::EndpointHandles(
        binding.curve.points[0], binding.curve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &binding));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(VnBindingAttachDesc);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = harkentest::ObjectId(10);
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(1), &attach));
    VnSnapshot before{};
    before.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&before));
    ArmLogicAllocFaultForTests(1, LogicAllocSite::DestroyBusDetaches);
    VnResult result = VN_DEVICE_ERROR;
    EXPECT_NO_THROW(result = Vn_DestroyBus(harkentest::ObjectId(10)));
    EXPECT_EQ(VN_BACKPRESSURE, result);
    VnSnapshot after{};
    after.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&after));
    EXPECT_EQ(before.busCount, after.busCount);
}

TEST_F(AbiExceptionFirewallSweep, AudibilityScratchInitCheckpoints) {
    VnConfig config = harkentest::MakeNullConfig(4, 256, 48000);
    config.maxParkedVoices = 4;
    config.audibilityParkEnabled = 1;
    struct FaultCase { LogicAllocSite site; std::size_t nth; };
    const FaultCase cases[] = {
        {LogicAllocSite::AudibilityParked, 1},
        {LogicAllocSite::AudibilityParked, 2},
    };
    for (const auto& fault : cases) {
        SCOPED_TRACE(static_cast<unsigned int>(fault.site));
        SCOPED_TRACE(fault.nth);
        ArmLogicAllocFaultForTests(fault.nth, fault.site);
        VnResult result = VN_DEVICE_ERROR;
        EXPECT_NO_THROW(result = Vn_Init(&config));
        if (result == VN_OK) Vn_Term();
        ASSERT_EQ(VN_INVALID_CONFIG, result);
        EXPECT_EQ(nullptr, harken::detail::ActiveEngine());
        EXPECT_EQ(VN_NOT_INITIALIZED, Vn_Update());
        DisarmLogicAllocFaultForTests();
        harkentest::EngineSession retry(config);
        ASSERT_EQ(VN_OK, retry.InitResult());
        EXPECT_EQ(VN_OK, Vn_Update());
    }
}

TEST_F(AbiExceptionFirewallSweep, ParkCaptureStorageCheckpoint) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    LogicAllocLogCapture logCapture;
    config.maxParkedVoices = 1;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    config.logCallback = &LogicAllocLogCapture::Callback;
    config.logUserData = &logCapture;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> pcm = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcmDesc = harkentest::MonoDesc(pcm, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(21), &pcmDesc));
    const VnAttenuationDesc attenuation =
        harkentest::MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(22), &attenuation));
    const VnPlayDesc play =
        harkentest::MakePlayDesc3D(harkentest::ObjectId(21), 100.0f, 0.0f, 0.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(22)));
    ASSERT_EQ(VN_OK, Vn_SetListener({0.0f, 0.0f, 0.0f},
                                    {0.0f, 0.0f, -1.0f},
                                    {0.0f, 1.0f, 0.0f}));
    session.Engine()->nullBackend.Pump(1);

    ArmLogicAllocFaultForTests(1, LogicAllocSite::ParkCaptureStorage);
    VnResult result = VN_DEVICE_ERROR;
    EXPECT_NO_THROW(result = Vn_Update());
    EXPECT_EQ(VN_OK, result);
    EXPECT_TRUE(logCapture.sawFailure);
    VnSnapshot skipped{};
    skipped.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&skipped));
    EXPECT_EQ(1u, skipped.activeVoiceCount);
    EXPECT_EQ(0u, skipped.parkedVoiceCount);
    EXPECT_EQ(VN_OK, Vn_SetVoiceVolume(handle, -3.0f, 0));

    DisarmLogicAllocFaultForTests();
    EXPECT_EQ(VN_OK, Vn_Update());
    VnSnapshot parked{};
    parked.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&parked));
    EXPECT_EQ(1u, parked.parkedVoiceCount);
}


TEST_F(AbiExceptionFirewallSweep, VoiceEffectParkedCheckpoint) {
    VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
    LogicAllocLogCapture logCapture;
    config.maxParkedVoices = 2;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    config.logCallback = &LogicAllocLogCapture::Callback;
    config.logUserData = &logCapture;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> pcm = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcmDesc = harkentest::MonoDesc(pcm, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(30), &pcmDesc));
    const VnAttenuationDesc attenuation =
        harkentest::MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(40), &attenuation));
    VnPlayDesc play = harkentest::MakePlayDesc3D(harkentest::ObjectId(30), 100.0f, 0.0f, 0.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(40)));
    ASSERT_EQ(VN_OK, Vn_SetListener({0.0f, 0.0f, 0.0f},
                                    {0.0f, 0.0f, -1.0f},
                                    {0.0f, 1.0f, 0.0f}));
    session.Engine()->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    session.Engine()->nullBackend.Pump(1);
    VnSnapshot before{};
    before.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&before));
    VnGainDesc gain{};
    gain.structSize = sizeof(VnGainDesc);
    gain.gainDb = 0.0f;
    // The park reserve makes this growth site unreachable; keep it armed as a capacity sentinel.
    ArmLogicAllocFaultForTests(1, LogicAllocSite::VoiceEffectParked);
    VnResult result = VN_DEVICE_ERROR;
    EXPECT_NO_THROW(
        result = Vn_SetVoiceEffect(handle, 0, VN_EFFECT_GAIN,
                                   reinterpret_cast<const VnEffectDesc*>(&gain)));
    EXPECT_EQ(VN_OK, result);
    const harken::VoiceLedger::Resolved parked =
        session.Engine()->ledger.Classify(handle);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, parked.kind);
    const harken::ParkRecord& parkRecord =
        session.Engine()->parkRecords[parked.ledgerSlot];
    ASSERT_EQ(1u, parkRecord.effects.size());
    EXPECT_EQ(0u, parkRecord.effects.front().slot);
    EXPECT_EQ(VN_EFFECT_GAIN, parkRecord.effects.front().type);
    VnSnapshot after{};
    after.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&after));
    EXPECT_EQ(before.parkedVoiceCount, after.parkedVoiceCount);
    EXPECT_EQ(before.activeVoiceCount, after.activeVoiceCount);
    DisarmLogicAllocFaultForTests();
    ASSERT_EQ(VN_OK, Vn_SetListener({99.0f, 0.0f, 0.0f},
                                    {0.0f, 0.0f, -1.0f},
                                    {0.0f, 1.0f, 0.0f}));
    // M8 pre-acquires bounded return effects into ParkRecord fixed storage;
    // there is no return-effect allocation checkpoint to arm.
    EXPECT_NO_THROW(result = Vn_Update());
    EXPECT_EQ(VN_OK, result);
    EXPECT_FALSE(logCapture.sawFailure);
    VnSnapshot returned{};
    returned.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&returned));
    EXPECT_EQ(0u, returned.parkedVoiceCount);
}

} // namespace
