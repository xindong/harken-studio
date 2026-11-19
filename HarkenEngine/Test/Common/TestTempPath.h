#pragma once

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace harkentest {

inline std::filesystem::path TestTempDirectory(const std::filesystem::path& root) {
    const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
    auto directory = root;
    if (test != nullptr)
        directory /= std::string(test->test_suite_name()) + "." + test->name();
    std::filesystem::create_directories(directory);
    return directory;
}

}
