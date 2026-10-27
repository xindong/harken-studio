#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>

#include <sddl.h>
#include <shellapi.h>

#include <cwctype>
#include <string>
#include <string_view>

#include "SetupStrings.h"
#include "SetupPayload.h"

namespace {

constexpr wchar_t kPayloadResource[] = L"HARKENHUBMSI";
constexpr wchar_t kChinesePayloadResource[] = L"HARKENHUBMSI_ZHCN";
constexpr wchar_t kSharedPayloadResource[] = L"HARKENHUBBLOCKS";
constexpr wchar_t kStagingSddl[] = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
constexpr std::wstring_view kChineseHandoff = L"--harken-setup-zh ";
constexpr std::wstring_view kEnglishHandoff = L"--harken-setup-en ";

bool isQuietCommandLine(PCWSTR commandLine)
{
    if (!commandLine)
        return false;
    const wchar_t* cursor = commandLine;
    while (*cursor) {
        while (*cursor == L' ' || *cursor == L'\t')
            ++cursor;
        if (!*cursor)
            break;
        if (*cursor == L'/' || *cursor == L'-') {
            if (std::towlower(cursor[1]) == L'q')
                return true;
            if (_wcsnicmp(cursor + 1, L"passive", 7) == 0)
                return true;
        }
        while (*cursor && *cursor != L' ' && *cursor != L'\t')
            ++cursor;
    }
    return false;
}

std::wstring setupText(UINT id)
{
    wchar_t* text = nullptr;
    const int length = LoadStringW(GetModuleHandleW(nullptr), id,
                                  reinterpret_cast<wchar_t*>(&text), 0);
    return std::wstring(text, length);
}

int fail(UINT message, bool quiet)
{
    if (!quiet)
        MessageBoxW(nullptr, setupText(message).c_str(),
                    setupText(HARKEN_SETUP_TITLE).c_str(), MB_OK | MB_ICONERROR);
    return ERROR_INSTALL_FAILURE;
}

int launchElevated(PCWSTR commandLine, bool chinese, bool quiet)
{
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                             static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size())
        return fail(HARKEN_SETUP_ELEVATION_FAILED, quiet);
    executable.resize(length);
    std::wstring arguments(chinese ? kChineseHandoff : kEnglishHandoff);
    arguments += commandLine;

    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    launch.lpVerb = L"runas";
    launch.lpFile = executable.c_str();
    launch.lpParameters = arguments.c_str();
    launch.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&launch)) {
        if (GetLastError() == ERROR_CANCELLED)
            return ERROR_INSTALL_USEREXIT;
        return fail(HARKEN_SETUP_ELEVATION_FAILED, quiet);
    }
    if (!launch.hProcess)
        return fail(HARKEN_SETUP_ELEVATION_FAILED, quiet);
    WaitForSingleObject(launch.hProcess, INFINITE);
    DWORD exitCode = ERROR_INSTALL_FAILURE;
    GetExitCodeProcess(launch.hProcess, &exitCode);
    CloseHandle(launch.hProcess);
    return static_cast<int>(exitCode);
}

int failStaging(UINT message, bool quiet, HANDLE stagingGuard,
                const std::wstring& msiPath, const std::wstring& stagingDir)
{
    if (stagingGuard != INVALID_HANDLE_VALUE)
        CloseHandle(stagingGuard);
    if (!msiPath.empty())
        DeleteFileW(msiPath.c_str());
    RemoveDirectoryW(stagingDir.c_str());
    return fail(message, quiet);
}

void launchInstalledHub()
{
    wchar_t installDir[MAX_PATH + 1]{};
    DWORD valueSize = sizeof(installDir) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\HarkenSound\\HarkenHub",
                     L"InstallDir", RRF_RT_REG_SZ, nullptr, installDir,
                     &valueSize) != ERROR_SUCCESS ||
        installDir[0] == L'\0')
        return;

    std::wstring directory(installDir);
    while (!directory.empty() &&
           (directory.back() == L'\\' || directory.back() == L'/'))
        directory.pop_back();
    if (directory.empty())
        return;
    const std::wstring hubPath = directory + L"\\HarkenHub.exe";
    std::wstring arguments = L"\"" + hubPath + L"\"";

    HWND shellWindow = GetShellWindow();
    if (!shellWindow)
        return;
    DWORD shellPid = 0;
    GetWindowThreadProcessId(shellWindow, &shellPid);
    if (!shellPid)
        return;
    HANDLE shellProcess =
        OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, shellPid);
    if (!shellProcess)
        return;
    HANDLE shellToken = nullptr;
    if (!OpenProcessToken(shellProcess, TOKEN_DUPLICATE, &shellToken)) {
        CloseHandle(shellProcess);
        return;
    }
    HANDLE launchToken = nullptr;
    const BOOL duplicated = DuplicateTokenEx(
        shellToken,
        TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY |
            TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
        nullptr, SecurityImpersonation, TokenPrimary, &launchToken);
    CloseHandle(shellToken);
    CloseHandle(shellProcess);
    if (!duplicated)
        return;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessWithTokenW(launchToken, 0, hubPath.c_str(),
                                arguments.data(), 0, nullptr,
                                directory.c_str(), &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    CloseHandle(launchToken);
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR rawCommandLine, int)
{
    std::wstring_view parameters(rawCommandLine ? rawCommandLine : L"");
    bool chinese = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
    bool handedOff = false;
    for (const auto prefix : {kChineseHandoff, kEnglishHandoff}) {
        if (parameters.substr(0, prefix.size()) == prefix) {
            chinese = prefix == kChineseHandoff;
            parameters.remove_prefix(prefix.size());
            handedOff = true;
            break;
        }
    }
    const PCWSTR commandLine = parameters.data();
    const bool quiet = isQuietCommandLine(commandLine);
    SetThreadUILanguage(chinese ? MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)
                                : MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));

    HRSRC located = FindResourceW(instance, chinese ? kChinesePayloadResource : kPayloadResource, RT_RCDATA);
    if (!located)
        return fail(HARKEN_SETUP_MISSING_PAYLOAD,
                    quiet);
    HGLOBAL loaded = LoadResource(instance, located);
    const DWORD payloadSize = SizeofResource(instance, located);
    const void* payload = loaded ? LockResource(loaded) : nullptr;
    if (!payload || payloadSize == 0)
        return fail(HARKEN_SETUP_UNREADABLE_PAYLOAD, quiet);

    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return fail(HARKEN_SETUP_ELEVATION_FAILED, quiet);
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL queried = GetTokenInformation(token, TokenElevation, &elevation,
                                            sizeof(elevation), &returned);
    CloseHandle(token);
    if (!queried || (!elevation.TokenIsElevated && handedOff))
        return fail(HARKEN_SETUP_ELEVATION_FAILED, quiet);
    if (!elevation.TokenIsElevated)
        return launchElevated(commandLine, chinese, quiet);

    HRSRC shared = FindResourceW(instance, kSharedPayloadResource, RT_RCDATA);
    HGLOBAL sharedLoaded = shared ? LoadResource(instance, shared) : nullptr;
    const void* blocks = sharedLoaded ? LockResource(sharedLoaded) : nullptr;
    const DWORD blocksSize = shared ? SizeofResource(instance, shared) : 0;
    std::vector<unsigned char> restored;
    if (!HarkenSetup::unpackPayload(payload, payloadSize, blocks, blocksSize, restored))
        return fail(HARKEN_SETUP_UNREADABLE_PAYLOAD, quiet);

    wchar_t tempDir[MAX_PATH + 1];
    const DWORD tempLen = GetTempPathW(MAX_PATH + 1, tempDir);
    if (tempLen == 0 || tempLen > MAX_PATH)
        return fail(HARKEN_SETUP_TEMP_UNAVAILABLE, quiet);

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kStagingSddl, SDDL_REVISION_1, &descriptor, nullptr))
        return fail(HARKEN_SETUP_STAGING_SECURITY_FAILED,
                    quiet);
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = descriptor;

    std::wstring stagingDir;
    bool created = false;
    for (unsigned attempt = 0; attempt < 16 && !created; ++attempt) {
        stagingDir = std::wstring(tempDir) + L"HarkenHubSetup-" +
                     std::to_wstring(GetCurrentProcessId()) + L"-" +
                     std::to_wstring(GetTickCount64() + attempt);
        created = CreateDirectoryW(stagingDir.c_str(), &security) != FALSE;
    }
    LocalFree(descriptor);
    if (!created)
        return fail(HARKEN_SETUP_STAGING_CREATE_FAILED,
                    quiet);

    HANDLE stagingGuard =
        CreateFileW(stagingDir.c_str(), GENERIC_READ, FILE_SHARE_READ,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                    nullptr);
    if (stagingGuard == INVALID_HANDLE_VALUE)
        return failStaging(HARKEN_SETUP_STAGING_LOCK_FAILED,
                           quiet, stagingGuard, std::wstring(), stagingDir);

    const std::wstring msiPath = stagingDir + L"\\HarkenHub.msi";
    HANDLE file = CreateFileW(msiPath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return failStaging(HARKEN_SETUP_PAYLOAD_WRITE_FAILED,
                           quiet, stagingGuard, std::wstring(), stagingDir);

    DWORD written = 0;
    const BOOL wrote = WriteFile(file, restored.data(), static_cast<DWORD>(restored.size()), &written, nullptr);
    CloseHandle(file);
    if (!wrote || written != restored.size())
        return failStaging(HARKEN_SETUP_PAYLOAD_WRITE_FAILED,
                           quiet, stagingGuard, msiPath, stagingDir);

    wchar_t systemDir[MAX_PATH + 1];
    const UINT systemLen = GetSystemDirectoryW(systemDir, MAX_PATH + 1);
    if (systemLen == 0 || systemLen > MAX_PATH)
        return failStaging(HARKEN_SETUP_SYSTEM_UNAVAILABLE,
                           quiet, stagingGuard, msiPath, stagingDir);
    const std::wstring msiexecPath =
        std::wstring(systemDir) + L"\\msiexec.exe";

    std::wstring arguments =
        L"\"" + msiexecPath + L"\" /i \"" + msiPath + L"\"";
    if (commandLine && *commandLine) {
        arguments += L" ";
        arguments += commandLine;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL launched =
        CreateProcessW(msiexecPath.c_str(), arguments.data(), nullptr,
                       nullptr, FALSE, 0, nullptr, nullptr, &startup,
                       &process);
    if (!launched)
        return failStaging(HARKEN_SETUP_LAUNCH_FAILED,
                           quiet, stagingGuard, msiPath, stagingDir);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = ERROR_INSTALL_FAILURE;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(stagingGuard);
    DeleteFileW(msiPath.c_str());
    RemoveDirectoryW(stagingDir.c_str());
    if (!quiet && exitCode == ERROR_SUCCESS)
        launchInstalledHub();
    return static_cast<int>(exitCode);
}
