#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <iostream>
#include <string>

namespace {

std::wstring WidenAscii(const char* text) {
    std::wstring wide;
    while (*text != '\0') {
        wide.push_back(static_cast<unsigned char>(*text));
        ++text;
    }
    return wide;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 2) {
        std::wcerr << L"usage: harken_shared_load_smoke <HarkenCore.dll>\n";
        return 1;
    }

    HMODULE library = LoadLibraryW(argv[1]);
    if (library == nullptr) {
        const DWORD error = GetLastError();
        std::wcerr << L"LoadLibraryW failed for \"" << argv[1]
                   << L"\" (GetLastError=" << error << L")\n";
        return 1;
    }

    constexpr std::array exports{
        "Vn_Init",
        "Vn_Suspend",
        "Vn_Resume",
        "Vn_Term",
        "Vn_PostAction",
        "Vn_StopActionFiring",
        "Vn_QueryActionFiring",
        "Vn_QueryActionStep",
        "Vn_ReleaseActionFiring",
        "Vn_Update",
        "Vn_RegisterStreamFile",
        "Vn_ReplacePack",
        "Vn_TraceStart",
        "Vn_SetBusOutputLayout",
        "Vn_RegisterBinding",
        "Vn_SetVoiceDistanceScale",
        "Vn_SetVoiceSpatializationMix",
        "Vn_CaptureEffectCapabilities",
        "Vn_CaptureEffectParameterCapabilities",
        "Vn_SetBusEffectDynamicRoute",
        "Vn_ClearBusEffectDynamicRoute",
        "Vn_ResetVoiceEffect",
        "Vn_ResetBusEffect",
        "Vn_CaptureEffectDynamicCapabilities",
        "Vn_CaptureEffectDynamicParameterCapabilities",
        "Vn_CaptureEffectDynamicRoutes",
        "Vn_CaptureCompressorRuntimeInfo",
        "Vn_CapturePropertyTargets",
    };

    bool missingExport = false;
    for (const char* symbol : exports) {
        SetLastError(ERROR_SUCCESS);
        if (GetProcAddress(library, symbol) == nullptr) {
            const DWORD error = GetLastError();
            std::wcerr << L"GetProcAddress failed for \"" << WidenAscii(symbol)
                       << L"\" (GetLastError=" << error << L")\n";
            missingExport = true;
        }
    }

    if (FreeLibrary(library) == FALSE) {
        const DWORD error = GetLastError();
        std::wcerr << L"FreeLibrary failed (GetLastError=" << error << L")\n";
        return 1;
    }

    return missingExport ? 1 : 0;
}
