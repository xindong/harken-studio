#include <harken/Harken.h>
#include "MonitorFixtureLoader.h"
#include "MonitorSha256.h"

#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifndef HARKEN_MONITOR_FIXTURE_DIR
#define HARKEN_MONITOR_FIXTURE_DIR "."
#endif
#ifndef HARKEN_MONITOR_EDITOR_FIXTURE_DIR
#define HARKEN_MONITOR_EDITOR_FIXTURE_DIR "."
#endif

namespace VNX {
namespace Monitor {
namespace TestSupport {
namespace {

uint32_t readU32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t readU64(const uint8_t* p)
{
    return static_cast<uint64_t>(readU32(p)) |
           (static_cast<uint64_t>(readU32(p + 4)) << 32);
}

std::vector<uint8_t> readAll(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

LoadedFixture stripCountPrefix(const std::vector<uint8_t>& fileBytes)
{
    LoadedFixture out;
    if (fileBytes.size() < 8)
    {
        out.status = VN_MONITOR_INCOMPATIBLE;
        return out;
    }
    const uint64_t count = readU64(fileBytes.data());
    if (count > 0xffffffffull)
    {
        out.status = VN_MONITOR_INCOMPATIBLE;
        return out;
    }
    out.packed.assign(fileBytes.begin() + 8, fileBytes.end());
    out.expectedRecords = static_cast<uint32_t>(count);
    out.status = VN_OK;
    return out;
}

} // namespace

LoadedFixture loadRuntimeLegalEvent()
{
    const auto bytes = readAll(std::string(HARKEN_MONITOR_FIXTURE_DIR) + "/legal_event.bin");
    LoadedFixture out;
    if (bytes.empty())
    {
        out.status = VN_MONITOR_INCOMPATIBLE;
        return out;
    }
    // Digest pinned from the committed Runtime fixture at generation time.
    static const char kLegalDigest[] = "8e3754f285bf1981d655c557409bd64de3ebd5db9bdbb57c74d05dcc270d3fc6";
    if (sha256Hex(bytes) != kLegalDigest)
    {
        out.status = VN_MONITOR_INCOMPATIBLE;
        return out;
    }
    const auto man = readAll(std::string(HARKEN_MONITOR_FIXTURE_DIR) + "/manifest.json");
    const std::string manText(man.begin(), man.end());
    if (manText.find("\"path\": \"legal_event.bin\"") == std::string::npos ||
        manText.find(kLegalDigest) == std::string::npos)
    {
        out.status = VN_MONITOR_INCOMPATIBLE;
        return out;
    }
    out = stripCountPrefix(bytes);
    out.expected = "ok";
    return out;
}

bool manifestEntryHas(const std::string& manText,
                      const char* path,
                      const char* digest,
                      const char* producerPackage)
{
    const std::string needle = std::string("\"path\": \"") + path + "\"";
    const auto pathPos = manText.find(needle);
    if (pathPos == std::string::npos)
        return false;
    const auto nextPath = manText.find("\"path\":", pathPos + needle.size());
    const auto end = nextPath == std::string::npos ? manText.size() : nextPath;
    const std::string entry = manText.substr(pathPos, end - pathPos);
    return entry.find(digest) != std::string::npos
        && entry.find(std::string("\"producerPackage\": \"") + producerPackage + "\"")
               != std::string::npos
        && entry.find("\"schemaVersion\": 1") != std::string::npos
        && entry.find("\"isProvisional\": false") != std::string::npos
        && entry.find("\"kind\": \"legal-event\"") != std::string::npos
        && entry.find("\"fingerprint\"") == std::string::npos;
}

LoadedFixture loadLifecycleLegalEvent()
{
    const std::string dir = HARKEN_MONITOR_FIXTURE_DIR;
    const auto bytes = readAll(dir + "/lifecycle/legal_event.bin");
    LoadedFixture out;
    if (bytes.empty())
    {
        out.status = VN_MONITOR_INCOMPATIBLE;
        return out;
    }
    static const char kDigest[] =
        "20176227f20d09bd4406fd1a1f2f3f92658c926f74548a4b468745b3c4cff6cd";
    const auto man = readAll(dir + "/manifest.json");
    const std::string manText(man.begin(), man.end());
    if (sha256Hex(bytes) != kDigest
        || !manifestEntryHas(manText, "lifecycle/legal_event.bin", kDigest, "m2-1"))
    {
        out.status = VN_MONITOR_INCOMPATIBLE;
        return out;
    }
    out = stripCountPrefix(bytes);
    out.expected = "ok";
    return out;
}

LoadedFixture loadEditorFixture(const std::string& fileName)
{
    const std::string dir = HARKEN_MONITOR_EDITOR_FIXTURE_DIR;
    const auto bytes = readAll(dir + "/" + fileName);
    LoadedFixture out;
    if (bytes.empty())
    {
        out.status = VN_MONITOR_INCOMPATIBLE;
        return out;
    }
    static const std::map<std::string, const char*> kDigest = {
        {"optional_unknown.bin", "9f76e20ea04df039de9380cf96d692aac8c938f3aeb45d06120607c8346a68b0"},
        {"required_unknown.bin", "5be9007940d6f66972bc947dfa46aaa87e018f45b58fadb0744666854f479f8e"},
        {"short_header.bin", "6d4b225592be79e408ca582087bab87e4a7237478a9bdc3411ced0ec237575a9"},
        {"truncated_record.bin", "f99ef2620c5be056907b7feb0732974251512cbc77384a2256d28bd78491f733"},
        {"legal_then_required_unknown.bin", "11a9087de688dabb686c730601196b0ddaa2830d76134954f5fb9226860f83c2"},
        {"count_bytes_mismatch.bin", "1482e36518ae38a280335d6249266199f768b93e21a80f489f7a7ef730b7f337"},
        {"native_layout.bin", "fb1c8523075417ac164247a98c91cc1d49bd330e8253bc2d7d284fce933abcc1"},
        {"be_count_prefix.bin", "4999a63e7e3e754481bf71c970ae6e7d50f85f18eda1e4923fc2f02c6e923541"},
        {"empty_batch.bin", "af5570f5a1810b7af78caf4bc70a660f0df51e42baf91d4de5b2328de0e83dfc"},
    };
    static const std::map<std::string, const char*> kExpected = {
        {"optional_unknown.bin", "ok-optional-skip"},
        {"required_unknown.bin", "incompatible"},
        {"short_header.bin", "incompatible"},
        {"truncated_record.bin", "incompatible"},
        {"legal_then_required_unknown.bin", "incompatible"},
        {"count_bytes_mismatch.bin", "incompatible"},
        {"native_layout.bin", "incompatible"},
        {"be_count_prefix.bin", "incompatible"},
        {"empty_batch.bin", "ok-empty"},
    };
    const auto digestIt = kDigest.find(fileName);
    const auto man = readAll(dir + "/manifest.json");
    const std::string manText(man.begin(), man.end());
    if (digestIt == kDigest.end() || sha256Hex(bytes) != digestIt->second ||
        manText.find(fileName) == std::string::npos ||
        manText.find(digestIt->second) == std::string::npos)
    {
        out.status = VN_MONITOR_INCOMPATIBLE;
        return out;
    }
    out = stripCountPrefix(bytes);
    const auto expectedIt = kExpected.find(fileName);
    if (expectedIt != kExpected.end())
        out.expected = expectedIt->second;
    return out;
}

} // namespace TestSupport
} // namespace Monitor
} // namespace VNX
