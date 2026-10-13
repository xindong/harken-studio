#include "ActionManager.h"
#include "Actions/AdvancedActionResult.h"
#include "AudioEngine.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QJsonObject>

#include <array>

namespace {

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

QJsonObject validArgs(const QString& action)
{
    if (action == QStringLiteral("set_game_object_volume")) {
        return {{QStringLiteral("game_object_id"), 42},
                {QStringLiteral("volume"), 0.5}};
    }
    if (action == QStringLiteral("set_game_object_mute")) {
        return {{QStringLiteral("game_object_id"), 42},
                {QStringLiteral("mute"), true}};
    }
    if (action == QStringLiteral("seek_action")) {
        return {{QStringLiteral("action_handle"), 1},
                {QStringLiteral("name"), QStringLiteral("ActionTest")},
                {QStringLiteral("position"), 125.0},
                {QStringLiteral("is_relative"), false},
                {QStringLiteral("snap_to_marker"), false}};
    }
    return {{QStringLiteral("name"), QStringLiteral("ActionTest")}};
}

constexpr std::array<const char*, 3> kResultBearingActions{
    "set_game_object_volume",
    "set_game_object_mute",
    "seek_action",
};

class AdvancedActionResultTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        VNX::AudioEngine::instance().shutdown();
        QCoreApplication::processEvents();
        VNX::ActionManager::instance().clearAllForTesting();
    }

    void TearDown() override
    {
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::AudioEngine::instance().shutdown();
        QCoreApplication::processEvents();
    }

    VNX::ActionResult run(const char* action)
    {
        const QString id = QString::fromLatin1(action);
        return VNX::ActionManager::instance().execute(
            id, validArgs(id), VNX::ActionSource::IPC);
    }
};

TEST_F(AdvancedActionResultTest, MissingEngineIsDistinctFromUninitializedEngine)
{
    VNX::registerAdvancedActions(VNX::ActionManager::instance(), nullptr);

    for (const char* action : kResultBearingActions) {
        const auto result = run(action);
        EXPECT_FALSE(result.success) << action;
        EXPECT_EQ(result.errorCode, QStringLiteral("ENGINE_NOT_AVAILABLE")) << action;
    }
}

TEST_F(AdvancedActionResultTest, UninitializedEngineResultIsNotReportedAsSuccess)
{
    VNX::registerAdvancedActions(
        VNX::ActionManager::instance(), &VNX::AudioEngine::instance());

    for (const char* action : kResultBearingActions) {
        const auto result = run(action);
        EXPECT_FALSE(result.success) << action;
        EXPECT_EQ(result.errorCode, QStringLiteral("NOT_INIT")) << action;
        EXPECT_EQ(result.data.value(QStringLiteral("engine_result")).toInteger(), -303)
            << action;
    }
}

TEST_F(AdvancedActionResultTest, InitializedUnsupportedResultIsNotReportedAsSuccess)
{
    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 64;
    config.maxVoices = 8;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(VNX::AudioEngine::instance().initialize(config));
    VNX::registerAdvancedActions(
        VNX::ActionManager::instance(), &VNX::AudioEngine::instance());

    for (const char* action : kResultBearingActions) {
        const auto result = run(action);
        EXPECT_FALSE(result.success) << action;
        if (QString::fromLatin1(action) == QStringLiteral("seek_action")) {
            EXPECT_EQ(result.errorCode, QStringLiteral("STALE_HANDLE")) << action;
            EXPECT_EQ(result.data.value(QStringLiteral("engine_result")).toInteger(), -201)
                << action;
        } else {
            EXPECT_EQ(result.errorCode, QStringLiteral("NOT_AVAILABLE")) << action;
            EXPECT_EQ(result.data.value(QStringLiteral("engine_result")).toInteger(), -303)
                << action;
        }
    }
}

TEST_F(AdvancedActionResultTest, InputErrorsRemainDistinctFromEngineRefusals)
{
    VNX::registerAdvancedActions(
        VNX::ActionManager::instance(), &VNX::AudioEngine::instance());

    const auto missingObject = VNX::ActionManager::instance().execute(
        QStringLiteral("set_game_object_volume"),
        {{QStringLiteral("game_object_id"), 0}}, VNX::ActionSource::IPC);
    EXPECT_FALSE(missingObject.success);
    EXPECT_EQ(missingObject.errorCode, QStringLiteral("INVALID_PARAM"));

    const auto markerSeek = VNX::ActionManager::instance().execute(
        QStringLiteral("seek_action"),
        {{QStringLiteral("name"), QStringLiteral("ActionTest")},
         {QStringLiteral("snap_to_marker"), true}},
        VNX::ActionSource::IPC);
    EXPECT_FALSE(markerSeek.success);
    EXPECT_EQ(markerSeek.errorCode, QStringLiteral("UNSUPPORTED"));
}

TEST_F(AdvancedActionResultTest, DecimalStringPreservesFullWidthGameObjectId)
{
    VNX::registerAdvancedActions(
        VNX::ActionManager::instance(), &VNX::AudioEngine::instance());
    const QJsonObject args{
        {QStringLiteral("game_object_id"),
         QStringLiteral("18446744073709551615")},
        {QStringLiteral("volume"), 0.5},
    };

    const auto result = VNX::ActionManager::instance().execute(
        QStringLiteral("set_game_object_volume"), args, VNX::ActionSource::IPC);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("NOT_INIT"));
    EXPECT_EQ(result.data.value(QStringLiteral("engine_result")).toInteger(), -303);
    EXPECT_EQ(result.data.value(QStringLiteral("game_object_id")).toString(),
              QStringLiteral("18446744073709551615"));
}

TEST_F(AdvancedActionResultTest, MapsEveryEngineResultWithoutLosingContext)
{
    using namespace HarkenEditor::Bridge;
    struct FailureCase
    {
        EngineResult result;
        const char* errorCode;
    };
    const std::array<FailureCase, 11> failures{{
        {kEngineInvalidArg, "INVALID_PARAM"},
        {kEngineUnknownId, "UNKNOWN_ID"},
        {kEngineStaleHandle, "STALE_HANDLE"},
        {kEngineNoVoice, "NO_VOICE"},
        {kEngineBackpressure, "BACKPRESSURE"},
        {kEngineInUse, "IN_USE"},
        {kEngineSendCycle, "SEND_CYCLE"},
        {kEngineCapabilityUnavailable, "NOT_AVAILABLE"},
        {kEngineRuntimeRecreateRequired, "RUNTIME_RECREATE_REQUIRED"},
        {kEngineDeviceError, "DEVICE_ERROR"},
        {static_cast<EngineResult>(-9999), "ENGINE_ERROR"},
    }};

    const QJsonObject context{{QStringLiteral("action"), QStringLiteral("ActionTest")}};
    const auto success = VNX::AdvancedActionDetail::fromEngineResult(
        true, kEngineOk, context, QStringLiteral("Test operation"));
    EXPECT_TRUE(success.success);
    EXPECT_EQ(success.data.value(QStringLiteral("action")).toString(),
              QStringLiteral("ActionTest"));
    EXPECT_EQ(success.data.value(QStringLiteral("engine_result")).toInteger(), 0);

    for (const auto& failure : failures) {
        const auto result = VNX::AdvancedActionDetail::fromEngineResult(
            true, failure.result, context, QStringLiteral("Test operation"));
        EXPECT_FALSE(result.success) << failure.errorCode;
        EXPECT_EQ(result.errorCode, QString::fromLatin1(failure.errorCode));
        EXPECT_EQ(result.data.value(QStringLiteral("action")).toString(),
                  QStringLiteral("ActionTest"));
        EXPECT_EQ(result.data.value(QStringLiteral("engine_result")).toInteger(),
                  static_cast<int32_t>(failure.result));
        EXPECT_TRUE(result.errorMessage.contains(QStringLiteral("Test operation")));
    }

    const auto notInitialized = VNX::AdvancedActionDetail::fromEngineResult(
        false, kEngineNotInit, context, QStringLiteral("Test operation"));
    EXPECT_FALSE(notInitialized.success);
    EXPECT_EQ(notInitialized.errorCode, QStringLiteral("NOT_INIT"));
    EXPECT_EQ(notInitialized.data.value(QStringLiteral("engine_result")).toInteger(), -303);

    const auto unavailable = VNX::AdvancedActionDetail::fromEngineResult(
        true, kEngineNotInit, context, QStringLiteral("Test operation"));
    EXPECT_FALSE(unavailable.success);
    EXPECT_EQ(unavailable.errorCode, QStringLiteral("NOT_AVAILABLE"));
    EXPECT_TRUE(unavailable.errorMessage.contains(QStringLiteral("Test operation")));
    EXPECT_EQ(unavailable.data.value(QStringLiteral("engine_result")).toInteger(), -303);
}

} // namespace
