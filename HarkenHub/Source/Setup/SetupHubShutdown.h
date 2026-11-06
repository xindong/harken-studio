#pragma once

#include <windows.h>

namespace HarkenSetup {

inline bool requestHubExit(PCWSTR pipeName)
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
    const bool exited = requested && WaitForSingleObject(process, remaining) == WAIT_OBJECT_0;
    if (process)
        CloseHandle(process);
    return exited;
}

}
