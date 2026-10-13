// Named-pipe test client for the remote-channel tests: a byte-stream
// client with deadline-bounded connect/read/write (overlapped I/O, so a
// wedged server can never hang the test run), frame-level send/receive
// over the remote wire format, and an RAII harken_host subprocess. Every
// operation that can wait takes an explicit timeout — CI must fail, not
// hang, when the server misbehaves.
#ifndef HARKEN_TESTS_TESTSUPPORT_PIPE_CLIENT_H
#define HARKEN_TESTS_TESTSUPPORT_PIPE_CLIENT_H

#include "harken_remote/WireFormat.h"

#include <cstdint>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace harkentest {

class PipeClient {
public:
    PipeClient() = default;
    ~PipeClient() { Close(); }
    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    // Connects to the pipe, retrying while the server has not created it
    // yet (file-not-found) — the caller starts the host asynchronously and
    // the ready condition is simply a successful open. Returns false when
    // the deadline passes. A busy pipe (second client while the first is
    // being served) is retried too unless retryBusy is false, so the
    // single-client refusal is testable as an immediate failure.
    bool Connect(const std::string& pipeName, int timeoutMs,
                 bool retryBusy = true, DWORD* lastError = nullptr) {
        Close();
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        for (;;) {
            pipe_ = CreateFileA(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                                0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED, nullptr);
            if (pipe_ != INVALID_HANDLE_VALUE) {
                break;
            }
            const DWORD error = GetLastError();
            if (lastError != nullptr) {
                *lastError = error;
            }
            const bool retryable =
                error == ERROR_FILE_NOT_FOUND ||
                (retryBusy && error == ERROR_PIPE_BUSY);
            if (!retryable || GetTickCount64() >= deadline) {
                return false;
            }
            Sleep(20);
        }
        event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return event_ != nullptr;
    }

    void Close() {
        if (pipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
        if (event_ != nullptr) {
            CloseHandle(event_);
            event_ = nullptr;
        }
    }

    bool IsOpen() const { return pipe_ != INVALID_HANDLE_VALUE; }

    // Writes the full buffer or fails. A timeout means the server has
    // stopped consuming (the backpressure observable); the pending write
    // is cancelled so the client stays usable for reads.
    bool WriteBytes(const void* data, std::size_t size, int timeoutMs) {
        ResetEvent(event_);
        OVERLAPPED ovl{};
        ovl.hEvent = event_;
        if (!WriteFile(pipe_, data, static_cast<DWORD>(size), nullptr, &ovl)) {
            if (GetLastError() != ERROR_IO_PENDING) {
                return false;
            }
            if (WaitForSingleObject(event_, timeoutMs) != WAIT_OBJECT_0) {
                CancelIoEx(pipe_, &ovl);
                DWORD ignored = 0;
                GetOverlappedResult(pipe_, &ovl, &ignored, TRUE);
                return false;
            }
        }
        DWORD written = 0;
        if (!GetOverlappedResult(pipe_, &ovl, &written, TRUE)) {
            return false;
        }
        return written == size;
    }

    // Reads exactly `size` bytes before the deadline, or fails. Fails
    // promptly (not at the deadline) when the server closed the pipe.
    bool ReadExact(void* data, std::size_t size, int timeoutMs) {
        std::uint8_t* out = static_cast<std::uint8_t*>(data);
        std::size_t got = 0;
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (got < size) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                return false;
            }
            ResetEvent(event_);
            OVERLAPPED ovl{};
            ovl.hEvent = event_;
            if (!ReadFile(pipe_, out + got, static_cast<DWORD>(size - got),
                          nullptr, &ovl)) {
                const DWORD error = GetLastError();
                if (error != ERROR_IO_PENDING) {
                    return false; // broken pipe / closed
                }
                const DWORD wait = WaitForSingleObject(
                    event_, static_cast<DWORD>(deadline - now));
                if (wait != WAIT_OBJECT_0) {
                    CancelIoEx(pipe_, &ovl);
                    DWORD ignored = 0;
                    GetOverlappedResult(pipe_, &ovl, &ignored, TRUE);
                    return false;
                }
            }
            DWORD bytes = 0;
            if (!GetOverlappedResult(pipe_, &ovl, &bytes, TRUE) || bytes == 0) {
                return false;
            }
            got += bytes;
        }
        return true;
    }

    // True when the server has closed the session: the next read fails
    // with a broken/ended pipe instead of data before the deadline.
    // Consumes (discards) any frames still buffered ahead of the close.
    bool WaitForClose(int timeoutMs) {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        std::uint8_t scratch[512];
        for (;;) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                return false;
            }
            ResetEvent(event_);
            OVERLAPPED ovl{};
            ovl.hEvent = event_;
            if (!ReadFile(pipe_, scratch, sizeof(scratch), nullptr, &ovl)) {
                const DWORD error = GetLastError();
                if (error != ERROR_IO_PENDING) {
                    return true; // broken pipe: the close is observed
                }
                const DWORD wait = WaitForSingleObject(
                    event_, static_cast<DWORD>(deadline - now));
                if (wait != WAIT_OBJECT_0) {
                    CancelIoEx(pipe_, &ovl);
                    DWORD ignored = 0;
                    GetOverlappedResult(pipe_, &ovl, &ignored, TRUE);
                    return false; // still open, just idle
                }
            }
            DWORD bytes = 0;
            if (!GetOverlappedResult(pipe_, &ovl, &bytes, TRUE) || bytes == 0) {
                return true;
            }
            // Data still draining; keep consuming until close or deadline.
        }
    }

    bool WriteFrame(const harkenremote::WireFrame& frame, int timeoutMs = 5000) {
        std::vector<std::uint8_t> bytes;
        harkenremote::AppendFrameBytes(frame, &bytes);
        return WriteBytes(bytes.data(), bytes.size(), timeoutMs);
    }

    bool ReadFrame(harkenremote::WireFrame* frame, int timeoutMs = 5000) {
        std::uint8_t header[harkenremote::kFrameHeaderBytes];
        if (!ReadExact(header, sizeof(header), timeoutMs)) {
            return false;
        }
        harkenremote::WireFrameHeader decoded;
        harkenremote::DecodeFrameHeader(header, &decoded);
        if (decoded.payloadBytes > harkenremote::kMaxFrameBytes) {
            return false; // the server never sends an oversized frame
        }
        frame->msgType = decoded.msgType;
        frame->flags = decoded.flags;
        frame->sequence = decoded.sequence;
        frame->payload.resize(decoded.payloadBytes);
        if (decoded.payloadBytes == 0) {
            return true;
        }
        return ReadExact(frame->payload.data(), decoded.payloadBytes,
                         timeoutMs);
    }

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
};

// harken_host subprocess with guaranteed teardown: the destructor
// terminates the process if the test did not, so a failed assertion never
// leaks a server holding the pipe name.
class HostProcess {
public:
    HostProcess() = default;
    ~HostProcess() { Terminate(); }
    HostProcess(const HostProcess&) = delete;
    HostProcess& operator=(const HostProcess&) = delete;

    bool Start(const std::string& commandLine) {
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::string mutableCmd = commandLine; // CreateProcess may edit it
        if (!CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr,
                            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                            &pi)) {
            return false;
        }
        process_ = pi.hProcess;
        CloseHandle(pi.hThread);
        return true;
    }

    void Terminate() {
        if (process_ != nullptr) {
            TerminateProcess(process_, 0);
            WaitForSingleObject(process_, 5000);
            CloseHandle(process_);
            process_ = nullptr;
        }
    }

    bool Running() const {
        if (process_ == nullptr) {
            return false;
        }
        return WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
    }

    // Waits for the process to exit on its own (a bounded-duration serve
    // run) and reports its exit code. Returns false when the deadline
    // passes first; the destructor still guarantees teardown.
    bool WaitExit(int timeoutMs, DWORD* exitCode) {
        if (process_ == nullptr) {
            return false;
        }
        if (WaitForSingleObject(process_, timeoutMs) != WAIT_OBJECT_0) {
            return false;
        }
        return GetExitCodeProcess(process_, exitCode) != 0;
    }

private:
    HANDLE process_ = nullptr;
};

} // namespace harkentest

#endif // HARKEN_TESTS_TESTSUPPORT_PIPE_CLIENT_H
