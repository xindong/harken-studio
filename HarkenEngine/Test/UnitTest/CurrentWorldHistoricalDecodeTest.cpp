#include "Common/ObjectIdTestSupport.h"
#include "harken/core/CurrentWorldCodec.h"
#include "harken/param/VariableService.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace {

class CurrentWorldHistoricalDecode : public testing::TestWithParam<std::uint32_t> {
protected:
    void SetUp() override {
        const auto directory = std::filesystem::path(__FILE__).parent_path() /
            "fixtures" / "current-world-pre443";
        const auto prefix = "v" + std::to_string(GetParam());
        for (const auto inlineBinding : {false, true}) {
            std::ifstream file(directory / (prefix + (inlineBinding
                ? "-inline-binding.bin" : "-without-inline-binding.bin")),
                std::ios::binary);
            ASSERT_TRUE(file.is_open());
            auto& bytes = inlineBinding ? unsupported_ : supported_;
            bytes.assign(std::istreambuf_iterator<char>(file),
                         std::istreambuf_iterator<char>());
            ASSERT_FALSE(file.bad());
        }
        payloadOffset_ = GetParam() == 5 ? 288u : GetParam() == 6 ? 312u : 316u;
        ASSERT_EQ(payloadOffset_ + 95u + (GetParam() == 7 ? 8u : 0u),
                  supported_.size());
        ASSERT_EQ(supported_.size() + 72u, unsupported_.size());
        harken::VariableDefinitionV1 variable{};
        variable.variableId = harkentest::ObjectId(900);
        variable.kind = harken::VariableKindV1::Smooth;
        variable.rangeMax = 1.0f;
        harken::ContainerWorldInstallViewV1 world{};
        world.variableCount = 1;
        world.variables = &variable;
        world.identity.capabilityMask = harken::kWorldCapabilityVariableDefinitions;
        world.identity.runtimeSemanticFingerprint =
            harken::ComputeCurrentWorldFingerprintV4(world);
        ASSERT_TRUE(harken::EncodeCurrentWorldShippingBody(world, shipping_));
        ASSERT_EQ(harken::CurrentWorldBodyDecodeResult::Ok,
                  production_.DecodeWithResult(shipping_.data(), shipping_.size(),
                      harken::kCurrentWorldShippingBodyVintage,
                      harken::CurrentWorldDecodeMode::StrictArtifact));
        ASSERT_NE(nullptr, production_.ProductionView());
    }

    void ExpectRejected(const std::vector<std::uint8_t>& bytes,
                        std::size_t byteCount) {
        const auto* production = production_.ProductionView();
        for (const auto mode : {harken::CurrentWorldDecodeMode::ReplayRawAttempt,
                                harken::CurrentWorldDecodeMode::StrictArtifact}) {
            SCOPED_TRACE(static_cast<unsigned>(mode));
            harken::CurrentWorldBodyImage empty;
            for (auto* image : {&empty, &production_}) {
                EXPECT_EQ(harken::CurrentWorldBodyDecodeResult::Invalid,
                          image->DecodeWithResult(bytes.data(), byteCount,
                                                  GetParam(), mode));
            }
            EXPECT_EQ(nullptr, empty.ProductionView());
            EXPECT_EQ(nullptr, empty.ReplayView());
            ASSERT_EQ(production, production_.ProductionView());
            EXPECT_EQ(nullptr, production_.ReplayView());
            ASSERT_EQ(1u, production->variableCount);
            EXPECT_EQ(harkentest::ObjectId(900u), production->variables[0].variableId);
            std::vector<std::uint8_t> after;
            ASSERT_TRUE(harken::EncodeCurrentWorldShippingBody(*production, after));
            EXPECT_EQ(shipping_, after);
        }
    }

    static void WriteU32(std::vector<std::uint8_t>& bytes, std::size_t offset,
                         std::uint32_t value) {
        ASSERT_LE(offset + 4, bytes.size());
        for (std::size_t i = 0; i < 4; ++i) {
            bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
        }
    }

    std::vector<std::uint8_t> supported_;
    std::vector<std::uint8_t> unsupported_;
    std::vector<std::uint8_t> shipping_;
    std::size_t payloadOffset_ = 0;
    harken::CurrentWorldBodyImage production_;
};

TEST_P(CurrentWorldHistoricalDecode, Pre443InlineBindingAndCurveAreUnsupported) {
    ExpectRejected(unsupported_, unsupported_.size());
}

TEST_P(CurrentWorldHistoricalDecode, Pre443WithoutInlineTablesIsRejected) {
    ExpectRejected(supported_, supported_.size());
}

TEST_P(CurrentWorldHistoricalDecode, TruncatedPre443InlineTablesLeaveOutputsIntact) {
    for (std::size_t length = 0; length < unsupported_.size(); ++length) {
        SCOPED_TRACE(length);
        ExpectRejected(unsupported_, length);
    }
}

TEST_P(CurrentWorldHistoricalDecode, NonzeroRetiredCountsLeaveOutputsIntact) {
    for (const std::size_t offset : {std::size_t{56}, std::size_t{60},
                                     payloadOffset_ + 79, payloadOffset_ + 83}) {
        SCOPED_TRACE(offset);
        for (const auto count : {1u,
                                 harken::kHardPlayablePropertyChildRowCapacity + 1u,
                                 std::numeric_limits<std::uint32_t>::max()}) {
            SCOPED_TRACE(count);
            auto bytes = supported_;
            WriteU32(bytes, offset, count);
            ExpectRejected(bytes, bytes.size());
        }
    }
}

TEST_P(CurrentWorldHistoricalDecode,
       NonzeroRetiredAttachmentWithoutTablesLeavesOutputsIntact) {
    const std::size_t attachmentOffset = payloadOffset_ + 67;
    for (const auto attachment : {UINT64_C(1), UINT64_C(9001), UINT64_C(1) << 32,
                                  std::numeric_limits<std::uint64_t>::max()}) {
        SCOPED_TRACE(attachment);
        auto bytes = supported_;
        WriteU32(bytes, attachmentOffset, static_cast<std::uint32_t>(attachment));
        WriteU32(bytes, attachmentOffset + 4,
                 static_cast<std::uint32_t>(attachment >> 32));
        ASSERT_NO_FATAL_FAILURE(ExpectRejected(bytes, bytes.size()));
    }
}

INSTANTIATE_TEST_SUITE_P(Pre443, CurrentWorldHistoricalDecode,
                        testing::Values(5u, 6u, 7u));

}
