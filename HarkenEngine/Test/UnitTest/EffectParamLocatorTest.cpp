// Frozen ABI of the effect-parameter binding target scopes and their packed
// target locator. The scope values are append-only and must never renumber; the
// locator bit layout (index 0..5, slot 6..8, type 9..31) is the public targetId
// encoding for those scopes, so its pack/unpack round trip and field ranges are
// pinned here. Nothing derives an expected value from the code under test.
#include <harken/Harken.h>

#include "harken/param/EffectParamLocator.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

// --- scope value freeze (append-only after VN_TARGET_SCOPE_BUS = 1) ---

static_assert(VN_TARGET_SCOPE_VOICE == 0, "scope value must not renumber");
static_assert(VN_TARGET_SCOPE_BUS == 1, "scope value must not renumber");
static_assert(VN_TARGET_SCOPE_VOICE_EFFECT_PARAM == 2,
              "appended scope must keep its value");
static_assert(VN_TARGET_SCOPE_BUS_EFFECT_PARAM == 3,
              "appended scope must keep its value");

// --- locator bit layout freeze ---

static_assert(harken::kEffectParamLocatorIndexShift == 0, "index field at bit 0");
static_assert(harken::kEffectParamLocatorSlotShift == 6, "slot field at bit 6");
static_assert(harken::kEffectParamLocatorTypeShift == 9, "type field at bit 9");
static_assert(harken::kEffectParamLocatorMaxIndex == 63, "index is 6 bits");
static_assert(harken::kEffectParamLocatorMaxSlot == 7, "slot is 3 bits");
static_assert(harken::kEffectParamLocatorMaxType == 0x7FFFFFu, "type is 23 bits");

// The three fields tile the 32-bit word without overlap: index(6) + slot(3) +
// type(23) = 32.
static_assert(harken::kEffectParamLocatorIndexShift +
                      6u == harken::kEffectParamLocatorSlotShift &&
                  harken::kEffectParamLocatorSlotShift +
                          3u == harken::kEffectParamLocatorTypeShift,
              "locator fields must be contiguous and non-overlapping");

// --- pack/unpack round trip (compile-time) ---

static_assert(harken::UnpackEffectParamLocator(
                  harken::PackEffectParamLocator(2, 5, 3))
                      .effectType == 2,
              "type survives round trip");
static_assert(harken::UnpackEffectParamLocator(
                  harken::PackEffectParamLocator(2, 5, 3))
                      .slot == 5,
              "slot survives round trip");
static_assert(harken::UnpackEffectParamLocator(
                  harken::PackEffectParamLocator(2, 5, 3))
                      .paramIndex == 3,
              "index survives round trip");

TEST(EffectParamLocator, PackUnpackRoundTripsAcrossFields) {
    // A spread of distinct field values, including the per-field maxima, must
    // survive pack -> unpack unchanged and land in the documented bit ranges.
    const struct {
        std::uint32_t type;
        std::uint32_t slot;
        std::uint32_t index;
    } cases[] = {
        {0, 0, 0},
        {2, 5, 3},
        {harken::kEffectParamLocatorMaxType, harken::kEffectParamLocatorMaxSlot,
         harken::kEffectParamLocatorMaxIndex},
        {1, 7, 63},
        {8388606, 1, 6},
    };
    for (const auto& c : cases) {
        ASSERT_TRUE(
            harken::EffectParamLocatorInRange(c.type, c.slot, c.index));
        const std::uint32_t packed =
            harken::PackEffectParamLocator(c.type, c.slot, c.index);
        const harken::EffectParamLocator loc =
            harken::UnpackEffectParamLocator(packed);
        EXPECT_EQ(c.type, loc.effectType);
        EXPECT_EQ(c.slot, loc.slot);
        EXPECT_EQ(c.index, loc.paramIndex);
    }
}

TEST(EffectParamLocator, FieldsOccupyDisjointBits) {
    // Each field, set alone to its max, must not spill into another field's
    // bits: a max index leaves slot and type zero, and so on.
    const harken::EffectParamLocator onlyIndex = harken::UnpackEffectParamLocator(
        harken::PackEffectParamLocator(0, 0, harken::kEffectParamLocatorMaxIndex));
    EXPECT_EQ(harken::kEffectParamLocatorMaxIndex, onlyIndex.paramIndex);
    EXPECT_EQ(0u, onlyIndex.slot);
    EXPECT_EQ(0u, onlyIndex.effectType);

    const harken::EffectParamLocator onlySlot = harken::UnpackEffectParamLocator(
        harken::PackEffectParamLocator(0, harken::kEffectParamLocatorMaxSlot, 0));
    EXPECT_EQ(0u, onlySlot.paramIndex);
    EXPECT_EQ(harken::kEffectParamLocatorMaxSlot, onlySlot.slot);
    EXPECT_EQ(0u, onlySlot.effectType);

    const harken::EffectParamLocator onlyType = harken::UnpackEffectParamLocator(
        harken::PackEffectParamLocator(harken::kEffectParamLocatorMaxType, 0, 0));
    EXPECT_EQ(0u, onlyType.paramIndex);
    EXPECT_EQ(0u, onlyType.slot);
    EXPECT_EQ(harken::kEffectParamLocatorMaxType, onlyType.effectType);
}

TEST(EffectParamLocator, RangeCheckRejectsOverflowingFields) {
    // Fields at their maxima are in range; one past a maximum is not.
    EXPECT_TRUE(harken::EffectParamLocatorInRange(
        harken::kEffectParamLocatorMaxType, harken::kEffectParamLocatorMaxSlot,
        harken::kEffectParamLocatorMaxIndex));
    EXPECT_FALSE(harken::EffectParamLocatorInRange(
        harken::kEffectParamLocatorMaxType + 1, 0, 0));
    EXPECT_FALSE(harken::EffectParamLocatorInRange(0, 8, 0));  // slot > 7
    EXPECT_FALSE(harken::EffectParamLocatorInRange(0, 0, 64)); // index > 63
}

} // namespace
