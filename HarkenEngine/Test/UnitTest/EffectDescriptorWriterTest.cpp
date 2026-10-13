// The load-bearing pin under building an effect descriptor from an index-ordered
// parameter array.
//
// The writer produces the common descriptor header followed by the type's
// parameters as floats in private index order. The engine owns the opposite
// direction and declares EffectParamsFromDesc the single source of that mapping,
// so the two agreeing is the whole basis for installing a compiled chain. This
// walks every row the generated registry publishes — never a hand-written list of
// the current five, so a sixth effect is covered the day it lands — writes a
// distinct probe value into every parameter, has the engine validate the bytes,
// reads them back through the engine's own mapping, and requires the values to
// come back bit-identical.
//
// Probe values are distinct per parameter index on purpose. Two parameters of one
// type frequently share a range (two shelf gains, two shelf frequencies), so
// midpoints would be equal and a writer that swapped them would still pass; each
// index therefore gets its own position inside its own resolved range. The
// comparison itself is a bit-pattern equality with no arithmetic, so nothing in the
// assertion can absorb a discrepancy.
//
// Ranges come from the engine's capability capture rather than the registry rows
// directly: the registry may hold sentinels for open or rate-dependent bounds,
// while the capture resolves them to concrete finite values for this engine.
#include "Common/TestUtil.h"

#include "harken_program_registration/EffectDescriptorWriter.h"

#include "harken/render/BuiltinEffects.h"
#include "harken/render/generated/harken_effect_registry.g.h"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using harkenprogramreg::EffectDescriptorBytes;
using harkenprogramreg::EffectDescriptorWriteStatus;
using harkenprogramreg::WriteEffectDescriptor;

constexpr std::uint32_t kRate = 48000;

bool ProbeBitsAlreadyUsed(float candidate, const std::vector<float>& probes,
                          std::uint32_t usedCount) {
    const std::uint32_t candidateBits =
        std::bit_cast<std::uint32_t>(candidate);
    for (std::uint32_t i = 0; i < usedCount; ++i) {
        if (std::bit_cast<std::uint32_t>(probes[i]) == candidateBits) {
            return true;
        }
    }
    return false;
}

// Start at the index-specific interior position, then resolve a bit collision
// by walking adjacent representable values toward either clamp bound. At most
// count earlier probes can occupy candidates, so the bounded walk finds a fresh
// in-range bit pattern before the assertion chain begins.
float UniqueProbeValue(float low, float high, std::uint32_t index,
                       std::uint32_t count,
                       const std::vector<float>& probes) {
    const float span = high - low;
    const float position =
        static_cast<float>(index + 1) / static_cast<float>(count + 1);
    const float initial = low + span * position;
    if (!ProbeBitsAlreadyUsed(initial, probes, index)) {
        return initial;
    }

    float upward = initial;
    float downward = initial;
    for (std::uint32_t step = 0; step <= count; ++step) {
        const float nextUp = std::nextafter(upward, high);
        if (nextUp != upward && nextUp <= high) {
            upward = nextUp;
            if (!ProbeBitsAlreadyUsed(upward, probes, index)) {
                return upward;
            }
        }
        const float nextDown = std::nextafter(downward, low);
        if (nextDown != downward && nextDown >= low) {
            downward = nextDown;
            if (!ProbeBitsAlreadyUsed(downward, probes, index)) {
                return downward;
            }
        }
    }
    return initial;
}

struct CapturedRegistry {
    std::vector<VnEffectCapability> types;
    std::vector<VnEffectParameterCapability> parameters;
};

CapturedRegistry CaptureRegistry() {
    CapturedRegistry captured;
    std::uint32_t required = 0;
    EXPECT_EQ(VN_OK, Vn_CaptureEffectCapabilities(nullptr, 0, &required));
    captured.types.resize(required);
    if (required > 0) {
        captured.types[0].structSize = sizeof(VnEffectCapability);
        EXPECT_EQ(VN_OK, Vn_CaptureEffectCapabilities(
                             captured.types.data(),
                             static_cast<std::uint32_t>(captured.types.size()),
                             &required));
    }
    required = 0;
    EXPECT_EQ(VN_OK,
              Vn_CaptureEffectParameterCapabilities(nullptr, 0, &required));
    captured.parameters.resize(required);
    if (required > 0) {
        captured.parameters[0].structSize =
            sizeof(VnEffectParameterCapability);
        EXPECT_EQ(VN_OK,
                  Vn_CaptureEffectParameterCapabilities(
                      captured.parameters.data(),
                      static_cast<std::uint32_t>(captured.parameters.size()),
                      &required));
    }
    return captured;
}

// The resolved range rows of one effect type, in parameter index order.
std::vector<VnEffectParameterCapability> RangesFor(
    const CapturedRegistry& captured, std::uint32_t effectType,
    std::uint32_t parameterCount) {
    std::vector<VnEffectParameterCapability> rows(parameterCount);
    std::vector<bool> seen(parameterCount, false);
    for (const VnEffectParameterCapability& row : captured.parameters) {
        if (row.effectType != effectType ||
            row.parameterIndex >= parameterCount) {
            continue;
        }
        rows[row.parameterIndex] = row;
        seen[row.parameterIndex] = true;
    }
    for (std::uint32_t i = 0; i < parameterCount; ++i) {
        EXPECT_TRUE(seen[i]) << "type " << effectType << " parameter " << i
                             << " has no capability row";
    }
    return rows;
}

// Every registered effect type: a generically built descriptor is accepted by the
// engine's own validator and reverses to exactly the values written.
TEST(EffectDescriptorWriter, RoundTripsEveryRegisteredTypeBitExactly) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(4, 256, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());

    const CapturedRegistry captured = CaptureRegistry();
    // The capture is the registry: covering it covers every published row.
    ASSERT_EQ(captured.types.size(), harken::kEffectRegistryRowCount);

    for (const VnEffectCapability& type : captured.types) {
        SCOPED_TRACE(testing::Message() << "effect type " << type.effectType);
        const std::vector<VnEffectParameterCapability> ranges =
            RangesFor(captured, type.effectType, type.parameterCount);
        ASSERT_EQ(ranges.size(), type.parameterCount);

        std::vector<float> probes(type.parameterCount);
        for (std::uint32_t i = 0; i < type.parameterCount; ++i) {
            probes[i] = UniqueProbeValue(
                ranges[i].clampMin, ranges[i].clampMax, i,
                type.parameterCount, probes);
        }
        // Distinctness is what makes a swapped pair of same-range parameters
        // visible; assert it rather than trusting the derivation.
        for (std::uint32_t i = 0; i + 1 < type.parameterCount; ++i) {
            for (std::uint32_t j = i + 1; j < type.parameterCount; ++j) {
                EXPECT_NE(std::bit_cast<std::uint32_t>(probes[i]),
                          std::bit_cast<std::uint32_t>(probes[j]))
                    << "probe values for parameters " << i << " and " << j
                    << " collide, so a swap between them would not be caught";
            }
        }

        EffectDescriptorBytes built;
        const VnObjectId effectId{0xa152432112345678ull, 0x123456789abcdef0ull};
        ASSERT_EQ(WriteEffectDescriptor(type.effectType, type.descriptorSize,
                                        type.descriptorVintage, probes.data(),
                                        type.parameterCount, &built, effectId),
                  EffectDescriptorWriteStatus::Ok);
        EXPECT_EQ(built.type, type.effectType);
        EXPECT_EQ(built.size, type.descriptorSize);
        // The header the descriptor contract fixes: the size the engine compares
        // for equality, then a zero reserved word.
        EXPECT_EQ(built.AsDesc()->structSize, type.descriptorSize);
        EXPECT_EQ(built.AsDesc()->reserved0, 0u);
        EXPECT_EQ(built.AsDesc()->effectId, effectId);

        // The engine's own validator accepts the bytes: sizes match its concrete
        // struct and every value is inside the range it enforces.
        EXPECT_EQ(harken::ValidateEffectDesc(type.effectType, built.data, kRate),
                  VN_OK);

        // The engine's own mapping reverses them, bit for bit.
        float recovered[harken::kMaxEffectParamCount] = {};
        const std::uint32_t recoveredCount = harken::EffectParamsFromDesc(
            type.effectType, built.data, recovered);
        ASSERT_EQ(recoveredCount, type.parameterCount);
        for (std::uint32_t i = 0; i < type.parameterCount; ++i) {
            EXPECT_EQ(std::bit_cast<std::uint32_t>(recovered[i]),
                      std::bit_cast<std::uint32_t>(probes[i]))
                << "parameter " << i << " did not survive the round trip";
        }
    }
}

// The layout gate: a size that is not the header plus one float per parameter
// means the type's descriptor is some other shape, so nothing is written.
TEST(EffectDescriptorWriter, RefusesADescriptorSizeThatIsNotHeaderPlusFloats) {
    const float params[2] = {0.0f, 0.0f};
    EffectDescriptorBytes built;
    built.size = 12345;
    EXPECT_EQ(WriteEffectDescriptor(0, /*descriptorSize=*/20,
                                    harkenprogramreg::
                                        kSupportedEffectDescriptorVintage,
                                    params, 2, &built),
              EffectDescriptorWriteStatus::LayoutUnsupported);
    EXPECT_EQ(built.size, 12345u) << "a refused write must touch nothing";
}

// The vintage gate: a descriptor layout this build has never seen is refused even
// when its size happens to satisfy the relation, which is what keeps a future
// non-float descriptor from being mis-written.
TEST(EffectDescriptorWriter, RefusesAnUnknownDescriptorVintage) {
    const float params[2] = {0.0f, 0.0f};
    EffectDescriptorBytes built;
    built.size = 12345;
    EXPECT_EQ(
        WriteEffectDescriptor(
            0, harkenprogramreg::kEffectDescriptorHeaderBytes + 4u * 2u,
            harkenprogramreg::kSupportedEffectDescriptorVintage + 1, params, 2,
            &built),
        EffectDescriptorWriteStatus::VintageUnsupported);
    EXPECT_EQ(built.size, 12345u) << "a refused write must touch nothing";
}

// The capacity gate: a descriptor wider than the buffer is refused, never
// truncated into it.
TEST(EffectDescriptorWriter, RefusesADescriptorWiderThanTheBuffer) {
    const std::uint32_t tooMany =
        harkenprogramreg::kMaxEffectDescriptorParams + 1;
    std::vector<float> params(tooMany, 0.0f);
    EffectDescriptorBytes built;
    EXPECT_EQ(
        WriteEffectDescriptor(
            0, harkenprogramreg::kEffectDescriptorHeaderBytes + 4u * tooMany,
            harkenprogramreg::kSupportedEffectDescriptorVintage, params.data(),
            tooMany, &built),
        EffectDescriptorWriteStatus::TooLarge);
}

// A parameterless type is a legal descriptor of header only.
TEST(EffectDescriptorWriter, WritesAHeaderOnlyDescriptor) {
    EffectDescriptorBytes built;
    ASSERT_EQ(
        WriteEffectDescriptor(
            7, harkenprogramreg::kEffectDescriptorHeaderBytes,
            harkenprogramreg::kSupportedEffectDescriptorVintage, nullptr, 0,
            &built),
        EffectDescriptorWriteStatus::Ok);
    EXPECT_EQ(built.type, 7u);
    EXPECT_EQ(built.size, harkenprogramreg::kEffectDescriptorHeaderBytes);
    EXPECT_EQ(built.AsDesc()->structSize,
              harkenprogramreg::kEffectDescriptorHeaderBytes);
    EXPECT_EQ(built.AsDesc()->reserved0, 0u);
}

}  // namespace
