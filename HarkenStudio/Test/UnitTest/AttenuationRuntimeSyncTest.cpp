#include "UnitTest/StudioObjectIdTestSupport.h"

#include "RuntimeBridge/ProjectSyncBridge.h"

#include "harken/Harken.h"

#include <gtest/gtest.h>

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
    const PackAttenuation att = linearAttenuation(0x100001001ull, 100.0f, 2);
    ASSERT_TRUE(HarkenEditor::Bridge::syncAttenuation(att));

    const VnAttenuationDesc probe = probeDesc();
    EXPECT_EQ(VN_OK, Vn_ReplaceAttenuation(harkentest::ToRuntime(att.id), &probe));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_ReplaceAttenuation(harkentest::ToRuntime(harkentest::StudioObjectId(0x1001)), &probe));
}
