// Engine-owned .stream v1 image builder shared with the host tool. Tests
// construct containers in memory so they can corrupt one specific region and
// prove the reader refuses it.
#ifndef HARKEN_TESTS_TESTSUPPORT_STREAM_BUILDER_H
#define HARKEN_TESTS_TESTSUPPORT_STREAM_BUILDER_H

#include "Common/ObjectIdTestSupport.h"
#include "StreamImage.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace harkentest {

using harkenhost::BuildStreamBytes;
using harkenhost::StreamSpec;

inline void WriteFileBytes(const std::string& path,
                           const std::vector<std::uint8_t>& bytes) {
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << "cannot open " << path;
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(out.good()) << "cannot write " << path;
}

} // namespace harkentest

#endif // HARKEN_TESTS_TESTSUPPORT_STREAM_BUILDER_H
