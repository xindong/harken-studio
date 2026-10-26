#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>

#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int showCommand)
{
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size())
        return static_cast<int>(GetLastError());
    executable.resize(length);
    const auto separator = executable.find_last_of(L"\\/");
    if (separator == std::wstring::npos)
        return ERROR_BAD_PATHNAME;
    executable.resize(separator + 1);
    executable += L"HarkenHub.exe";

    std::wstring arguments = L"\"" + executable + L"\"";
    if (commandLine && *commandLine) {
        arguments += L" ";
        arguments += commandLine;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = static_cast<WORD>(showCommand);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), arguments.data(), nullptr, nullptr,
                        FALSE, 0, nullptr, nullptr, &startup, &process))
        return static_cast<int>(GetLastError());
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
