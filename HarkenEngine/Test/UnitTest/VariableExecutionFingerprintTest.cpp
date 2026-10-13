// Variable execution fingerprint: canonical F32, Discrete anchors in the
// execution stream, names only in the catalog domain, and deterministic
// FNV-1a 64 over HARKEN-VARIABLE-EXECUTION-V1 including NUL.
#include "Common/ObjectIdTestSupport.h"
#include "harken/core/VariableExecutionFingerprint.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {

using ComputeFn = bool (*)(VnObjectId, std::uint8_t, std::uint8_t,
                           std::uint8_t, float, float, float,
                           const std::int32_t*, std::uint32_t,
                           std::uint64_t*) noexcept;

constexpr ComputeFn kComputeExecution =
    &harken::ComputeVariableExecutionFingerprintV1;
constexpr std::uint64_t kSentinel = 0xdeadbeefdeadbeefull;

bool DiscreteFingerprint(const std::int32_t* anchors, std::uint32_t count,
                         std::uint64_t* out) {
    return harken::ComputeVariableExecutionFingerprintV1(
        harkentest::ObjectId(7), 0, 1, 1, 0.0f, 8.0f, 0.0f, anchors, count, out);
}

std::uint64_t CatalogNameHash(const char* name) {
    harken::MediaContentRevisionHasherV1 hash(
        harken::kVariableCatalogDomain, sizeof(harken::kVariableCatalogDomain));
    const auto n = static_cast<std::uint32_t>(std::strlen(name));
    hash.AddU32(n);
    hash.AddBytes(name, n);
    return hash.Finish();
}

} // namespace

TEST(VariableContractFingerprint, DiscreteAnchorsInExecution) {
    const std::int32_t first[] = {0, 1};
    const std::int32_t second[] = {0, 2};
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    ASSERT_TRUE(DiscreteFingerprint(first, 2, &a));
    ASSERT_TRUE(DiscreteFingerprint(second, 2, &b));
    EXPECT_NE(a, b);
}

TEST(VariableContractFingerprint, NamesExcludedFromExecution) {
    const std::int32_t anchors[] = {0, 1};
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    ASSERT_TRUE(kComputeExecution(harkentest::ObjectId(7), 0, 1, 1, 0.0f, 8.0f, 0.0f, anchors, 2,
                                  &first));
    ASSERT_TRUE(kComputeExecution(harkentest::ObjectId(7), 0, 1, 1, 0.0f, 8.0f, 0.0f, anchors, 2,
                                  &second));
    EXPECT_EQ(first, second);
    EXPECT_NE(CatalogNameHash("health"), CatalogNameHash("hp"));
}

TEST(VariableContractFingerprint, CatalogNameDoesNotBreakExecution) {
    const std::int32_t anchors[] = {0, 1};
    std::uint64_t execution = 0;
    ASSERT_TRUE(DiscreteFingerprint(anchors, 2, &execution));
    const std::uint64_t codeName = CatalogNameHash("health");
    const std::uint64_t displayName = CatalogNameHash("Health");
    EXPECT_NE(codeName, displayName);
    EXPECT_NE(execution, codeName);
    EXPECT_NE(execution, displayName);
    std::uint64_t again = 0;
    ASSERT_TRUE(DiscreteFingerprint(anchors, 2, &again));
    EXPECT_EQ(execution, again);
}

TEST(VariableContractFingerprint, NonCanonicalF32Rejected) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const float negativeZero = std::bit_cast<float>(0x80000000u);
    std::uint32_t bits = 0xabcdefu;
    EXPECT_FALSE(harken::CanonicalF32Bits(nan, &bits));
    EXPECT_EQ(0xabcdefu, bits);
    EXPECT_FALSE(harken::CanonicalF32Bits(inf, &bits));
    EXPECT_EQ(0xabcdefu, bits);
    EXPECT_FALSE(harken::CanonicalF32Bits(negativeZero, &bits));
    EXPECT_EQ(0xabcdefu, bits);

    std::uint64_t fingerprint = kSentinel;
    EXPECT_FALSE(harken::ComputeVariableExecutionFingerprintV1(
        harkentest::ObjectId(7), 1, 1, 1, 0.0f, 1.0f, nan, nullptr, 0, &fingerprint));
    EXPECT_EQ(kSentinel, fingerprint);
    EXPECT_FALSE(harken::ComputeVariableExecutionFingerprintV1(
        harkentest::ObjectId(7), 1, 1, 1, 0.0f, 1.0f, inf, nullptr, 0, &fingerprint));
    EXPECT_EQ(kSentinel, fingerprint);
    EXPECT_FALSE(harken::ComputeVariableExecutionFingerprintV1(
        harkentest::ObjectId(7), 1, 1, 1, negativeZero, 1.0f, 0.0f, nullptr, 0, &fingerprint));
    EXPECT_EQ(kSentinel, fingerprint);
}

TEST(VariableContractFingerprint, DuplicateAnchorRejected) {
    const std::int32_t anchors[] = {1, 1};
    std::uint64_t fingerprint = kSentinel;
    EXPECT_FALSE(DiscreteFingerprint(anchors, 2, &fingerprint));
    EXPECT_EQ(kSentinel, fingerprint);
}

TEST(VariableContractFingerprint, UnsortedAnchorRejected) {
    const std::int32_t anchors[] = {2, 1};
    std::uint64_t fingerprint = kSentinel;
    EXPECT_FALSE(DiscreteFingerprint(anchors, 2, &fingerprint));
    EXPECT_EQ(kSentinel, fingerprint);
}

TEST(VariableContractFingerprint, ContinuousRejectsAnchors) {
    const std::int32_t anchors[] = {0};
    std::uint64_t fingerprint = kSentinel;
    EXPECT_FALSE(harken::ComputeVariableExecutionFingerprintV1(
        harkentest::ObjectId(7), 1, 1, 1, 0.0f, 8.0f, 0.0f, anchors, 1, &fingerprint));
    EXPECT_EQ(kSentinel, fingerprint);
}

TEST(VariableContractFingerprint, DeterministicBytes) {
    const std::int32_t anchors[] = {0, 3, 8};
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    ASSERT_TRUE(DiscreteFingerprint(anchors, 3, &first));
    ASSERT_TRUE(DiscreteFingerprint(anchors, 3, &second));
    EXPECT_EQ(first, second);
    EXPECT_NE(0u, first);
}
