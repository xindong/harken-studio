/* The named-pipe transport: one I/O thread owning the pipe handle for the
 * server's whole life. It accepts one client at a time (the pipe is a
 * single instance, so the OS refuses a second connect until the first
 * disconnects), performs the versioned handshake, validates framing, and
 * moves complete frames between the pipe and the shared queues. It never
 * touches the engine. */
#ifndef HARKEN_REMOTE_SRC_PIPE_TRANSPORT_WIN_H
#define HARKEN_REMOTE_SRC_PIPE_TRANSPORT_WIN_H

#include "RemoteShared.h"

#include <string>
#include <thread>

namespace harkenremote {

struct PipeTransportConfig {
    std::string pipeName;
    std::string version;
    std::uint32_t sampleRate = 0;
    std::uint32_t blockFrames = 0;
    std::uint32_t configStructSize = 0;
    std::uint32_t readTimeoutMs = kDefaultReadTimeoutMs;
    std::uint32_t inboundQueueFrames = 64;
    std::uint32_t reliableQueueFrames = 256;
    std::uint32_t closeLingerMs = 100;
};

class PipeTransport {
public:
    PipeTransport(const PipeTransportConfig& config, RemoteShared& shared);
    ~PipeTransport();

    /* Creates the pipe instance and starts the I/O thread; false when the
     * pipe cannot be created. */
    bool Start();

    /* Signals shutdown, joins the thread, closes the pipe. Idempotent. */
    void Stop();

private:
    void ThreadMain();
    void ServeSession();

    PipeTransportConfig config_;
    RemoteShared& shared_;
    void* pipe_ = nullptr;         // HANDLE; INVALID_HANDLE_VALUE when closed
    void* connectEvent_ = nullptr; // manual-reset, for overlapped connect
    void* readEvent_ = nullptr;    // manual-reset, for overlapped reads
    void* writeEvent_ = nullptr;   // manual-reset, for overlapped writes
    std::thread thread_;
    bool started_ = false;
};

} // namespace harkenremote

#endif // HARKEN_REMOTE_SRC_PIPE_TRANSPORT_WIN_H
