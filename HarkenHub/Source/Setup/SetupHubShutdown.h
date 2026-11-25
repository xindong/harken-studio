#pragma once

#include <windows.h>
#include <string>

namespace HarkenSetup {

inline std::wstring installedHubExecutable()
{
    DWORD bytes = 0;
    constexpr auto key = L"Software\\HarkenSound\\HarkenHub";
    constexpr DWORD flags = RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"InstallDir", flags, nullptr, nullptr, &bytes)
        != ERROR_SUCCESS || bytes <= sizeof(wchar_t))
        return {};
    std::wstring path(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"InstallDir", flags, nullptr, path.data(), &bytes)
        != ERROR_SUCCESS)
        return {};
    path.resize(wcslen(path.c_str()));
    if (path.empty()) return {};
    if (path.back() != L'\\') path += L'\\';
    return path + L"HarkenHub.exe";
}

inline bool matchesExecutable(DWORD processId, const std::wstring& executable)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return false;
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    const bool match = QueryFullProcessImageNameW(process, 0, path.data(), &size)
        && CompareStringOrdinal(path.data(), size, executable.c_str(), -1, TRUE) == CSTR_EQUAL;
    CloseHandle(process);
    return match;
}

struct HubWindowSearch {
    DWORD processId;
    HWND window = nullptr;
};

inline BOOL CALLBACK findOnlyVisibleWindow(HWND window, LPARAM data)
{
    auto& search = *reinterpret_cast<HubWindowSearch*>(data);
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner != search.processId || !IsWindowVisible(window))
        return TRUE;
    if (search.window)
        return FALSE;
    search.window = window;
    return TRUE;
}

inline void closeStartupDialog(DWORD processId)
{
    HubWindowSearch search{processId};
    if (!EnumWindows(findOnlyVisibleWindow, reinterpret_cast<LPARAM>(&search)))
        return;
    HWND window = search.window;
    if (!window || GetWindow(window, GW_OWNER) || !IsWindowEnabled(window))
        return;
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    if (!(style & WS_SYSMENU) || (style & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX)))
        return;
    wchar_t title[32]{};
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner == processId && GetWindowTextW(window, title, 32) && wcscmp(title, L"Harken Hub") == 0)
        PostMessageW(window, WM_CLOSE, 0, 0);
}

inline bool requestHubExit(PCWSTR pipeName, const std::wstring& guiExecutable = {})
{
    HANDLE pipe = CreateFileW(pipeName, GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    ULONG processId = 0;
    HANDLE process = GetNamedPipeServerProcessId(pipe, &processId)
        ? OpenProcess(SYNCHRONIZE, FALSE, processId) : nullptr;
    const ULONGLONG started = GetTickCount64();
    OVERLAPPED write{};
    write.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool requested = false;
    if (process && write.hEvent) {
        constexpr char quit[] = "[\"--quit\"]";
        DWORD written = 0;
        BOOL completed = WriteFile(pipe, quit, sizeof(quit) - 1, &written, &write);
        if (!completed && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(write.hEvent, 5000) != WAIT_OBJECT_0)
                CancelIoEx(pipe, &write);
            completed = GetOverlappedResult(pipe, &write, &written, TRUE);
        }
        requested = completed && written == sizeof(quit) - 1;
    }
    CloseHandle(pipe);
    if (write.hEvent)
        CloseHandle(write.hEvent);
    const ULONGLONG elapsed = GetTickCount64() - started;
    const DWORD remaining = elapsed < 5000 ? static_cast<DWORD>(5000 - elapsed) : 0;
    bool exited = false;
    if (requested) {
        const DWORD grace = !guiExecutable.empty() && remaining > 1000 ? 1000 : remaining;
        const DWORD wait = WaitForSingleObject(process, grace);
        exited = wait == WAIT_OBJECT_0;
        if (wait == WAIT_TIMEOUT && remaining > grace) {
            if (matchesExecutable(processId, guiExecutable))
                closeStartupDialog(processId);
            const ULONGLONG used = GetTickCount64() - started;
            exited = WaitForSingleObject(process, used < 5000 ? static_cast<DWORD>(5000 - used) : 0)
                == WAIT_OBJECT_0;
        }
    }
    if (process)
        CloseHandle(process);
    return exited;
}

}
