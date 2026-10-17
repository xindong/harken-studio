/* Named-pipe transport implementation. One thread owns the pipe for the
 * server's life: an accept loop (one client at a time) around a session
 * loop that interleaves overlapped reads, overlapped writes, and the
 * logic thread's wake event. Framing is validated here — an oversized
 * length prefix or a half-frame stalled past the read timeout closes the
 * session — while everything payload-level (opcodes, marshalling) is the
 * logic-side dispatcher's job, so a malformed command still gets an
 * in-order error reply.
 *
 * Backpressure: a read is posted only while the inbound queue has room
 * AND the reliable outbound half can still absorb a reply for every
 * request already admitted. A client that issues commands without
 * reading replies therefore first stops being read (the pipe's own flow
 * control pushes back), and only a reply that still cannot be queued —
 * possible because one read chunk can carry many small frames past the
 * gate — closes the session. */
#include "PipeTransportWin.h"

#include <cstring>

namespace harkenremote {

namespace {

constexpr DWORD kReadChunkBytes = 64 * 1024;
constexpr DWORD kHousekeepingWaitMs = 100;

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(GetTickCount64());
}

bool IsSessionEndError(DWORD error) {
    return error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA ||
           error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_OPERATION_ABORTED;
}

} // namespace

PipeTransport::PipeTransport(const PipeTransportConfig& config,
                             RemoteShared& shared)
    : config_(config), shared_(shared) {
    pipe_ = INVALID_HANDLE_VALUE;
}

PipeTransport::~PipeTransport() { Stop(); }

bool PipeTransport::Start() {
    if (started_) {
        return true;
    }
    connectEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    readEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    writeEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (connectEvent_ == nullptr || readEvent_ == nullptr ||
        writeEvent_ == nullptr) {
        return false;
    }
    /* One instance: the OS itself refuses a second client with
     * ERROR_PIPE_BUSY until this one disconnects — the single-client
     * contract needs no server-side bookkeeping. Local-only default
     * ACLs; remote clients rejected outright. */
    pipe_ = CreateNamedPipeA(
        config_.pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 64 * 1024, 64 * 1024, 0, nullptr);
    if (pipe_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    started_ = true;
    thread_ = std::thread(&PipeTransport::ThreadMain, this);
    return true;
}

void PipeTransport::Stop() {
    if (!started_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(shared_.mutex);
        shared_.shutdown = true;
    }
    SetEvent(shared_.wake);
    CancelIoEx(pipe_, nullptr);
    if (thread_.joinable()) {
        thread_.join();
    }
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
    CloseHandle(connectEvent_);
    CloseHandle(readEvent_);
    CloseHandle(writeEvent_);
    connectEvent_ = readEvent_ = writeEvent_ = nullptr;
    started_ = false;
}

void PipeTransport::ThreadMain() {
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(shared_.mutex);
            if (shared_.shutdown) {
                return;
            }
        }
        ResetEvent(connectEvent_);
        OVERLAPPED connectOvl{};
        connectOvl.hEvent = connectEvent_;
        BOOL connected = ConnectNamedPipe(pipe_, &connectOvl);
        DWORD error = connected ? ERROR_PIPE_CONNECTED : GetLastError();
        if (!connected && error == ERROR_IO_PENDING) {
            /* Wait for a client, staying responsive to shutdown. Wake
             * pulses for outbound work are ignored here — there is no
             * session to carry them, and the queues are epoch-guarded. */
            for (;;) {
                HANDLE handles[2] = {connectEvent_, shared_.wake};
                const DWORD wait =
                    WaitForMultipleObjects(2, handles, FALSE, kHousekeepingWaitMs);
                {
                    std::lock_guard<std::mutex> lock(shared_.mutex);
                    if (shared_.shutdown) {
                        CancelIoEx(pipe_, &connectOvl);
                        DWORD ignored = 0;
                        GetOverlappedResult(pipe_, &connectOvl, &ignored, TRUE);
                        return;
                    }
                }
                if (wait == WAIT_OBJECT_0) {
                    DWORD ignored = 0;
                    if (!GetOverlappedResult(pipe_, &connectOvl, &ignored, FALSE)) {
                        error = GetLastError();
                    } else {
                        error = ERROR_PIPE_CONNECTED;
                    }
                    break;
                }
            }
        }
        if (error == ERROR_PIPE_CONNECTED) {
            ServeSession();
        } else {
            /* Transient accept failure: back off briefly rather than spin. */
            Sleep(kHousekeepingWaitMs);
        }
        /* Session teardown: whatever queued for the dead client is
         * discarded (requests were never acknowledged; replies have no
         * reader), and the epoch bump keeps the logic thread from
         * executing a dead session's leftovers it may already hold. */
        {
            std::lock_guard<std::mutex> lock(shared_.mutex);
            shared_.sessionOpen = false;
            ++shared_.sessionEpoch;
            shared_.requestsDiscardedOnClose += shared_.inbound.size();
            shared_.inbound.clear();
            shared_.reliable.clear();
            shared_.advisory.clear();
            shared_.closeRequested = false;
            if (shared_.shutdown) {
                DisconnectNamedPipe(pipe_);
                return;
            }
        }
        DisconnectNamedPipe(pipe_);
    }
}

void PipeTransport::ServeSession() {
    enum class End { None, ClientGone, ServerClose, Shutdown };
    End end = End::None;
    bool handshakeDone = false;
    bool closeWhenDrained = false; // set after a HelloReject is queued

    std::vector<std::uint8_t> rx;
    std::vector<std::uint8_t> readChunk(kReadChunkBytes);
    std::vector<std::uint8_t> writeBuf;
    std::deque<WireFrame> handshakeOut; // Welcome / HelloReject, ahead of all

    bool readPending = false;
    bool writePending = false;
    OVERLAPPED readOvl{};
    OVERLAPPED writeOvl{};
    std::uint64_t partialStartMs = 0; // 0 = no incomplete frame at the head

    /* Routes one complete, size-sane frame. Returns false to end the
     * session (protocol violation). */
    auto routeFrame = [&](const WireFrameHeader& header,
                          const std::uint8_t* payload) -> bool {
        if (!handshakeDone) {
            if (header.msgType != kMsgHello || header.payloadBytes < 4) {
                return false; // the client must open with Hello
            }
            std::uint16_t major = 0;
            std::uint16_t minor = 0;
            std::memcpy(&major, payload, 2);
            std::memcpy(&minor, payload + 2, 2);
            (void)minor; // minor is additive-compatible; nothing to gate on
            WireFrame frame;
            frame.sequence = header.sequence;
            if (major != kProtocolMajor) {
                frame.msgType = kMsgHelloReject;
                WireWriter writer(&frame.payload);
                writer.U16(kHelloRejectMajorMismatch);
                writer.U16(kProtocolMajor);
                writer.U16(kProtocolMinor);
                writer.U16(0);
                handshakeOut.push_back(std::move(frame));
                closeWhenDrained = true;
                return true;
            }
            frame.msgType = kMsgWelcome;
            WireWriter writer(&frame.payload);
            writer.U16(kProtocolMajor);
            writer.U16(kProtocolMinor);
            writer.U32(config_.sampleRate);
            writer.U32(config_.blockFrames);
            writer.U32(config_.configStructSize);
            writer.String(config_.version.c_str());
            handshakeOut.push_back(std::move(frame));
            handshakeDone = true;
            std::lock_guard<std::mutex> lock(shared_.mutex);
            shared_.sessionOpen = true;
            ++shared_.sessionsAccepted;
            return true;
        }
        if (header.msgType == kMsgHello) {
            return false; // a second handshake is a protocol violation
        }
        /* Everything else — commands, subscribes, and unknown message
         * types alike — rides the inbound queue so replies keep strict
         * arrival order; the logic side answers unknown types with a
         * wire error. */
        InboundRequest request;
        request.msgType = header.msgType;
        request.sequence = header.sequence;
        request.payload.assign(payload, payload + header.payloadBytes);
        std::lock_guard<std::mutex> lock(shared_.mutex);
        request.epoch = shared_.sessionEpoch;
        shared_.inbound.push_back(std::move(request));
        return true;
    };

    while (end == End::None) {
        bool closeRequested = false;
        bool inboundGateOpen = false;
        bool outboundAvailable = false;
        {
            std::lock_guard<std::mutex> lock(shared_.mutex);
            if (shared_.shutdown) {
                end = End::Shutdown;
                break;
            }
            closeRequested = shared_.closeRequested;
            inboundGateOpen =
                shared_.inbound.size() < config_.inboundQueueFrames &&
                shared_.inbound.size() + shared_.reliable.size() <
                    config_.reliableQueueFrames;
            outboundAvailable =
                !shared_.reliable.empty() || !shared_.advisory.empty();
        }
        if (closeRequested) {
            end = End::ServerClose;
            break;
        }
        if (closeWhenDrained && handshakeOut.empty() && !writePending) {
            end = End::ServerClose;
            break;
        }

        if (!readPending && !closeWhenDrained && inboundGateOpen) {
            ResetEvent(readEvent_);
            readOvl = OVERLAPPED{};
            readOvl.hEvent = readEvent_;
            if (!ReadFile(pipe_, readChunk.data(), kReadChunkBytes, nullptr,
                          &readOvl)) {
                const DWORD error = GetLastError();
                if (error == ERROR_IO_PENDING) {
                    readPending = true;
                } else if (IsSessionEndError(error)) {
                    end = End::ClientGone;
                    break;
                } else {
                    end = End::ServerClose;
                    break;
                }
            } else {
                readPending = true; // completed synchronously; event is set
            }
        }

        if (!writePending &&
            (!handshakeOut.empty() || outboundAvailable)) {
            WireFrame frame;
            bool haveFrame = false;
            if (!handshakeOut.empty()) {
                frame = std::move(handshakeOut.front());
                handshakeOut.pop_front();
                haveFrame = true;
            } else {
                std::lock_guard<std::mutex> lock(shared_.mutex);
                if (!shared_.reliable.empty()) {
                    frame = std::move(shared_.reliable.front());
                    shared_.reliable.pop_front();
                    haveFrame = true;
                } else if (!shared_.advisory.empty()) {
                    frame = std::move(shared_.advisory.front());
                    shared_.advisory.pop_front();
                    haveFrame = true;
                }
            }
            if (haveFrame) {
                writeBuf.clear();
                AppendFrameBytes(frame, &writeBuf);
                ResetEvent(writeEvent_);
                writeOvl = OVERLAPPED{};
                writeOvl.hEvent = writeEvent_;
                if (!WriteFile(pipe_, writeBuf.data(),
                               static_cast<DWORD>(writeBuf.size()), nullptr,
                               &writeOvl)) {
                    const DWORD error = GetLastError();
                    if (error == ERROR_IO_PENDING) {
                        writePending = true;
                    } else if (IsSessionEndError(error)) {
                        end = End::ClientGone;
                        break;
                    } else {
                        end = End::ServerClose;
                        break;
                    }
                } else {
                    writePending = true; // synchronous; event is set
                }
            }
        }

        DWORD waitMs = kHousekeepingWaitMs;
        if (partialStartMs != 0) {
            const std::uint64_t deadline = partialStartMs + config_.readTimeoutMs;
            const std::uint64_t now = NowMs();
            if (now >= deadline) {
                end = End::ServerClose; // half-frame stalled past the timeout
                break;
            }
            const std::uint64_t remaining = deadline - now;
            if (remaining < waitMs) {
                waitMs = static_cast<DWORD>(remaining);
            }
        }

        HANDLE handles[3];
        DWORD handleCount = 0;
        handles[handleCount++] = shared_.wake;
        if (readPending) {
            handles[handleCount++] = readEvent_;
        }
        if (writePending) {
            handles[handleCount++] = writeEvent_;
        }
        WaitForMultipleObjects(handleCount, handles, FALSE, waitMs);

        if (writePending &&
            WaitForSingleObject(writeEvent_, 0) == WAIT_OBJECT_0) {
            DWORD written = 0;
            if (!GetOverlappedResult(pipe_, &writeOvl, &written, FALSE)) {
                end = End::ClientGone;
                break;
            }
            writePending = false;
        }

        if (readPending &&
            WaitForSingleObject(readEvent_, 0) == WAIT_OBJECT_0) {
            DWORD bytesRead = 0;
            if (!GetOverlappedResult(pipe_, &readOvl, &bytesRead, FALSE) ||
                bytesRead == 0) {
                end = End::ClientGone;
                break;
            }
            readPending = false;
            rx.insert(rx.end(), readChunk.data(), readChunk.data() + bytesRead);

            /* Frame extraction. The length prefix is checked before any
             * allocation or wait depends on it; a frame is routed only
             * once it is completely present. */
            std::size_t consumed = 0;
            bool violation = false;
            while (rx.size() - consumed >= kFrameHeaderBytes) {
                WireFrameHeader header;
                DecodeFrameHeader(rx.data() + consumed, &header);
                if (header.payloadBytes > kMaxFrameBytes) {
                    violation = true; // oversized length prefix: close
                    break;
                }
                const std::size_t frameBytes =
                    kFrameHeaderBytes + header.payloadBytes;
                if (rx.size() - consumed < frameBytes) {
                    break; // incomplete: wait for more bytes
                }
                if (!routeFrame(header,
                                rx.data() + consumed + kFrameHeaderBytes)) {
                    violation = true;
                    break;
                }
                consumed += frameBytes;
            }
            if (violation) {
                end = End::ServerClose;
                break;
            }
            if (consumed > 0) {
                rx.erase(rx.begin(),
                         rx.begin() + static_cast<std::ptrdiff_t>(consumed));
            }
            /* The half-frame clock starts when an incomplete frame first
             * heads the buffer and is NOT reset by trickled bytes that
             * still leave it incomplete — a one-byte-per-interval drip
             * must not hold a session open forever. */
            if (rx.empty()) {
                partialStartMs = 0;
            } else if (partialStartMs == 0 || consumed > 0) {
                partialStartMs = NowMs();
            }
        }
    }

    /* Reap outstanding overlapped I/O before the buffers go out of scope. */
    CancelIoEx(pipe_, nullptr);
    DWORD ignored = 0;
    if (readPending) {
        GetOverlappedResult(pipe_, &readOvl, &ignored, TRUE);
    }
    if (writePending) {
        GetOverlappedResult(pipe_, &writeOvl, &ignored, TRUE);
    }

    if (end == End::ServerClose) {
        {
            std::lock_guard<std::mutex> lock(shared_.mutex);
            ++shared_.sessionsClosedByServer;
        }
        /* Give a live client a bounded window to read the final frames
         * already written (DisconnectNamedPipe discards unread data; an
         * unbounded flush could hang on the very client that stopped
         * reading — the case this close exists for). */
        if (config_.closeLingerMs > 0) {
            Sleep(config_.closeLingerMs);
        }
    }
}

} // namespace harkenremote
