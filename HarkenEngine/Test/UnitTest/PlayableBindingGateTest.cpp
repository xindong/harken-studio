
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "Engine.h"
#include "harken/core/CurrentWorld.h"
#include "harken/core/PlayablePropertyCut.h"
#include "harken/param/ParamTargets.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using harkentest::EngineSession;
using harkentest::MakeDC;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;
using harkentest::Peak;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;
constexpr VnObjectId kVariable = harkentest::ObjectId(7);
constexpr VnObjectId kBinding = harkentest::ObjectId(9001);

VnBindingDesc MakeVoiceVolumeBinding(float y0, float y1)
{
    VnBindingDesc desc{};
    desc.structSize = sizeof(VnBindingDesc);
    desc.variableId = harkentest::ObjectId(kVariable);
    desc.targetScopeKind = VN_TARGET_SCOPE_VOICE;
    desc.targetId = harken::kTargetVoiceVolume;
    desc.curve.structSize = sizeof(VnCurveDesc);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {0.0f, y0};
    desc.curve.points[1] = {1.0f, y1};
    desc.curve.handles[0] = harkentest::EndpointHandles(
        desc.curve.points[0], desc.curve.points[1]);
    return desc;
}

void RegisterLeaf(VnSoundId id)
{
    static const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(id, &desc));
}


}

TEST(PlayableBindingGate, VoiceAttachmentPlayIsNotCapabilityUnavailable)
{
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(engine, {kVariable}, 0.0f, 1.0f).status);
    RegisterLeaf(harkentest::ObjectId(21));

    const VnBindingDesc desc = MakeVoiceVolumeBinding(0.0f, -6.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(kBinding, &desc));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(attach);
    attach.scopeKind = VN_TARGET_SCOPE_VOICE;
    attach.objectId = harkentest::ObjectId(21);
    ASSERT_EQ(VN_OK, Vn_AttachBinding(kBinding, &attach));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(21));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    EXPECT_NE(handle, 0u);
}



TEST(PlayableBindingGate, PitchContributionIsCentNotSemitone)
{
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(engine, {kVariable}, 0.0f, 1.0f).status);
    RegisterLeaf(harkentest::ObjectId(21));

    VnBindingDesc desc{};
    desc.structSize = sizeof(VnBindingDesc);
    desc.variableId = harkentest::ObjectId(kVariable);
    desc.targetScopeKind = VN_TARGET_SCOPE_VOICE;
    desc.targetId = harken::kTargetVoicePitch;
    desc.curve.structSize = sizeof(VnCurveDesc);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {0.0f, 0.0f};
    desc.curve.points[1] = {1.0f, 1200.0f};
    desc.curve.handles[0] = harkentest::EndpointHandles(
        desc.curve.points[0], desc.curve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(kBinding, &desc));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(attach);
    attach.scopeKind = VN_TARGET_SCOPE_VOICE;
    attach.objectId = harkentest::ObjectId(21);
    ASSERT_EQ(VN_OK, Vn_AttachBinding(kBinding, &attach));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(21));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_SetValue(kVariable, 1.0f));
    harkentest::AdvanceVariableWrite(engine);

    const VnVoiceHandle render = harkentest::RenderHandle(engine, handle);
    ASSERT_NE(render, 0u);
    EXPECT_NEAR(engine->params.ResolvedValue(render, harken::kTargetVoicePitch),
                1200.0f, 0.01f);
}
