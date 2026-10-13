#include <harken/Harken.h>
#include "harken_binding_abi_pin.g.h"

#include <windows.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

template <typename T>
T Load(HMODULE module, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(module, name));
}

struct Api {
    decltype(&Vn_Init) init = nullptr;
    decltype(&Vn_Term) term = nullptr;
    decltype(&Vn_Update) update = nullptr;
    decltype(&Vn_CaptureSnapshot) captureSnapshot = nullptr;
    decltype(&Vn_CaptureEffectParameterCapabilities)
        captureParameterCapabilities = nullptr;
};

bool LoadCommon(HMODULE module, Api* api) {
    api->init = Load<decltype(api->init)>(module, "Vn_Init");
    api->term = Load<decltype(api->term)>(module, "Vn_Term");
    api->update = Load<decltype(api->update)>(module, "Vn_Update");
    api->captureSnapshot =
        Load<decltype(api->captureSnapshot)>(
            module, "Vn_CaptureSnapshot");
    api->captureParameterCapabilities =
        Load<decltype(api->captureParameterCapabilities)>(
            module, "Vn_CaptureEffectParameterCapabilities");
    return api->init != nullptr && api->term != nullptr &&
           api->update != nullptr && api->captureSnapshot != nullptr &&
           api->captureParameterCapabilities != nullptr;
}

VnConfig NullConfig(std::uint32_t structSize,
                    std::uint32_t sampleRate) {
    VnConfig config{};
    config.structSize = structSize;
    config.sampleRate = sampleRate;
    config.blockFrames = 256;
    config.maxVoices = 4;
    config.deviceMode = VN_DEVICE_NULL;
    return config;
}

bool ResolvesDefaultRateTo48000(const Api& api) {
    std::array<VnEffectParameterCapability, 64> rows{};
    for (VnEffectParameterCapability& row : rows) {
        row.structSize = sizeof(VnEffectParameterCapability);
    }
    std::uint32_t required = 0;
    if (api.captureParameterCapabilities(
            rows.data(), static_cast<std::uint32_t>(rows.size()),
            &required) != VN_OK ||
        required != 20 || required > rows.size()) {
        return false;
    }
    const float expected = std::nextafter(24000.0f, 0.0f);
    return rows[5].clampMax == expected;
}

bool LoadedExpectedModule(HMODULE module,
                          const std::filesystem::path& expected) {
    std::array<wchar_t, 32768> loaded{};
    const DWORD length = GetModuleFileNameW(
        module, loaded.data(), static_cast<DWORD>(loaded.size()));
    if (length == 0 || length >= loaded.size()) {
        return false;
    }
    std::error_code ec;
    const std::filesystem::path actual =
        std::filesystem::weakly_canonical(
            std::filesystem::path(loaded.data()), ec);
    if (ec) {
        return false;
    }
    const std::filesystem::path canonicalExpected =
        std::filesystem::weakly_canonical(expected, ec);
    return !ec && actual == canonicalExpected;
}

int RunOldClientNewRuntime(HMODULE module, const Api& api) {
    if (sizeof(VnConfig) != 264 || sizeof(VnSnapshot) != 3536) {
        std::fprintf(
            stderr,
            "old-client profile compiled with unexpected layouts "
            "(config=%zu snapshot=%zu)\n",
            sizeof(VnConfig), sizeof(VnSnapshot));
        return 1;
    }
    VnConfig config =
        NullConfig(static_cast<std::uint32_t>(sizeof(VnConfig)), 0);
    if (api.init(&config) != VN_OK) {
        std::fprintf(stderr, "old config was refused by current runtime\n");
        return 1;
    }

    bool ok = ResolvesDefaultRateTo48000(api);
    struct SnapshotGuard {
        VnSnapshot snapshot;
        std::array<std::uint8_t, 64> canary;
    };
    static_assert(
        offsetof(SnapshotGuard, canary) == sizeof(VnSnapshot));
    SnapshotGuard guarded{};
    guarded.canary.fill(0xA5);
    guarded.snapshot.structSize = sizeof(VnSnapshot);
    ok = ok && api.captureSnapshot(&guarded.snapshot) == VN_OK;
    for (const std::uint8_t byte : guarded.canary) {
        ok = ok && byte == 0xA5;
    }
    ok = ok && api.update() == VN_OK;
    ok = ok &&
         GetProcAddress(module, "Vn_ApplyBusTopology") != nullptr;
    ok = ok && api.term() == VN_OK;

    if (!ok) {
        std::fprintf(
            stderr,
            "old-client common contract failed against current runtime\n");
        return 1;
    }
    std::printf(
        "{\"profile\":\"old-client-new-runtime\","
        "\"configSize\":264,\"snapshotSize\":3536,"
        "\"resolvedRate\":48000,\"result\":\"PASS\"}\n");
    return 0;
}

int RunCurrentClientOldRuntime(HMODULE module, const Api& api) {
    if (sizeof(VnConfig) != 280 || sizeof(VnSnapshot) != 4008) {
        std::fprintf(
            stderr,
            "current-client profile compiled with unexpected layouts "
            "(config=%zu snapshot=%zu)\n",
            sizeof(VnConfig), sizeof(VnSnapshot));
        return 1;
    }

    VnConfig current =
        NullConfig(static_cast<std::uint32_t>(sizeof(VnConfig)), 48000);
    const VnResult defaultInit = api.init(&current);
    if (defaultInit == VN_OK) {
        api.term();
    }
    if (defaultInit != VN_INVALID_ARG) {
        std::fprintf(
            stderr,
            "old runtime did not refuse current default config (%d)\n",
            static_cast<int>(defaultInit));
        return 1;
    }

    VnConfig downlevel = NullConfig(264, 48000);
    if (api.init(&downlevel) != VN_OK) {
        std::fprintf(
            stderr,
            "old runtime refused explicit 264-byte config\n");
        return 1;
    }

    alignas(VnSnapshot)
        std::array<std::uint8_t, sizeof(VnSnapshot)> snapshotBytes{};
    snapshotBytes.fill(0xA5);
    VnSnapshot* snapshot =
        reinterpret_cast<VnSnapshot*>(snapshotBytes.data());
    snapshot->structSize = sizeof(VnSnapshot);
    bool ok = api.captureSnapshot(snapshot) == VN_INVALID_ARG;

    snapshotBytes.fill(0xA5);
    snapshot->structSize = 3536;
    ok = ok && api.captureSnapshot(snapshot) == VN_OK;
    for (std::size_t i = 3536; i < snapshotBytes.size(); ++i) {
        if (snapshotBytes[i] != 0xA5) {
            ok = false;
            break;
        }
    }
    ok = ok && api.update() == VN_OK;
    ok = ok &&
         GetProcAddress(module, "Vn_ApplyBusTopology") == nullptr;
    ok = ok && api.term() == VN_OK;

    if (!ok) {
        std::fprintf(
            stderr,
            "current-client downlevel contract failed against old runtime\n");
        return 1;
    }
    std::printf(
        "{\"profile\":\"current-client-old-runtime\","
        "\"defaultConfig\":\"EXPECTED_REFUSAL\","
        "\"downlevelConfigSize\":264,"
        "\"defaultSnapshot\":\"EXPECTED_REFUSAL\","
        "\"downlevelSnapshotSize\":3536,"
        "\"newExport\":\"UNAVAILABLE\","
        "\"result\":\"PASS\"}\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(
            stderr,
            "usage: harken_abi_history_client <profile> <HarkenCore.dll>\n");
        return 2;
    }
    std::error_code ec;
    const std::filesystem::path runtimePath =
        std::filesystem::weakly_canonical(
            std::filesystem::u8path(argv[2]), ec);
    if (ec || !runtimePath.is_absolute()) {
        std::fprintf(stderr, "runtime path is not canonical and absolute\n");
        return 2;
    }
    HMODULE module = LoadLibraryExW(
        runtimePath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
            LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module == nullptr) {
        std::fprintf(stderr, "failed to load runtime: %s\n", argv[2]);
        return 2;
    }
    if (!LoadedExpectedModule(module, runtimePath)) {
        std::fprintf(stderr, "loaded module path does not match request\n");
        FreeLibrary(module);
        return 2;
    }
    Api api;
    if (!LoadCommon(module, &api)) {
        std::fprintf(stderr, "runtime is missing a common export\n");
        FreeLibrary(module);
        return 2;
    }

    const std::string profile = argv[1];
    int result = 2;
    if (profile == "old-client-new-runtime") {
        result = RunOldClientNewRuntime(module, api);
    } else if (profile == "current-client-old-runtime") {
        result = RunCurrentClientOldRuntime(module, api);
    } else {
        std::fprintf(stderr, "unknown profile: %s\n", argv[1]);
    }
    FreeLibrary(module);
    return result;
}
