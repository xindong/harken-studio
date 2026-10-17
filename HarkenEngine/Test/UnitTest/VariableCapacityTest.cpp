#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Common/TestUtil.h"
#include "Common/VariableOutcomeCapture.h"
#include "harken/param/VariableService.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace {

using harkentest::EngineSession;
using harkentest::InstallDefinitionsWorld;
using harkentest::MakeNullConfig;
using harkentest::MakeVariableDefinition;

std::vector<harken::VariableDefinitionV1> SmoothBank(std::uint32_t count) {
    std::vector<harken::VariableDefinitionV1> definitions;
    definitions.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        definitions.push_back(MakeVariableDefinition(
            harkentest::ObjectId(i + 1), harken::VariableKindV1::Smooth, -1.0f, 1.0f, 0.0f));
    }
    return definitions;
}

VnVariableBatchDescV1 MakeBatch(const VnVariableWriteV1* writes,
                                std::uint32_t count) {
    VnVariableBatchDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.writeCount = count;
    desc.writes = writes;
    return desc;
}

float ReadValue(harken::Engine* engine, VnVariableId id) {
    float value = 99.0f;
    EXPECT_TRUE(engine->variables.CurrentValue(id, &value));
    return value;
}

TEST(VariableCapacity, All1024EmitterKeysAreAcceptedBeforeUpdate) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    const auto definitions = SmoothBank(8);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallDefinitionsWorld(
                  session.Engine(), definitions.data(),
                  static_cast<std::uint32_t>(definitions.size()))
                  .status);
    std::vector<VnEmitterHandle> emitters(128);
    for (auto& emitter : emitters) {
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
        for (std::uint32_t variable = 1; variable <= 8; ++variable) {
            ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, harkentest::ObjectId(variable),
                static_cast<float>(variable) / 8.0f));
        }
    }
    EXPECT_EQ(1024u, engine->variables.PendingCount());
    EXPECT_EQ(0u, engine->variables.RefusalCount());
    EXPECT_FLOAT_EQ(0.0f, ReadValue(engine, harkentest::ObjectId(1)));
    for (unsigned quantum = 0; quantum < 128 && engine->variables.PendingCount() != 0; ++quantum) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    ASSERT_EQ(0u, engine->variables.PendingCount());
    EXPECT_EQ(1024u, engine->variables.AppliedCount());
    for (auto emitter : emitters) {
        for (std::uint32_t variable = 1; variable <= 8; ++variable) {
            float value = 0.0f;
            ASSERT_TRUE(engine->variables.CurrentValue(emitter, harkentest::ObjectId(variable), &value));
            EXPECT_FLOAT_EQ(static_cast<float>(variable) / 8.0f, value);
        }
    }
}

TEST(VariableCapacity, SaturatedRevisionsAllowNetZeroReplacementAndPreserveFailedBatch) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    const auto definitions = SmoothBank(8);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallDefinitionsWorld(
                  session.Engine(), definitions.data(),
                  static_cast<std::uint32_t>(definitions.size()))
                  .status);
    const auto capacity = engine->variables.PendingCapacity();
    auto observation = harken::ObserveVariableCallAt(engine->variables.CurrentWorldGeneration(),
        engine->variables.CurrentTransportEpoch(), 256, 0, 0);
    for (std::uint32_t i = 0; i < capacity; ++i) {
        observation.requestedEffectiveFrame = static_cast<std::uint64_t>(i + 1) * 256;
        ASSERT_EQ(VN_OK, engine->variables.AcceptGlobal(harkentest::ObjectId(1), 0.5f,
            observation, engine->params, engine->bindings, engine->busTargets).result);
    }
    const auto issued = engine->logicCommits.IssuedCount();
    auto extra = observation;
    extra.requestedEffectiveFrame += 256;
    EXPECT_EQ(VN_VARIABLE_TRANSACTION_EXHAUSTED, engine->variables.AcceptGlobal(
        harkentest::ObjectId(1), 0.25f, extra, engine->params, engine->bindings, engine->busTargets).result);
    EXPECT_EQ(issued, engine->logicCommits.IssuedCount());
    harken::PreparedVariableWriteToken privateWrite;
    EXPECT_EQ(VN_VARIABLE_TRANSACTION_EXHAUSTED, engine->variables.PrepareGlobalWrite(
        harkentest::ObjectId(1), 0.25f, observation, 0, &privateWrite).result);
    const VnVariableWriteV1 invalid[] = {
        {harkentest::ObjectId(1), 0.25f, 0},
        {harkentest::ObjectId(2), std::numeric_limits<float>::quiet_NaN(), 0}};
    auto desc = MakeBatch(invalid, 2);
    VnVariableTicketV1 failed{};
    EXPECT_NE(VN_OK, Vn_SetValues(&desc, &failed));
    EXPECT_EQ(0u, failed.sequence);
    EXPECT_EQ(issued, engine->logicCommits.IssuedCount());
    EXPECT_EQ(capacity, engine->variables.PendingCount());
    EXPECT_EQ(0u, engine->variables.SupersededCount());
    const auto replacement = engine->variables.AcceptGlobal(harkentest::ObjectId(1), 0.75f,
        observation, engine->params, engine->bindings, engine->busTargets);
    ASSERT_EQ(VN_OK, replacement.result);
    engine->CompleteVariableOperation();
    EXPECT_EQ(issued + 1, engine->logicCommits.IssuedCount());
    EXPECT_EQ(capacity, engine->variables.PendingCount());
    EXPECT_EQ(1u, engine->variables.SupersededCount());
    EXPECT_FLOAT_EQ(0.0f, ReadValue(engine, harkentest::ObjectId(1)));
    harken::VariableReadCutRef cut{};
    ASSERT_TRUE(engine->variables.CaptureReadCut(0, observation.requestedEffectiveFrame,
        replacement.ticket.sequence, UINT32_MAX, &cut));
    float observedValue = 0.0f;
    ASSERT_TRUE(engine->variables.ReadCutValue(cut, harkentest::ObjectId(1), &observedValue));
    EXPECT_FLOAT_EQ(0.75f, observedValue);
    EXPECT_EQ(VN_VARIABLE_TRANSACTION_EXHAUSTED, engine->variables.AcceptGlobal(
        harkentest::ObjectId(1), 0.5f, observation, engine->params, engine->bindings, engine->busTargets).result);
    EXPECT_EQ(issued + 1, engine->logicCommits.IssuedCount());
    EXPECT_TRUE(engine->variables.ReleaseReadCut(cut));
}

TEST(VariableCapacity, SaturatedEightMemberReplacementUsesNetCreditsAtomically) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    const auto definitions = SmoothBank(8);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        InstallDefinitionsWorld(engine, definitions.data(),
            static_cast<std::uint32_t>(definitions.size())).status);
    const auto capacity = engine->variables.PendingCapacity();
    auto observation = harken::ObserveVariableCallAt(engine->variables.CurrentWorldGeneration(),
        engine->variables.CurrentTransportEpoch(), 256, 0, 0);
    for (std::uint32_t i = 0; i < capacity - 8; ++i) {
        observation.requestedEffectiveFrame = static_cast<std::uint64_t>(i + 1) * 256;
        ASSERT_EQ(VN_OK, engine->variables.AcceptGlobal(harkentest::ObjectId(1), 0.25f,
            observation, engine->params, engine->bindings, engine->busTargets).result);
    }
    observation.requestedEffectiveFrame = static_cast<std::uint64_t>(capacity + 1) * 256;
    std::array<VnVariableWriteV1, 8> writes{};
    for (std::uint32_t i = 0; i < writes.size(); ++i) {
        writes[i] = {harkentest::ObjectId(i + 1), 0.75f, 0};
        ASSERT_EQ(VN_OK, engine->variables.AcceptGlobal(writes[i].variableId, 0.25f,
            observation, engine->params, engine->bindings, engine->busTargets).result);
    }
    ASSERT_EQ(capacity, engine->variables.PendingCount());
    const auto issued = engine->logicCommits.IssuedCount();
    harkentest::VariableOutcomeCapture outcomes(*engine);
    const auto replacement = engine->variables.AcceptGlobalBatch(writes.data(),
        static_cast<std::uint32_t>(writes.size()), observation,
        engine->params, engine->bindings, engine->busTargets);
    ASSERT_EQ(VN_OK, replacement.result);
    EXPECT_EQ(issued + 1, replacement.ticket.sequence);
    EXPECT_EQ(capacity, engine->variables.PendingCount());
    engine->CompleteVariableOperation();
    EXPECT_EQ(8u, outcomes.Count());
    for (std::uint32_t i = 0; i < writes.size(); ++i) {
        harken::VariableOutcomeRecord outcome{};
        ASSERT_TRUE(outcomes.Pop(&outcome));
        EXPECT_EQ(harken::VariableOutcomeKind::Superseded, outcome.outcome);
        EXPECT_EQ(issued - 7 + i, outcome.acceptedTicket.sequence);
        EXPECT_EQ(replacement.ticket.sequence, outcome.relatedTicket.sequence);
        EXPECT_FLOAT_EQ(0.0f, ReadValue(engine, writes[i].variableId));
    }
    EXPECT_FALSE(outcomes.Overflowed());
}

void RunReplacementSeries(bool traced) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallSmoothDefinitions(engine, {harkentest::ObjectId(1)}).status);
    const std::string tracePath = std::string(HARKEN_TEMP_DIR) + "/variable_replacement_capacity.vtrc";
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    if (traced) ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
    harkentest::VariableOutcomeCapture outcomes(*engine, 10000);
    const auto first = engine->logicCommits.IssuedCount() + 1;
    for (unsigned i = 0; i < 10000; ++i) {
        ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(1), i % 2 ? 0.25f : 0.75f));
        ASSERT_EQ(1u, engine->variables.PendingCount());
        ASSERT_FALSE(engine->variables.HasStagedOutcomes());
    }
    EXPECT_EQ(9999u, engine->variables.SupersededCount());
    EXPECT_EQ(9999u, outcomes.Count());
    harken::VariableOutcomeRecord record{};
    for (unsigned i = 0; i < 9999; ++i) {
        ASSERT_TRUE(outcomes.Pop(&record));
        EXPECT_EQ(first + i, record.acceptedTicket.sequence);
        EXPECT_EQ(harken::VariableOutcomeKind::Superseded, record.outcome);
        EXPECT_EQ(first + i + 1, record.relatedTicket.sequence);
    }
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    EXPECT_FLOAT_EQ(0.25f, ReadValue(engine, harkentest::ObjectId(1)));
    EXPECT_EQ(1u, engine->variables.AppliedCount());
    ASSERT_TRUE(outcomes.Pop(&record));
    EXPECT_EQ(harken::VariableOutcomeKind::Applied, record.outcome);
    EXPECT_FALSE(outcomes.Overflowed());
    if (traced) {
        ASSERT_EQ(VN_OK, Vn_TraceStop());
        std::filesystem::remove(tracePath);
    }
}

TEST(VariableCapacity, TenThousandReplacementsWithoutUpdateDrainEachOperation) {
    RunReplacementSeries(false);
}

TEST(VariableCapacity, TracedTenThousandReplacementsWithoutUpdateDrainEachOperation) {
    RunReplacementSeries(true);
}

void RunCallbackReplacementSeries(bool traced) {
    struct CallbackState { unsigned calls = 0; unsigned accepted = 0; } state;
    auto config = MakeNullConfig(4, 256, 48000);
    config.notificationUserData = &state;
    config.onNotification = [](const VnNotification* notification, void* context) {
        auto& callback = *static_cast<CallbackState*>(context);
        if (notification->type != VN_NOTIFY_VOICE_ENDED || callback.calls++ != 0) return;
        for (unsigned i = 0; i < 10000; ++i) {
            if (Vn_SetValue(harkentest::ObjectId(1), i % 2 ? 0.25f : 0.75f) == VN_OK)
                ++callback.accepted;
        }
    };
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallSmoothDefinitions(engine, {harkentest::ObjectId(1)}).status);
    const std::vector<float> pcm(512, 0.25f);
    auto pcmDesc = harkentest::MonoDesc(pcm, 48000);
    ASSERT_EQ(VN_OK, engine->registry.RegisterPCM(harkentest::ObjectId(2), &pcmDesc));
    auto play = harkentest::MakePlayDesc(harkentest::ObjectId(2));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const std::string tracePath = std::string(HARKEN_TEMP_DIR) + "/variable_callback_capacity.vtrc";
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    if (traced) ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
    for (unsigned quantum = 0; quantum < 16; ++quantum) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    EXPECT_GT(state.calls, 0u);
    EXPECT_EQ(10000u, state.accepted);
    EXPECT_EQ(9999u, engine->variables.SupersededCount());
    EXPECT_EQ(0u, engine->variables.RefusalCount());
    EXPECT_FALSE(engine->variables.HasStagedOutcomes());
    EXPECT_FLOAT_EQ(0.25f, ReadValue(engine, harkentest::ObjectId(1)));
    if (traced) {
        ASSERT_EQ(VN_OK, Vn_TraceStop());
        std::filesystem::remove(tracePath);
    }
}

TEST(VariableCapacity, CallbackWritesDrainIndividuallyWithinOuterUpdate) {
    RunCallbackReplacementSeries(false);
}

TEST(VariableCapacity, TracedCallbackWritesDrainIndividuallyWithinOuterUpdate) {
    RunCallbackReplacementSeries(true);
}

} // namespace
