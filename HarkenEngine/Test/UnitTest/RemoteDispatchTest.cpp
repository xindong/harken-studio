// The generated command mirror, driven as a pure function against a live
// null-backend engine: scalar params, out params, desc marshalling with
// out-of-line sections (steps, container children at both stride
// vintages, strings with NULL sentinels), the declared policy refusals,
// and the wire-level error paths (unknown opcode, short payload, trailing
// bytes, oversized declared vintage). No pipe: DispatchCommand is exactly
// what the serving host's logic thread executes between Vn_Update pumps.
#include "Common/ObjectIdTestSupport.h"
#include "harken_remote/WireDispatch.h"

#include "Common/RecipePackTestUtil.h"
#include "Common/TestUtil.h"
#include "harken/param/EffectParamLocator.h"
#include "harken/render/BuiltinEffects.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using namespace harkenremote;

struct ParsedReply {
    bool isError = false;
    std::uint16_t errorCode = 0;
    std::uint16_t opcode = 0;
    std::int32_t result = 0;
    std::vector<std::uint64_t> outs;
};

ParsedReply Dispatch(const std::vector<std::uint8_t>& payload,
                     std::uint32_t sequence = 1) {
    WireFrame reply;
    DispatchCommand(payload.data(), payload.size(), sequence, &reply);
    EXPECT_EQ(sequence, reply.sequence);
    ParsedReply parsed;
    WireReader reader(reply.payload.data(), reply.payload.size());
    if (reply.msgType == kMsgError) {
        parsed.isError = true;
        EXPECT_TRUE(reader.ReadU16(&parsed.errorCode));
        EXPECT_TRUE(reader.ReadU16(&parsed.opcode));
        return parsed;
    }
    EXPECT_EQ(kMsgReply, reply.msgType);
    std::uint16_t reserved = 0;
    EXPECT_TRUE(reader.ReadU16(&parsed.opcode));
    EXPECT_TRUE(reader.ReadU16(&reserved));
    EXPECT_TRUE(reader.ReadI32(&parsed.result));
    while (reader.Remaining() >= sizeof(std::uint64_t)) {
        std::uint64_t value = 0;
        EXPECT_TRUE(reader.ReadU64(&value));
        parsed.outs.push_back(value);
    }
    EXPECT_TRUE(reader.AtEnd());
    return parsed;
}

std::vector<std::uint8_t> Command(std::uint16_t opcode) {
    std::vector<std::uint8_t> payload;
    WireWriter writer(&payload);
    writer.U16(opcode);
    writer.U16(0);
    return payload;
}

TEST(RemoteDispatch, LoadCurrentWorldNullPathReturnsCompleteReport) {
    auto payload = Command(kOpVn_LoadCurrentWorld);
    WireWriter writer(&payload);
    writer.String(nullptr);

    WireFrame reply;
    DispatchCommand(payload.data(), payload.size(), 1, &reply);
    ASSERT_EQ(kMsgReply, reply.msgType);

    WireReader reader(reply.payload.data(), reply.payload.size());
    std::uint16_t opcode = 0;
    std::uint16_t reserved = 0;
    std::int32_t result = 0;
    std::uint32_t reportSize = 0;
    std::uint32_t status = 0;
    std::int32_t reportResult = 0;
    std::uint32_t reportReserved = 0;
    std::array<std::uint64_t, 6> reportTail{};
    ASSERT_TRUE(reader.ReadU16(&opcode));
    ASSERT_TRUE(reader.ReadU16(&reserved));
    ASSERT_TRUE(reader.ReadI32(&result));
    ASSERT_TRUE(reader.ReadU32(&reportSize));
    ASSERT_TRUE(reader.ReadU32(&status));
    ASSERT_TRUE(reader.ReadI32(&reportResult));
    ASSERT_TRUE(reader.ReadU32(&reportReserved));
    for (std::uint64_t& value : reportTail) {
        ASSERT_TRUE(reader.ReadU64(&value));
    }

    EXPECT_EQ(kOpVn_LoadCurrentWorld, opcode);
    EXPECT_EQ(0u, reserved);
    EXPECT_EQ(VN_INVALID_ARG, result);
    EXPECT_EQ(sizeof(VnCurrentWorldLoadReport), reportSize);
    EXPECT_EQ(VN_CURRENT_WORLD_LOAD_INVALID_ARGUMENT, status);
    EXPECT_EQ(VN_INVALID_ARG, reportResult);
    EXPECT_EQ(0u, reportReserved);
    EXPECT_EQ((std::array<std::uint64_t, 6>{}), reportTail);
    EXPECT_TRUE(reader.AtEnd());
}

// Appends a desc image: u32 declared size + the raw struct bytes.
// Pointer fields must already be zeroed by the caller; their content
// arrives in sections appended afterwards.
template <typename T>
void AppendDescImage(std::vector<std::uint8_t>* payload, const T& desc) {
    WireWriter writer(payload);
    writer.U32(static_cast<std::uint32_t>(sizeof(T)));
    writer.Bytes(&desc, sizeof(T));
}

void RegisterTestSound(VnSoundId id) {
    const std::vector<float> tone =
        harkentest::MakeSine(4800, 440.0, 48000.0, 0.5f);
    VnPCMDesc pcm{};
    pcm.structSize = sizeof(VnPCMDesc);
    pcm.sampleRate = 48000;
    pcm.channels = 1;
    pcm.layout = VN_PCM_LAYOUT_INTERLEAVED;
    pcm.policy = VN_PCM_POLICY_COPY;
    pcm.frames = tone.size();
    pcm.samplesInterleaved = tone.data();
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(id, &pcm));
}

TEST(RemoteDispatch, ScalarCommandExecutesAgainstEngine) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    auto payload = Command(kOpVn_CreateBus);
    WireWriter writer(&payload);
    writer.ObjectId(harkentest::ObjectId(77));            // busId
    writer.ObjectId(VN_BUS_MASTER); // parentId
    const ParsedReply reply = Dispatch(payload);
    ASSERT_FALSE(reply.isError);
    EXPECT_EQ(kOpVn_CreateBus, reply.opcode);
    EXPECT_EQ(VN_OK, reply.result);

    // busCount is audio-published: the render side reports it once per
    // block, and a bus create lands at a block boundary through its
    // topology plan — so blocks must render before a snapshot can see
    // the new bus (the established null-pump-then-capture pattern).
    session.Engine()->nullBackend.Pump(4);

    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    EXPECT_EQ(2u, snapshot.busCount); // master + the wire-created bus

    // The identical command again must round-trip the engine's own
    // refusal (id in use), not a wire error.
    const ParsedReply again = Dispatch(payload, 2);
    ASSERT_FALSE(again.isError);
    EXPECT_EQ(VN_IN_USE, again.result);
}

TEST(RemoteDispatch, ActionSeekOpcodesDispatchAndValidateThePublicContract) {
    EXPECT_EQ(132, kOpVn_SeekActionFiring);
    EXPECT_EQ(133, kOpVn_QueryActionSeek);
    EXPECT_EQ(134, kOpVn_ReleaseActionSeek);
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    VnActionSeekDescV1 seek{};
    seek.structSize = sizeof(seek);
    seek.firing = 1;
    seek.positionMs = 25.0;
    seek.mode = VN_ACTION_SEEK_ABSOLUTE;
    auto seekPayload = Command(kOpVn_SeekActionFiring);
    AppendDescImage(&seekPayload, seek);
    const ParsedReply seekReply = Dispatch(seekPayload, 10);
    ASSERT_FALSE(seekReply.isError);
    EXPECT_EQ(kOpVn_SeekActionFiring, seekReply.opcode);
    EXPECT_EQ(VN_STALE_HANDLE, seekReply.result);
    ASSERT_EQ(1u, seekReply.outs.size());
    EXPECT_EQ(0u, seekReply.outs[0]);

    auto queryPayload = Command(kOpVn_QueryActionSeek);
    WireWriter queryWriter(&queryPayload);
    queryWriter.U64(1);
    WireFrame queryFrame;
    DispatchCommand(queryPayload.data(), queryPayload.size(), 11, &queryFrame);
    ASSERT_EQ(kMsgReply, queryFrame.msgType);
    WireReader queryReader(queryFrame.payload.data(), queryFrame.payload.size());
    std::uint16_t queryOpcode = 0;
    std::uint16_t queryReserved = 0;
    std::int32_t queryResult = 0;
    VnActionSeekInfoV1 queryInfo{};
    ASSERT_TRUE(queryReader.ReadU16(&queryOpcode));
    ASSERT_TRUE(queryReader.ReadU16(&queryReserved));
    ASSERT_TRUE(queryReader.ReadI32(&queryResult));
    ASSERT_TRUE(queryReader.ReadU32(&queryInfo.structSize));
    ASSERT_TRUE(queryReader.ReadU32(&queryInfo.state));
    ASSERT_TRUE(queryReader.ReadI32(&queryInfo.result));
    ASSERT_TRUE(queryReader.ReadU32(&queryInfo.reserved0));
    ASSERT_TRUE(queryReader.ReadU64(&queryInfo.firing));
    ASSERT_TRUE(queryReader.ReadU32(&queryInfo.targetCount));
    ASSERT_TRUE(queryReader.ReadU32(&queryInfo.appliedCount));
    ASSERT_TRUE(queryReader.ReadU32(&queryInfo.cancelledCount));
    ASSERT_TRUE(queryReader.ReadU32(&queryInfo.reserved1));
    ASSERT_TRUE(queryReader.ReadU64(&queryInfo.acceptedLogicSequence));
    ASSERT_TRUE(queryReader.ReadU64(&queryInfo.appliedAudioBlockSequence));
    ASSERT_TRUE(queryReader.ReadU64(&queryInfo.minimumAppliedSourceFrame));
    ASSERT_TRUE(queryReader.ReadU64(&queryInfo.maximumAppliedSourceFrame));
    EXPECT_TRUE(queryReader.AtEnd());
    EXPECT_EQ(kOpVn_QueryActionSeek, queryOpcode);
    EXPECT_EQ(0u, queryReserved);
    EXPECT_EQ(VN_STALE_HANDLE, queryResult);
    EXPECT_EQ(sizeof(VnActionSeekInfoV1), queryInfo.structSize);
    EXPECT_EQ(0u, queryInfo.state);

    auto releasePayload = Command(kOpVn_ReleaseActionSeek);
    WireWriter releaseWriter(&releasePayload);
    releaseWriter.U64(1);
    const ParsedReply releaseReply = Dispatch(releasePayload, 12);
    ASSERT_FALSE(releaseReply.isError);
    EXPECT_EQ(kOpVn_ReleaseActionSeek, releaseReply.opcode);
    EXPECT_EQ(VN_STALE_HANDLE, releaseReply.result);

    const auto expectInvalidSeek = [&](const VnActionSeekDescV1& invalid,
                                       std::uint32_t sequence) {
        auto payload = Command(kOpVn_SeekActionFiring);
        AppendDescImage(&payload, invalid);
        const ParsedReply reply = Dispatch(payload, sequence);
        EXPECT_FALSE(reply.isError);
        EXPECT_EQ(kOpVn_SeekActionFiring, reply.opcode);
        EXPECT_EQ(VN_INVALID_ARG, reply.result);
        ASSERT_EQ(1u, reply.outs.size());
        EXPECT_EQ(0u, reply.outs[0]);
    };

    VnActionSeekDescV1 invalid = seek;
    invalid.structSize = sizeof(invalid) - 1u;
    auto malformedSizePayload = Command(kOpVn_SeekActionFiring);
    AppendDescImage(&malformedSizePayload, invalid);
    const ParsedReply malformedSize = Dispatch(malformedSizePayload, 13);
    ASSERT_TRUE(malformedSize.isError);
    EXPECT_EQ(kWireErrMalformedPayload, malformedSize.errorCode);
    EXPECT_EQ(kOpVn_SeekActionFiring, malformedSize.opcode);

    invalid = seek;
    invalid.flags = 1;
    expectInvalidSeek(invalid, 14);
    invalid = seek;
    invalid.mode = 99;
    expectInvalidSeek(invalid, 15);
}

TEST(RemoteDispatch, OutputVolumeOpcodesPreserveMainSendAndLegacyVolumeSemantics) {
    EXPECT_EQ(8, kOpVn_SetBusVolume);
    EXPECT_EQ(10, kOpVn_SetVoiceVolume);
    EXPECT_EQ(100, kOpVn_UnloadPackEx);
    EXPECT_EQ(101, kOpVn_SetBusOutputVolume);
    EXPECT_EQ(102, kOpVn_SetVoiceOutputVolume);
    constexpr std::uint32_t block = 64;
    VnConfig config = harkentest::MakeNullConfig(2, block, 48000);
    config.maxBuses = 4;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto dc = harkentest::MakeDC(block, 0.25f);
    const auto pcm = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(77), VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(78), VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(harkentest::ObjectId(77), harkentest::ObjectId(78), 0.0f));
    auto play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    play.busId = harkentest::ObjectId(77);
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_SetVoiceSend(handle, 0, harkentest::ObjectId(78), 0.0f));
    harkentest::Capture warm;
    harkentest::PumpAndCapture(session.Engine(), 1, warm);
    ASSERT_NEAR(0.75f, warm.left.back(), 1e-5f);

    const float halfDb = 20.0f * std::log10(0.5f);
    const auto setVolume = [&](std::uint16_t opcode, auto id,
                               std::uint32_t rampMs) {
        auto payload = Command(opcode);
        WireWriter writer(&payload);
        if constexpr (std::is_same_v<decltype(id), VnObjectId>) writer.ObjectId(id);
        else writer.U64(id);
        writer.F32(halfDb);
        writer.U32(rampMs);
        const auto reply = Dispatch(payload);
        ASSERT_FALSE(reply.isError);
        EXPECT_EQ(opcode, reply.opcode);
        EXPECT_EQ(VN_OK, reply.result);
    };
    setVolume(kOpVn_SetBusOutputVolume, harkentest::ObjectId(77), 2);
    setVolume(kOpVn_SetVoiceOutputVolume, handle, 2);
    harkentest::Capture ramp;
    harkentest::PumpAndCapture(session.Engine(), 2, ramp);
    for (std::size_t i = 0; i < ramp.left.size(); ++i) {
        const float progress = std::min(1.0f, static_cast<float>(i + 1) / 96.0f);
        const float gain = 1.0f - 0.5f * progress;
        const float expected = 0.25f + 0.25f * gain + 0.25f * gain * gain;
        ASSERT_NEAR(expected, ramp.left[i], 1e-5f);
        ASSERT_NEAR(expected, ramp.right[i], 1e-5f);
    }
    setVolume(kOpVn_SetBusVolume, harkentest::ObjectId(77), 0);
    setVolume(kOpVn_SetVoiceVolume, handle, 0);
    harkentest::Capture legacy;
    harkentest::PumpAndCapture(session.Engine(), 1, legacy);
    ASSERT_NEAR(0.171875f, legacy.left.front(), 1e-5f);
    ASSERT_NEAR(0.171875f, legacy.right.back(), 1e-5f);
}

TEST(RemoteDispatch, OutputVolumeOpcodesValidatePayloadsBeforeCallingTheApi) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto published = session.Engine()->commands.PublishedHead();
    for (const auto opcode : {kOpVn_SetBusOutputVolume, kOpVn_SetVoiceOutputVolume}) {
        auto payload = Command(opcode);
        WireWriter writer(&payload);
        if (opcode == kOpVn_SetBusOutputVolume) writer.ObjectId(VN_BUS_MASTER);
        else writer.U64(1);
        writer.F32(-6.0f);
        writer.U32(10);
        auto shortPayload = payload;
        shortPayload.pop_back();
        const auto shortReply = Dispatch(shortPayload);
        EXPECT_TRUE(shortReply.isError);
        EXPECT_EQ(kWireErrMalformedPayload, shortReply.errorCode);
        payload.push_back(0);
        const auto trailingReply = Dispatch(payload);
        EXPECT_TRUE(trailingReply.isError);
        EXPECT_EQ(kWireErrMalformedPayload, trailingReply.errorCode);

        for (const float value : {0.0f, std::numeric_limits<float>::quiet_NaN()}) {
            auto unknown = Command(opcode);
            WireWriter unknownWriter(&unknown);
            if (opcode == kOpVn_SetBusOutputVolume) unknownWriter.ObjectId({});
            else unknownWriter.U64(0);
            unknownWriter.F32(value);
            unknownWriter.U32(0);
            const auto reply = Dispatch(unknown);
            EXPECT_FALSE(reply.isError);
            EXPECT_EQ(opcode, reply.opcode);
            EXPECT_EQ(!std::isfinite(value) ? VN_INVALID_ARG
                          : opcode == kOpVn_SetBusOutputVolume ? VN_UNKNOWN_ID
                                                             : VN_STALE_HANDLE,
                      reply.result);
        }
    }
    EXPECT_EQ(published, session.Engine()->commands.PublishedHead());
}

TEST(RemoteDispatch, BusTopologyDescriptorSectionsRoundTripAtomically) {
    VnConfig config = harkentest::MakeNullConfig();
    config.maxBuses = 4;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    std::array<VnBusTopologyBus, 2> buses{};
    buses[0].structSize = sizeof(VnBusTopologyBus);
    buses[0].busId = harkentest::ObjectId(8301);
    buses[0].parentId = VN_BUS_MASTER;
    buses[1].structSize = sizeof(VnBusTopologyBus);
    buses[1].busId = harkentest::ObjectId(8302);
    buses[1].parentId = harkentest::ObjectId(8301);
    std::array<VnBusTopologySend, 1> sends{};
    sends[0].structSize = sizeof(VnBusTopologySend);
    sends[0].fromBusId = harkentest::ObjectId(8302);
    sends[0].toBusId = VN_BUS_MASTER;
    sends[0].levelDb = -6.0f;

    VnBusTopologyDesc desc{};
    desc.structSize = sizeof(desc);
    desc.ownerId = 0x52454D4F54454255ull;
    desc.revision = 1;
    desc.busCount = static_cast<std::uint32_t>(buses.size());
    desc.busStructSize = sizeof(VnBusTopologyBus);
    desc.sendCount = static_cast<std::uint32_t>(sends.size());
    desc.sendStructSize = sizeof(VnBusTopologySend);

    auto payload = Command(kOpVn_ApplyBusTopology);
    AppendDescImage(&payload, desc);
    WireWriter writer(&payload);
    writer.U32(sizeof(buses));
    writer.Bytes(buses.data(), sizeof(buses));
    writer.U32(sizeof(sends));
    writer.Bytes(sends.data(), sizeof(sends));

    const ParsedReply reply = Dispatch(payload);
    ASSERT_FALSE(reply.isError);
    EXPECT_EQ(kOpVn_ApplyBusTopology, reply.opcode);
    EXPECT_EQ(VN_OK, reply.result);
    EXPECT_NE(harken::kInvalidBusIndex,
              session.Engine()->buses.FindIndex(harkentest::ObjectId(8302)));

    const ParsedReply retry = Dispatch(payload, 2);
    ASSERT_FALSE(retry.isError);
    EXPECT_EQ(VN_OK, retry.result);
}

TEST(RemoteDispatch, OutParamsTravelInReplies) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    const ParsedReply created = Dispatch(Command(kOpVn_CreateEmitter));
    ASSERT_FALSE(created.isError);
    EXPECT_EQ(VN_OK, created.result);
    ASSERT_EQ(1u, created.outs.size());
    const std::uint64_t emitter = created.outs[0];
    EXPECT_NE(0u, emitter);

    auto payload = Command(kOpVn_SetEmitterPosition);
    WireWriter writer(&payload);
    writer.U64(emitter);
    writer.F32(1.0f);
    writer.F32(2.0f);
    writer.F32(3.0f);
    const ParsedReply moved = Dispatch(payload, 2);
    ASSERT_FALSE(moved.isError);
    EXPECT_EQ(VN_OK, moved.result);
}

TEST(RemoteDispatch, LegacyOpcodesArePermanentUnknownTombstones) {
    const std::uint16_t opcodes[] = {
        kOpReservedLegacyDirectPlay,
        kOpReservedLegacyContainerRegister,
        kOpReservedLegacyContainerUnregister,
        kOpReservedLegacyContainerReplace,
        kOpReservedLegacyActionRegister,
        kOpReservedLegacyActionUnregister,
        kOpReservedLegacyActionTrigger,
        kOpReservedLegacyActionFindByName,
        kOpReservedLegacyActionTriggerByName,
        kOpReservedLegacyActionReplace,
        kOpReservedLegacyActionTriggerEx,
        kOpReservedLegacyActionStop,
        kOpReservedLegacyActionInstanceState,
    };
    std::uint64_t sequence = 1;
    for (std::uint16_t opcode : opcodes) {
        const ParsedReply reply = Dispatch(Command(opcode), sequence++);
        ASSERT_TRUE(reply.isError);
        EXPECT_EQ(kWireErrUnknownOpcode, reply.errorCode);
        EXPECT_EQ(opcode, reply.opcode);
    }
}

TEST(RemoteDispatch, PolyDescConfiguresBusEffect) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    VnFeedbackDelayDesc delay{};
    delay.structSize = sizeof(VnFeedbackDelayDesc);
    delay.delayMs = 120.0f;
    delay.feedback = 0.4f;
    delay.wet = 0.5f;
    delay.dry = 1.0f;

    auto payload = Command(kOpVn_SetBusEffect);
    WireWriter writer(&payload);
    writer.ObjectId(VN_BUS_MASTER);
    writer.U32(0); // slot
    writer.U32(VN_EFFECT_FEEDBACK_DELAY);
    writer.U32(sizeof(delay)); // polydesc: declared size + raw bytes
    writer.Bytes(&delay, sizeof(delay));
    const ParsedReply reply = Dispatch(payload);
    ASSERT_FALSE(reply.isError);
    EXPECT_EQ(VN_OK, reply.result);
}

TEST(RemoteDispatch, BusMuteSoloMirrorAndRefusals) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(77), VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(78), VN_BUS_MASTER));

    auto mute = Command(kOpVn_SetBusMute);
    {
        WireWriter writer(&mute);
        writer.ObjectId(harkentest::ObjectId(77)); // busId
        writer.U32(1);  // mute01
        writer.U32(0);  // fadeMs
    }
    const ParsedReply muted = Dispatch(mute);
    ASSERT_FALSE(muted.isError);
    EXPECT_EQ(kOpVn_SetBusMute, muted.opcode);
    EXPECT_EQ(VN_OK, muted.result);

    auto solo = Command(kOpVn_SetBusSolo);
    {
        WireWriter writer(&solo);
        writer.ObjectId(harkentest::ObjectId(78)); // busId
        writer.U32(1);  // solo01
        writer.U32(0);  // fadeMs
    }
    const ParsedReply soloed = Dispatch(solo);
    ASSERT_FALSE(soloed.isError);
    EXPECT_EQ(VN_OK, soloed.result);

    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    EXPECT_EQ(0x2u, snapshot.busMuteMask); // dense index 1 = bus 77
    // Solo path of 78 (index 2) = itself + the master; 77 muted anyway.
    EXPECT_EQ(0x5u, snapshot.busSoloEffectiveMask);

    // API refusals mirror as results, not wire errors.
    auto badMute = Command(kOpVn_SetBusMute);
    {
        WireWriter writer(&badMute);
        writer.ObjectId(harkentest::ObjectId(77));
        writer.U32(2); // mute01 above 1
        writer.U32(0);
    }
    const ParsedReply refused = Dispatch(badMute);
    ASSERT_FALSE(refused.isError);
    EXPECT_EQ(VN_INVALID_ARG, refused.result);
}

TEST(RemoteDispatch, BusReparentDestroyMirrorAndRefusals) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(81), VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(82), VN_BUS_MASTER));

    auto reparent = Command(kOpVn_ReparentBus);
    {
        WireWriter writer(&reparent);
        writer.ObjectId(harkentest::ObjectId(82)); // busId
        writer.ObjectId(harkentest::ObjectId(81)); // newParentId
    }
    const ParsedReply moved = Dispatch(reparent);
    ASSERT_FALSE(moved.isError);
    EXPECT_EQ(kOpVn_ReparentBus, moved.opcode);
    EXPECT_EQ(VN_OK, moved.result);

    auto destroy = Command(kOpVn_DestroyBus);
    {
        WireWriter writer(&destroy);
        writer.ObjectId(harkentest::ObjectId(81)); // cascades to the reparented 82
    }
    const ParsedReply destroyed = Dispatch(destroy);
    ASSERT_FALSE(destroyed.isError);
    EXPECT_EQ(VN_OK, destroyed.result);

    // Both ids resolve nowhere now; the refusals mirror as results.
    auto again = Command(kOpVn_DestroyBus);
    {
        WireWriter writer(&again);
        writer.ObjectId(harkentest::ObjectId(82));
    }
    const ParsedReply refused = Dispatch(again);
    ASSERT_FALSE(refused.isError);
    EXPECT_EQ(VN_UNKNOWN_ID, refused.result);
}

TEST(RemoteDispatch, PolicyOpcodesRefuseWithDeclaredWireErrors) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    const struct {
        std::uint16_t opcode;
        std::uint16_t expected;
    } cases[] = {
        {kOpVn_Init, kWireErrRefusedLifecycle},
        {kOpVn_Term, kWireErrRefusedLifecycle},
        {kOpVn_Update, kWireErrRefusedLifecycle},
        {kOpVn_Suspend, kWireErrRefusedLifecycle},
        {kOpVn_Resume, kWireErrRefusedLifecycle},
        {kOpVn_RegisterPCM, kWireErrRefusedMemoryDesc},
        {kOpVn_ReplacePCM, kWireErrRefusedMemoryDesc},
        {kOpVn_CaptureSnapshot, kWireErrUseSubscription},
    };
    for (const auto& c : cases) {
        const ParsedReply reply = Dispatch(Command(c.opcode));
        EXPECT_TRUE(reply.isError) << "opcode " << c.opcode;
        EXPECT_EQ(c.expected, reply.errorCode) << "opcode " << c.opcode;
        EXPECT_EQ(c.opcode, reply.opcode);
    }
}

TEST(RemoteDispatch, WireFaultsAnswerWithErrors) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    // Unknown opcode.
    {
        const ParsedReply reply = Dispatch(Command(999));
        ASSERT_TRUE(reply.isError);
        EXPECT_EQ(kWireErrUnknownOpcode, reply.errorCode);
        EXPECT_EQ(999u, reply.opcode);
    }
    // Payload too short for the opcode word itself.
    {
        const std::vector<std::uint8_t> empty;
        const ParsedReply reply = Dispatch(empty);
        ASSERT_TRUE(reply.isError);
        EXPECT_EQ(kWireErrMalformedPayload, reply.errorCode);
    }
    // Short params: Vn_Stop wants u64 + u32, gets four bytes.
    {
        auto payload = Command(kOpVn_Stop);
        WireWriter writer(&payload);
        writer.U32(7);
        const ParsedReply reply = Dispatch(payload);
        ASSERT_TRUE(reply.isError);
        EXPECT_EQ(kWireErrMalformedPayload, reply.errorCode);
        EXPECT_EQ(kOpVn_Stop, reply.opcode);
    }
    // Trailing bytes after a complete param list are refused too.
    {
        auto payload = Command(kOpVn_Stop);
        WireWriter writer(&payload);
        writer.U64(1);
        writer.U32(0);
        writer.U32(0xCAFE);
        const ParsedReply reply = Dispatch(payload);
        ASSERT_TRUE(reply.isError);
        EXPECT_EQ(kWireErrMalformedPayload, reply.errorCode);
    }
}

// The outrows reply form: the command carries only the capacity, the
// server owns the row buffer, and the reply carries requiredCount as the
// u64 out plus one trailing array section of raw rows. Parsed by hand —
// the generic ParsedReply helper deliberately assumes u64-only tails.
TEST(RemoteDispatch, CaptureRowsTravelAsAReplySection) {
    harkentest::EngineSession session(
        harkentest::MakeNullConfig(4, 256, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(100), VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(100), -18.0f, 0));

    auto payload = Command(kOpVn_CapturePropertyTargets);
    WireWriter writer(&payload);
    writer.U32(4); // capacity
    WireFrame reply;
    DispatchCommand(payload.data(), payload.size(), 1, &reply);
    ASSERT_EQ(kMsgReply, reply.msgType);
    WireReader reader(reply.payload.data(), reply.payload.size());
    std::uint16_t opcode = 0;
    std::uint16_t reserved = 0;
    std::int32_t result = 0;
    ASSERT_TRUE(reader.ReadU16(&opcode));
    ASSERT_TRUE(reader.ReadU16(&reserved));
    ASSERT_TRUE(reader.ReadI32(&result));
    EXPECT_EQ(kOpVn_CapturePropertyTargets, opcode);
    EXPECT_EQ(VN_OK, result);
    std::uint64_t requiredCount = 0;
    ASSERT_TRUE(reader.ReadU64(&requiredCount));
    EXPECT_EQ(1u, requiredCount);
    std::uint32_t rowBytes = 0;
    ASSERT_TRUE(reader.ReadU32(&rowBytes));
    ASSERT_EQ(sizeof(VnPropertyTargetInfo), rowBytes);
    const std::uint8_t* raw = reader.ReadBytes(rowBytes);
    ASSERT_NE(nullptr, raw);
    ASSERT_TRUE(reader.AtEnd());
    VnPropertyTargetInfo row{};
    std::memcpy(&row, raw, sizeof(row));
    EXPECT_EQ(sizeof(VnPropertyTargetInfo), row.structSize);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_TARGET_SCOPE_BUS),
              row.scopeKind);
    EXPECT_EQ(harkentest::ObjectId(100u), row.objectId);
    EXPECT_FLOAT_EQ(-18.0f, row.apiDirectValue);
    EXPECT_FLOAT_EQ(-18.0f, row.finalValue);
}

TEST(RemoteDispatch, GainAndEffectParameterBindingScopesUsePublicValidation) {
    VnConfig config = harkentest::MakeNullConfig(4, 256, 48000);
    config.fxSlotsPerVoice = 1;
    config.fxSlotsPerBus = 1;
    config.gainEffectPoolCapacity = 4;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    RegisterTestSound(harkentest::ObjectId(301));
    VnVoiceHandle handle = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(301));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);

    auto createBus = Command(kOpVn_CreateBus);
    WireWriter createBusWriter(&createBus);
    createBusWriter.ObjectId(harkentest::ObjectId(302));
    createBusWriter.ObjectId(VN_BUS_MASTER);
    ASSERT_EQ(VN_OK, Dispatch(createBus).result);

    VnGainDesc gain{};
    gain.structSize = sizeof(VnGainDesc);
    gain.gainDb = -6.0f;
    auto voiceEffect = Command(kOpVn_SetVoiceEffect);
    WireWriter voiceEffectWriter(&voiceEffect);
    voiceEffectWriter.U64(handle);
    voiceEffectWriter.U32(0);
    voiceEffectWriter.U32(VN_EFFECT_GAIN);
    AppendDescImage(&voiceEffect, gain);
    EXPECT_EQ(VN_OK, Dispatch(voiceEffect, 2).result);

    auto busEffect = Command(kOpVn_SetBusEffect);
    WireWriter busEffectWriter(&busEffect);
    busEffectWriter.ObjectId(harkentest::ObjectId(302));
    busEffectWriter.U32(0);
    busEffectWriter.U32(VN_EFFECT_GAIN);
    AppendDescImage(&busEffect, gain);
    EXPECT_EQ(VN_OK, Dispatch(busEffect, 3).result);
    auto voiceBypass = Command(kOpVn_SetVoiceEffectBypass);
    WireWriter voiceBypassWriter(&voiceBypass);
    voiceBypassWriter.U64(handle);
    voiceBypassWriter.U32(0);
    voiceBypassWriter.U32(1);
    EXPECT_EQ(VN_OK, Dispatch(voiceBypass, 10).result);
    auto busBypass = Command(kOpVn_SetBusEffectBypass);
    WireWriter busBypassWriter(&busBypass);
    busBypassWriter.ObjectId(harkentest::ObjectId(302));
    busBypassWriter.U32(0);
    busBypassWriter.U32(1);
    EXPECT_EQ(VN_OK, Dispatch(busBypass, 11).result);

    const auto makeBinding = [](std::uint32_t scope,
                                std::uint32_t effectType,
                                std::uint32_t paramIndex) {
        VnBindingDesc desc{};
        desc.structSize = sizeof(VnBindingDesc);
        desc.variableId = harkentest::ObjectId(400);
        desc.targetScopeKind = scope;
        desc.targetId =
            harken::PackEffectParamLocator(effectType, 0, paramIndex);
        desc.curve.structSize = sizeof(VnCurveDesc);
        desc.curve.pointCount = 2;
        desc.curve.points[0] = {0.0f, 0.0f};
        desc.curve.points[1] = {1.0f, 6.0f};
        desc.curve.handles[0] = harkentest::EndpointHandles(
            desc.curve.points[0], desc.curve.points[1]);
        return desc;
    };
    VnBindingDesc voiceBinding = makeBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_GAIN, 0);
    auto registerVoice = Command(kOpVn_RegisterBinding);
    WireWriter registerVoiceWriter(&registerVoice);
    registerVoiceWriter.ObjectId(harkentest::ObjectId(401));
    AppendDescImage(&registerVoice, voiceBinding);
    EXPECT_EQ(VN_OK, Dispatch(registerVoice, 4).result);
    auto applyVoice = Command(kOpVn_ApplyVoiceBinding);
    WireWriter applyVoiceWriter(&applyVoice);
    applyVoiceWriter.U64(handle);
    applyVoiceWriter.ObjectId(harkentest::ObjectId(401));
    EXPECT_EQ(VN_OK, Dispatch(applyVoice, 5).result);

    VnBindingDesc busBinding = makeBinding(
        VN_TARGET_SCOPE_BUS_EFFECT_PARAM, VN_EFFECT_GAIN, 0);
    auto registerBus = Command(kOpVn_RegisterBinding);
    WireWriter registerBusWriter(&registerBus);
    registerBusWriter.ObjectId(harkentest::ObjectId(402));
    AppendDescImage(&registerBus, busBinding);
    EXPECT_EQ(VN_OK, Dispatch(registerBus, 6).result);
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(VnBindingAttachDesc);
    attach.scopeKind = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    attach.objectId = harkentest::ObjectId(302);
    auto attachBus = Command(kOpVn_AttachBinding);
    WireWriter attachBusWriter(&attachBus);
    attachBusWriter.ObjectId(harkentest::ObjectId(402));
    AppendDescImage(&attachBus, attach);
    EXPECT_EQ(VN_OK, Dispatch(attachBus, 7).result);
    attach.objectId = harkentest::ObjectId(999999);
    auto unknownBus = Command(kOpVn_AttachBinding);
    WireWriter unknownBusWriter(&unknownBus);
    unknownBusWriter.ObjectId(harkentest::ObjectId(402));
    AppendDescImage(&unknownBus, attach);
    EXPECT_EQ(VN_UNKNOWN_ID, Dispatch(unknownBus, 8).result);

    VnBindingDesc unbindable = makeBinding(
        VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_EFFECT_FEEDBACK_DELAY, 0);
    auto registerUnbindable = Command(kOpVn_RegisterBinding);
    WireWriter unbindableWriter(&registerUnbindable);
    unbindableWriter.ObjectId(harkentest::ObjectId(403));
    AppendDescImage(&registerUnbindable, unbindable);
    EXPECT_EQ(VN_INVALID_ARG, Dispatch(registerUnbindable, 9).result);
}

TEST(RemoteDispatch,
     LimiterInstallParamsBypassAndScopeBindingsRoundTripThroughWire) {
    constexpr VnBusId kBus = harkentest::ObjectId(304);
    VnConfig config = harkentest::MakeNullConfig(4, 256, 48000);
    config.fxSlotsPerVoice = 1;
    config.fxSlotsPerBus = 1;
    config.limiterEffectPoolCapacity = 4;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  session.Engine(), {harkentest::ObjectId(440), harkentest::ObjectId(441)}, 0.0f, 1.0f).status);
    RegisterTestSound(harkentest::ObjectId(303));
    VnVoiceHandle handle = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(303));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);

    auto createBus = Command(kOpVn_CreateBus);
    WireWriter createBusWriter(&createBus);
    createBusWriter.ObjectId(kBus);
    createBusWriter.ObjectId(VN_BUS_MASTER);
    ASSERT_EQ(VN_OK, Dispatch(createBus, 1).result);

    VnLimiterDesc limiter{};
    limiter.structSize = sizeof(VnLimiterDesc);
    limiter.ceilingDb = -12.0f;
    limiter.releaseMs = 100.0f;
    auto voiceEffect = Command(kOpVn_SetVoiceEffect);
    WireWriter voiceEffectWriter(&voiceEffect);
    voiceEffectWriter.U64(handle);
    voiceEffectWriter.U32(0);
    voiceEffectWriter.U32(VN_EFFECT_LIMITER);
    AppendDescImage(&voiceEffect, limiter);
    ASSERT_EQ(VN_OK, Dispatch(voiceEffect, 2).result);

    auto busEffect = Command(kOpVn_SetBusEffect);
    WireWriter busEffectWriter(&busEffect);
    busEffectWriter.ObjectId(kBus);
    busEffectWriter.U32(0);
    busEffectWriter.U32(VN_EFFECT_LIMITER);
    AppendDescImage(&busEffect, limiter);
    ASSERT_EQ(VN_OK, Dispatch(busEffect, 3).result);

    auto voiceParam = Command(kOpVn_SetVoiceEffectParam);
    WireWriter voiceParamWriter(&voiceParam);
    voiceParamWriter.U64(handle);
    voiceParamWriter.U32(0);
    voiceParamWriter.U32(harken::kLimiterParamReleaseMs);
    voiceParamWriter.F32(75.0f);
    EXPECT_EQ(VN_OK, Dispatch(voiceParam, 4).result);
    auto busParam = Command(kOpVn_SetBusEffectParam);
    WireWriter busParamWriter(&busParam);
    busParamWriter.ObjectId(kBus);
    busParamWriter.U32(0);
    busParamWriter.U32(harken::kLimiterParamCeilingDb);
    busParamWriter.F32(-9.0f);
    EXPECT_EQ(VN_OK, Dispatch(busParam, 5).result);

    auto voiceBypass = Command(kOpVn_SetVoiceEffectBypass);
    WireWriter voiceBypassWriter(&voiceBypass);
    voiceBypassWriter.U64(handle);
    voiceBypassWriter.U32(0);
    voiceBypassWriter.U32(1);
    EXPECT_EQ(VN_OK, Dispatch(voiceBypass, 6).result);
    auto voiceEnable = Command(kOpVn_SetVoiceEffectBypass);
    WireWriter voiceEnableWriter(&voiceEnable);
    voiceEnableWriter.U64(handle);
    voiceEnableWriter.U32(0);
    voiceEnableWriter.U32(0);
    EXPECT_EQ(VN_OK, Dispatch(voiceEnable, 7).result);
    auto busBypass = Command(kOpVn_SetBusEffectBypass);
    WireWriter busBypassWriter(&busBypass);
    busBypassWriter.ObjectId(kBus);
    busBypassWriter.U32(0);
    busBypassWriter.U32(1);
    EXPECT_EQ(VN_OK, Dispatch(busBypass, 8).result);
    auto busEnable = Command(kOpVn_SetBusEffectBypass);
    WireWriter busEnableWriter(&busEnable);
    busEnableWriter.ObjectId(kBus);
    busEnableWriter.U32(0);
    busEnableWriter.U32(0);
    EXPECT_EQ(VN_OK, Dispatch(busEnable, 9).result);

    const auto makeBinding = [](VnVariableId variableId,
                                std::uint32_t scope,
                                std::uint32_t paramIndex, float y0,
                                float y1) {
        VnBindingDesc desc{};
        desc.structSize = sizeof(VnBindingDesc);
        desc.variableId = variableId;
        desc.targetScopeKind = scope;
        desc.targetId = harken::PackEffectParamLocator(
            VN_EFFECT_LIMITER, 0, paramIndex);
        desc.curve.structSize = sizeof(VnCurveDesc);
        desc.curve.pointCount = 2;
        desc.curve.points[0] = {0.0f, y0};
        desc.curve.points[1] = {1.0f, y1};
        desc.curve.handles[0] = harkentest::EndpointHandles(
            desc.curve.points[0], desc.curve.points[1]);
        return desc;
    };
    const VnBindingDesc ceilingBinding = makeBinding(
        harkentest::ObjectId(440), VN_TARGET_SCOPE_VOICE_EFFECT_PARAM,
        harken::kLimiterParamCeilingDb, -18.0f, -6.0f);
    auto registerCeiling = Command(kOpVn_RegisterBinding);
    WireWriter registerCeilingWriter(&registerCeiling);
    registerCeilingWriter.ObjectId(harkentest::ObjectId(450));
    AppendDescImage(&registerCeiling, ceilingBinding);
    EXPECT_EQ(VN_OK, Dispatch(registerCeiling, 10).result);
    auto applyCeiling = Command(kOpVn_ApplyVoiceBinding);
    WireWriter applyCeilingWriter(&applyCeiling);
    applyCeilingWriter.U64(handle);
    applyCeilingWriter.ObjectId(harkentest::ObjectId(450));
    EXPECT_EQ(VN_OK, Dispatch(applyCeiling, 11).result);

    const VnBindingDesc releaseBinding = makeBinding(
        harkentest::ObjectId(441), VN_TARGET_SCOPE_BUS_EFFECT_PARAM,
        harken::kLimiterParamReleaseMs, 0.0f, 1.0f);
    auto registerRelease = Command(kOpVn_RegisterBinding);
    WireWriter registerReleaseWriter(&registerRelease);
    registerReleaseWriter.ObjectId(harkentest::ObjectId(451));
    AppendDescImage(&registerRelease, releaseBinding);
    EXPECT_EQ(VN_OK, Dispatch(registerRelease, 12).result);
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(VnBindingAttachDesc);
    attach.scopeKind = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    attach.objectId = kBus;
    auto attachRelease = Command(kOpVn_AttachBinding);
    WireWriter attachReleaseWriter(&attachRelease);
    attachReleaseWriter.ObjectId(harkentest::ObjectId(451));
    AppendDescImage(&attachRelease, attach);
    EXPECT_EQ(VN_OK, Dispatch(attachRelease, 13).result);

    auto setCeiling = Command(kOpVn_SetValue);
    WireWriter setCeilingWriter(&setCeiling);
    setCeilingWriter.ObjectId(harkentest::ObjectId(440));
    setCeilingWriter.F32(1.0f);
    EXPECT_EQ(VN_OK, Dispatch(setCeiling, 14).result);
    auto setRelease = Command(kOpVn_SetValue);
    WireWriter setReleaseWriter(&setRelease);
    setReleaseWriter.ObjectId(harkentest::ObjectId(441));
    setReleaseWriter.F32(1.0f);
    EXPECT_EQ(VN_OK, Dispatch(setRelease, 15).result);

    harkentest::AdvanceVariableWrite(session.Engine());
    session.Engine()->nullBackend.Pump(1);
    const VnVoiceHandle renderHandle =
        harkentest::RenderHandle(session.Engine(), handle);
    harken::VoiceRenderContext* voice =
        session.Engine()->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, voice);
    ASSERT_EQ(VN_EFFECT_LIMITER, voice->fxSlotEffectRef[0].type);
    const auto* voiceLimiter =
        static_cast<const harken::LimiterState*>(voice->fxSlots[0].state);
    ASSERT_NE(nullptr, voiceLimiter);
    EXPECT_NEAR(std::pow(10.0, -18.0 / 20.0),
                voiceLimiter->ceilingLinear, 1.0e-6);
    EXPECT_NEAR(std::exp(-1.0 / (0.075 * 48000.0)),
                voiceLimiter->releaseCoeff, 1.0e-7);
    const std::uint32_t busIndex =
        session.Engine()->buses.FindIndex(kBus);
    harken::BusTree::Bus* bus =
        session.Engine()->buses.BusState(busIndex);
    ASSERT_NE(nullptr, bus);
    ASSERT_EQ(VN_EFFECT_LIMITER, bus->fxSlotEffectRef[0].type);
    const auto* busLimiter =
        static_cast<const harken::LimiterState*>(bus->fxSlots[0].state);
    ASSERT_NE(nullptr, busLimiter);
    EXPECT_NEAR(std::pow(10.0, -9.0 / 20.0),
                busLimiter->ceilingLinear, 1.0e-6);
    EXPECT_NEAR(std::exp(-1.0 / (0.200 * 48000.0)),
                busLimiter->releaseCoeff, 1.0e-7);
    EXPECT_FLOAT_EQ(0.0f, voice->fxBypassTarget[0]);
    EXPECT_FLOAT_EQ(0.0f, bus->fxBypassTarget[0]);
}

TEST(RemoteDispatch, EffectCapabilitiesTravelAsReplySections) {
    harkentest::EngineSession session(
        harkentest::MakeNullConfig(4, 256, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());

    const auto capture = [](std::uint16_t opcode, std::uint32_t capacity) {
        auto payload = Command(opcode);
        WireWriter writer(&payload);
        writer.U32(capacity);
        WireFrame reply;
        DispatchCommand(payload.data(), payload.size(), 1, &reply);
        return reply;
    };
    WireFrame effects =
        capture(kOpVn_CaptureEffectCapabilities, 5);
    WireReader effectReader(effects.payload.data(), effects.payload.size());
    std::uint16_t opcode = 0;
    std::uint16_t reserved = 0;
    std::int32_t result = 0;
    std::uint64_t required = 0;
    std::uint32_t rowBytes = 0;
    ASSERT_TRUE(effectReader.ReadU16(&opcode));
    ASSERT_TRUE(effectReader.ReadU16(&reserved));
    ASSERT_TRUE(effectReader.ReadI32(&result));
    ASSERT_TRUE(effectReader.ReadU64(&required));
    ASSERT_TRUE(effectReader.ReadU32(&rowBytes));
    EXPECT_EQ(kOpVn_CaptureEffectCapabilities, opcode);
    EXPECT_EQ(VN_OK, result);
    EXPECT_EQ(5u, required);
    ASSERT_EQ(5u * sizeof(VnEffectCapability), rowBytes);
    const std::uint8_t* raw = effectReader.ReadBytes(rowBytes);
    ASSERT_NE(nullptr, raw);
    VnEffectCapability gain{};
    std::memcpy(&gain, raw + 2 * sizeof(VnEffectCapability), sizeof(gain));
    EXPECT_EQ(VN_EFFECT_GAIN, gain.effectType);
    EXPECT_EQ(sizeof(VnGainDesc), gain.descriptorSize);
    EXPECT_EQ(VN_EFFECT_HOST_SCOPE_VOICE | VN_EFFECT_HOST_SCOPE_BUS,
              gain.hostScopeMask);
    VnEffectCapability limiter{};
    std::memcpy(&limiter, raw + 3 * sizeof(VnEffectCapability),
                sizeof(limiter));
    EXPECT_EQ(VN_EFFECT_LIMITER, limiter.effectType);
    EXPECT_EQ(sizeof(VnLimiterDesc), limiter.descriptorSize);
    VnEffectCapability compressor{};
    std::memcpy(&compressor, raw + 4 * sizeof(VnEffectCapability),
                sizeof(compressor));
    EXPECT_EQ(VN_EFFECT_COMPRESSOR, compressor.effectType);
    EXPECT_EQ(sizeof(VnCompressorDesc), compressor.descriptorSize);

    WireFrame params =
        capture(kOpVn_CaptureEffectParameterCapabilities, 20);
    WireReader paramReader(params.payload.data(), params.payload.size());
    ASSERT_TRUE(paramReader.ReadU16(&opcode));
    ASSERT_TRUE(paramReader.ReadU16(&reserved));
    ASSERT_TRUE(paramReader.ReadI32(&result));
    ASSERT_TRUE(paramReader.ReadU64(&required));
    ASSERT_TRUE(paramReader.ReadU32(&rowBytes));
    EXPECT_EQ(kOpVn_CaptureEffectParameterCapabilities, opcode);
    EXPECT_EQ(VN_OK, result);
    EXPECT_EQ(20u, required);
    EXPECT_EQ(20u * sizeof(VnEffectParameterCapability), rowBytes);
}

TEST(RemoteDispatch, DynamicRouteResetCapabilityAndTelemetryOpcodesRoundTrip) {
    static_assert(kOpVn_SetBusEffectDynamicRoute == 81);
    static_assert(kOpVn_ClearBusEffectDynamicRoute == 82);
    static_assert(kOpVn_ResetVoiceEffect == 83);
    static_assert(kOpVn_ResetBusEffect == 84);
    static_assert(kOpVn_CaptureEffectDynamicCapabilities == 85);
    static_assert(kOpVn_CaptureEffectDynamicParameterCapabilities == 86);
    static_assert(kOpVn_CaptureEffectDynamicRoutes == 87);
    static_assert(kOpVn_CaptureCompressorRuntimeInfo == 88);

    VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
    config.fxSlotsPerVoice = 1;
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 2;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    RegisterTestSound(harkentest::ObjectId(89));

    constexpr VnBusId kSourceBus = harkentest::ObjectId(8901);
    constexpr VnBusId kTargetBus = harkentest::ObjectId(8902);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));
    VnCompressorDesc compressor{};
    compressor.structSize = sizeof compressor;
    compressor.thresholdDb = -24.0f;
    compressor.ratio = 4.0f;
    compressor.attackMs = 10.0f;
    compressor.releaseMs = 100.0f;
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(
                  kTargetBus, 0, VN_EFFECT_COMPRESSOR,
                  reinterpret_cast<const VnEffectDesc*>(&compressor)));
    VnPlayDesc play{};
    play.structSize = sizeof play;
    play.soundId = harkentest::ObjectId(89);
    play.busId = kSourceBus;
    play.pitchRatio = 1.0f;
    play.priority = 128;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK,
              Vn_SetVoiceEffect(
                  voice, 0, VN_EFFECT_COMPRESSOR,
                  reinterpret_cast<const VnEffectDesc*>(&compressor)));

    VnEffectDynamicRouteDesc route{};
    route.structSize = sizeof route;
    route.signalForm = VN_DYNAMIC_SIGNAL_RAW_AUDIO;
    route.sourceKind = VN_DYNAMIC_SOURCE_BUS;
    route.tap = VN_DYNAMIC_TAP_BUS_POST_EFFECTS;
    route.sourceId = kSourceBus;
    route.expectedEffectType = VN_EFFECT_COMPRESSOR;
    auto setRoute = Command(kOpVn_SetBusEffectDynamicRoute);
    WireWriter routeWriter(&setRoute);
    routeWriter.ObjectId(kTargetBus);
    routeWriter.U32(0);
    routeWriter.U32(0);
    AppendDescImage(&setRoute, route);
    EXPECT_EQ(VN_OK, Dispatch(setRoute).result);

    auto resetVoice = Command(kOpVn_ResetVoiceEffect);
    WireWriter resetVoiceWriter(&resetVoice);
    resetVoiceWriter.U64(voice);
    resetVoiceWriter.U32(0);
    resetVoiceWriter.U32(VN_EFFECT_COMPRESSOR);
    EXPECT_EQ(VN_OK, Dispatch(resetVoice, 2).result);

    auto resetBus = Command(kOpVn_ResetBusEffect);
    WireWriter resetBusWriter(&resetBus);
    resetBusWriter.ObjectId(kTargetBus);
    resetBusWriter.U32(0);
    resetBusWriter.U32(VN_EFFECT_COMPRESSOR);
    EXPECT_EQ(VN_OK, Dispatch(resetBus, 3).result);
    session.Engine()->nullBackend.Pump(1);

    const auto captureRows =
        [](std::uint16_t opcode, std::uint32_t capacity,
           std::uint32_t expectedRequired, std::uint32_t rowSize) {
            auto payload = Command(opcode);
            WireWriter writer(&payload);
            writer.U32(capacity);
            WireFrame reply;
            DispatchCommand(payload.data(), payload.size(), 4, &reply);
            EXPECT_EQ(kMsgReply, reply.msgType);
            WireReader reader(reply.payload.data(), reply.payload.size());
            std::uint16_t returnedOpcode = 0;
            std::uint16_t reserved = 0;
            std::int32_t result = 0;
            std::uint64_t required = 0;
            std::uint32_t rowBytes = 0;
            EXPECT_TRUE(reader.ReadU16(&returnedOpcode));
            EXPECT_TRUE(reader.ReadU16(&reserved));
            EXPECT_TRUE(reader.ReadI32(&result));
            EXPECT_TRUE(reader.ReadU64(&required));
            EXPECT_TRUE(reader.ReadU32(&rowBytes));
            EXPECT_EQ(opcode, returnedOpcode);
            EXPECT_EQ(VN_OK, result);
            EXPECT_EQ(expectedRequired, required);
            EXPECT_EQ((std::min)(capacity, expectedRequired) * rowSize,
                      rowBytes);
            EXPECT_NE(nullptr, reader.ReadBytes(rowBytes));
            EXPECT_TRUE(reader.AtEnd());
        };
    captureRows(kOpVn_CaptureEffectDynamicCapabilities, 5, 5,
                sizeof(VnEffectDynamicCapability));
    captureRows(kOpVn_CaptureEffectDynamicParameterCapabilities, 18, 18,
                sizeof(VnEffectDynamicParameterCapability));
    captureRows(kOpVn_CaptureEffectDynamicRoutes, 1, 1,
                sizeof(VnEffectDynamicRouteInfo));
    captureRows(kOpVn_CaptureCompressorRuntimeInfo, 2, 2,
                sizeof(VnCompressorRuntimeInfo));

    auto clearRoute = Command(kOpVn_ClearBusEffectDynamicRoute);
    WireWriter clearWriter(&clearRoute);
    clearWriter.ObjectId(kTargetBus);
    clearWriter.U32(0);
    clearWriter.U32(0);
    clearWriter.U32(VN_EFFECT_COMPRESSOR);
    EXPECT_EQ(VN_OK, Dispatch(clearRoute, 5).result);
}

TEST(RemoteDispatch, KindElevenRecipePackLoadsThroughTheWireMirror) {
    VnConfig config = harkentest::MakeNullConfig(4, 256, 48000);
    config.fxSlotsPerVoice = 1;
    config.gainEffectPoolCapacity = 4;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const harkentest::RecipePackArtifact artifact =
        harkentest::BuildGainRecipePack("remote_recipe_kind11");
    ASSERT_TRUE(artifact.built);

    VnPackDesc desc{};
    desc.structSize = sizeof(VnPackDesc);
    auto payload = Command(kOpVn_LoadPack);
    AppendDescImage(&payload, desc);
    WireWriter writer(&payload);
    writer.String(artifact.path.c_str());
    const ParsedReply loaded = Dispatch(payload);
    ASSERT_FALSE(loaded.isError);
    ASSERT_EQ(VN_OK, loaded.result);
    ASSERT_EQ(1u, loaded.outs.size());
    EXPECT_NE(0u, loaded.outs[0]);
    const harken::PackLogicImage::VoiceRecipe* recipe =
        session.Engine()->logicPacks.FindVoiceRecipe(harkentest::ObjectId(9101));
    ASSERT_NE(nullptr, recipe);
    ASSERT_EQ(1u, recipe->installs.size());
    EXPECT_EQ(VN_EFFECT_GAIN, recipe->installs[0].effectType);
}

TEST(RemoteDispatch, LimiterKindElevenRecipePackLoadsThroughWireMirror) {
    VnConfig config = harkentest::MakeNullConfig(4, 256, 48000);
    config.fxSlotsPerVoice = 1;
    config.limiterEffectPoolCapacity = 4;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const harkentest::RecipePackArtifact artifact =
        harkentest::BuildLimiterRecipePack("remote_limiter_recipe_kind11");
    ASSERT_TRUE(artifact.built);

    VnPackDesc desc{};
    desc.structSize = sizeof(VnPackDesc);
    auto payload = Command(kOpVn_LoadPack);
    AppendDescImage(&payload, desc);
    WireWriter writer(&payload);
    writer.String(artifact.path.c_str());
    const ParsedReply loaded = Dispatch(payload);
    ASSERT_FALSE(loaded.isError);
    ASSERT_EQ(VN_OK, loaded.result);
    ASSERT_EQ(1u, loaded.outs.size());
    EXPECT_NE(0u, loaded.outs[0]);
    const harken::PackLogicImage::VoiceRecipe* recipe =
        session.Engine()->logicPacks.FindVoiceRecipe(harkentest::ObjectId(9101));
    ASSERT_NE(nullptr, recipe);
    ASSERT_EQ(1u, recipe->installs.size());
    EXPECT_EQ(VN_EFFECT_LIMITER, recipe->installs[0].effectType);
    ASSERT_EQ(2u, recipe->installs[0].paramCount);
    EXPECT_FLOAT_EQ(-12.0f, recipe->installs[0].params[0]);
    EXPECT_FLOAT_EQ(150.0f, recipe->installs[0].params[1]);
}

TEST(RemoteDispatch, AuthoredProximityAttenuationRegistersThroughWire) {
    VnConfig config = harkentest::MakeNullConfig(4, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    RegisterTestSound(harkentest::ObjectId(1));

    // The full-size descriptor carries a non-zero authored corner, so the
    // register command must marshal the appended proximityFrequency and the
    // engine mirror must hold it verbatim.
    VnAttenuationDesc attenuation = harkentest::MakeAttenuationDesc(1.0f, 20.0f);
    attenuation.proximityRadius = 10.0f;
    attenuation.proximityMaxBoostDb = 12.0f;
    attenuation.proximityFrequency = 400.0f;
    auto registerCmd = Command(kOpVn_RegisterAttenuation);
    {
        WireWriter writer(&registerCmd);
        writer.ObjectId(harkentest::ObjectId(9));
    }
    AppendDescImage(&registerCmd, attenuation);
    const ParsedReply registered = Dispatch(registerCmd);
    ASSERT_FALSE(registered.isError);
    ASSERT_EQ(VN_OK, registered.result);
    const harken::AttenuationProfile* profile =
        session.Engine()->spatial.FindProfile(harkentest::ObjectId(9));
    ASSERT_NE(nullptr, profile);
    EXPECT_FLOAT_EQ(400.0f, profile->proximityFrequency);

}

TEST(RemoteDispatch, AuthoredToneRecipePackLoadsThroughTheWireMirror) {
    VnConfig config = harkentest::MakeNullConfig(4, 256, 48000);
    config.fxSlotsPerVoice = 1;
    config.gainEffectPoolCapacity = 4;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const harkentest::RecipePackArtifact artifact =
        harkentest::BuildToneGainRecipePack("remote_tone_recipe_kind11");
    ASSERT_TRUE(artifact.built);

    VnPackDesc desc{};
    desc.structSize = sizeof(VnPackDesc);
    auto payload = Command(kOpVn_LoadPack);
    AppendDescImage(&payload, desc);
    WireWriter writer(&payload);
    writer.String(artifact.path.c_str());
    const ParsedReply loaded = Dispatch(payload);
    ASSERT_FALSE(loaded.isError);
    ASSERT_EQ(VN_OK, loaded.result);
    ASSERT_EQ(1u, loaded.outs.size());
    EXPECT_NE(0u, loaded.outs[0]);
    const harken::PackLogicImage::VoiceRecipe* recipe =
        session.Engine()->logicPacks.FindVoiceRecipe(harkentest::ObjectId(9101));
    ASSERT_NE(nullptr, recipe);
    // The authored tone slot survived packc, wire load, and mirror parse
    // with its low-shelf intact.
    EXPECT_EQ(1u, recipe->tone.present);
    EXPECT_FLOAT_EQ(200.0f, recipe->tone.lowShelfFrequencyHz);
    EXPECT_FLOAT_EQ(6.0f, recipe->tone.lowShelfGainDb);
    ASSERT_EQ(1u, recipe->installs.size());
    EXPECT_EQ(VN_EFFECT_GAIN, recipe->installs[0].effectType);
}

} // namespace
