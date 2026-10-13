#include "UnitTest/StudioObjectIdTestSupport.h"
#include "RuntimeBridge/NativeDiagnostics.h"

#include <harken/Harken.h>

#include <QJsonValue>
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace Bridge = HarkenEditor::Bridge;

namespace {

VnSnapshot makeBlankSnapshot()
{
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    return snapshot;
}

}

TEST(DebugHonestyMapper, MapsBusIdsAndPeaksAndMarksTheRestUnavailable)
{
    VnSnapshot snapshot = makeBlankSnapshot();
    snapshot.busCount = 3;
    snapshot.busIds[0] = harkentest::ObjectId(1);
    snapshot.busIds[1] = harkentest::ObjectId(42);
    snapshot.busIds[2] = harkentest::ObjectId(0);
    snapshot.busPeaks[0] = 0.25f;
    snapshot.busPeaks[1] = 0.50f;
    snapshot.busPeaks[2] = 0.75f;
    snapshot.mixChannelCount = 2;
    snapshot.busChannelPeaks[0] = 0.25f;
    snapshot.busChannelPeaks[1] = 0.125f;
    snapshot.busChannelPeaks[VN_MAX_CHANNELS] = 0.50f;
    snapshot.busChannelPeaks[VN_MAX_CHANNELS + 1] = 0.0625f;
    snapshot.busMuteMask = 0x2u;
    snapshot.busSoloEffectiveMask = 0x1u;

    std::vector<Bridge::DebugBusData> buses;
    Bridge::mapSnapshotToDebugBuses(snapshot, buses);
    ASSERT_EQ(buses.size(), 3u);

    EXPECT_EQ(buses[0].busIdAvailable, Bridge::DebugValueAvailability::Available);
    EXPECT_EQ(buses[0].busId, harkentest::StudioObjectId(1u));
    EXPECT_EQ(buses[0].peakAvailable, Bridge::DebugValueAvailability::Available);
    EXPECT_FLOAT_EQ(buses[0].peakL, 0.25f);
    EXPECT_FLOAT_EQ(buses[0].peakR, 0.125f);
    EXPECT_EQ(buses[0].nameAvailable, Bridge::DebugValueAvailability::Unavailable);
    EXPECT_TRUE(buses[0].name.isEmpty());
    EXPECT_EQ(buses[0].volumeAvailable, Bridge::DebugValueAvailability::Unavailable);
    EXPECT_EQ(buses[0].rmsAvailable, Bridge::DebugValueAvailability::Unavailable);
    EXPECT_EQ(buses[0].voiceCountAvailable, Bridge::DebugValueAvailability::Unavailable);
    EXPECT_EQ(buses[0].parentBusIdAvailable, Bridge::DebugValueAvailability::Unavailable);
    EXPECT_EQ(buses[0].mutedAvailable, Bridge::DebugValueAvailability::Available);
    EXPECT_FALSE(buses[0].muted);
    EXPECT_EQ(buses[0].soloedAvailable, Bridge::DebugValueAvailability::Available);
    EXPECT_TRUE(buses[0].soloed);

    EXPECT_EQ(buses[1].busId, harkentest::StudioObjectId(42u));
    EXPECT_FLOAT_EQ(buses[1].peakL, 0.50f);
    EXPECT_FLOAT_EQ(buses[1].peakR, 0.0625f);
    EXPECT_TRUE(buses[1].muted);
    EXPECT_FALSE(buses[1].soloed);

    EXPECT_EQ(buses[2].busIdAvailable, Bridge::DebugValueAvailability::Unavailable);
    EXPECT_EQ(buses[2].peakAvailable, Bridge::DebugValueAvailability::Unavailable);
    EXPECT_EQ(buses[2].busId, harkentest::StudioObjectId(0u));
    snapshot.mixChannelCount = 1;
    Bridge::mapSnapshotToDebugBuses(snapshot, buses);
    EXPECT_FLOAT_EQ(buses[0].peakR, buses[0].peakL);
    EXPECT_FLOAT_EQ(buses[1].peakR, buses[1].peakL);
}

TEST(DebugHonestyMapper, MarksVoicePeakAndIdentityFieldsUnavailable)
{
    VnSnapshot snapshot = makeBlankSnapshot();
    snapshot.activeVoiceCount = 1;
    snapshot.voices[0].state = VN_VOICE_STATE_ACTIVE;
    snapshot.voices[0].tapPostGain = 0.8f;
    snapshot.voicePublicHandles[0] = 99;

    std::vector<Bridge::DebugVoiceData> voices;
    Bridge::mapSnapshotToDebugVoices(snapshot, voices);
    ASSERT_EQ(voices.size(), 1u);
    EXPECT_EQ(voices[0].stateAvailable, Bridge::DebugValueAvailability::Available);
    EXPECT_EQ(voices[0].state, static_cast<int32_t>(VN_VOICE_STATE_ACTIVE));
    EXPECT_EQ(voices[0].playingIDAvailable, Bridge::DebugValueAvailability::Available);
    EXPECT_EQ(voices[0].playingID, 99u);
    EXPECT_EQ(voices[0].pipelineIDAvailable, Bridge::DebugValueAvailability::Available);
    EXPECT_EQ(voices[0].pipelineID, 0u);
    EXPECT_EQ(voices[0].peakLevelAvailable, Bridge::DebugValueAvailability::Unavailable);
    EXPECT_FLOAT_EQ(voices[0].peakLevel, 0.0f);
    EXPECT_NE(voices[0].peakLevel, 0.8f);
    EXPECT_EQ(voices[0].objectIDAvailable, Bridge::DebugValueAvailability::Unavailable);
    EXPECT_EQ(voices[0].volumeAvailable, Bridge::DebugValueAvailability::Unavailable);
    EXPECT_EQ(voices[0].priorityAvailable, Bridge::DebugValueAvailability::Unavailable);
    EXPECT_EQ(voices[0].positionAvailable, Bridge::DebugValueAvailability::Unavailable);
}

TEST(DebugHonestyMapper, RenderPlanUsesLiveBusIdsAndSkipsDeadSlots)
{
    VnSnapshot snapshot = makeBlankSnapshot();
    snapshot.busCount = 4;
    snapshot.busIds[0] = harkentest::ObjectId(1);
    snapshot.busIds[1] = harkentest::ObjectId(9);
    snapshot.busIds[2] = harkentest::ObjectId(0);
    snapshot.busIds[3] = harkentest::ObjectId(4);
    snapshot.topologyPlanCount = 7;

    Bridge::RenderPlanInfo plan;
    ASSERT_TRUE(Bridge::mapSnapshotToRenderPlan(snapshot, plan));
    EXPECT_TRUE(plan.valid);
    EXPECT_EQ(plan.busCount, 3u);
    EXPECT_EQ(plan.busOrder[0], harkentest::StudioObjectId(1u));
    EXPECT_EQ(plan.busOrder[1], harkentest::StudioObjectId(9u));
    EXPECT_EQ(plan.busOrder[2], harkentest::StudioObjectId(4u));
    EXPECT_EQ(plan.standardBusEnd, 3u);
    EXPECT_EQ(plan.receiveBusEnd, 3u);
    EXPECT_EQ(plan.version, 7u);
}

TEST(DebugHonestyMapper, RenderPlanRejectsAllDeadSlots)
{
    VnSnapshot snapshot = makeBlankSnapshot();
    snapshot.busCount = 2;
    snapshot.busIds[0] = harkentest::ObjectId(0);
    snapshot.busIds[1] = harkentest::ObjectId(0);

    Bridge::RenderPlanInfo plan;
    EXPECT_FALSE(Bridge::mapSnapshotToRenderPlan(snapshot, plan));
    EXPECT_FALSE(plan.valid);
    EXPECT_EQ(plan.busCount, 0u);
}

TEST(DebugHonestyMapper, RenderPlanPreservesFullWidthBusIds)
{
    VnSnapshot snapshot = makeBlankSnapshot();
    snapshot.busCount = 1;
    snapshot.busIds[0] = harkentest::ObjectId((uint64_t{1} << 32) | 7u);

    Bridge::RenderPlanInfo plan;
    EXPECT_TRUE(Bridge::mapSnapshotToRenderPlan(snapshot, plan));
    EXPECT_TRUE(plan.valid);
    ASSERT_EQ(plan.busCount, 1u);
    EXPECT_EQ(plan.busOrder[0], snapshot.busIds[0]);
}

TEST(DebugHonestyMapper, VoiceInfoListDoesNotCopyTapOrInventPitch)
{
    VnSnapshot snapshot = makeBlankSnapshot();
    snapshot.voices[0].state = VN_VOICE_STATE_ACTIVE;
    snapshot.voices[0].tapPostGain = 0.7f;
    snapshot.voicePublicHandles[0] = 11;

    std::vector<Bridge::VoiceInfoData> voices;
    Bridge::mapSnapshotToVoiceInfoList(snapshot, voices);
    ASSERT_EQ(voices.size(), 1u);
    EXPECT_EQ(voices[0].state, static_cast<int32_t>(VN_VOICE_STATE_ACTIVE));
    EXPECT_FLOAT_EQ(voices[0].volume, 0.0f);
    EXPECT_NE(voices[0].volume, 0.7f);
    EXPECT_FLOAT_EQ(voices[0].pitch, 0.0f);
    EXPECT_NE(voices[0].pitch, 1.0f);
}

TEST(DebugHonestyMapper, LiveGettersStayEmptyWithoutSession)
{
    std::vector<Bridge::DebugVoiceData> voices;
    EXPECT_FALSE(Bridge::getDebugVoices(voices));
    EXPECT_TRUE(voices.empty());

    std::vector<Bridge::DebugBusData> buses;
    EXPECT_FALSE(Bridge::getDebugBuses(buses));
    EXPECT_TRUE(buses.empty());

    Bridge::RenderPlanInfo plan;
    EXPECT_FALSE(Bridge::getRenderPlan(plan));
    EXPECT_FALSE(plan.valid);
}

TEST(DebugHonestyMapper, StatusBarTextDoesNotUseMinusOneAsUnavailable)
{
    const auto available = Bridge::formatVoiceCountAvailable(3);
    EXPECT_EQ(available.text, QStringLiteral("Voices: 3/64"));
    EXPECT_EQ(available.voiceLoadRole, QStringLiteral("safe"));

    const auto unavailable = Bridge::formatVoiceCountUnavailable();
    EXPECT_EQ(unavailable.text, QStringLiteral("Voices: —"));
    EXPECT_TRUE(unavailable.voiceLoadRole.isEmpty());
    EXPECT_NE(unavailable.text, Bridge::formatVoiceCountAvailable(-1).text);
    EXPECT_NE(unavailable.text, Bridge::formatVoiceCountAvailable(0).text);
}

TEST(DebugHonestyMapper, ConsumersOmitUnavailableNumericDefaults)
{
    VnSnapshot snapshot = makeBlankSnapshot();
    snapshot.activeVoiceCount = 1;
    snapshot.voices[0].state = VN_VOICE_STATE_ACTIVE;
    snapshot.voicePublicHandles[0] = 7;
    snapshot.busCount = 1;
    snapshot.busIds[0] = harkentest::ObjectId(11);
    snapshot.busPeaks[0] = 0.4f;

    std::vector<Bridge::DebugVoiceData> voices;
    Bridge::mapSnapshotToDebugVoices(snapshot, voices);
    ASSERT_EQ(voices.size(), 1u);
    EXPECT_TRUE(Bridge::jsonIntIfAvailable(voices[0].objectIDAvailable,
                                           static_cast<int>(voices[0].objectID)).isNull());
    EXPECT_TRUE(Bridge::jsonNumberIfAvailable(voices[0].peakLevelAvailable,
                                              static_cast<double>(voices[0].peakLevel)).isNull());
    EXPECT_TRUE(Bridge::jsonNumberIfAvailable(voices[0].volumeAvailable,
                                              static_cast<double>(voices[0].volume)).isNull());
    EXPECT_FALSE(Bridge::jsonIntIfAvailable(voices[0].stateAvailable, voices[0].state).isNull());
    EXPECT_EQ(Bridge::displayDashIfUnavailable(voices[0].peakLevelAvailable, QStringLiteral("0.0 dB")),
              QStringLiteral("—"));

    std::vector<Bridge::DebugBusData> buses;
    Bridge::mapSnapshotToDebugBuses(snapshot, buses);
    ASSERT_EQ(buses.size(), 1u);
    EXPECT_TRUE(Bridge::jsonStringIfAvailable(buses[0].nameAvailable, buses[0].name).isNull());
    EXPECT_TRUE(Bridge::jsonNumberIfAvailable(buses[0].volumeAvailable,
                                              static_cast<double>(buses[0].volume)).isNull());
    EXPECT_FALSE(Bridge::jsonNumberIfAvailable(buses[0].peakAvailable,
                                               static_cast<double>(buses[0].peakL)).isNull());
}

TEST(DebugHonestyMapper, FirstZeroCountIsStillAnAvailableReading)
{
    EXPECT_EQ(Bridge::formatVoiceCountAvailable(0).text, QStringLiteral("Voices: 0/64"));
    EXPECT_NE(Bridge::formatVoiceCountAvailable(0).text,
              Bridge::formatVoiceCountUnavailable().text);
}
