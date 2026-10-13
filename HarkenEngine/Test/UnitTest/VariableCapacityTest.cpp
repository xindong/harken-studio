#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Common/TestUtil.h"
#include "harken/param/VariableService.h"

#include <gtest/gtest.h>

#include <cstdint>
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

TEST(VariableCapacity, Pending257IsTransactionExhausted) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto definitions =
        SmoothBank(harken::kPendingVariableWriteCapacity);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallDefinitionsWorld(
                  session.Engine(), definitions.data(),
                  static_cast<std::uint32_t>(definitions.size()))
                  .status);
    for (std::uint32_t i = 0; i < harken::kPendingVariableWriteCapacity; ++i) {
        ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(i + 1), 0.5f));
    }
    EXPECT_EQ(harken::kPendingVariableWriteCapacity,
              session.Engine()->variables.PendingCount());
    EXPECT_FLOAT_EQ(0.0f, ReadValue(session.Engine(), harkentest::ObjectId(1)));
    EXPECT_EQ(VN_VARIABLE_TRANSACTION_EXHAUSTED, Vn_SetValue(harkentest::ObjectId(1), 0.25f));
    EXPECT_EQ(harken::kPendingVariableWriteCapacity,
              session.Engine()->variables.PendingCount());
    EXPECT_FLOAT_EQ(0.0f, ReadValue(session.Engine(), harkentest::ObjectId(1)));
}

TEST(VariableCapacity, StartingPointsStillAdmitThenRefuse) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto definitions =
        SmoothBank(harken::kPendingVariableWriteCapacity);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              InstallDefinitionsWorld(
                  session.Engine(), definitions.data(),
                  static_cast<std::uint32_t>(definitions.size()))
                  .status);
    VnVariableWriteV1 one{};
    one.variableId = harkentest::ObjectId(1);
    one.value = 0.5f;
    const auto oneDesc = MakeBatch(&one, 1);
    ASSERT_EQ(VN_OK, Vn_SetValues(&oneDesc, nullptr));
    VnVariableWriteV1 eight[8]{};
    for (std::uint32_t i = 0; i < 8; ++i) {
        eight[i].variableId = harkentest::ObjectId(i + 2);
        eight[i].value = 0.5f;
    }
    const auto eightDesc = MakeBatch(eight, 8);
    ASSERT_EQ(VN_OK, Vn_SetValues(&eightDesc, nullptr));
    for (std::uint32_t id = 10; id <= harken::kPendingVariableWriteCapacity;
         ++id) {
        ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(id), 0.5f));
    }
    EXPECT_EQ(harken::kPendingVariableWriteCapacity,
              session.Engine()->variables.PendingCount());
    EXPECT_EQ(harken::kPendingVariableWriteCapacity,
              session.Engine()->variables.AcceptedCount());
    EXPECT_EQ(0u, session.Engine()->variables.AppliedCount());
    EXPECT_EQ(VN_VARIABLE_TRANSACTION_EXHAUSTED, Vn_SetValue(harkentest::ObjectId(1), 0.25f));
    EXPECT_EQ(1u, session.Engine()->variables.RefusalCount());
    EXPECT_EQ(harken::kPendingVariableWriteCapacity,
              session.Engine()->variables.PendingCount());
    EXPECT_FLOAT_EQ(0.0f, ReadValue(session.Engine(), harkentest::ObjectId(1)));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    EXPECT_EQ(0u, session.Engine()->variables.PendingCount());
    EXPECT_EQ(harken::kPendingVariableWriteCapacity,
              session.Engine()->variables.AppliedCount());
    EXPECT_FLOAT_EQ(0.5f, ReadValue(session.Engine(), harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(1), 0.25f));
    EXPECT_EQ(harken::kPendingVariableWriteCapacity + 1u,
              session.Engine()->variables.AcceptedCount());
    EXPECT_EQ(1u, session.Engine()->variables.RefusalCount());
}

} // namespace
