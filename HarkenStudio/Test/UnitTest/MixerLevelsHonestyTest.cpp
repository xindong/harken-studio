#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Core/AudioEngine.h"
#include "Core/IPC/IPCBinaryMixerLevels.h"
#include "Core/IPC/IPCBinaryVoiceSnapshot.h"
#include "Core/IPC/IPCServer.h"

#include <QJsonObject>

#include <cstring>
#include <gtest/gtest.h>

TEST(MixerLevelsHonesty, NoEngineCommandFailsWithoutFabricatedSilence)
{
    VNX::IPCServer server(nullptr);
    const QJsonObject response = server.dispatchCommand(
        QStringLiteral("get_mixer_levels"), QJsonObject{});

    EXPECT_EQ(response.value(QStringLiteral("status")).toString(),
              QStringLiteral("error"));
    EXPECT_EQ(response.value(QStringLiteral("code")).toString(),
              QStringLiteral("NO_ENGINE"));
    EXPECT_FALSE(response.contains(QStringLiteral("result")));
}

TEST(RenderPlanHonesty, NoEngineCommandFailsWithoutFabricatedOrder)
{
    VNX::IPCServer server(nullptr);
    const QJsonObject response = server.dispatchCommand(
        QStringLiteral("get_render_plan"), QJsonObject{});

    EXPECT_EQ(response.value(QStringLiteral("status")).toString(),
              QStringLiteral("error"));
    EXPECT_FALSE(response.contains(QStringLiteral("result")));
}

TEST(MixerLevelsHonesty, MeteringSnapshotSharesTheFailure)
{
    VNX::IPCServer server(nullptr);
    const QJsonObject response = server.dispatchCommand(
        QStringLiteral("get_metering_snapshot"), QJsonObject{});
    EXPECT_EQ(response.value(QStringLiteral("status")).toString(),
              QStringLiteral("error"));
}

TEST(AudioEngineHonesty, ShutdownAfterUninitializedStaysUninitialized)
{
    VNX::AudioEngine engine;
    engine.shutdown();
    EXPECT_FALSE(engine.isInitialized());
}

TEST(AudioEngineHonesty, GetBusMeterPeakDoesNotWriteOnFailure)
{
    VNX::AudioEngine engine;
    float peakL = 12.0f;
    float peakR = 34.0f;
    EXPECT_EQ(engine.getBusMeterPeakById(harkentest::StudioObjectId(UINT64_C(0x100000005)), peakL, peakR),
              HarkenEditor::Bridge::IpcQueryStatus::NoEngine);
    EXPECT_FLOAT_EQ(peakL, 12.0f);
    EXPECT_FLOAT_EQ(peakR, 34.0f);
}

TEST(MixerLevelsHonesty, BinaryCommandFailsWithoutEngineInsteadOfStayingSilent)
{
    QByteArray payload;
    payload.append('x');
    const auto outcome = VNX::BinaryMixerLevels::encode(false, false, {}, payload);
    EXPECT_EQ(outcome, VNX::BinaryMixerLevels::Outcome::Error);
    EXPECT_TRUE(payload.isEmpty());
}

TEST(MixerLevelsHonesty, BinaryCommandFailsWhenSnapshotUnavailable)
{
    QByteArray payload;
    const auto outcome = VNX::BinaryMixerLevels::encode(true, false, {}, payload);
    EXPECT_EQ(outcome, VNX::BinaryMixerLevels::Outcome::Error);
    EXPECT_TRUE(payload.isEmpty());
}

TEST(VoiceSnapshotHonesty, BinaryCommandFailsClosedWithoutHonestRows)
{
    QByteArray payload;
    payload.append('x');
    EXPECT_EQ(VNX::BinaryVoiceSnapshot::encode(false, false, false, payload),
              VNX::BinaryVoiceSnapshot::Outcome::Error);
    EXPECT_TRUE(payload.isEmpty());

    EXPECT_EQ(VNX::BinaryVoiceSnapshot::encode(true, false, false, payload),
              VNX::BinaryVoiceSnapshot::Outcome::Error);
    EXPECT_TRUE(payload.isEmpty());

    EXPECT_EQ(VNX::BinaryVoiceSnapshot::encode(true, true, false, payload),
              VNX::BinaryVoiceSnapshot::Outcome::Error);
    EXPECT_TRUE(payload.isEmpty());
}

TEST(MixerLevelsHonesty, BinaryPayloadOmitsUnavailableBusRows)
{
    using HarkenEditor::Bridge::DebugBusData;
    using HarkenEditor::Bridge::DebugValueAvailability;

    DebugBusData live;
    live.busId = harkentest::StudioObjectId(42);
    live.peakL = 0.50f;
    live.peakR = 0.50f;
    live.muted = true;
    live.soloed = false;
    live.busIdAvailable = DebugValueAvailability::Available;
    live.peakAvailable = DebugValueAvailability::Available;
    live.mutedAvailable = DebugValueAvailability::Available;
    live.soloedAvailable = DebugValueAvailability::Available;
    live.rmsAvailable = DebugValueAvailability::Unavailable;

    DebugBusData destroyed;
    destroyed.busId = harkentest::StudioObjectId(0);
    destroyed.peakL = 0.75f;
    destroyed.peakR = 0.75f;
    destroyed.muted = false;
    destroyed.soloed = true;
    destroyed.busIdAvailable = DebugValueAvailability::Unavailable;
    destroyed.peakAvailable = DebugValueAvailability::Unavailable;
    destroyed.mutedAvailable = DebugValueAvailability::Available;
    destroyed.soloedAvailable = DebugValueAvailability::Available;

    QByteArray payload;
    const auto outcome = VNX::BinaryMixerLevels::encode(
        true, true, {destroyed, live}, payload);
    EXPECT_EQ(outcome, VNX::BinaryMixerLevels::Outcome::Ok);
    ASSERT_EQ(payload.size(), static_cast<int>(sizeof(VNX::IPCBusLevelEntry)));

    VNX::IPCBusLevelEntry entry{};
    std::memcpy(&entry, payload.constData(), sizeof(entry));
    EXPECT_EQ(harkenstudio::ObjectIdFromBytes(entry.busId), harkentest::StudioObjectId(42));
    EXPECT_FLOAT_EQ(entry.peakL, 0.50f);
    EXPECT_FLOAT_EQ(entry.peakR, 0.50f);
    EXPECT_EQ(entry.muted, 1);
    EXPECT_EQ(entry.soloed, 0);
    EXPECT_FLOAT_EQ(entry.rmsL, 0.0f);
    EXPECT_FLOAT_EQ(entry.rmsR, 0.0f);
}
