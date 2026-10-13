#ifndef HARKEN_STUDIO_JSON_READER_H
#define HARKEN_STUDIO_JSON_READER_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace harkenstudiojson {

struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    std::string text; // String: decoded value; Number: literal text
    std::vector<JsonValue> items; // Array elements in order
    std::vector<std::pair<std::string, JsonValue>> members; // Object, in
    const JsonValue* Find(const std::string& key) const {
        if (kind != Kind::Object) {
            return nullptr;
        }
        for (const auto& member : members) {
            if (member.first == key) {
                return &member.second;
            }
        }
        return nullptr;
    }
    bool AsU64(std::uint64_t* out) const {
        if (kind != Kind::Number || text.empty()) {
            return false;
        }
        std::uint64_t value = 0;
        for (const char c : text) {
            if (c < '0' || c > '9') {
                return false; // minus sign, fraction, or exponent
            }
            const std::uint64_t digit =
                static_cast<std::uint64_t>(c - '0');
            if (value > (UINT64_MAX - digit) / 10) {
                return false; // would not fit 64 bits
            }
            value = value * 10 + digit;
        }
        *out = value;
        return true;
    }
};

namespace jsondetail {
struct Parser {
    const std::string& text;
    std::size_t pos = 0;
    std::string error;

    static constexpr int kMaxDepth = 64;

    bool allowUnicodeEscapes = false;

    explicit Parser(const std::string& t, bool unicodeEscapes = false) : text(t), allowUnicodeEscapes(unicodeEscapes) {}

    bool Fail(const std::string& message) {
        if (error.empty()) {
            error = message + " at byte " + std::to_string(pos);
        }
        return false;
    }

    void SkipWhitespace() {
        while (pos < text.size()) {
            const char c = text[pos];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++pos;
        }
    }

    bool ConsumeLiteral(const char* literal, std::size_t length) {
        if (text.compare(pos, length, literal, length) != 0) {
            return Fail("invalid literal");
        }
        pos += length;
        return true;
    }
    bool ParseUtf8Sequence(std::string* out) {
        const unsigned char lead = static_cast<unsigned char>(text[pos]);
        std::size_t length = 0;
        unsigned char firstLo = 0x80;
        unsigned char firstHi = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
        } else if (lead == 0xE0) {
            length = 3;
            firstLo = 0xA0;
        } else if (lead == 0xED) {
            length = 3;
            firstHi = 0x9F;
        } else if (lead >= 0xE1 && lead <= 0xEF) {
            length = 3;
        } else if (lead == 0xF0) {
            length = 4;
            firstLo = 0x90;
        } else if (lead == 0xF4) {
            length = 4;
            firstHi = 0x8F;
        } else if (lead >= 0xF1 && lead <= 0xF3) {
            length = 4;
        } else {
            return Fail("invalid UTF-8 byte in string");
        }
        if (pos + length > text.size()) {
            return Fail("truncated UTF-8 sequence in string");
        }
        for (std::size_t i = 1; i < length; ++i) {
            const unsigned char cont =
                static_cast<unsigned char>(text[pos + i]);
            const unsigned char lo = (i == 1) ? firstLo : 0x80;
            const unsigned char hi = (i == 1) ? firstHi : 0xBF;
            if (cont < lo || cont > hi) {
                return Fail("malformed UTF-8 sequence in string");
            }
        }
        out->append(text, pos, length);
        pos += length;
        return true;
    }

    bool ParseUnicodeEscape(std::string* out) {
        const auto readHex = [&](std::uint32_t* value) {
            if (pos + 4 > text.size()) return false;
            *value = 0;
            for (int i = 0; i < 4; ++i) {
                const char c = text[pos++];
                const int digit = c >= '0' && c <= '9' ? c - '0'
                    : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (digit < 0) return false;
                *value = (*value << 4) | static_cast<std::uint32_t>(digit);
            }
            return true;
        };
        std::uint32_t code = 0;
        if (!readHex(&code)) return Fail("invalid Unicode escape");
        if (code >= 0xD800 && code <= 0xDBFF) {
            if (text.compare(pos, 2, "\\u") != 0) return Fail("unpaired Unicode surrogate");
            pos += 2;
            std::uint32_t low = 0;
            if (!readHex(&low) || low < 0xDC00 || low > 0xDFFF) return Fail("unpaired Unicode surrogate");
            code = 0x10000 + ((code - 0xD800) << 10) + low - 0xDC00;
        } else if (code >= 0xDC00 && code <= 0xDFFF) return Fail("unpaired Unicode surrogate");
        if (code < 0x80) out->push_back(static_cast<char>(code));
        else {
            if (code < 0x800) out->push_back(static_cast<char>(0xC0 | (code >> 6)));
            else {
                if (code < 0x10000) out->push_back(static_cast<char>(0xE0 | (code >> 12)));
                else {
                    out->push_back(static_cast<char>(0xF0 | (code >> 18)));
                    out->push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
                }
                out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            }
            out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        return true;
    }

    bool ParseString(std::string* out) {
        ++pos; // opening quote already checked by the caller
        out->clear();
        while (pos < text.size()) {
            const unsigned char c =
                static_cast<unsigned char>(text[pos]);
            if (c == '"') {
                ++pos;
                return true;
            }
            if (c < 0x20) {
                return Fail("raw control character in string");
            }
            if (c >= 0x80) {
                if (!ParseUtf8Sequence(out)) {
                    return false;
                }
                continue;
            }
            if (c != '\\') {
                out->push_back(static_cast<char>(c));
                ++pos;
                continue;
            }
            if (pos + 1 >= text.size()) {
                return Fail("unterminated escape");
            }
            const char escape = text[pos + 1];
            pos += 2;
            switch (escape) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u':
                if (allowUnicodeEscapes) {
                    if (!ParseUnicodeEscape(out)) return false;
                    break;
                }
                pos -= 2;
                return Fail("\\u escapes are not supported — write the "
                            "character directly in UTF-8");
            default:
                pos -= 2;
                return Fail("unknown escape");
            }
        }
        return Fail("unterminated string");
    }

    bool ParseNumber(JsonValue* out) {
        const std::size_t start = pos;
        if (pos < text.size() && text[pos] == '-') {
            ++pos;
        }
        if (pos >= text.size() || text[pos] < '0' || text[pos] > '9') {
            return Fail("malformed number");
        }
        if (text[pos] == '0') {
            ++pos;
        } else {
            while (pos < text.size() && text[pos] >= '0' &&
                   text[pos] <= '9') {
                ++pos;
            }
        }
        if (pos < text.size() && text[pos] == '.') {
            ++pos;
            if (pos >= text.size() || text[pos] < '0' ||
                text[pos] > '9') {
                return Fail("malformed number");
            }
            while (pos < text.size() && text[pos] >= '0' &&
                   text[pos] <= '9') {
                ++pos;
            }
        }
        if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
            ++pos;
            if (pos < text.size() &&
                (text[pos] == '+' || text[pos] == '-')) {
                ++pos;
            }
            if (pos >= text.size() || text[pos] < '0' ||
                text[pos] > '9') {
                return Fail("malformed number");
            }
            while (pos < text.size() && text[pos] >= '0' &&
                   text[pos] <= '9') {
                ++pos;
            }
        }
        out->kind = JsonValue::Kind::Number;
        out->text = text.substr(start, pos - start);
        return true;
    }

    bool ParseValue(JsonValue* out, int depth) {
        if (depth > kMaxDepth) {
            return Fail("nesting too deep");
        }
        SkipWhitespace();
        if (pos >= text.size()) {
            return Fail("unexpected end of document");
        }
        const char c = text[pos];
        if (c == '{') {
            ++pos;
            out->kind = JsonValue::Kind::Object;
            SkipWhitespace();
            if (pos < text.size() && text[pos] == '}') {
                ++pos;
                return true;
            }
            for (;;) {
                SkipWhitespace();
                if (pos >= text.size() || text[pos] != '"') {
                    return Fail("expected object key string");
                }
                std::string key;
                if (!ParseString(&key)) {
                    return false;
                }
                for (const auto& member : out->members) {
                    if (member.first == key) {
                        return Fail("duplicate object key \"" + key +
                                    "\"");
                    }
                }
                SkipWhitespace();
                if (pos >= text.size() || text[pos] != ':') {
                    return Fail("expected ':' after key");
                }
                ++pos;
                JsonValue value;
                if (!ParseValue(&value, depth + 1)) {
                    return false;
                }
                out->members.emplace_back(std::move(key),
                                          std::move(value));
                SkipWhitespace();
                if (pos < text.size() && text[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (pos < text.size() && text[pos] == '}') {
                    ++pos;
                    return true;
                }
                return Fail("expected ',' or '}' in object");
            }
        }
        if (c == '[') {
            ++pos;
            out->kind = JsonValue::Kind::Array;
            SkipWhitespace();
            if (pos < text.size() && text[pos] == ']') {
                ++pos;
                return true;
            }
            for (;;) {
                JsonValue element;
                if (!ParseValue(&element, depth + 1)) {
                    return false;
                }
                out->items.push_back(std::move(element));
                SkipWhitespace();
                if (pos < text.size() && text[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (pos < text.size() && text[pos] == ']') {
                    ++pos;
                    return true;
                }
                return Fail("expected ',' or ']' in array");
            }
        }
        if (c == '"') {
            out->kind = JsonValue::Kind::String;
            return ParseString(&out->text);
        }
        if (c == 't') {
            out->kind = JsonValue::Kind::Bool;
            out->boolean = true;
            return ConsumeLiteral("true", 4);
        }
        if (c == 'f') {
            out->kind = JsonValue::Kind::Bool;
            out->boolean = false;
            return ConsumeLiteral("false", 5);
        }
        if (c == 'n') {
            out->kind = JsonValue::Kind::Null;
            return ConsumeLiteral("null", 4);
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return ParseNumber(out);
        }
        return Fail("unexpected character");
    }
};

} // namespace jsondetail
inline bool ParseJson(const std::string& text, JsonValue* out,
                      std::string* error, bool unicodeEscapes = false) {
    jsondetail::Parser parser(text, unicodeEscapes);
    if (!parser.ParseValue(out, 0)) {
        *error = parser.error;
        return false;
    }
    parser.SkipWhitespace();
    if (parser.pos != text.size()) {
        *error = "trailing content after document at byte " +
                 std::to_string(parser.pos);
        return false;
    }
    return true;
}

} // namespace harkenstudiojson

#endif // HARKEN_HOST_JSON_READER_H
