#include "UnitTest/StudioObjectIdTestSupport.h"
#include "AttenuationStore.h"

#include "RuntimeBridge/ProjectSyncBridge.h"

#include "harken/Harken.h"

#include <gtest/gtest.h>

#include <QString>

namespace {

using HarkenEditor::Bridge::PackAttenuation;

VnAttenuationDesc probeDesc()
{
    VnAttenuationDesc desc{};
    desc.structSize = sizeof(VnAttenuationDesc);
    desc.rolloff = VN_ROLLOFF_LINEAR;
    desc.minDistance = 1.0f;
    desc.maxDistance = 100.0f;
    desc.airAbsorptionNearHz = 20000.0f;
    desc.airAbsorptionFarHz = 20000.0f;
    return desc;
}

PackAttenuation linearAttenuation(uint64_t id, float maxDistance, int pointCount)
{
    PackAttenuation att;
    att.id = harkentest::StudioObjectId(id);
    att.maxDistance = maxDistance;

    PackAttenuation::Slot slot;
    slot.slot = 0;
    for (int i = 0; i < pointCount; ++i)
    {
        const float t = pointCount > 1
            ? static_cast<float>(i) / static_cast<float>(pointCount - 1)
            : 0.0f;
        slot.points.push_back({t * maxDistance, -96.0f * t});
    }
    for (int i = 0; i + 1 < pointCount; ++i)
        slot.handles.push_back({slot.points[i].distance, slot.points[i].value,
                                slot.points[i + 1].distance,
                                slot.points[i + 1].value});
    att.curveSlots.push_back(std::move(slot));
    return att;
}

class AttenuationRuntimeSync : public ::testing::Test {
protected:
    void SetUp() override
    {
        VnConfig config{};
        config.structSize = sizeof(VnConfig);
        config.deviceMode = VN_DEVICE_NULL;
        ASSERT_EQ(VN_OK, Vn_Init(&config));
    }

    void TearDown() override
    {
        Vn_Term();
    }
};

} // namespace

TEST_F(AttenuationRuntimeSync, RegistersFullCurveUnderGivenId)
{
    const PackAttenuation att = linearAttenuation(0x100001001ull, 100.0f, 8);
    ASSERT_TRUE(HarkenEditor::Bridge::syncAttenuation(att));

    const VnAttenuationDesc probe = probeDesc();
    EXPECT_EQ(VN_OK, Vn_ReplaceAttenuation(harkentest::ToRuntime(att.id), &probe));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_ReplaceAttenuation(harkentest::ToRuntime(harkentest::StudioObjectId(0x1001)), &probe));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_ReplaceAttenuation(harkentest::ToRuntime(harkentest::StudioObjectId(0x1002)), &probe));
}

TEST_F(AttenuationRuntimeSync, ReplacesProfileOnRepeatedSync)
{
    EXPECT_TRUE(HarkenEditor::Bridge::syncAttenuation(
        linearAttenuation(0x2001, 100.0f, 4)));
    EXPECT_TRUE(HarkenEditor::Bridge::syncAttenuation(
        linearAttenuation(0x2001, 250.0f, 8)));
}

TEST_F(AttenuationRuntimeSync, RefusesZeroId)
{
    EXPECT_FALSE(HarkenEditor::Bridge::syncAttenuation(
        linearAttenuation(0, 100.0f, 2)));
}

TEST_F(AttenuationRuntimeSync, RefusesNonPositiveMaxDistance)
{
    EXPECT_FALSE(HarkenEditor::Bridge::syncAttenuation(
        linearAttenuation(0x3001, 0.0f, 2)));
    EXPECT_FALSE(HarkenEditor::Bridge::syncAttenuation(
        linearAttenuation(0x3001, -50.0f, 2)));

    const VnAttenuationDesc probe = probeDesc();
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_ReplaceAttenuation(harkentest::ToRuntime(harkentest::StudioObjectId(0x3001)), &probe));
}

TEST_F(AttenuationRuntimeSync, RefusesCurveBeyondRuntimeCapacity)
{
    EXPECT_FALSE(HarkenEditor::Bridge::syncAttenuation(
        linearAttenuation(0x4001, 100.0f, 9)));

    const VnAttenuationDesc probe = probeDesc();
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_ReplaceAttenuation(harkentest::ToRuntime(harkentest::StudioObjectId(0x4001)), &probe));
}

TEST_F(AttenuationRuntimeSync, MismatchedHandlesDegradeToDisabledCurve)
{
    PackAttenuation att;
    att.id = harkentest::StudioObjectId(0x5001);
    att.maxDistance = 100.0f;
    PackAttenuation::Slot slot;
    slot.slot = 0;
    slot.points.push_back({0.0f, 0.0f});
    slot.points.push_back({0.0f, -96.0f});
    slot.handles.push_back({0.0f, 0.0f, 0.0f, -96.0f});
    att.curveSlots.push_back(slot);

    EXPECT_FALSE(HarkenEditor::Bridge::syncAttenuation(att));

    att.curveSlots[0].handles.clear();
    EXPECT_TRUE(HarkenEditor::Bridge::syncAttenuation(att));
}

TEST_F(AttenuationRuntimeSync, RegistersSinglePointCurve)
{
    EXPECT_TRUE(HarkenEditor::Bridge::syncAttenuation(
        linearAttenuation(0x6001, 100.0f, 1)));
}

TEST_F(AttenuationRuntimeSync, RegistersProfileWithoutVolumeSlot)
{
    PackAttenuation att;
    att.id = harkentest::StudioObjectId(0x7001);
    att.maxDistance = 42.0f;
    EXPECT_TRUE(HarkenEditor::Bridge::syncAttenuation(att));
}

TEST_F(AttenuationRuntimeSync, StoreSyncRegistersUnderStableId)
{
    VNX::AttenuationSnapshot def;
    def.name = QStringLiteral("StoreSyncAttenuation");
    def.maxDistance = 80.0f;
    auto& volume = def.ensureDimension(VNX::AttenuationDimension::Volume);
    volume.curve.push_back({0.0f, 0.0f});
    volume.curve.push_back({80.0f, -96.0f});
    volume.handles.push_back({0.0f, 0.0f, 80.0f, -96.0f});

    VNX::ProjectData project;
    VNX::AttenuationStore::ensureGuid(def);
    auto* component = VNX::AttenuationStore::create(&project, def);
    ASSERT_NE(component, nullptr);
    EXPECT_EQ(component->hostItem()->runtimeId(), VNX::runtimeObjectId(def.guid));
    ASSERT_TRUE(VNX::AttenuationStore::syncToRuntime(project, *component));

    const VnAttenuationDesc probe = probeDesc();
    EXPECT_EQ(VN_OK, Vn_ReplaceAttenuation(harkentest::ToRuntime(VNX::AttenuationStore::runtimeId(project, *component)),
                                           &probe));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_ReplaceAttenuation(harkentest::ToRuntime(harkentest::StudioObjectId(0x1001)), &probe));
}

TEST_F(AttenuationRuntimeSync, StoreSyncSkipsPitchDimension)
{
    VNX::AttenuationSnapshot def;
    def.name = QStringLiteral("PitchOnlyAttenuation");
    def.maxDistance = 100.0f;
    auto& pitch = def.ensureDimension(VNX::AttenuationDimension::Pitch);
    for (int i = 0; i < 9; ++i)
        pitch.curve.push_back({static_cast<float>(i) * 10.0f, 0.0f});

    VNX::ProjectData project;
    VNX::AttenuationStore::ensureGuid(def);
    auto* component = VNX::AttenuationStore::create(&project, def);
    ASSERT_NE(component, nullptr);
    EXPECT_TRUE(VNX::AttenuationStore::syncToRuntime(project, *component));

    auto& volume = def.ensureDimension(VNX::AttenuationDimension::Volume);
    for (int i = 0; i < 9; ++i)
        volume.curve.push_back({static_cast<float>(i) * 10.0f,
                                static_cast<float>(i) * -1.0f});

    component->dimensions = def.dimensions;
    EXPECT_FALSE(VNX::AttenuationStore::syncToRuntime(project, *component));
}

TEST(AttenuationRuntimeId, UsesProjectItemIdentityAcrossNameChanges)
{
    VNX::ProjectData project;
    VNX::AttenuationSnapshot def;
    def.name = QStringLiteral("PreviewAttenuation");
    VNX::VAttenuation unbound;
    EXPECT_EQ(VNX::AttenuationStore::runtimeId(project, unbound), harkentest::StudioObjectId(0u));
    VNX::AttenuationStore::ensureGuid(def);
    auto* component = VNX::AttenuationStore::create(&project, def);
    ASSERT_NE(component, nullptr);
    auto* item = component->hostItem();
    ASSERT_NE(item->runtimeId(), harkentest::StudioObjectId(0u));
    EXPECT_EQ(VNX::AttenuationStore::runtimeId(project, *component), item->runtimeId());

    item->name = QStringLiteral("RenamedAttenuation");
    EXPECT_EQ(component->snapshot().name, item->name);
    EXPECT_EQ(VNX::AttenuationStore::runtimeId(project, *component), item->runtimeId());

    VNX::ProjectData otherProject;
    EXPECT_EQ(VNX::AttenuationStore::runtimeId(otherProject, *component), harkentest::StudioObjectId(0u));
}
