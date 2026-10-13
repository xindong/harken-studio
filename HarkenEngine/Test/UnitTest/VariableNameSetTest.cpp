#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include <gtest/gtest.h>

TEST(VariableNameSet, NamesRequireAnActivePackEvenWhenGuidDefinitionIsInstalled) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    const VnObjectId id = harkentest::ObjectId(7);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallSmoothDefinitions(session.Engine(), {id}, 0.0f, 1.0f).status);

    VnVariableResolved resolved{};
    resolved.structSize = sizeof(resolved);
    VnPostActionByNameDesc desc{};
    desc.structSize = sizeof(desc);
    desc.utf8Name = "Wood";
    EXPECT_EQ(VN_CAPABILITY_UNAVAILABLE, Vn_ResolveVariableByName(&desc, &resolved));
    EXPECT_EQ(VN_CAPABILITY_UNAVAILABLE, Vn_SetValueByName("Wood", 0.75f));
    ASSERT_EQ(VN_OK, Vn_SetValue(id, 0.25f));
    harkentest::AdvanceVariableWrite(session.Engine());
    VnVariableQueryDescV1 query{};
    query.structSize = sizeof(query);
    query.variableId = id;
    VnVariableQueryResultV1 value{};
    value.structSize = sizeof(value);
    ASSERT_EQ(VN_OK, Vn_QueryVariable(&query, &value));
    EXPECT_FLOAT_EQ(0.25f, value.currentValue);

    for (const char* name : {"", "Bad Name", "Bad-Name", "Bad.Name", "1Name", "_Name"}) {
        desc.utf8Name = name;
        EXPECT_EQ(VN_INVALID_ARG, Vn_ResolveVariableByName(&desc, &resolved)) << name;
    }
    desc.utf8Name = "Wood";
    EXPECT_EQ(VN_INVALID_ARG, Vn_ResolveVariableByName(nullptr, &resolved));
    EXPECT_EQ(VN_INVALID_ARG, Vn_ResolveVariableByName(&desc, nullptr));
    desc.emitter = 1;
    EXPECT_EQ(VN_INVALID_ARG, Vn_ResolveVariableByName(&desc, &resolved));
    desc.emitter = 0;
    desc.flags = 1;
    EXPECT_EQ(VN_INVALID_ARG, Vn_ResolveVariableByName(&desc, &resolved));
}
