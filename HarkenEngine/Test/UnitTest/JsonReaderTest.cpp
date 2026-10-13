// The manifest JSON reader's parsing contract, tested through the pure
// parser the pack compiler uses (host pure-helper precedent): the
// accepted strict subset round-trips into the value tree, and every
// documented refusal — trailing content, trailing commas, duplicate keys,
// comments, control characters, unsupported escapes, malformed numbers —
// fails with an error instead of guessing.
#include "JsonReader.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace {

using harkenhost::JsonValue;
using harkenhost::ParseJson;

bool Parses(const std::string& text, JsonValue* out) {
    std::string error;
    return ParseJson(text, out, &error);
}

bool Refuses(const std::string& text) {
    JsonValue value;
    std::string error;
    const bool ok = ParseJson(text, &value, &error);
    // A refusal always carries a message; a silent failure would leave
    // the tool with nothing to print.
    return !ok && !error.empty();
}

TEST(JsonReader, ParsesManifestShapedDocument) {
    const std::string text =
        "{\n"
        "  \"sounds\": [\n"
        "    { \"path\": \"a.wav\" },\n"
        "    { \"path\": \"b.wav\", \"id\": 42,\n"
        "      \"loopStartFrame\": 0, \"loopEndFrame\": 48000 }\n"
        "  ]\n"
        "}\n";
    JsonValue root;
    ASSERT_TRUE(Parses(text, &root));
    ASSERT_EQ(JsonValue::Kind::Object, root.kind);

    const JsonValue* sounds = root.Find("sounds");
    ASSERT_NE(nullptr, sounds);
    ASSERT_EQ(JsonValue::Kind::Array, sounds->kind);
    ASSERT_EQ(2u, sounds->items.size());

    const JsonValue* path = sounds->items[0].Find("path");
    ASSERT_NE(nullptr, path);
    EXPECT_EQ(JsonValue::Kind::String, path->kind);
    EXPECT_EQ("a.wav", path->text);
    EXPECT_EQ(nullptr, sounds->items[0].Find("id"));

    std::uint64_t id = 0;
    const JsonValue* idValue = sounds->items[1].Find("id");
    ASSERT_NE(nullptr, idValue);
    ASSERT_TRUE(idValue->AsU64(&id));
    EXPECT_EQ(42u, id);
}

TEST(JsonReader, ParsesScalarsEscapesAndNesting) {
    JsonValue value;
    ASSERT_TRUE(Parses("[true, false, null, -1.5e3, \"a\\n\\\"b\\\\\"]",
                       &value));
    ASSERT_EQ(5u, value.items.size());
    EXPECT_EQ(JsonValue::Kind::Bool, value.items[0].kind);
    EXPECT_TRUE(value.items[0].boolean);
    EXPECT_FALSE(value.items[1].boolean);
    EXPECT_EQ(JsonValue::Kind::Null, value.items[2].kind);
    EXPECT_EQ(JsonValue::Kind::Number, value.items[3].kind);
    EXPECT_EQ("-1.5e3", value.items[3].text);
    EXPECT_EQ("a\n\"b\\", value.items[4].text);

    // UTF-8 passes through verbatim.
    ASSERT_TRUE(Parses("\"路径/音频.wav\"", &value));
    EXPECT_EQ("路径/音频.wav", value.text);

    // Empty containers are valid values.
    ASSERT_TRUE(Parses("{ \"a\": [], \"b\": {} }", &value));
    EXPECT_EQ(2u, value.members.size());
}

TEST(JsonReader, AsU64ConvertsOnlyCleanIntegers) {
    JsonValue value;
    std::uint64_t out = 7;

    ASSERT_TRUE(Parses("0", &value));
    EXPECT_TRUE(value.AsU64(&out));
    EXPECT_EQ(0u, out);

    ASSERT_TRUE(Parses("18446744073709551615", &value)); // UINT64_MAX
    EXPECT_TRUE(value.AsU64(&out));
    EXPECT_EQ(UINT64_MAX, out);

    // One past UINT64_MAX must refuse, not wrap.
    ASSERT_TRUE(Parses("18446744073709551616", &value));
    EXPECT_FALSE(value.AsU64(&out));

    ASSERT_TRUE(Parses("-3", &value));
    EXPECT_FALSE(value.AsU64(&out));
    ASSERT_TRUE(Parses("1.5", &value));
    EXPECT_FALSE(value.AsU64(&out));
    ASSERT_TRUE(Parses("1e3", &value));
    EXPECT_FALSE(value.AsU64(&out));

    // Non-numbers refuse regardless of content.
    ASSERT_TRUE(Parses("\"42\"", &value));
    EXPECT_FALSE(value.AsU64(&out));
}

TEST(JsonReader, RefusesMalformedDocuments) {
    EXPECT_TRUE(Refuses(""));
    EXPECT_TRUE(Refuses("{"));
    EXPECT_TRUE(Refuses("{ \"a\": }"));
    EXPECT_TRUE(Refuses("{ a: 1 }"));            // unquoted key
    EXPECT_TRUE(Refuses("[1, 2,]"));             // trailing comma
    EXPECT_TRUE(Refuses("{ \"a\": 1, }"));       // trailing comma
    EXPECT_TRUE(Refuses("[1] extra"));           // trailing content
    EXPECT_TRUE(Refuses("[1][2]"));              // two documents
    EXPECT_TRUE(Refuses("// comment\n[1]"));     // comments
    EXPECT_TRUE(Refuses("{ \"a\": 1, \"a\": 2 }")); // duplicate key
    EXPECT_TRUE(Refuses("True"));                // wrong literal case
    EXPECT_TRUE(Refuses("nul"));                 // truncated literal
}

TEST(JsonReader, RefusesMalformedStringsAndNumbers) {
    EXPECT_TRUE(Refuses("\"unterminated"));
    EXPECT_TRUE(Refuses("\"bad \\x escape\""));
    EXPECT_TRUE(Refuses("\"unicode \\u0041\"")); // documented refusal
    EXPECT_TRUE(Refuses(std::string("\"ctl \x01\""))); // raw control char
    EXPECT_TRUE(Refuses("01"));   // leading zero
    EXPECT_TRUE(Refuses("-"));
    EXPECT_TRUE(Refuses("1."));
    EXPECT_TRUE(Refuses("1e"));
    EXPECT_TRUE(Refuses("+1"));
    EXPECT_TRUE(Refuses(".5"));
}

TEST(JsonReader, ValidatesRawUtf8InStrings) {
    // Well-formed sequences of every length pass through byte-exact.
    JsonValue value;
    ASSERT_TRUE(Parses("\"\xC3\xA9\"", &value)); // U+00E9, two bytes
    EXPECT_EQ("\xC3\xA9", value.text);
    ASSERT_TRUE(Parses("\"\xF0\x9F\x8E\xB5\"", &value)); // U+1F3B5
    EXPECT_EQ("\xF0\x9F\x8E\xB5", value.text);

    // Ill-formed byte sequences refuse instead of passing through.
    EXPECT_TRUE(Refuses("\"\xC3\x28\""));     // invalid continuation
    EXPECT_TRUE(Refuses("\"\xC0\xAF\""));     // overlong two-byte '/'
    EXPECT_TRUE(Refuses("\"\xE0\x80\x80\"")); // overlong three-byte
    EXPECT_TRUE(Refuses("\"\xED\xA0\x80\"")); // surrogate U+D800
    EXPECT_TRUE(Refuses("\"\x80\""));         // lone continuation byte
    EXPECT_TRUE(Refuses("\"\xF4\x90\x80\x80\"")); // above U+10FFFF
    EXPECT_TRUE(Refuses("\"\xF5\x80\x80\x80\"")); // invalid lead byte
    EXPECT_TRUE(Refuses("\"\xE2\x82\""));     // sequence cut short
}

TEST(JsonReader, RefusesExcessiveNesting) {
    // One past the depth cap must refuse instead of recursing without
    // bound; at the cap it still parses.
    std::string deep;
    for (int i = 0; i < 65; ++i) {
        deep += '[';
    }
    deep += "1";
    for (int i = 0; i < 65; ++i) {
        deep += ']';
    }
    EXPECT_TRUE(Refuses(deep));

    std::string atCap;
    for (int i = 0; i < 63; ++i) {
        atCap += '[';
    }
    atCap += "1";
    for (int i = 0; i < 63; ++i) {
        atCap += ']';
    }
    JsonValue value;
    EXPECT_TRUE(Parses(atCap, &value));
}

} // namespace
