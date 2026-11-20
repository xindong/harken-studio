#include "Setup/SetupPayload.h"

#include <gtest/gtest.h>
#include <QCryptographicHash>

namespace {

struct PayloadFixture {
    QByteArray file;
    QByteArray manifest;
    QByteArray blocks;

    PayloadFixture()
    {
        QByteArray block(4096, '\0');
        for (int i = 0; i < block.size(); ++i)
            block[i] = static_cast<char>(i * 31);
        const QByteArray tail("\0signed package trailer\xff", 24);
        file = block + block + tail;
        blocks = block + tail + QByteArray(4096 - tail.size(), '\0');
        manifest = QByteArray("HKMSI001", 8);
        const uint32_t size = static_cast<uint32_t>(file.size());
        manifest.append(reinterpret_cast<const char*>(&size), sizeof(size));
        manifest += QCryptographicHash::hash(file, QCryptographicHash::Sha256);
        const uint32_t indices[] = {0, 0, 1};
        manifest.append(reinterpret_cast<const char*>(indices), sizeof(indices));
    }

    bool unpack(std::vector<unsigned char>& output) const
    {
        return HarkenSetup::unpackPayload(manifest.constData(), static_cast<DWORD>(manifest.size()),
            blocks.constData(), static_cast<DWORD>(blocks.size()), output);
    }
};

}

TEST(SetupPayloadTest, RestoresExactBytesIncludingRepeatedDataAndPartialTail)
{
    PayloadFixture fixture;
    std::vector<unsigned char> output;
    ASSERT_TRUE(fixture.unpack(output));
    EXPECT_EQ(QByteArray(reinterpret_cast<const char*>(output.data()), output.size()), fixture.file);
}

TEST(SetupPayloadTest, RejectsTruncatedAndOutOfBoundsMetadata)
{
    const PayloadFixture original;
    const std::vector<unsigned char> sentinel = {1, 2, 3};
    for (qsizetype length = 0; length < original.manifest.size(); ++length) {
        PayloadFixture fixture = original;
        fixture.manifest.truncate(length);
        auto output = sentinel;
        EXPECT_FALSE(fixture.unpack(output));
        EXPECT_EQ(output, sentinel);
    }
    for (int offset : {0, 8, 44}) {
        PayloadFixture fixture = original;
        std::memset(fixture.manifest.data() + offset, 0xff, 4);
        auto output = sentinel;
        EXPECT_FALSE(fixture.unpack(output));
        EXPECT_EQ(output, sentinel);
    }
    PayloadFixture fixture = original;
    fixture.blocks.chop(1);
    auto output = sentinel;
    EXPECT_FALSE(fixture.unpack(output));
    fixture = original;
    fixture.manifest.append('\0');
    EXPECT_FALSE(fixture.unpack(output));
}

TEST(SetupPayloadTest, RejectsCorruptedDataAndDigestWithoutReturningPartialFile)
{
    for (bool changeDigest : {false, true}) {
        PayloadFixture fixture;
        auto& bytes = changeDigest ? fixture.manifest : fixture.blocks;
        bytes[12] = static_cast<char>(bytes[12] ^ 0x80);
        const std::vector<unsigned char> sentinel = {1, 2, 3};
        auto output = sentinel;
        EXPECT_FALSE(fixture.unpack(output));
        EXPECT_EQ(output, sentinel);
    }
}
