// Wire-format primitives: frame header round trip, bounded payload
// reading, the string/array section encodings with their NULL sentinels,
// and the shared reply/error encoders. Pure byte-level cases — no engine,
// no pipe — proving the framing layer independently of transport.
#include "harken_remote/WireFormat.h"

#include <gtest/gtest.h>

#include <cstring>

namespace {

using namespace harkenremote;

TEST(RemoteWireFormat, FrameHeaderRoundTrip) {
    WireFrameHeader in;
    in.payloadBytes = 0x00ABCDEFu;
    in.msgType = kMsgCommand;
    in.flags = kFlagAdvisory;
    in.sequence = 0xDEADBEEFu;
    std::uint8_t bytes[kFrameHeaderBytes];
    EncodeFrameHeader(in, bytes);
    WireFrameHeader out;
    DecodeFrameHeader(bytes, &out);
    EXPECT_EQ(in.payloadBytes, out.payloadBytes);
    EXPECT_EQ(in.msgType, out.msgType);
    EXPECT_EQ(in.flags, out.flags);
    EXPECT_EQ(in.sequence, out.sequence);
    // The layout is normative: u32 length little-endian first, so the
    // low byte of payloadBytes is byte 0 on the wire.
    EXPECT_EQ(0xEFu, bytes[0]);
    EXPECT_EQ(0xCDu, bytes[1]);
    EXPECT_EQ(0xABu, bytes[2]);
}

TEST(RemoteWireFormat, AppendFrameBytesCarriesHeaderAndPayload) {
    WireFrame frame;
    frame.msgType = kMsgReply;
    frame.flags = 0;
    frame.sequence = 7;
    frame.payload = {1, 2, 3, 4, 5};
    std::vector<std::uint8_t> bytes;
    AppendFrameBytes(frame, &bytes);
    ASSERT_EQ(kFrameHeaderBytes + 5u, bytes.size());
    WireFrameHeader header;
    DecodeFrameHeader(bytes.data(), &header);
    EXPECT_EQ(5u, header.payloadBytes);
    EXPECT_EQ(kMsgReply, header.msgType);
    EXPECT_EQ(7u, header.sequence);
    EXPECT_EQ(0, std::memcmp(bytes.data() + kFrameHeaderBytes,
                             frame.payload.data(), 5));
}

TEST(RemoteWireFormat, ReaderRefusesShortReads) {
    const std::uint8_t bytes[6] = {1, 0, 0, 0, 2, 0};
    WireReader reader(bytes, sizeof(bytes));
    std::uint32_t u32 = 0;
    EXPECT_TRUE(reader.ReadU32(&u32));
    EXPECT_EQ(1u, u32);
    std::uint64_t u64 = 0;
    EXPECT_FALSE(reader.ReadU64(&u64)); // only two bytes remain
    std::uint16_t u16 = 0;
    EXPECT_TRUE(reader.ReadU16(&u16));
    EXPECT_EQ(2u, u16);
    EXPECT_TRUE(reader.AtEnd());
    EXPECT_EQ(nullptr, reader.ReadBytes(1));
}

TEST(RemoteWireFormat, StringEncodingRoundTripAndSentinels) {
    std::vector<std::uint8_t> payload;
    WireWriter writer(&payload);
    writer.String(nullptr);
    writer.String("");
    writer.String("packs/\xE9\x9F\xB3.pack"); // UTF-8 survives byte-exactly

    WireReader reader(payload.data(), payload.size());
    WireArena arena;
    const char* nullString = "sentinel";
    ASSERT_TRUE(ReadWireString(reader, arena, &nullString));
    EXPECT_EQ(nullptr, nullString);
    const char* empty = nullptr;
    ASSERT_TRUE(ReadWireString(reader, arena, &empty));
    ASSERT_NE(nullptr, empty);
    EXPECT_STREQ("", empty);
    const char* utf8 = nullptr;
    ASSERT_TRUE(ReadWireString(reader, arena, &utf8));
    ASSERT_NE(nullptr, utf8);
    EXPECT_STREQ("packs/\xE9\x9F\xB3.pack", utf8);
    EXPECT_TRUE(reader.AtEnd());
}

TEST(RemoteWireFormat, StringSectionRefusesTruncatedBytes) {
    std::vector<std::uint8_t> payload;
    WireWriter writer(&payload);
    writer.U32(10); // announces ten bytes ...
    writer.Bytes("abc", 3); // ... delivers three
    WireReader reader(payload.data(), payload.size());
    WireArena arena;
    const char* out = nullptr;
    EXPECT_FALSE(ReadWireString(reader, arena, &out));
}

TEST(RemoteWireFormat, ArraySectionValidatesExactStrideProduct) {
    struct Element {
        std::uint64_t a;
        std::uint32_t b;
        std::uint32_t c;
    };
    const Element elements[2] = {{1, 2, 3}, {4, 5, 6}};

    std::vector<std::uint8_t> good;
    {
        WireWriter writer(&good);
        writer.U32(static_cast<std::uint32_t>(sizeof(elements)));
        writer.Bytes(elements, sizeof(elements));
    }
    {
        WireReader reader(good.data(), good.size());
        WireArena arena;
        const void* out = nullptr;
        ASSERT_TRUE(ReadArraySection(reader, arena, 2, sizeof(Element), true,
                                     &out));
        ASSERT_NE(nullptr, out);
        EXPECT_EQ(0, std::memcmp(out, elements, sizeof(elements)));
        EXPECT_TRUE(reader.AtEnd());
    }
    // byteLength != count * stride is refused when the stride is known.
    {
        WireReader reader(good.data(), good.size());
        WireArena arena;
        const void* out = nullptr;
        EXPECT_FALSE(ReadArraySection(reader, arena, 3, sizeof(Element), true,
                                      &out));
    }
    // An unknown stride vintage passes the bytes through: the engine's
    // own vintage validation refuses the desc, not the wire layer.
    {
        WireReader reader(good.data(), good.size());
        WireArena arena;
        const void* out = nullptr;
        ASSERT_TRUE(ReadArraySection(reader, arena, 3, 0, false, &out));
        ASSERT_NE(nullptr, out);
    }
    // The NULL sentinel decodes to a null pointer.
    {
        std::vector<std::uint8_t> nul;
        WireWriter writer(&nul);
        writer.U32(kWireNullLength);
        WireReader reader(nul.data(), nul.size());
        WireArena arena;
        const void* out = reinterpret_cast<const void*>(0x1);
        ASSERT_TRUE(ReadArraySection(reader, arena, 2, sizeof(Element), true,
                                     &out));
        EXPECT_EQ(nullptr, out);
    }
}

TEST(RemoteWireFormat, ReplyEncodingLayout) {
    WireFrame reply;
    const std::uint64_t outs[2] = {0x1111222233334444ull, 5};
    EncodeReply(&reply, 42, 19, VN_OK, outs, 2);
    EXPECT_EQ(kMsgReply, reply.msgType);
    EXPECT_EQ(42u, reply.sequence);
    ASSERT_EQ(2u + 2u + 4u + 16u, reply.payload.size());
    WireReader reader(reply.payload.data(), reply.payload.size());
    std::uint16_t opcode = 0;
    std::uint16_t reserved = 0;
    std::int32_t result = -1;
    std::uint64_t out0 = 0;
    std::uint64_t out1 = 0;
    ASSERT_TRUE(reader.ReadU16(&opcode));
    ASSERT_TRUE(reader.ReadU16(&reserved));
    ASSERT_TRUE(reader.ReadI32(&result));
    ASSERT_TRUE(reader.ReadU64(&out0));
    ASSERT_TRUE(reader.ReadU64(&out1));
    EXPECT_EQ(19u, opcode);
    EXPECT_EQ(0u, reserved);
    EXPECT_EQ(VN_OK, result);
    EXPECT_EQ(outs[0], out0);
    EXPECT_EQ(outs[1], out1);
}

TEST(RemoteWireFormat, ErrorEncodingLayout) {
    WireFrame error;
    EncodeWireError(&error, 9, kWireErrUnknownOpcode, 999);
    EXPECT_EQ(kMsgError, error.msgType);
    EXPECT_EQ(9u, error.sequence);
    ASSERT_EQ(4u, error.payload.size());
    WireReader reader(error.payload.data(), error.payload.size());
    std::uint16_t code = 0;
    std::uint16_t opcode = 0;
    ASSERT_TRUE(reader.ReadU16(&code));
    ASSERT_TRUE(reader.ReadU16(&opcode));
    EXPECT_EQ(kWireErrUnknownOpcode, code);
    EXPECT_EQ(999u, opcode);
}

} // namespace
