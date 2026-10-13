#ifndef HARKEN_TESTS_TESTSUPPORT_JSON_TEST_UTIL_H
#define HARKEN_TESTS_TESTSUPPORT_JSON_TEST_UTIL_H

#include "JsonReader.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace harkentest {

inline bool ReadFileText(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
    return in.good() || in.eof();
}

inline const harkenhost::JsonValue& Member(
    const harkenhost::JsonValue& object, const char* key) {
    const harkenhost::JsonValue* value = object.Find(key);
    EXPECT_NE(nullptr, value) << "missing member \"" << key << '"';
    static const harkenhost::JsonValue kNull;
    return value != nullptr ? *value : kNull;
}

inline std::uint64_t MemberU64(const harkenhost::JsonValue& object,
                               const char* key) {
    std::uint64_t value = 0;
    EXPECT_TRUE(Member(object, key).AsU64(&value))
        << "member \"" << key << "\" is not a clean integer";
    return value;
}

inline std::string MemberString(const harkenhost::JsonValue& object,
                                const char* key) {
    const harkenhost::JsonValue& value = Member(object, key);
    EXPECT_EQ(harkenhost::JsonValue::Kind::String, value.kind)
        << "member \"" << key << "\" is not a string";
    return value.text;
}

inline bool MemberBool(const harkenhost::JsonValue& object,
                       const char* key) {
    const harkenhost::JsonValue& value = Member(object, key);
    EXPECT_EQ(harkenhost::JsonValue::Kind::Bool, value.kind)
        << "member \"" << key << "\" is not a boolean";
    return value.boolean;
}

inline double MemberNumber(const harkenhost::JsonValue& object,
                           const char* key) {
    const harkenhost::JsonValue& value = Member(object, key);
    EXPECT_EQ(harkenhost::JsonValue::Kind::Number, value.kind)
        << "member \"" << key << "\" is not a number";
    char* end = nullptr;
    const double parsed = std::strtod(value.text.c_str(), &end);
    EXPECT_EQ(value.text.c_str() + value.text.size(), end)
        << "member \"" << key << "\" carries non-numeric trailing text: \""
        << value.text << '"';
    return parsed;
}

inline void ParseJsonFile(const std::string& path,
                          harkenhost::JsonValue* out) {
    std::string text;
    ASSERT_TRUE(ReadFileText(path, &text)) << "cannot read " << path;
    std::string error;
    ASSERT_TRUE(harkenhost::ParseJson(text, out, &error))
        << path << ": " << error;
}

} // namespace harkentest

#endif // HARKEN_TESTS_TESTSUPPORT_JSON_TEST_UTIL_H
