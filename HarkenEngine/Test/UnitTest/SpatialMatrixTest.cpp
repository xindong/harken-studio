#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"
#include "harken/core/StreamRing.h"
#include "harken/render/SpatialMatrix.h"
#include "harken/param/PropertyPublisher.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kCenter = 0.70710678118654752440;
constexpr VnVoiceHandle kVoice = 1ULL << 32;

std::array<double, 8> ReferencePan(std::uint32_t layout, double pan) {
    std::array<double, 8> result{};
    if (layout == VN_CHANNEL_LAYOUT_MONO) { result[0] = 1; return result; }
    const bool center = layout == VN_CHANNEL_LAYOUT_5_1 || layout == VN_CHANNEL_LAYOUT_7_1;
    const double q = center ? std::abs(pan) : (pan + 1) / 2;
    if (center) {
        result[2] = std::cos(q * kPi / 2);
        result[pan < 0 ? 0 : 1] = std::sin(q * kPi / 2);
    } else {
        result[0] = std::cos(q * kPi / 2);
        result[1] = std::sin(q * kPi / 2);
    }
    return result;
}

std::array<std::array<double, 8>, 2> ReferenceStereo(
    std::uint32_t layout, double pan, double width) {
    std::array<std::array<double, 8>, 2> columns{{
        ReferencePan(layout, (1 - width) * pan - width),
        ReferencePan(layout, (1 - width) * pan + width)}};
    // Normalize the summed dual-mono image to total power two, using
    // analytic double-precision panning rather than the Runtime table.
    double power = 0;
    for (std::uint32_t ch = 0; ch < 8; ++ch) {
        const double sum = columns[0][ch] + columns[1][ch];
        power += sum * sum;
    }
    const double scale = std::sqrt(2 / power);
    for (auto& column : columns)
        for (double& gain : column) gain *= scale;
    return columns;
}

TEST(SpatialMatrix, CompleteMatricesMatchIndependentDoubleReference) {
    harken::BiquadTrigTable trig;
    for (auto layout : {VN_CHANNEL_LAYOUT_MONO, VN_CHANNEL_LAYOUT_STEREO,
                        VN_CHANNEL_LAYOUT_QUAD, VN_CHANNEL_LAYOUT_5_1, VN_CHANNEL_LAYOUT_7_1})
    for (std::uint32_t sources : {1u, 2u})
    for (float p2 : {-1.0f, -0.35f, 0.0f, 0.65f, 1.0f})
    for (float p3 : {-1.0f, -0.55f, 0.0f, 0.25f, 1.0f})
    for (float width : {0.0f, 0.35f, 1.0f})
    for (float mix : {0.0f, 0.5f, 1.0f}) {
        const auto actual = harken::MakeSpatialMatrix(sources, layout, p2, p3, width, mix, trig);
        for (std::uint32_t source = 0; source < sources; ++source) {
            std::array<double, 8> direct{};
            std::array<double, 8> spatial{};
            if (sources == 1) {
                direct = ReferencePan(layout == VN_CHANNEL_LAYOUT_MONO
                    ? VN_CHANNEL_LAYOUT_MONO : VN_CHANNEL_LAYOUT_STEREO, p2);
                spatial = ReferencePan(layout, p3);
                const auto center = ReferencePan(layout, 0);
                double power = 0;
                for (std::uint32_t ch = 0; ch < 8; ++ch) {
                    spatial[ch] = (1.0 - width) * spatial[ch] + width * center[ch];
                    power += spatial[ch] * spatial[ch];
                }
                for (double& g : spatial) g /= std::sqrt(power);
                if (layout != VN_CHANNEL_LAYOUT_MONO) {
                    for (double& g : direct) g *= std::sqrt(2.0);
                    for (double& g : spatial) g *= std::sqrt(2.0);
                }
            } else {
                const double balance = std::min(1.0, source == 0 ? 1.0 - p2 : 1.0 + p2);
                direct[layout == VN_CHANNEL_LAYOUT_MONO ? 0 : source] =
                    balance * (layout == VN_CHANNEL_LAYOUT_MONO ? kCenter : 1.0);
                spatial = ReferenceStereo(layout, p3, width)[source];
            }
            for (std::uint32_t ch = 0; ch < 8; ++ch)
                ASSERT_NEAR((1.0 - mix) * direct[ch] + mix * spatial[ch], actual.gain[source][ch], 2e-6)
                    << layout << ' ' << sources << ' ' << p2 << ' ' << p3 << ' ' << width << ' ' << mix;
        }
    }
}

TEST(SpatialMatrix, InterruptedTransitionStartsAtLastRenderedImage) {
    for (std::uint32_t rate : {44100u, 48000u, 96000u})
    for (std::uint32_t block : {1u, 7u, 64u, 256u, 1024u}) {
        harken::SpatialMatrixTransition transition;
        harken::SpatialMatrix first, second, third;
        first.gain[0][0] = 1; second.gain[1][1] = 1; third.gain[0][2] = 1;
        transition.Set(first, 1);
        transition.Set(second, rate / 100);
        std::array<float, 2> last{};
        for (std::uint32_t i = 0; i < block; ++i) {
            last = {transition.Gain(0, 0, transition.Mix()), transition.Gain(1, 1, transition.Mix())};
            transition.AdvanceFrame();
        }
        transition.Set(third, rate / 100);
        EXPECT_FLOAT_EQ(last[0], transition.Gain(0, 0, transition.Mix()));
        EXPECT_FLOAT_EQ(last[1], transition.Gain(1, 1, transition.Mix()));
        for (std::uint32_t i = 0; i < rate / 100 - 1; ++i) transition.AdvanceFrame();
        EXPECT_FLOAT_EQ(1, transition.Gain(0, 2, transition.Mix()));
    }
}

TEST(SpatialMatrix, ActualMonoAndCorrelatedStereoKeepCenterLevelAcrossSpatialization) {
    const std::array<float, 8> source{0.0f, .125f, -.25f, .7024878f, -.7f, .9f, -.9f, 0.0f};
    for (std::uint32_t sources : {1u, 2u}) {
        for (std::uint32_t mode = 0; mode < 4; ++mode) {
            SCOPED_TRACE(::testing::Message() << sources << " channels, mode " << mode);
            harkentest::EngineSession session(harkentest::MakeNullConfig());
            ASSERT_EQ(VN_OK, session.InitResult());
            std::vector<float> pcm(4096 * sources);
            for (std::size_t frame = 0; frame < 4096; ++frame)
                for (std::uint32_t ch = 0; ch < sources; ++ch)
                    pcm[frame * sources + ch] = source[frame % source.size()];
            auto pcmDesc = harkentest::InterleavedDesc(pcm, sources, 48000);
            const VnSoundId sound = harkentest::ObjectId(991);
            ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcmDesc));
            auto play = harkentest::MakePlayDesc(sound);
            play.spatialized = mode != 0;
            play.positionZ = -10;
            VnVoiceHandle voice{};
            ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
            if (mode != 0)
                ASSERT_EQ(VN_OK, Vn_SetVoiceSpatializationMix(voice, static_cast<float>(mode - 1) / 2.0f));
            ASSERT_EQ(VN_OK, Vn_Update());
            harkentest::Capture capture;
            const auto before = session.Engine()->mixer.BlocksRendered();
            harkentest::PumpAndCapture(session.Engine(), 2, capture);
            ASSERT_EQ(before + 2, session.Engine()->mixer.BlocksRendered());
            ASSERT_EQ(2u * session.Engine()->nullBackend.BlockFrames(), capture.left.size());
            ASSERT_EQ(capture.left.size(), capture.right.size());
            for (std::size_t i = 0; i < capture.left.size(); ++i) {
                // The original source is the level reference, not the other mode.
                ASSERT_NEAR(source[i % source.size()], capture.left[i], 2e-6) << i;
                ASSERT_NEAR(source[i % source.size()], capture.right[i], 2e-6) << i;
            }
            if (mode == 3) {
                // Equal center endpoints must also remain unity during a live
                // transition, including a retarget before the 10 ms ramp ends.
                std::size_t sourceFrame = capture.left.size();
                for (float mix : {0.0f, 1.0f, .5f, 0.0f}) {
                    ASSERT_EQ(VN_OK, Vn_SetVoiceSpatializationMix(voice, mix));
                    ASSERT_EQ(VN_OK, Vn_Update());
                    harkentest::Capture moving;
                    const auto block = session.Engine()->mixer.BlocksRendered();
                    harkentest::PumpAndCapture(session.Engine(), 1, moving);
                    ASSERT_EQ(block + 1, session.Engine()->mixer.BlocksRendered());
                    for (std::size_t i = 0; i < moving.left.size(); ++i, ++sourceFrame) {
                        ASSERT_NEAR(source[sourceFrame % source.size()], moving.left[i], 2e-6);
                        ASSERT_NEAR(source[sourceFrame % source.size()], moving.right[i], 2e-6);
                    }
                }
            }
        }
    }
}

TEST(SpatialMatrix, FullySpreadStereoPreservesOriginalSamplesAcrossMixAndRetarget) {
    const std::array<std::array<float, 8>, 2> source{{
        {{0.0f, .125f, -.25f, .7024878f, -.7f, .9f, -.9f, 0.0f}},
        {{.2f, -.35f, .4f, -.1f, .55f, -.6f, .1f, -.2f}}}};
    for (std::uint32_t mode = 0; mode < 4; ++mode) {
        SCOPED_TRACE(mode);
        harkentest::EngineSession session(harkentest::MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        std::vector<float> pcm(4096 * 2);
        for (std::size_t frame = 0; frame < 4096; ++frame)
            for (std::uint32_t ch = 0; ch < 2; ++ch)
                pcm[frame * 2 + ch] = source[ch][frame % source[ch].size()];
        const auto desc = harkentest::InterleavedDesc(pcm, 2, 48000);
        const auto sound = harkentest::ObjectId(996);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &desc));
        auto play = harkentest::MakePlayDesc(sound);
        play.spatialized = mode != 0;
        play.positionX = 10;
        VnVoiceHandle voice{};
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        ASSERT_EQ(VN_OK, Vn_SetVoiceWidth(voice, 1));
        if (mode != 0)
            ASSERT_EQ(VN_OK, Vn_SetVoiceSpatializationMix(voice, (mode - 1) / 2.0f));
        ASSERT_EQ(VN_OK, Vn_Update());
        std::size_t sourceFrame = 0;
        auto check = [&](std::uint32_t blocks) {
            harkentest::Capture capture;
            const auto before = session.Engine()->mixer.BlocksRendered();
            harkentest::PumpAndCapture(session.Engine(), blocks, capture);
            ASSERT_EQ(before + blocks, session.Engine()->mixer.BlocksRendered());
            ASSERT_EQ(blocks * session.Engine()->nullBackend.BlockFrames(), capture.left.size());
            ASSERT_EQ(capture.left.size(), capture.right.size());
            for (std::size_t i = 0; i < capture.left.size(); ++i, ++sourceFrame) {
                ASSERT_NEAR(source[0][sourceFrame % 8], capture.left[i], 2e-6) << sourceFrame;
                ASSERT_NEAR(source[1][sourceFrame % 8], capture.right[i], 2e-6) << sourceFrame;
            }
        };
        ASSERT_NO_FATAL_FAILURE(check(2));
        if (mode == 3) {
            for (float mix : {0.0f, 1.0f, .5f, 0.0f}) {
                ASSERT_EQ(VN_OK, Vn_SetVoiceSpatializationMix(voice, mix));
                ASSERT_EQ(VN_OK, Vn_Update());
                ASSERT_NO_FATAL_FAILURE(check(1));
            }
        }
    }
}

TEST(SpatialMatrix, StereoPointFoldKeepsNaturalPhaseSummation) {
    const std::array<float, 8> left{{.2f, .2f, .2f, .4f, 0, -.3f, .1f, 0}};
    const std::array<float, 8> right{{.2f, -.2f, 0, -.1f, .3f, .1f, -.4f, 0}};
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<float> pcm(4096 * 2);
    for (std::size_t frame = 0; frame < 4096; ++frame) {
        pcm[frame * 2] = left[frame % 8];
        pcm[frame * 2 + 1] = right[frame % 8];
    }
    const auto desc = harkentest::InterleavedDesc(pcm, 2, 48000);
    const auto sound = harkentest::ObjectId(997);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &desc));
    auto play = harkentest::MakePlayDesc3D(sound, 0, 0, -10);
    VnVoiceHandle voice{};
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK, Vn_Update());
    harkentest::Capture capture;
    const auto before = session.Engine()->mixer.BlocksRendered();
    harkentest::PumpAndCapture(session.Engine(), 2, capture);
    ASSERT_EQ(before + 2, session.Engine()->mixer.BlocksRendered());
    ASSERT_EQ(2u * session.Engine()->nullBackend.BlockFrames(), capture.left.size());
    ASSERT_EQ(capture.left.size(), capture.right.size());
    for (std::size_t i = 0; i < capture.left.size(); ++i) {
        const float expected = .5f * (left[i % 8] + right[i % 8]);
        ASSERT_NEAR(expected, capture.left[i], 2e-6) << i;
        ASSERT_NEAR(expected, capture.right[i], 2e-6) << i;
    }
}

TEST(SpatialMatrix, MonoReferencePowerAndDualMonoImageFollowOutputLayout) {
    harken::BiquadTrigTable trig;
    for (auto layout : {VN_CHANNEL_LAYOUT_MONO, VN_CHANNEL_LAYOUT_STEREO,
                        VN_CHANNEL_LAYOUT_QUAD, VN_CHANNEL_LAYOUT_5_1, VN_CHANNEL_LAYOUT_7_1})
    for (float pan : {-1.0f, -.5f, 0.0f, .5f, 1.0f}) {
        SCOPED_TRACE(::testing::Message() << layout << " pan " << pan);
        for (float mix : {0.0f, 1.0f}) {
            const auto mono = harken::MakeSpatialMatrix(1, layout, pan, pan, 0, mix, trig);
            double power = 0;
            for (float gain : mono.gain[0]) power += gain * gain;
            EXPECT_NEAR(layout == VN_CHANNEL_LAYOUT_MONO ? 1.0 : 2.0, power, 3e-6);
        }
        if (layout == VN_CHANNEL_LAYOUT_MONO) continue;
        const auto mono = harken::MakeSpatialMatrix(1, layout, 0, pan, 0, 1, trig);
        const auto stereo = harken::MakeSpatialMatrix(2, layout, 0, pan, 0, 1, trig);
        for (std::uint32_t ch = 0; ch < 8; ++ch)
            EXPECT_NEAR(mono.gain[0][ch], stereo.gain[0][ch] + stereo.gain[1][ch], 2e-6);
    }
}

TEST(SpatialMatrix, CenterMonoPreservesOriginalSamplesThroughDeviceDownmix) {
    for (auto layout : {VN_CHANNEL_LAYOUT_MONO, VN_CHANNEL_LAYOUT_STEREO,
                        VN_CHANNEL_LAYOUT_QUAD, VN_CHANNEL_LAYOUT_5_1, VN_CHANNEL_LAYOUT_7_1}) {
        const auto device = layout == VN_CHANNEL_LAYOUT_MONO ? layout : VN_CHANNEL_LAYOUT_STEREO;
        harkentest::EngineSession session(harkentest::MakeNullConfigLayout(2, 64, 48000, layout, device));
        ASSERT_EQ(VN_OK, session.InitResult());
        const std::vector<float> pcm(4096, .125f);
        const auto desc = harkentest::MonoDesc(pcm, 48000);
        const auto sound = harkentest::ObjectId(995);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &desc));
        for (std::uint32_t mode = 0; mode < 4; ++mode) {
            SCOPED_TRACE(::testing::Message() << layout << " mode " << mode);
            auto play = harkentest::MakePlayDesc(sound);
            play.spatialized = mode != 0;
            play.positionZ = -10;
            VnVoiceHandle voice{};
            ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
            if (mode != 0)
                ASSERT_EQ(VN_OK, Vn_SetVoiceSpatializationMix(voice, static_cast<float>(mode - 1) / 2.0f));
            ASSERT_EQ(VN_OK, Vn_Update());
            harkentest::CaptureMulti capture;
            harkentest::PumpAndCaptureMulti(session.Engine(), 2, capture);
            ASSERT_EQ(device == VN_CHANNEL_LAYOUT_MONO ? 1u : 2u, capture.channelCount);
            for (std::uint32_t ch = 0; ch < capture.channelCount; ++ch)
                for (float sample : capture.channels[ch]) ASSERT_NEAR(.125f, sample, 2e-6);
            ASSERT_EQ(VN_OK, Vn_Stop(voice, 0));
            ASSERT_EQ(VN_OK, Vn_Update());
            harkentest::CaptureMulti stopped;
            harkentest::PumpAndCaptureMulti(session.Engine(), 2, stopped);
        }
    }
}

TEST(SpatialPublication, ClampedPanStillPublishesAuthoredChangesAndFrozenClaims) {
    harken::ParamSystem params(harken::EngineConfig{});
    params.OnVoiceStarted(kVoice, 0, 1);
    params.SetAttenuationSlot(kVoice, harken::kTargetVoicePan, 1);
    params.SetMeterProbeContribution(kVoice, harkentest::ObjectId(91), harken::kTargetVoicePan, .2f, 0);
    std::vector<harken::ParamCommit> commits;
    auto flush = [&] {
        commits.clear();
        params.FlushCommits([&](const auto& commit) {
            commits.push_back(commit); return harken::FlushOutcome::Committed;
        });
    };
    flush();
    ASSERT_EQ(1u, commits.size());
    EXPECT_FLOAT_EQ(1, commits[0].resolved);
    EXPECT_FLOAT_EQ(.2f, commits[0].pan2d);
    params.SetMeterProbeContribution(kVoice, harkentest::ObjectId(91), harken::kTargetVoicePan, .4f, 0);
    flush();
    ASSERT_EQ(1u, commits.size());
    EXPECT_FLOAT_EQ(1, commits[0].resolved);
    EXPECT_FLOAT_EQ(.4f, commits[0].pan2d);
    const harken::VariableSliceRef owner{0, 1};
    ASSERT_TRUE(params.TryClaim(kVoice, harken::kTargetVoicePan, owner,
                                params.TargetRevision(kVoice, harken::kTargetVoicePan)));
    float frozen = 0, p2 = 0;
    std::uint32_t ramp = 0, revision = 0;
    ASSERT_TRUE(params.CaptureSnapshotForClaim(kVoice, harken::kTargetVoicePan, owner,
                                               &frozen, &ramp, &revision, &p2));
    params.SetMeterProbeContribution(kVoice, harkentest::ObjectId(91), harken::kTargetVoicePan, .6f, 0);
    params.MarkSliceCommitted(kVoice, harken::kTargetVoicePan, owner, frozen, ramp, p2);
    flush();
    ASSERT_EQ(1u, commits.size());
    EXPECT_FLOAT_EQ(1, commits[0].resolved);
    EXPECT_FLOAT_EQ(.6f, commits[0].pan2d);
}

TEST(SpatialPublication, RetiringUncapturedClaimKeepsBothPanValues) {
    harken::ParamSystem params(harken::EngineConfig{});
    params.OnVoiceStarted(kVoice, 0, 1);
    params.SetApiDirect(kVoice, harken::kTargetVoicePan, .3f);
    params.SetAttenuationSlot(kVoice, harken::kTargetVoicePan, 1);
    const harken::VariableSliceRef owner{0, 1};
    ASSERT_TRUE(params.TryClaim(kVoice, harken::kTargetVoicePan, owner,
                                params.TargetRevision(kVoice, harken::kTargetVoicePan)));
    params.RetireVoiceTargets(kVoice);
    float p3 = 0, p2 = 0;
    std::uint32_t ramp = 0, revision = 0;
    ASSERT_TRUE(params.CaptureSnapshotForClaim(kVoice, harken::kTargetVoicePan, owner,
                                               &p3, &ramp, &revision, &p2));
    EXPECT_FLOAT_EQ(1, p3); EXPECT_FLOAT_EQ(.3f, p2);
    params.MarkSliceCommitted(kVoice, harken::kTargetVoicePan, owner, p3, ramp, p2);
    EXPECT_EQ(0u, params.RetiringTargetCountForTests());
}

TEST(SpatialPublication, FullBatchMergesBothOrdersBeforeQueueAdmission) {
    for (bool panFirst : {false, true}) {
        harken::PropertyPublisher publisher(1);
        harken::PropertyCommit pan{}, width{};
        pan.scopeKind = width.scopeKind = harken::kTargetScopeVoice;
        pan.objectId = width.objectId = kVoice;
        pan.targetId = harken::kTargetVoicePan;
        pan.resolved = 1; pan.pan2d = .4f; pan.spatialMask = harken::kSpatialPropertyPan;
        width.targetId = harken::kTargetVoiceWidth;
        width.resolved = width.spatialWidth = .7f; width.spatialMask = harken::kSpatialPropertyWidth;
        std::uint32_t group = ~0u;
        ASSERT_TRUE(publisher.StageSpatial(panFirst ? pan : width, group));
        ASSERT_TRUE(publisher.StageSpatial(panFirst ? width : pan, group));
        EXPECT_EQ(1u, publisher.StagedCount());
        std::vector<harken::PropertyCommit> pushed;
        const auto push = +[](void* context, const harken::PropertyCommit& value) {
            static_cast<std::vector<harken::PropertyCommit>*>(context)->push_back(value); return true;
        };
        EXPECT_FALSE(publisher.Publish(+[](void*) { return 0u; }, push, &pushed));
        EXPECT_TRUE(pushed.empty());
        EXPECT_TRUE(publisher.Publish(+[](void*) { return 1u; }, push, &pushed));
        ASSERT_EQ(1u, pushed.size());
        EXPECT_EQ(3u, pushed[0].spatialMask);
        EXPECT_EQ(harken::kTargetVoicePan, pushed[0].targetId);
        EXPECT_FLOAT_EQ(1, pushed[0].resolved);
        EXPECT_FLOAT_EQ(.4f, pushed[0].pan2d);
        EXPECT_FLOAT_EQ(.7f, pushed[0].spatialWidth);
    }
}
TEST(SpatialMatrix, DistinctStereoColumnsShareOffsetRetargetAndSendImage) {
    harken::BiquadTrigTable trig;
    harken::VoiceRenderContext ctx;
    ctx.biquadTrig = &trig;
    ctx.pcm.channelCount = 2;
    ctx.engineSampleRate = 48000;
    ctx.spatialized = true;
    ctx.firstBlock = false;
    ctx.outputGain.Set(.5f);
    ctx.voiceSends[0].active = true;
    ctx.voiceSends[0].targetBusIndex = 1;
    ctx.voiceSends[0].gain.current = ctx.voiceSends[0].gain.target = .25f;
    std::array<std::array<float, 1024>, 2> source{}, tap{};
    std::array<std::array<float, 1024>, 4> buses{};
    float* busPlanes[4];
    for (std::uint32_t ch = 0; ch < 4; ++ch) busPlanes[ch] = buses[ch].data();
    for (std::uint32_t ch = 0; ch < 2; ++ch) {
        ctx.cascade.scratch[ch] = source[ch].data();
        ctx.cascade.tap[ch] = tap[ch].data();
    }
    for (std::uint32_t i = 0; i < 1024; ++i) {
        source[0][i] = .2f * static_cast<float>(std::sin(i * .13));
        source[1][i] = .1f * static_cast<float>(std::cos(i * .07));
    }
    ctx.cascade.tapReady = true;
    ctx.cascade.offset = 3;
    using Matrix = std::array<std::array<double, 2>, 2>;
    auto target = [&](float p2, float p3, float width, float mix) {
        ctx.pan2d = p2; ctx.pan = p3; ctx.width = width;
        ctx.spatializationMix = mix; ctx.spatialMatrixDirty = true;
        Matrix expected{};
        const auto spatial = ReferenceStereo(VN_CHANNEL_LAYOUT_STEREO, p3, width);
        for (std::uint32_t s = 0; s < 2; ++s) {
            const double balance = std::min(1.0, s == 0 ? 1.0 - p2 : 1.0 + p2);
            for (std::uint32_t ch = 0; ch < 2; ++ch)
                expected[s][ch] = (1.0 - mix) * (s == ch ? balance : 0.0) +
                                  mix * spatial[s][ch];
        }
        return expected;
    };
    auto render = [&](std::uint32_t frames, const Matrix& from, const Matrix& to,
                      bool snap, float outputFrom = .5f, float outputTo = .5f) {
        ctx.outputGain.Set(outputFrom);
        ctx.outputGain.Set(outputTo, 97);
        for (auto& channel : buses) channel.fill(0);
        ctx.cascade.produced = frames;
        harken::FoldVoiceIntoBuses(ctx, busPlanes, 1024, 2, VN_CHANNEL_LAYOUT_STEREO, 0);
        Matrix last = from;
        for (std::uint32_t i = 0; i < frames; ++i) {
            const double mix = snap ? 1.0 : std::min(1.0, static_cast<double>(i) / 479);
            for (std::uint32_t ch = 0; ch < 2; ++ch) {
                double expected = 0;
                for (std::uint32_t s = 0; s < 2; ++s) {
                    last[s][ch] = (1 - mix) * from[s][ch] + mix * to[s][ch];
                    expected += source[s][i + 3] * last[s][ch];
                }
                EXPECT_NEAR(expected, tap[ch][i + 3], 2e-6);
                const double output = outputFrom + (outputTo - outputFrom) *
                    std::min(1.0, static_cast<double>(i + 1) / 97);
                EXPECT_NEAR(expected * output, buses[ch][i + 3], 2e-6);
                EXPECT_NEAR(expected * .25, buses[ch + 2][i + 3], 2e-6);
            }
        }
        const double finalOutput = outputFrom + (outputTo - outputFrom) *
            std::min(1.0, static_cast<double>(frames) / 97);
        EXPECT_NEAR(finalOutput, ctx.outputGain.current, 2e-6);
        EXPECT_EQ(outputFrom == outputTo || frames >= 97 ? 0u : 97 - frames,
                  ctx.outputGain.remainingFrames);
        for (auto& channel : buses) {
            EXPECT_FLOAT_EQ(0, channel[0]); EXPECT_FLOAT_EQ(0, channel[2]);
            EXPECT_FLOAT_EQ(0, channel[3 + frames]);
        }
        return last;
    };
    const auto unrendered = target(-.2f, .8f, .3f, .4f);
    render(0, unrendered, unrendered, true);
    EXPECT_FALSE(ctx.spatialMatrixHasRendered);
    const auto first = target(.3f, -.7f, .8f, .6f);
    render(7, first, first, true);
    EXPECT_TRUE(ctx.spatialMatrix.Settled());
    const auto second = target(-.5f, .4f, .1f, .2f);
    const auto last = render(64, first, second, false, .5f, .8f);
    const auto third = target(.1f, -.3f, .6f, .9f);
    render(512, last, third, false, .5f + .3f * 64 / 97, .2f);
    EXPECT_TRUE(ctx.spatialMatrix.Settled());
}

TEST(SpatialMatrix, WiderSpatialSourcesCannotClaimResidentOrStreamSlots) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(2, 64, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    std::vector<float> samples(4 * 1024, .1f);
    const auto pcm = harkentest::InterleavedDesc(samples, 4, 48000);
    const auto sound = harkentest::ObjectId(992);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
    const auto before = engine.voices.AvailableSlotCount();
    const auto pan = harken::ResolvePanGains(VN_CHANNEL_LAYOUT_STEREO, 0);
    for (bool prepared : {false, true}) {
        VnVoiceHandle handle = 0;
        EXPECT_EQ(VN_INVALID_ARG, engine.voices.Play(engine.registry.Lookup(sound),
            0, 1, 0, true, 0, 0, -1, pan, 0, 1024, 1,
            harken::ResampleProfile::LegacyCompatibility, &handle, 1, ~0u, 0, prepared));
        EXPECT_EQ(0u, handle);
        EXPECT_EQ(VN_INVALID_ARG, engine.voices.PlayStream(nullptr, 4, 48000, 64,
            0, 1, 0, true, 0, 0, -1, pan, 0, 1024, 0, 1024, 1,
            harken::ResampleProfile::LegacyCompatibility, &handle, 1, ~0u, 0,
            nullptr, 0, prepared));
        EXPECT_EQ(0u, handle);
        EXPECT_EQ(before, engine.voices.AvailableSlotCount());
    }
}

TEST(SpatialMatrix, StereoResamplingAndStatefulVoiceFxKeepIndependentHistories) {
    constexpr std::uint32_t kFrames = 4096;
    constexpr std::uint32_t kBlock = 64;
    constexpr std::uint32_t kBlocks = 16;
    constexpr float kPan = .25f;
    constexpr float kWidth = .65f;
    std::vector<float> pcm(kFrames * 2);
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        pcm[2 * i] = .12f * static_cast<float>(std::sin(i * .17));
        pcm[2 * i + 1] = .09f * static_cast<float>(std::cos(i * .073));
    }
    // Dry 2D proves that the stateful effect actually changes the signal;
    // wet 2D supplies the two independently processed source histories.
    std::array<harkentest::Capture, 3> captured;
    for (std::uint32_t pass = 0; pass < captured.size(); ++pass) {
        SCOPED_TRACE(pass);
        auto config = harkentest::MakeNullConfig(1, kBlock, 48000);
        config.fxSlotsPerVoice = 1;
        config.delayEffectPoolCapacity = 1;
        harkentest::EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        const auto sound = harkentest::ObjectId(993);
        const auto desc = harkentest::InterleavedDesc(pcm, 2, 48000);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &desc));
        auto play = harkentest::MakePlayDesc(sound, 0, 1.25f);
        play.spatialized = pass == 2 ? 1u : 0u;
        play.positionZ = -10;
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        if (pass != 0) {
            VnFeedbackDelayDesc delay{};
            delay.structSize = sizeof(delay);
            delay.delayMs = 2;
            delay.feedback = .55f;
            delay.wet = .7f;
            delay.dry = .4f;
            ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(voice, 0, VN_EFFECT_FEEDBACK_DELAY,
                static_cast<const VnEffectDesc*>(static_cast<const void*>(&delay))));
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        if (pass == 2) {
            ASSERT_EQ(VN_OK, Vn_SetVoicePan(voice, kPan));
            ASSERT_EQ(VN_OK, Vn_SetVoiceWidth(voice, kWidth));
        }
        harkentest::PumpAndCapture(session.Engine(), kBlocks, captured[pass]);
        const auto* ctx = session.Engine()->voices.Resolve(
            harkentest::RenderHandle(session.Engine(), voice));
        ASSERT_NE(nullptr, ctx);
        EXPECT_EQ(2u, ctx->pcm.channelCount);
        EXPECT_DOUBLE_EQ(1.25, ctx->resampler.CurrentRatio());
        EXPECT_EQ(kBlock * kBlocks * 5u / 4u,
                  ctx->resampler.SourceFrame() - harken::kResampleHistoryPad);
        if (pass == 2) {
            EXPECT_FLOAT_EQ(kPan, ctx->pan);
            EXPECT_FLOAT_EQ(kWidth, ctx->width);
        }
        ASSERT_EQ(kBlock * kBlocks, captured[pass].left.size());
        ASSERT_EQ(kBlock * kBlocks, captured[pass].right.size());
        HARKENTEST_ASSERT_NON_SILENT(captured[pass].left);
        HARKENTEST_ASSERT_NON_SILENT(captured[pass].right);
        ASSERT_LT(harkentest::Peak(captured[pass].left), .5f);
        ASSERT_LT(harkentest::Peak(captured[pass].right), .5f);
    }
    const auto columns = ReferenceStereo(VN_CHANNEL_LAYOUT_STEREO, kPan, kWidth);
    const auto& leftColumn = columns[0];
    const auto& rightColumn = columns[1];
    double effectDifference = 0;
    double channelDifference = 0;
    for (std::size_t i = 0; i < captured[1].left.size(); ++i) {
        const double left = captured[1].left[i];
        const double right = captured[1].right[i];
        EXPECT_NEAR(left * leftColumn[0] + right * rightColumn[0],
                    captured[2].left[i], 2e-6) << i;
        EXPECT_NEAR(left * leftColumn[1] + right * rightColumn[1],
                    captured[2].right[i], 2e-6) << i;
        // Examine later frames so this is sensitive to actual delay history,
        // not only the dry gain applied before the first 96-frame echo.
        if (i >= 192) {
            effectDifference += std::abs(left - .4 * captured[0].left[i]) +
                                std::abs(right - .4 * captured[0].right[i]);
            channelDifference += std::abs(left - right);
        }
    }
    EXPECT_GT(effectDifference, 1.0);
    EXPECT_GT(channelDifference, 1.0);
}

TEST(SpatialMatrix, PublicRawPlayRejectsWiderSpatialSourcesWithoutResourceChanges) {
    for (std::uint32_t channels : {3u, 4u, 6u, 8u}) {
        SCOPED_TRACE(channels);
        harkentest::EngineSession session(harkentest::MakeNullConfig(2, 64, 48000));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        const auto sound = harkentest::ObjectId(994);
        const std::vector<float> pcm(4096 * channels, .01f);
        const auto desc = harkentest::InterleavedDesc(pcm, channels, 48000);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &desc));
        const auto view = engine.registry.Lookup(sound);
        ASSERT_NE(nullptr, view.useCount);
        auto resources = [&] {
            std::uint32_t live = 0;
            engine.ledger.ForEachLive([&](auto, auto, auto, auto) { ++live; });
            return std::array<std::uint32_t, 6>{engine.voices.AvailableSlotCount(),
                engine.voices.ActiveCount(), live, engine.ledger.PendingStartCount(),
                engine.ledger.ParkedCount(), engine.ledger.QuarantinedCount()};
        };
        const auto before = resources();
        const auto refs = view.useCount->load(std::memory_order_acquire);
        const auto commandHead = engine.commands.PublishedHead();
        auto play = harkentest::MakePlayDesc3D(sound, 0, 0, -10);
        for (bool staged : {false, true}) {
            play.reserved0 = staged ? VN_PLAY_FLAG_STAGED_INSTALL : 0u;
            // Repetition also catches a silently retained reservation.
            for (std::uint32_t attempt = 0; attempt < 3; ++attempt) {
                SCOPED_TRACE(staged);
                VnVoiceHandle voice = 0;
                EXPECT_EQ(VN_INVALID_ARG, harkentest::PlayRaw(&play, &voice));
                EXPECT_EQ(0u, voice);
                EXPECT_EQ(before, resources());
                EXPECT_EQ(refs, view.useCount->load(std::memory_order_acquire));
                EXPECT_EQ(commandHead, engine.commands.PublishedHead());
            }
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        harkentest::Capture rejected;
        harkentest::PumpAndCapture(&engine, 1, rejected);
        EXPECT_FLOAT_EQ(0, harkentest::Peak(rejected.left));
        EXPECT_FLOAT_EQ(0, harkentest::Peak(rejected.right));
        EXPECT_EQ(before, resources());
        EXPECT_EQ(refs, view.useCount->load(std::memory_order_acquire));

        // The same source remains legal in 2D and can occupy both available
        // slots immediately; rejection must not leak public or render slots.
        play.spatialized = 0;
        play.reserved0 = 0;
        for (std::uint32_t i = 0; i < 2; ++i) {
            VnVoiceHandle voice = 0;
            ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
            ASSERT_NE(0u, harkentest::RenderHandle(&engine, voice));
        }
        EXPECT_EQ(0u, engine.voices.AvailableSlotCount());
        ASSERT_EQ(VN_OK, Vn_Update());
        harkentest::Capture accepted;
        harkentest::PumpAndCapture(&engine, 1, accepted);
        HARKENTEST_ASSERT_NON_SILENT(accepted.left);
        HARKENTEST_ASSERT_NON_SILENT(accepted.right);
    }
}

TEST(SpatialMatrix, StreamGateAndFirstStarvationKeepLatestUnrenderedTarget) {
    constexpr std::uint32_t kBlock = 64;
    harken::ResampleTables tables;
    tables.Init();
    harken::BiquadTrigTable trig;
    harken::StreamRing stream;
    std::array<std::array<float, kBlock>, 2> storage{};
    float* streamPlanes[harken::kMaxChannels] = {};
    for (std::uint32_t ch = 0; ch < 2; ++ch)
        streamPlanes[ch] = storage[ch].data();
    stream.Wire(streamPlanes, kBlock);
    stream.ResetForUse(2);
    std::array<std::vector<float>, 2> fifo;
    float* fifoPlanes[2];
    for (std::uint32_t ch = 0; ch < 2; ++ch) {
        fifo[ch].resize(harken::kResampleFifoFrames);
        fifoPlanes[ch] = fifo[ch].data();
    }
    harken::Resampler resamplerBacking;
    harken::VoiceRenderContext ctx;
    ctx.biquadTrig = &trig;
    ctx.pcm.channelCount = 2;
    ctx.pcm.sampleRate = ctx.engineSampleRate = 48000;
    ctx.streamRing = &stream;
    ctx.prebufferTargetFrames = 1;
    ctx.phase = harken::SourcePhase::Active;
    ctx.spatialized = true;
    ctx.gain.Set(1, 0);
    ctx.outputGain.Set(1);
    ctx.resampler = resamplerBacking;
    ctx.resampler.Reset(fifoPlanes, 2, harken::ResampleQuality::Linear, 1, &tables);
    harken::BlockArena arena{65536};
    std::array<std::array<float, kBlock>, 2> output{};
    float* buses[2] = {output[0].data(), output[1].data()};
    auto render = [&] {
        arena.Reset();
        for (auto& channel : output) channel.fill(0);
        return harken::RenderVoiceBlock(ctx, arena, buses, kBlock, 2,
                                        VN_CHANNEL_LAYOUT_STEREO);
    };
    auto target = [&](float pan, float width) {
        ctx.pan = pan;
        ctx.width = width;
        ctx.spatialMatrixDirty = true;
    };
    auto feed = [&](std::uint32_t frames) {
        float* writable[harken::kMaxChannels] = {};
        ASSERT_GE(stream.ContiguousWrite(writable), frames);
        std::fill_n(writable[0], frames, .12f);
        std::fill_n(writable[1], frames, -.08f);
        stream.PublishWrite(frames);
    };
    auto reference = [](float pan, float width) {
        const auto columns = ReferenceStereo(VN_CHANNEL_LAYOUT_STEREO, pan, width);
        return std::array<double, 2>{
            .12f * columns[0][0] - .08f * columns[1][0],
            .12f * columns[0][1] - .08f * columns[1][1]};
    };
    target(.8f, .2f);
    EXPECT_EQ(0u, render().framesProduced);
    EXPECT_FALSE(ctx.streamGateOpen);
    EXPECT_TRUE(ctx.firstBlock);
    EXPECT_FALSE(ctx.spatialMatrixHasRendered);
    EXPECT_FLOAT_EQ(0, harkentest::Peak(output[0].data(), kBlock));
    EXPECT_FLOAT_EQ(0, harkentest::Peak(output[1].data(), kBlock));

    target(-.7f, .1f);
    feed(1); // The gate opens, but linear SRC still lacks its look-ahead frame.
    EXPECT_EQ(0u, render().framesProduced);
    EXPECT_TRUE(ctx.streamGateOpen);
    EXPECT_TRUE(ctx.starvedThisBlock);
    EXPECT_FALSE(ctx.firstBlock);
    EXPECT_FALSE(ctx.spatialMatrixHasRendered);

    target(.25f, .65f);
    feed(kBlock - 1);
    ASSERT_EQ(kBlock - 1, render().framesProduced);
    ASSERT_TRUE(ctx.spatialMatrixHasRendered);
    EXPECT_TRUE(ctx.spatialMatrix.Settled());
    EXPECT_TRUE(ctx.starvedThisBlock);
    const auto first = reference(.25f, .65f);
    for (std::uint32_t ch = 0; ch < 2; ++ch) {
        EXPECT_GT(harkentest::Peak(output[ch].data(), kBlock), 1e-4f);
        for (std::uint32_t i = 0; i < kBlock - 1; ++i)
            EXPECT_NEAR(first[ch], output[ch][i], 2e-6);
        EXPECT_FLOAT_EQ(0, output[ch][kBlock - 1]);
    }
    target(-.4f, .3f);
    const auto progress = ctx.spatialMatrix.progress;
    EXPECT_EQ(0u, render().framesProduced);
    EXPECT_EQ(progress, ctx.spatialMatrix.progress);
    EXPECT_TRUE(ctx.spatialMatrixDirty);
    feed(kBlock);
    ASSERT_EQ(kBlock, render().framesProduced);
    EXPECT_EQ(kBlock, ctx.spatialMatrix.progress);
    const auto next = reference(-.4f, .3f);
    for (std::uint32_t ch = 0; ch < 2; ++ch)
        for (std::uint32_t i = 0; i < kBlock; ++i) {
            const double alpha = static_cast<double>(i) / 479;
            EXPECT_NEAR((1 - alpha) * first[ch] + alpha * next[ch], output[ch][i], 2e-6);
        }
}
} // namespace
