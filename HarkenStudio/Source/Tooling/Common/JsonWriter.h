#ifndef HARKEN_STUDIO_JSON_WRITER_H
#define HARKEN_STUDIO_JSON_WRITER_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace harkenstudiojson {

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
    void ValueRawNumber(const std::string& literal) {
        BeginValue();
        text_ += literal;
    }
    std::string Finish() {
        return text_ + "\n";
    }
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

} // namespace harkenstudiojson

#endif // HARKEN_HOST_JSON_WRITER_H
