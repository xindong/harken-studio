#include "Security/TrustAnchors.h"

#include <gtest/gtest.h>

using VNH::TrustAnchors;

namespace {

class TrustAnchorsTest : public ::testing::Test
{
protected:
    void TearDown() override { qunsetenv("HARKEN_HUB_MANIFEST_PUBKEY"); }
};

}

TEST_F(TrustAnchorsTest, BuiltinReleaseKeyIsServedWithoutOverride)
{
    qunsetenv("HARKEN_HUB_MANIFEST_PUBKEY");
    const QByteArray key = TrustAnchors::manifestPublicKey();
    ASSERT_EQ(key.size(), 32);
    EXPECT_EQ(key.toHex(),
              QByteArrayLiteral("bc2fea5095f03d2f9512407325a5c61c68b3e089"
                                "01fd10db355891f052ac6f0f"));
}

TEST_F(TrustAnchorsTest, EnvironmentOverrideWinsOverBuiltin)
{
    qputenv("HARKEN_HUB_MANIFEST_PUBKEY", QByteArray(64, 'a'));
    const QByteArray key = TrustAnchors::manifestPublicKey();
    ASSERT_EQ(key.size(), 32);
    EXPECT_EQ(key.toHex(), QByteArray(64, 'a'));
}

TEST_F(TrustAnchorsTest, MalformedOverrideFailsClosed)
{
    qputenv("HARKEN_HUB_MANIFEST_PUBKEY", "not-hex");
    EXPECT_TRUE(TrustAnchors::manifestPublicKey().isEmpty());

    qputenv("HARKEN_HUB_MANIFEST_PUBKEY", QByteArray(63, 'a'));
    EXPECT_TRUE(TrustAnchors::manifestPublicKey().isEmpty());
}
