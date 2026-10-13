// Minimal deterministic JSON writer for the host tools' generated files
// (build cache, validation report, normalized v2 manifest). Counterpart
// of JsonReader.h: everything this writer emits, that strict reader
// accepts. Determinism is the point — members appear exactly in call
// order, indentation is fixed at two spaces with LF newlines, numbers are
// emitted through locale-free code paths, and nothing derived from time,
// process, or iteration order of unordered containers ever enters the
// text. Identical logical content therefore serializes to identical
// bytes, so generated files can be diffed and byte-compared in tests.
//
// String handling mirrors the reader's strictness: '"' and '\\' are
// escaped, the five short control escapes (\b \f \n \r \t) are used for
// their bytes, and any other control byte below 0x20 is replaced with
// '_' — such a byte cannot appear in a real path or in any value this
// project generates, and the reader would refuse both the raw byte and
// its \u escape form. Non-ASCII bytes pass through verbatim (inputs are
// UTF-8 by contract; the reader validates on the way back in).
#ifndef HARKEN_HOST_JSON_WRITER_H
#define HARKEN_HOST_JSON_WRITER_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace harkenhost {

class JsonWriter {
public:
    void BeginObject() {
        BeginValue();
        text_ += '{';
        frames_.push_back({false, 0});
    }

    void EndObject() {
        CloseFrame('}');
    }

    void BeginArray() {
        BeginValue();
        text_ += '[';
        frames_.push_back({true, 0});
    }

    void EndArray() {
        CloseFrame(']');
    }

    void Key(const char* name) {
        Separate();
        NewlineIndent();
        AppendString(name);
        text_ += ": ";
        keyPending_ = true;
    }

    void ValueString(const std::string& value) {
        BeginValue();
        AppendString(value.c_str());
    }

    void ValueU64(std::uint64_t value) {
        BeginValue();
        AppendU64(value);
    }

    void ValueBool(bool value) {
        BeginValue();
        text_ += value ? "true" : "false";
    }

    // Emits a pre-formatted JSON number literal (e.g. a %.9g float). The
    // caller owns making the literal valid JSON.
    void ValueRawNumber(const std::string& literal) {
        BeginValue();
        text_ += literal;
    }

    // Whole-document accessor; call after the root value is closed. The
    // trailing LF makes the file a well-formed text file.
    std::string Finish() {
        return text_ + "\n";
    }

    // Fixed-width lowercase hex images for checksum-like fields, where a
    // stable textual width keeps the file layout uniform.
    static std::string HexU64(std::uint64_t value) {
        char buffer[17];
        std::snprintf(buffer, sizeof buffer, "%016llx",
                      static_cast<unsigned long long>(value));
        return std::string(buffer);
    }

    static std::string DecimalU64(std::uint64_t value) {
        char buffer[21];
        std::snprintf(buffer, sizeof buffer, "%llu",
                      static_cast<unsigned long long>(value));
        return std::string(buffer);
    }

private:
    struct Frame {
        bool isArray = false;
        std::size_t count = 0;
    };

    void Separate() {
        if (!frames_.empty() && frames_.back().count > 0) {
            text_ += ',';
        }
        if (!frames_.empty()) {
            ++frames_.back().count;
        }
    }

    void NewlineIndent() {
        text_ += '\n';
        text_.append(frames_.size() * 2, ' ');
    }

    // Array elements separate and indent themselves; object values follow
    // their key on the same line.
    void BeginValue() {
        if (keyPending_) {
            keyPending_ = false;
            return;
        }
        if (!frames_.empty() && frames_.back().isArray) {
            Separate();
            NewlineIndent();
        }
    }

    void CloseFrame(char closer) {
        const bool hadMembers =
            !frames_.empty() && frames_.back().count > 0;
        frames_.pop_back();
        if (hadMembers) {
            text_ += '\n';
            text_.append(frames_.size() * 2, ' ');
        }
        text_ += closer;
    }

    void AppendString(const char* value) {
        text_ += '"';
        for (const char* p = value; *p != '\0'; ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            switch (c) {
            case '"': text_ += "\\\""; break;
            case '\\': text_ += "\\\\"; break;
            case '\b': text_ += "\\b"; break;
            case '\f': text_ += "\\f"; break;
            case '\n': text_ += "\\n"; break;
            case '\r': text_ += "\\r"; break;
            case '\t': text_ += "\\t"; break;
            default:
                if (c < 0x20) {
                    text_ += '_';
                } else {
                    text_ += static_cast<char>(c);
                }
                break;
            }
        }
        text_ += '"';
    }

    void AppendU64(std::uint64_t value) {
        text_ += DecimalU64(value);
    }

    std::string text_;
    std::vector<Frame> frames_;
    bool keyPending_ = false;
};

} // namespace harkenhost

#endif // HARKEN_HOST_JSON_WRITER_H
