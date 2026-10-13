#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "MonitorCaptureHubTestHooks.h"
#include "Common/TestUtil.h"
#include "harken/render/BusTree.h"
#include "harken/render/InsertEffect.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

VnMonitorSessionDesc MakeControlDesc() {
    VnMonitorSessionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.requestedStreamKindMask =
        VN_MONITOR_STREAM_EVENT | VN_MONITOR_STREAM_STATE |
        VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS |
                                   VN_MONITOR_PERM_OBSERVE_STATE |
                                   VN_MONITOR_PERM_OBSERVE_MEASUREMENTS |
                                   VN_MONITOR_PERM_CAPTURE_CONTROL |
                                   VN_MONITOR_PERM_RAW_AUDIO;
    desc.consumerQueueBytes = 65536;
    return desc;
}

VnResult Acquire(VnMonitorSessionHandle handle,
                 VnMonitorCaptureControlStatus* status) {
    status->structSize = sizeof(*status);
    return Vn_MonitorAcquireCaptureControl(handle, status);
}

VnMonitorTapDescriptor MakeMasterTap(std::uint32_t selectorMode) {
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapKind = VN_MONITOR_TAP_MASTER_OUTPUT;
    tap.ownerKind = VN_MONITOR_ENTITY_BUS;
    tap.selectorMode = selectorMode;
    tap.ownerStableId = VN_BUS_MASTER;
    tap.slotId = 0;
    return tap;
}

std::uint32_t ReadU32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t ReadU64(const unsigned char* p) {
    return static_cast<std::uint64_t>(ReadU32(p)) |
           (static_cast<std::uint64_t>(ReadU32(p + 4)) << 32);
}

bool PollUntilAudioChunk(VnMonitorSessionHandle handle,
                         std::vector<unsigned char>* buffer,
                         VnMonitorPollResult* result) {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        buffer->assign(1u << 20, 0);
        VnMonitorPollDesc desc{};
        desc.structSize = sizeof(desc);
        desc.buffer = buffer->data();
        desc.bufferBytes = buffer->size();
        result->structSize = sizeof(*result);
        if (Vn_MonitorPoll(handle, &desc, result) != VN_OK) {
            return false;
        }
        std::uint64_t offset = 0;
        while (offset + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES <=
               result->bytesWritten) {
            const unsigned char* rec = buffer->data() + offset;
            const std::uint32_t kind = ReadU32(rec);
            const std::uint64_t recordBytes = ReadU64(rec + 16);
            if (kind == VN_MONITOR_RECORD_AUDIO_CHUNK) {
                return true;
            }
            if (recordBytes == 0) {
                break;
            }
            offset += recordBytes;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

TEST(MonitorMeasure, RegistryListsFirstBatchKinds) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());

    uint32_t required = 0;
    uint64_t revision = 0;
    ASSERT_EQ(VN_MONITOR_BUFFER_TOO_SMALL,
              Vn_MonitorCaptureRegistry(VN_MONITOR_REGISTRY_TAP, nullptr, 0,
                                        &required, &revision));
    ASSERT_GT(required, 0u);
    std::vector<unsigned char> blob(required);
    ASSERT_EQ(VN_OK, Vn_MonitorCaptureRegistry(VN_MONITOR_REGISTRY_TAP, blob.data(),
                                               required, &required, &revision));
    EXPECT_GT(revision, 0u);
    ASSERT_EQ(0, required % static_cast<uint32_t>(sizeof(VnMonitorTapDescriptor)));
    const auto* taps =
        reinterpret_cast<const VnMonitorTapDescriptor*>(blob.data());
    const uint32_t count =
        required / static_cast<uint32_t>(sizeof(VnMonitorTapDescriptor));
    bool sawMaster = false;
    bool sawDeviceGap = false;
    std::uint32_t kinds = 0;
    for (uint32_t i = 0; i < count; ++i) {
        EXPECT_EQ(sizeof(VnMonitorTapDescriptor), taps[i].structSize);
        kinds |= 1u << taps[i].tapKind;
        if (taps[i].tapKind == VN_MONITOR_TAP_MASTER_OUTPUT) {
            EXPECT_EQ(static_cast<uint32_t>(VN_MONITOR_ENTITY_BUS),
                      taps[i].ownerKind);
            EXPECT_EQ(VN_BUS_MASTER, taps[i].ownerStableId);
            EXPECT_EQ(0u, taps[i].availabilityState);
            sawMaster = true;
        }
        if (taps[i].tapKind == VN_MONITOR_TAP_DEVICE_OUTPUT) {
            EXPECT_NE(0u, taps[i].availabilityState);
            sawDeviceGap = true;
        }
    }
    EXPECT_TRUE(sawMaster);
    EXPECT_TRUE(sawDeviceGap);
    EXPECT_NE(0u, kinds & (1u << VN_MONITOR_TAP_VOICE_PRE_CHAIN));
    EXPECT_NE(0u, kinds & (1u << VN_MONITOR_TAP_VOICE_POST_CHAIN));
    EXPECT_NE(0u, kinds & (1u << VN_MONITOR_TAP_EFFECT_SLOT_INPUT));
    EXPECT_NE(0u, kinds & (1u << VN_MONITOR_TAP_EFFECT_SLOT_OUTPUT));
    EXPECT_NE(0u, kinds & (1u << VN_MONITOR_TAP_BUS_POST_EFFECTS));
    EXPECT_NE(0u, kinds & (1u << VN_MONITOR_TAP_BUS_PRE_FADER));
    EXPECT_NE(0u, kinds & (1u << VN_MONITOR_TAP_BUS_POST_FADER));
}

TEST(MonitorMeasure, RegistryRejectsUnknownKindWithoutInventingRows) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    uint32_t required = 0;
    uint64_t revision = 0;
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED,
              Vn_MonitorCaptureRegistry(99u, nullptr, 0, &required, &revision));
}

TEST(MonitorMeasure, CapturePlanAcceptsExactInstanceMasterTap) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));

    VnMonitorTapDescriptor tap =
        MakeMasterTap(VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE);
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.downsample = 1;
    plan.taps = &tap;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorValidateCapturePlan(handle, lease.leaseGeneration,
                                                   &plan, &accepted));
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    EXPECT_EQ(static_cast<uint32_t>(VN_MONITOR_PLAN_PENDING),
              accepted.activationState);
    harken::monitor::AdvanceExecuteFrameForTests(1);
    VnMonitorEffectiveCapturePlan effective{};
    effective.structSize = sizeof(effective);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 0, &effective));
    EXPECT_EQ(static_cast<uint32_t>(VN_MONITOR_PLAN_ACTIVE),
              effective.activationState);
    EXPECT_EQ(1u, effective.accepted.tapCount);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, CapturePlanRejectsUnknownSelectorWithoutFollowSlotFallback) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorTapDescriptor tap = MakeMasterTap(99u);
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.downsample = 1;
    plan.taps = &tap;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED,
              Vn_MonitorValidateCapturePlan(handle, lease.leaseGeneration, &plan,
                                            &accepted));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, CapturePlanRejectsNonUnityDownsample) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorTapDescriptor tap = MakeMasterTap(0);
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.downsample = 2;
    plan.taps = &tap;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED,
              Vn_MonitorValidateCapturePlan(handle, lease.leaseGeneration, &plan,
                                            &accepted));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, CaptureOffDoesNotChangePassthrough) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorTapDescriptor tap = MakeMasterTap(0);
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.downsample = 1;
    plan.taps = &tap;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    VnMonitorEffectiveCapturePlan stopped{};
    stopped.structSize = sizeof(stopped);
    ASSERT_EQ(VN_OK, Vn_MonitorStopCapture(handle, lease.leaseGeneration, &stopped));
    harken::monitor::AdvanceExecuteFrameForTests(1);

    const std::vector<float> sine =
        harkentest::MakeSine(256, 750.0, 48000.0, 0.7f);
    const VnPCMDesc pcm = harkentest::MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    harkentest::Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    ASSERT_EQ(sine.size(), out.left.size());
    for (std::size_t i = 0; i < sine.size(); ++i) {
        ASSERT_EQ(sine[i], out.left[i])
            << "left sample " << i;
        ASSERT_EQ(sine[i], out.right[i])
            << "right sample " << i;
    }
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, MasterTapPublishesAudioChunk) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorTapDescriptor tap =
        MakeMasterTap(VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE);
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.downsample = 1;
    plan.taps = &tap;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    const std::vector<float> sine =
        harkentest::MakeSine(256, 750.0, 48000.0, 0.7f);
    const VnPCMDesc pcm = harkentest::MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    harkentest::Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    std::vector<unsigned char> buffer;
    VnMonitorPollResult result{};
    ASSERT_TRUE(PollUntilAudioChunk(handle, &buffer, &result));
    bool sawChunk = false;
    std::uint64_t offset = 0;
    while (offset + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES <= result.bytesWritten) {
        const unsigned char* rec = buffer.data() + offset;
        const std::uint32_t kind = ReadU32(rec);
        const std::uint64_t recordBytes = ReadU64(rec + 16);
        if (kind == VN_MONITOR_RECORD_AUDIO_CHUNK) {
            sawChunk = true;
            const auto* chunk = reinterpret_cast<const VnMonitorAudioChunk*>(
                rec + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES);
            EXPECT_EQ(sizeof(VnMonitorAudioChunk), chunk->structSize);
            EXPECT_EQ(0u, chunk->algorithmId);
            EXPECT_EQ(0u, chunk->algorithmVersion);
            EXPECT_EQ(256u, chunk->sourceFrameEnd - chunk->sourceFrameBegin);
            EXPECT_EQ(static_cast<std::uint64_t>(VN_MONITOR_TAP_MASTER_OUTPUT),
                      chunk->tapDescriptorId);
            break;
        }
        if (recordBytes == 0) {
            break;
        }
        offset += recordBytes;
    }
    EXPECT_TRUE(sawChunk);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, ExactInstanceStaleDoesNotBindReplacement) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorTapDescriptor tap =
        MakeMasterTap(VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE);
    tap.ownerGeneration = 99;
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.downsample = 1;
    plan.taps = &tap;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_EQ(VN_MONITOR_STALE_TAP,
              Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                         &accepted));
    session.Engine()->nullBackend.Pump(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::vector<unsigned char> buffer(1u << 20, 0);
    VnMonitorPollDesc pollDesc{};
    pollDesc.structSize = sizeof(pollDesc);
    pollDesc.buffer = buffer.data();
    pollDesc.bufferBytes = buffer.size();
    VnMonitorPollResult result{};
    result.structSize = sizeof(result);
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(handle, &pollDesc, &result));
    bool sawChunk = false;
    std::uint64_t offset = 0;
    while (offset + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES <= result.bytesWritten) {
        const unsigned char* rec = buffer.data() + offset;
        const std::uint32_t kind = ReadU32(rec);
        const std::uint64_t recordBytes = ReadU64(rec + 16);
        if (kind == VN_MONITOR_RECORD_AUDIO_CHUNK) {
            sawChunk = true;
            break;
        }
        if (recordBytes == 0) {
            break;
        }
        offset += recordBytes;
    }
    EXPECT_FALSE(sawChunk);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, CaptureOffStopsAudioChunks) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorTapDescriptor tap = MakeMasterTap(0);
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.downsample = 1;
    plan.taps = &tap;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    session.Engine()->nullBackend.Pump(1);
    std::vector<unsigned char> armedBuffer;
    VnMonitorPollResult armed{};
    ASSERT_TRUE(PollUntilAudioChunk(handle, &armedBuffer, &armed));
    VnMonitorEffectiveCapturePlan stopped{};
    stopped.structSize = sizeof(stopped);
    ASSERT_EQ(VN_OK, Vn_MonitorStopCapture(handle, lease.leaseGeneration, &stopped));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::vector<unsigned char> drain(1u << 20, 0);
    VnMonitorPollDesc drainDesc{};
    drainDesc.structSize = sizeof(drainDesc);
    drainDesc.buffer = drain.data();
    drainDesc.bufferBytes = drain.size();
    VnMonitorPollResult drainResult{};
    drainResult.structSize = sizeof(drainResult);
    (void)Vn_MonitorPoll(handle, &drainDesc, &drainResult);
    session.Engine()->nullBackend.Pump(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::vector<unsigned char> buffer(1u << 20, 0);
    VnMonitorPollDesc pollDesc{};
    pollDesc.structSize = sizeof(pollDesc);
    pollDesc.buffer = buffer.data();
    pollDesc.bufferBytes = buffer.size();
    VnMonitorPollResult result{};
    result.structSize = sizeof(result);
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(handle, &pollDesc, &result));
    bool sawChunk = false;
    std::uint64_t offset = 0;
    while (offset + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES <= result.bytesWritten) {
        const unsigned char* rec = buffer.data() + offset;
        const std::uint32_t kind = ReadU32(rec);
        const std::uint64_t recordBytes = ReadU64(rec + 16);
        if (kind == VN_MONITOR_RECORD_AUDIO_CHUNK) {
            sawChunk = true;
            break;
        }
        if (recordBytes == 0) {
            break;
        }
        offset += recordBytes;
    }
    EXPECT_FALSE(sawChunk);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, RingOverflowDropsProducerNotFakeChunk) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorTapDescriptor tap = MakeMasterTap(0);
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.downsample = 1;
    plan.taps = &tap;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    harken::monitor::StopDrainForTests();
    harken::monitor::FillProducerRingForTests();
    const VnMonitorDropSummary before = harken::monitor::ProducerDropForTests();
    session.Engine()->nullBackend.Pump(1);
    const VnMonitorDropSummary after = harken::monitor::ProducerDropForTests();
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_DROP_SOURCE_PRODUCER),
              after.stage);
    EXPECT_GT(after.recordCount, before.recordCount);
    harken::monitor::StartDrainForTests();
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, FollowSlotEmitsBindingChangedOnOwnerChange) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorTapDescriptor tap =
        MakeMasterTap(VN_MONITOR_TAP_SELECTOR_FOLLOW_SLOT);
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.downsample = 1;
    plan.taps = &tap;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    session.Engine()->nullBackend.Pump(1);
    std::vector<unsigned char> first;
    VnMonitorPollResult armed{};
    ASSERT_TRUE(PollUntilAudioChunk(handle, &first, &armed));
    float left[8] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
    float right[8] = {0.0f};
    float* planes[2] = {left, right};
    harken::monitor::PublishPlanarAudioChunk(
        VN_MONITOR_TAP_MASTER_OUTPUT, 1, 99u, 0, planes, 8, 2, 0, 0,
        48000, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::vector<unsigned char> buffer(1u << 20, 0);
    VnMonitorPollDesc pollDesc{};
    pollDesc.structSize = sizeof(pollDesc);
    pollDesc.buffer = buffer.data();
    pollDesc.bufferBytes = buffer.size();
    VnMonitorPollResult result{};
    result.structSize = sizeof(result);
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(handle, &pollDesc, &result));
    bool sawChanged = false;
    std::uint64_t offset = 0;
    while (offset + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES <= result.bytesWritten) {
        const unsigned char* rec = buffer.data() + offset;
        const std::uint32_t kind = ReadU32(rec);
        const std::uint64_t recordBytes = ReadU64(rec + 16);
        if (kind == VN_MONITOR_RECORD_TAP_BINDING_CHANGED) {
            sawChanged = true;
            EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_TAP_SELECTOR_FOLLOW_SLOT),
                      ReadU32(rec + 248));
            break;
        }
        if (recordBytes == 0) {
            break;
        }
        offset += recordBytes;
    }
    EXPECT_TRUE(sawChanged);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, BusPreFaderTapPublishesAudioChunk) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapKind = VN_MONITOR_TAP_BUS_PRE_FADER;
    tap.ownerKind = VN_MONITOR_ENTITY_BUS;
    tap.selectorMode = VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE;
    tap.ownerStableId = VN_BUS_MASTER;
    tap.ownerInstanceHandle = 1;
    tap.ownerGeneration = session.Engine()->buses.GenerationAt(0);
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.downsample = 1;
    plan.taps = &tap;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    const std::vector<float> sine =
        harkentest::MakeSine(256, 750.0, 48000.0, 0.7f);
    const VnPCMDesc pcm = harkentest::MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(3), &pcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(3));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    harkentest::Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    std::vector<unsigned char> buffer;
    VnMonitorPollResult result{};
    ASSERT_TRUE(PollUntilAudioChunk(handle, &buffer, &result));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, VoicePreChainTapPublishesAudioChunk) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapKind = VN_MONITOR_TAP_VOICE_PRE_CHAIN;
    tap.ownerKind = VN_MONITOR_ENTITY_VOICE;
    tap.selectorMode = VN_MONITOR_TAP_SELECTOR_FOLLOW_SLOT;
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.downsample = 1;
    plan.taps = &tap;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    const std::vector<float> sine =
        harkentest::MakeSine(256, 750.0, 48000.0, 0.7f);
    const VnPCMDesc pcm = harkentest::MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(4), &pcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(4));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    harkentest::Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    std::vector<unsigned char> buffer;
    VnMonitorPollResult result{};
    ASSERT_TRUE(PollUntilAudioChunk(handle, &buffer, &result));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, AudioCopyPathHasNoMutexOrAlloc) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorTapDescriptor tap = MakeMasterTap(0);
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.downsample = 1;
    plan.taps = &tap;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    const std::uint64_t beforeHits = harken::memguard::HitCount();
    const std::uint32_t beforeMutex = harken::monitor::PublishMutexHitsForTests();
    session.Engine()->nullBackend.Pump(1);
    EXPECT_EQ(beforeHits, harken::memguard::HitCount());
    EXPECT_EQ(beforeMutex, harken::monitor::PublishMutexHitsForTests());
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, DeviceTapPlanAcceptsAsGapWithoutProducer) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapKind = VN_MONITOR_TAP_DEVICE_OUTPUT;
    tap.ownerKind = VN_MONITOR_ENTITY_NONE;
    tap.selectorMode = VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE;
    tap.availabilityState = 1;
    tap.reason = VN_MONITOR_UNSUPPORTED;
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.downsample = 1;
    plan.taps = &tap;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    session.Engine()->nullBackend.Pump(1);
    std::vector<unsigned char> buffer(1u << 20, 0);
    VnMonitorPollDesc pollDesc{};
    pollDesc.structSize = sizeof(pollDesc);
    pollDesc.buffer = buffer.data();
    pollDesc.bufferBytes = buffer.size();
    VnMonitorPollResult result{};
    result.structSize = sizeof(result);
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(handle, &pollDesc, &result));
    bool sawChunk = false;
    std::uint64_t offset = 0;
    while (offset + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES <= result.bytesWritten) {
        const unsigned char* rec = buffer.data() + offset;
        const std::uint32_t kind = ReadU32(rec);
        const std::uint64_t recordBytes = ReadU64(rec + 16);
        if (kind == VN_MONITOR_RECORD_AUDIO_CHUNK) {
            sawChunk = true;
            break;
        }
        if (recordBytes == 0) {
            break;
        }
        offset += recordBytes;
    }
    EXPECT_FALSE(sawChunk);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, ExactInstanceZeroGenerationIsRejected) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapKind = VN_MONITOR_TAP_BUS_PRE_FADER;
    tap.ownerKind = VN_MONITOR_ENTITY_BUS;
    tap.selectorMode = VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE;
    tap.ownerStableId = VN_BUS_MASTER;
    tap.ownerInstanceHandle = 1;
    tap.ownerGeneration = 0;
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.downsample = 1;
    plan.taps = &tap;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_MonitorValidateCapturePlan(handle, lease.leaseGeneration, &plan,
                                            &accepted));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, ExactInstanceZeroOwnerIsRejected) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapKind = VN_MONITOR_TAP_BUS_PRE_FADER;
    tap.ownerKind = VN_MONITOR_ENTITY_BUS;
    tap.selectorMode = VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE;
    tap.ownerInstanceHandle = 0;
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.downsample = 1;
    plan.taps = &tap;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_MonitorValidateCapturePlan(handle, lease.leaseGeneration, &plan,
                                            &accepted));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, VoicePreAndPostChunksDifferAfterGain) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorTapDescriptor taps[2]{};
    taps[0].structSize = sizeof(taps[0]);
    taps[0].tapKind = VN_MONITOR_TAP_VOICE_PRE_CHAIN;
    taps[0].ownerKind = VN_MONITOR_ENTITY_VOICE;
    taps[0].selectorMode = VN_MONITOR_TAP_SELECTOR_FOLLOW_SLOT;
    taps[1] = taps[0];
    taps[1].tapKind = VN_MONITOR_TAP_VOICE_POST_CHAIN;
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 2;
    plan.downsample = 1;
    plan.taps = taps;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    const std::vector<float> sine =
        harkentest::MakeSine(256, 750.0, 48000.0, 0.7f);
    const VnPCMDesc pcm = harkentest::MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(5), &pcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(5), -6.0f);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    harkentest::Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::vector<unsigned char> buffer(1u << 20, 0);
    VnMonitorPollDesc pollDesc{};
    pollDesc.structSize = sizeof(pollDesc);
    pollDesc.buffer = buffer.data();
    pollDesc.bufferBytes = buffer.size();
    VnMonitorPollResult result{};
    result.structSize = sizeof(result);
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(handle, &pollDesc, &result));
    const VnMonitorAudioChunk* pre = nullptr;
    const VnMonitorAudioChunk* post = nullptr;
    std::uint64_t offset = 0;
    while (offset + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES <= result.bytesWritten) {
        const unsigned char* rec = buffer.data() + offset;
        const std::uint32_t kind = ReadU32(rec);
        const std::uint64_t recordBytes = ReadU64(rec + 16);
        if (kind == VN_MONITOR_RECORD_AUDIO_CHUNK) {
            const auto* chunk = reinterpret_cast<const VnMonitorAudioChunk*>(
                rec + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES);
            if (chunk->tapDescriptorId == VN_MONITOR_TAP_VOICE_PRE_CHAIN) {
                pre = chunk;
            } else if (chunk->tapDescriptorId == VN_MONITOR_TAP_VOICE_POST_CHAIN) {
                post = chunk;
            }
        }
        if (recordBytes == 0) {
            break;
        }
        offset += recordBytes;
    }
    ASSERT_NE(nullptr, pre);
    ASSERT_NE(nullptr, post);
    EXPECT_EQ(0u, pre->sourceFrameBegin);
    EXPECT_EQ(pre->sourceFrameBegin, post->sourceFrameBegin);
    EXPECT_EQ(256u, pre->sourceFrameEnd - pre->sourceFrameBegin);
    const auto* preSamples = reinterpret_cast<const float*>(
        reinterpret_cast<const unsigned char*>(pre) + sizeof(VnMonitorAudioChunk));
    const auto* postSamples = reinterpret_cast<const float*>(
        reinterpret_cast<const unsigned char*>(post) + sizeof(VnMonitorAudioChunk));
    float prePeak = 0.0f;
    float postPeak = 0.0f;
    for (std::uint32_t i = 0; i < 256; ++i) {
        prePeak = std::max(prePeak, std::abs(preSamples[i]));
        postPeak = std::max(postPeak, std::abs(postSamples[i]));
    }
    EXPECT_GT(prePeak, 0.0f);
    EXPECT_LT(postPeak, prePeak * 0.75f);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, StopCaptureDoesNotReplayOldChunksAsActive) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorTapDescriptor tap = MakeMasterTap(VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE);
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.downsample = 1;
    plan.taps = &tap;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    const std::vector<float> sine =
        harkentest::MakeSine(256, 750.0, 48000.0, 0.7f);
    const VnPCMDesc pcm = harkentest::MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(6), &pcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(6));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    harkentest::Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    std::vector<unsigned char> first;
    VnMonitorPollResult armed{};
    ASSERT_TRUE(PollUntilAudioChunk(handle, &first, &armed));
    VnMonitorEffectiveCapturePlan stopped{};
    stopped.structSize = sizeof(stopped);
    ASSERT_EQ(VN_OK, Vn_MonitorStopCapture(handle, lease.leaseGeneration, &stopped));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    session.Engine()->nullBackend.Pump(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::vector<unsigned char> buffer(1u << 20, 0);
    VnMonitorPollDesc pollDesc{};
    pollDesc.structSize = sizeof(pollDesc);
    pollDesc.buffer = buffer.data();
    pollDesc.bufferBytes = buffer.size();
    VnMonitorPollResult result{};
    result.structSize = sizeof(result);
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(handle, &pollDesc, &result));
    bool sawChunk = false;
    std::uint64_t offset = 0;
    while (offset + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES <= result.bytesWritten) {
        const unsigned char* rec = buffer.data() + offset;
        const std::uint32_t kind = ReadU32(rec);
        const std::uint64_t recordBytes = ReadU64(rec + 16);
        if (kind == VN_MONITOR_RECORD_AUDIO_CHUNK) {
            sawChunk = true;
            break;
        }
        if (recordBytes == 0) {
            break;
        }
        offset += recordBytes;
    }
    EXPECT_FALSE(sawChunk);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, ExactInstanceStaleRejectsUnknownEffect) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapKind = VN_MONITOR_TAP_EFFECT_SLOT_INPUT;
    tap.ownerKind = VN_MONITOR_ENTITY_EFFECT;
    tap.selectorMode = VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE;
    tap.ownerInstanceHandle = 1;
    tap.ownerGeneration = 99;
    tap.slotId = 0;
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.downsample = 1;
    plan.taps = &tap;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_EQ(VN_MONITOR_STALE_TAP,
              Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                         &accepted));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, ExactInstanceStaleRejectsUnknownBus) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapKind = VN_MONITOR_TAP_BUS_PRE_FADER;
    tap.ownerKind = VN_MONITOR_ENTITY_BUS;
    tap.selectorMode = VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE;
    tap.ownerStableId = harkentest::ObjectId(99);
    tap.ownerInstanceHandle = 99;
    tap.ownerGeneration = 1;
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.downsample = 1;
    plan.taps = &tap;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_EQ(VN_MONITOR_STALE_TAP,
              Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                         &accepted));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, ApplyReplacementDoesNotReplayPriorPlanChunks) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorTapDescriptor master = MakeMasterTap(VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE);
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.downsample = 1;
    plan.taps = &master;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    const std::vector<float> sine =
        harkentest::MakeSine(256, 750.0, 48000.0, 0.7f);
    const VnPCMDesc pcm = harkentest::MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(7), &pcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(7));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    harkentest::Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    std::vector<unsigned char> first;
    VnMonitorPollResult armed{};
    ASSERT_TRUE(PollUntilAudioChunk(handle, &first, &armed));
    VnMonitorTapDescriptor voiceTap{};
    voiceTap.structSize = sizeof(voiceTap);
    voiceTap.tapKind = VN_MONITOR_TAP_VOICE_PRE_CHAIN;
    voiceTap.ownerKind = VN_MONITOR_ENTITY_VOICE;
    voiceTap.selectorMode = VN_MONITOR_TAP_SELECTOR_FOLLOW_SLOT;
    plan.taps = &voiceTap;
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    session.Engine()->nullBackend.Pump(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::vector<unsigned char> buffer(1u << 20, 0);
    VnMonitorPollDesc pollDesc{};
    pollDesc.structSize = sizeof(pollDesc);
    pollDesc.buffer = buffer.data();
    pollDesc.bufferBytes = buffer.size();
    VnMonitorPollResult result{};
    result.structSize = sizeof(result);
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(handle, &pollDesc, &result));
    bool sawMaster = false;
    std::uint64_t offset = 0;
    while (offset + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES <= result.bytesWritten) {
        const unsigned char* rec = buffer.data() + offset;
        const std::uint32_t kind = ReadU32(rec);
        const std::uint64_t recordBytes = ReadU64(rec + 16);
        if (kind == VN_MONITOR_RECORD_AUDIO_CHUNK) {
            const auto* chunk = reinterpret_cast<const VnMonitorAudioChunk*>(
                rec + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES);
            if (chunk->tapDescriptorId == VN_MONITOR_TAP_MASTER_OUTPUT) {
                sawMaster = true;
            }
        }
        if (recordBytes == 0) {
            break;
        }
        offset += recordBytes;
    }
    EXPECT_FALSE(sawMaster);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, VoiceExactInstancePublishesPublicHandleThenStalesOnStop) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    const std::vector<float> sine =
        harkentest::MakeSine(2048, 750.0, 48000.0, 0.7f);
    const VnPCMDesc pcm = harkentest::MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(8), &pcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(8));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapKind = VN_MONITOR_TAP_VOICE_PRE_CHAIN;
    tap.ownerKind = VN_MONITOR_ENTITY_VOICE;
    tap.selectorMode = VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE;
    tap.ownerInstanceHandle = voice;
    tap.ownerGeneration = voice >> 32;
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.downsample = 1;
    plan.taps = &tap;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    harkentest::Capture out;
    session.Engine()->nullBackend.Pump(1);
    std::vector<unsigned char> first;
    VnMonitorPollResult armed{};
    ASSERT_TRUE(PollUntilAudioChunk(handle, &first, &armed));
    ASSERT_EQ(VN_OK, Vn_Stop(voice, 0));
    harkentest::PumpAndCapture(session.Engine(), 2, out);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::vector<unsigned char> buffer(1u << 20, 0);
    VnMonitorPollDesc pollDesc{};
    pollDesc.structSize = sizeof(pollDesc);
    pollDesc.buffer = buffer.data();
    pollDesc.bufferBytes = buffer.size();
    VnMonitorPollResult result{};
    result.structSize = sizeof(result);
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(handle, &pollDesc, &result));
    bool sawStale = false;
    bool sawChunk = false;
    std::uint64_t offset = 0;
    while (offset + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES <= result.bytesWritten) {
        const unsigned char* rec = buffer.data() + offset;
        const std::uint32_t kind = ReadU32(rec);
        const std::uint64_t recordBytes = ReadU64(rec + 16);
        if (kind == VN_MONITOR_RECORD_TAP_BINDING_CHANGED) {
            EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_STALE_TAP),
                      ReadU32(rec + 248));
            sawStale = true;
        }
        if (kind == VN_MONITOR_RECORD_AUDIO_CHUNK) {
            sawChunk = true;
        }
        if (recordBytes == 0) {
            break;
        }
        offset += recordBytes;
    }
    EXPECT_TRUE(sawStale);
    EXPECT_FALSE(sawChunk);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, VoiceExactInstanceIgnoresSiblingVoice) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    const std::vector<float> sine =
        harkentest::MakeSine(2048, 750.0, 48000.0, 0.7f);
    const VnPCMDesc pcm = harkentest::MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(11), &pcm));
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(12), &pcm));
    const VnPlayDesc playA = harkentest::MakePlayDesc(harkentest::ObjectId(11));
    const VnPlayDesc playB = harkentest::MakePlayDesc(harkentest::ObjectId(12));
    VnVoiceHandle voiceA = 0;
    VnVoiceHandle voiceB = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&playA, &voiceA));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&playB, &voiceB));
    ASSERT_NE(voiceA, voiceB);
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapKind = VN_MONITOR_TAP_VOICE_PRE_CHAIN;
    tap.ownerKind = VN_MONITOR_ENTITY_VOICE;
    tap.selectorMode = VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE;
    tap.ownerInstanceHandle = voiceA;
    tap.ownerGeneration = voiceA >> 32;
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.downsample = 1;
    plan.taps = &tap;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    harkentest::Capture out;
    harkentest::PumpAndCapture(session.Engine(), 2, out);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::vector<unsigned char> buffer(1u << 20, 0);
    VnMonitorPollDesc pollDesc{};
    pollDesc.structSize = sizeof(pollDesc);
    pollDesc.buffer = buffer.data();
    pollDesc.bufferBytes = buffer.size();
    VnMonitorPollResult result{};
    result.structSize = sizeof(result);
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(handle, &pollDesc, &result));
    bool sawStale = false;
    bool sawChunk = false;
    std::uint64_t offset = 0;
    while (offset + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES <= result.bytesWritten) {
        const unsigned char* rec = buffer.data() + offset;
        const std::uint32_t kind = ReadU32(rec);
        const std::uint64_t recordBytes = ReadU64(rec + 16);
        if (kind == VN_MONITOR_RECORD_TAP_BINDING_CHANGED) {
            sawStale = true;
        }
        if (kind == VN_MONITOR_RECORD_AUDIO_CHUNK) {
            sawChunk = true;
        }
        if (recordBytes == 0) {
            break;
        }
        offset += recordBytes;
    }
    EXPECT_TRUE(sawChunk);
    EXPECT_FALSE(sawStale);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, EffectExactInstanceUsesPackedHandleAndStalesOnClear) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnGainDesc gain{};
    gain.structSize = sizeof(gain);
    gain.gainDb = -6.0f;
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(VN_BUS_MASTER, 0, VN_EFFECT_GAIN,
                                     reinterpret_cast<const VnEffectDesc*>(&gain)));
    session.Engine()->nullBackend.Pump(2);
    const harken::BusTree::Bus* master = session.Engine()->buses.BusState(0);
    ASSERT_NE(nullptr, master);
    ASSERT_NE(harken::kEffectTypeNone, master->fxSlotEffectRef[0].type);
    const std::uint64_t packed =
        harken::PackedMonitorEffectHandle(0, 0);
    const std::uint64_t generation = master->fxSlotEffectRef[0].generation;
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapKind = VN_MONITOR_TAP_EFFECT_SLOT_INPUT;
    tap.ownerKind = VN_MONITOR_ENTITY_EFFECT;
    tap.selectorMode = VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE;
    tap.ownerInstanceHandle = packed;
    tap.ownerGeneration = generation;
    tap.slotId = 0;
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.downsample = 1;
    plan.taps = &tap;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    const std::vector<float> sine =
        harkentest::MakeSine(256, 750.0, 48000.0, 0.7f);
    const VnPCMDesc pcm = harkentest::MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(9), &pcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(9));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    harkentest::Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    std::vector<unsigned char> first;
    VnMonitorPollResult armed{};
    ASSERT_TRUE(PollUntilAudioChunk(handle, &first, &armed));
    ASSERT_EQ(VN_OK, Vn_ClearBusEffect(VN_BUS_MASTER, 0));
    session.Engine()->nullBackend.Pump(2);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::vector<unsigned char> buffer(1u << 20, 0);
    VnMonitorPollDesc pollDesc{};
    pollDesc.structSize = sizeof(pollDesc);
    pollDesc.buffer = buffer.data();
    pollDesc.bufferBytes = buffer.size();
    VnMonitorPollResult result{};
    result.structSize = sizeof(result);
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(handle, &pollDesc, &result));
    bool sawStale = false;
    bool sawChunk = false;
    std::uint64_t offset = 0;
    while (offset + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES <= result.bytesWritten) {
        const unsigned char* rec = buffer.data() + offset;
        const std::uint32_t kind = ReadU32(rec);
        const std::uint64_t recordBytes = ReadU64(rec + 16);
        if (kind == VN_MONITOR_RECORD_TAP_BINDING_CHANGED) {
            EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_STALE_TAP),
                      ReadU32(rec + 248));
            sawStale = true;
        }
        if (kind == VN_MONITOR_RECORD_AUDIO_CHUNK) {
            sawChunk = true;
        }
        if (recordBytes == 0) {
            break;
        }
        offset += recordBytes;
    }
    EXPECT_TRUE(sawStale);
    EXPECT_FALSE(sawChunk);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}
