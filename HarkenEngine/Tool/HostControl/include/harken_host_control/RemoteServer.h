/* The remote-control channel server: a tooling layer harken_host embeds
 * behind its remote-serve verb. It owns a Windows named pipe and one pipe
 * I/O thread; decoded requests queue to the embedding host's logic thread,
 * which executes them between Vn_Update pumps via PumpLogic and replies
 * through the same bounded outbound buffer. The engine itself has no IPC
 * awareness — everything crosses the public C API under its external
 * synchronization contract.
 *
 * Delivery classes: command replies are reliable and never dropped —
 * when a client issues commands without reading replies the server first
 * stops consuming request frames (natural pipe backpressure), and closes
 * the session only if a produced reply still cannot be queued. Snapshot
 * and notification frames are advisory: a full advisory buffer drops the
 * oldest frame and counts it in the server status frame. The host logic
 * thread never blocks on the pipe; all pipe waits live on the I/O thread.
 *
 * Single client: the pipe is created with one instance, so a second
 * connect is refused by the OS until the first client disconnects.
 * Client death or a pipe break drops the session and the server accepts
 * the next connect with the engine untouched. */
#ifndef HARKEN_REMOTE_REMOTE_SERVER_H
#define HARKEN_REMOTE_REMOTE_SERVER_H

#include <harken/Harken.h>

#include "harken_wire_ids.h"

#include <cstdint>
#include <memory>
#include <string>

namespace harkenremote {

struct RemoteServerConfig {
    /* Full pipe path; empty selects the default kPipePrefix + <pid>. */
    std::string pipeName;

    /* Runtime facts reported in the Welcome frame. */
    std::string version;
    std::uint32_t sampleRate = 0;
    std::uint32_t blockFrames = 0;
    std::uint32_t configStructSize = 0;

    /* A frame left incomplete on the wire past this window closes the
     * session (protocol default 10 s; tests shorten it). */
    std::uint32_t readTimeoutMs = kDefaultReadTimeoutMs;

    /* Queue bounds, in frames. The read gate stops consuming request
     * frames while reliable + inbound occupancy reaches the reliable
     * bound, so replies for admitted requests normally always fit; the
     * bounds are host flags so tests can drive the overflow edges. */
    std::uint32_t inboundQueueFrames = 64;
    std::uint32_t reliableQueueFrames = 256;
    std::uint32_t advisoryQueueFrames = 64;

    /* Requests executed per PumpLogic call, bounding pump latency under a
     * command flood; the rest wait for the next pump. */
    std::uint32_t maxCommandsPerPump = 256;

    /* Grace before a server-initiated disconnect tears the pipe down, so
     * a live client can read the final frames already written (a named
     * pipe discards unread data at disconnect; an unbounded flush could
     * hang on the very client that stopped reading). */
    std::uint32_t closeLingerMs = 100;
};

struct RemoteServerStats {
    std::uint64_t sessionsAccepted = 0;
    std::uint64_t commandsExecuted = 0;
    std::uint64_t repliesQueued = 0;
    std::uint64_t advisoryDropCount = 0;
    std::uint32_t advisoryHighWater = 0;
    std::uint64_t snapshotsPushed = 0;
    std::uint64_t notificationsForwarded = 0;
    std::uint64_t sessionsClosedByServer = 0;
    std::uint64_t requestsDiscardedOnClose = 0;
};

class RemoteServer {
public:
    explicit RemoteServer(const RemoteServerConfig& config);
    ~RemoteServer();

    RemoteServer(const RemoteServer&) = delete;
    RemoteServer& operator=(const RemoteServer&) = delete;

    /* Creates the pipe and starts the I/O thread. False when the pipe
     * cannot be created (name in use, invalid); the server is inert then. */
    bool Start();

    /* Stops the I/O thread and closes the pipe. Safe to call twice. */
    void Stop();

    /* Logic-thread pump: executes queued requests strictly FIFO (each
     * fully decoded before its API call; the reply is queued only after
     * the call returns, so a dropped session never leaves a command
     * half-executed), then services the snapshot subscription and the
     * status cadence. Call between Vn_Update pumps on the thread that
     * owns the engine. */
    void PumpLogic();

    /* VnConfig.onNotification bridge: install with `notificationUserData`
     * pointing at this server. Runs inside Vn_Update on the logic thread
     * and forwards the drained stream as advisory frames. */
    static void NotificationBridge(const VnNotification* notification,
                                   void* userData);

    RemoteServerStats Stats() const;
    const std::string& PipeName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace harkenremote

#endif // HARKEN_REMOTE_REMOTE_SERVER_H
