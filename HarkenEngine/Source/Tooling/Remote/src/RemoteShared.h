/* State shared between the remote server's two threads: the pipe I/O
 * thread (transport owner) and the host logic thread (API executor).
 * One mutex guards everything here; both sides hold it only for queue
 * moves and flag flips, never across pipe waits or API calls. */
#ifndef HARKEN_REMOTE_SRC_REMOTE_SHARED_H
#define HARKEN_REMOTE_SRC_REMOTE_SHARED_H

#include "harken_remote/WireFormat.h"

#include <cstdint>
#include <deque>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace harkenremote {

/* One client frame routed to the logic thread. The epoch pins it to the
 * session it arrived on: a request that outlives its session is discarded
 * instead of executed against the engine on a dead client's behalf. */
struct InboundRequest {
    std::uint64_t epoch = 0;
    std::uint16_t msgType = 0;
    std::uint32_t sequence = 0;
    std::vector<std::uint8_t> payload;
};

struct RemoteShared {
    std::mutex mutex;

    /* I/O -> logic: requests in arrival order (strict per-session FIFO). */
    std::deque<InboundRequest> inbound;

    /* logic -> I/O: the reliable half (command replies, never dropped)
     * and the advisory half (snapshots, notifications, status; oldest
     * dropped on overflow). The writer drains reliable first. */
    std::deque<WireFrame> reliable;
    std::deque<WireFrame> advisory;

    /* Bumped by the I/O thread when a session ends; sessionOpen spans
     * handshake-complete to session end. */
    std::uint64_t sessionEpoch = 0;
    bool sessionOpen = false;

    /* logic -> I/O: drop the current session (reliable overflow). */
    bool closeRequested = false;

    /* Server shutdown (Stop). */
    bool shutdown = false;

    /* Counters (guarded by the mutex; surfaced via Stats and the wire
     * ServerStatus frame). */
    std::uint64_t sessionsAccepted = 0;
    std::uint64_t commandsExecuted = 0;
    std::uint64_t repliesQueued = 0;
    std::uint64_t advisoryDropCount = 0;
    /* Deepest the advisory queue has ever been, in frames. Guarded by mutex
       like the counters beside it. */
    std::uint32_t advisoryHighWater = 0;
    std::uint64_t snapshotsPushed = 0;
    std::uint64_t notificationsForwarded = 0;
    std::uint64_t sessionsClosedByServer = 0;
    std::uint64_t requestsDiscardedOnClose = 0;

    /* Sequence stream for server-originated (advisory) frames; client
     * sequences are echoed on replies instead. */
    std::uint32_t serverSequence = 0;

    /* Auto-reset event the logic thread sets when outbound work or a
     * close request appears; the I/O thread waits on it alongside its
     * overlapped I/O events. Owned by RemoteServer::Impl. */
    HANDLE wake = nullptr;
};

} // namespace harkenremote

#endif // HARKEN_REMOTE_SRC_REMOTE_SHARED_H
