#include <gtest/gtest.h>
#include "harken/render/SignalFeedbackPlan.h"
#include "harken/render/SignalPathBounds.h"

#include <array>
#include <limits>

using namespace harken;

TEST(SignalFeedbackPlan, FeedforwardRetainsBaselineAndDelayedCyclesAreSupported) {
    SignalFeedbackPlan plan;
    const std::array edges{SignalFeedbackEdge{0, 1, 3, true},
                          SignalFeedbackEdge{1, 2, 1, true},
                          SignalFeedbackEdge{2, 1, 1, true}};
    ASSERT_TRUE(plan.Prepare(3, edges));
    EXPECT_FALSE(plan.Nodes()[0].feedbackInput);
    EXPECT_DOUBLE_EQ(plan.Nodes()[0].slope, kDuckingBaselineSlope);
    EXPECT_TRUE(plan.Nodes()[1].feedbackInput);
    EXPECT_EQ(plan.Nodes()[1].component, plan.Nodes()[2].component);
    EXPECT_DOUBLE_EQ(plan.Nodes()[1].slope, 1.0 / (1.0 / kDuckingBaselineSlope + 1));
    EXPECT_DOUBLE_EQ(plan.Nodes()[1].slope, plan.Nodes()[2].slope);
    const std::array oneWay{SignalFeedbackEdge{0, 1, 100, true}};
    ASSERT_TRUE(plan.Prepare(2, oneWay));
    EXPECT_FALSE(plan.Nodes()[1].feedbackInput);
    EXPECT_DOUBLE_EQ(plan.Nodes()[1].slope, kDuckingBaselineSlope);
}

TEST(SignalFeedbackPlan, ParentReturnIncludesNormalPathAmplification) {
    SignalFeedbackPlan plan;
    const std::array edges{SignalFeedbackEdge{0, 1, 2, false},
                          SignalFeedbackEdge{1, 0, 1, true}};
    ASSERT_TRUE(plan.Prepare(2, edges));
    EXPECT_TRUE(plan.Nodes()[0].feedbackInput);
    EXPECT_DOUBLE_EQ(plan.Nodes()[0].slope, 1.0 / (1.0 / kDuckingBaselineSlope + 2));
    EXPECT_FALSE(plan.Nodes()[1].feedbackInput);
    EXPECT_DOUBLE_EQ(plan.Nodes()[1].slope, kDuckingBaselineSlope);
}

TEST(SignalFeedbackPlan, NestedTargetsFoldAllNormalPathsBeforeChoosingCommonSlope) {
    SignalFeedbackPlan plan;
    const std::array edges{SignalFeedbackEdge{0, 1, 1, false},
                          SignalFeedbackEdge{2, 0, 1, true},
                          SignalFeedbackEdge{2, 1, 1, true},
                          SignalFeedbackEdge{1, 2, 1, true}};
    ASSERT_TRUE(plan.Prepare(3, edges));
    for (const auto& node : plan.Nodes()) {
        EXPECT_TRUE(node.feedbackInput);
        EXPECT_DOUBLE_EQ(node.slope, 1.0 / (1.0 / kDuckingBaselineSlope + 2));
    }
}

TEST(SignalFeedbackPlan, ZeroGainEdgesRetainCyclesAndParallelPathsAdd) {
    SignalFeedbackPlan plan;
    const std::array edges{SignalFeedbackEdge{0, 1, 0, true},
                          SignalFeedbackEdge{1, 0, 1, true},
                          SignalFeedbackEdge{1, 0, 2, true}};
    ASSERT_TRUE(plan.Prepare(2, edges));
    for (const auto& node : plan.Nodes()) {
        EXPECT_TRUE(node.feedbackInput);
        EXPECT_DOUBLE_EQ(node.slope, 1.0 / (1.0 / kDuckingBaselineSlope + 3));
    }
}

TEST(SignalFeedbackPlan, InvalidOrUnboundedPlanPreservesPublishedCandidate) {
    SignalFeedbackPlan plan;
    const std::array self{SignalFeedbackEdge{0, 0, 1, true}};
    ASSERT_TRUE(plan.Prepare(1, self));
    const auto previous = plan.Nodes()[0].slope;
    const std::array normalCycle{SignalFeedbackEdge{0, 1, 0, false},
                                SignalFeedbackEdge{1, 0, 0, false}};
    EXPECT_FALSE(plan.Prepare(2, normalCycle));
    auto invalid = self;
    invalid[0].bound = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(plan.Prepare(1, invalid));
    invalid[0].bound = -1;
    EXPECT_FALSE(plan.Prepare(1, invalid));
    invalid[0].bound = 1;
    invalid[0].target = 1;
    EXPECT_FALSE(plan.Prepare(1, invalid));
    const auto largest = std::numeric_limits<double>::max();
    const std::array overflow{SignalFeedbackEdge{0, 1, largest, false},
                             SignalFeedbackEdge{1, 0, 2, true}};
    EXPECT_FALSE(plan.Prepare(2, overflow));
    ASSERT_EQ(plan.Nodes().size(), 1u);
    EXPECT_DOUBLE_EQ(plan.Nodes()[0].slope, previous);
}

TEST(SignalFeedbackPlan, DeepSparseGraphDoesNotRequireRecursiveTraversal) {
    constexpr std::uint32_t count = 30000;
    std::vector<SignalFeedbackEdge> edges;
    for (std::uint32_t i = 1; i < count; ++i)
        edges.push_back({i - 1, i, 1, false});
    edges.push_back({count - 1, 0, 1, true});
    SignalFeedbackPlan plan;
    ASSERT_TRUE(plan.Prepare(count, edges));
    EXPECT_EQ(plan.NormalOrder().size(), count);
    EXPECT_DOUBLE_EQ(plan.Nodes()[0].slope, 1.0 / (1.0 / kDuckingBaselineSlope + 1));
    EXPECT_EQ(plan.Nodes()[0].component, plan.Nodes()[count - 1].component);
    ASSERT_TRUE(plan.Prepare(0, {}));
    EXPECT_TRUE(plan.Nodes().empty());
}

TEST(SignalFeedbackPlan, NumericUpdatesPreserveStructureAndRejectInvalidVectorsAtomically) {
    SignalFeedbackPlan plan;
    const std::array edges{SignalFeedbackEdge{0, 1, 1, false},
                          SignalFeedbackEdge{1, 0, 1, true},
                          SignalFeedbackEdge{1, 2, 1, true}};
    ASSERT_TRUE(plan.Prepare(3, edges));
    const auto component = plan.Nodes()[0].component;
    const std::array<double, 3> loud{4, 2, 100};
    ASSERT_TRUE(plan.UpdateBounds(loud));
    EXPECT_EQ(component, plan.Nodes()[0].component);
    EXPECT_DOUBLE_EQ(plan.Nodes()[0].slope, 1.0 / (1.0 / kDuckingBaselineSlope + 8));
    EXPECT_DOUBLE_EQ(plan.Nodes()[2].slope, kDuckingBaselineSlope);
    const double previous = plan.Nodes()[0].slope;
    auto invalid = loud;
    invalid[1] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(plan.UpdateBounds(invalid));
    EXPECT_DOUBLE_EQ(plan.Nodes()[0].slope, previous);
    EXPECT_FALSE(plan.UpdateBounds({}));
    const double infinity = std::numeric_limits<double>::infinity();
    const std::array<double, 3> unbounded{infinity, 1, infinity};
    ASSERT_TRUE(plan.UpdateBounds(unbounded));
    EXPECT_DOUBLE_EQ(plan.Nodes()[0].slope, 0);
    EXPECT_DOUBLE_EQ(plan.Nodes()[2].slope, kDuckingBaselineSlope);
    const std::array<double, 3> muted{infinity, 0, infinity};
    ASSERT_TRUE(plan.UpdateBounds(muted));
    EXPECT_TRUE(plan.Nodes()[0].feedbackInput);
    EXPECT_DOUBLE_EQ(plan.Nodes()[0].slope, kDuckingBaselineSlope);
}

TEST(SignalFeedbackPlan, AdmittedMisalignedScratchSupportsCurrentGraphAndFailureIsUnpublished) {
    const std::array edges{SignalFeedbackEdge{0, 1, 2, false},
        SignalFeedbackEdge{1, 0, 1, true}, SignalFeedbackEdge{1, 2, 1, true}};
    const auto required = SignalFeedbackPlan::ScratchBytes(3, edges.size());
    std::vector<std::byte> storage(required + 1);
    // A float arena slab need not satisfy double/size_t alignment. Exercise
    // an even less aligned address, without changing the arena's allocation rule.
    auto scratch = std::span(storage).subspan(1);
    SignalFeedbackView view;
    ASSERT_TRUE(SignalFeedbackPlan::Compile(3, edges, scratch, view));
    EXPECT_DOUBLE_EQ(view.nodes[0].slope, 1 / (1 / kDuckingBaselineSlope + 2));
    EXPECT_DOUBLE_EQ(view.nodes[2].slope, kDuckingBaselineSlope);
    auto* published = view.nodes.data();
    EXPECT_FALSE(SignalFeedbackPlan::Compile(3, edges, scratch.first(required - 1), view));
    EXPECT_EQ(view.nodes.data(), published);
    auto next = edges;
    next[0].target = 2;
    ASSERT_TRUE(SignalFeedbackPlan::Compile(3, next, scratch, view));
    EXPECT_FALSE(view.nodes[0].feedbackInput);
    EXPECT_DOUBLE_EQ(view.nodes[0].slope, kDuckingBaselineSlope);
    next[0] = {0, 1, std::numeric_limits<double>::infinity(), false};
    ASSERT_TRUE(SignalFeedbackPlan::Compile(3, next, scratch, view));
    EXPECT_DOUBLE_EQ(view.nodes[0].slope, 0);
    EXPECT_DOUBLE_EQ(view.nodes[2].slope, kDuckingBaselineSlope);
}

TEST(SignalFeedbackPlan, MovingFilterBoundCoversRealFilterStateAcrossCoefficientChanges) {
    for (float q : {.499f, .5f, .501f, 1.0f, 4.0f}) {
        SCOPED_TRACE(q);
        SignalBiquadBound bound;
        BiquadState state{};
        std::uint32_t random = 17;
        for (std::uint32_t block = 0; block < 400; ++block) {
            const float frequency = block < 200 ? 10000 - 9980 * (block / 199.0f)
                                                : 20 + 9980 * ((block - 200) / 199.0f);
            const auto coefficients = MakeLowpass(48000, frequency, q);
            const double maximum = bound.Advance(coefficients, 16);
            ASSERT_TRUE(std::isfinite(maximum));
            std::array<float, 16> samples;
            for (auto& sample : samples) {
                random = random * 1664525u + 1013904223u;
                sample = (random & 0x80000000u) ? 1.0f : -1.0f;
            }
            BiquadProcess(coefficients, state, samples.data(), samples.size());
            for (auto sample : samples) EXPECT_LE(std::abs(static_cast<double>(sample)), maximum);
        }
    }
}
