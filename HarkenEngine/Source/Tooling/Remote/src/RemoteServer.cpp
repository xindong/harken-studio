/* Remote server logic-side implementation: request execution between the
 * host's Vn_Update pumps, the snapshot subscription, notification
 * forwarding, and the server status frame. Everything here runs on the
 * thread that owns the engine (plus thread-safe enqueue helpers shared
 * with the notification bridge, which Vn_Update invokes on that same
 * thread); the pipe itself lives entirely on the transport thread. */
#include "harken_remote/RemoteServer.h"

#include "harken_remote/WireDispatch.h"

#include "PipeTransportWin.h"
#include "RemoteShared.h"

#include <chrono>
#include <cstring>

namespace harkenremote {

namespace {

std::uint64_t SteadyNowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

struct RemoteServer::Impl {
    RemoteServerConfig config;
    RemoteShared shared;
    PipeTransport transport;
    bool started = false;

    /* Subscription state (logic thread only). lastSeenEpoch detects a
     * session change so a new client never inherits the old client's
     * subscription. */
    std::uint64_t lastSeenEpoch = 0;
    std::uint32_t subscribePeriodMs = 0; // 0 = not subscribed
    std::uint64_t lastSnapshotPushMs = 0;
    std::uint64_t lastStatusDropCount = 0;

    VnSnapshot snapshot{}; // capture target, off the stack (2.6 KiB)

    static PipeTransportConfig TransportConfig(const RemoteServerConfig& c) {
        PipeTransportConfig t;
        t.pipeName = c.pipeName;
        t.version = c.version;
        t.sampleRate = c.sampleRate;
        t.blockFrames = c.blockFrames;
        t.configStructSize = c.configStructSize;
        t.readTimeoutMs = c.readTimeoutMs;
        t.inboundQueueFrames = c.inboundQueueFrames;
        t.reliableQueueFrames = c.reliableQueueFrames;
        t.closeLingerMs = c.closeLingerMs;
        return t;
    }

    explicit Impl(const RemoteServerConfig& c)
        : config(c), transport(TransportConfig(c), shared) {}

    /* Queues a reply on the reliable half. Returns false only when the
     * reply genuinely cannot be queued for a live session — the
     * close-session condition; a session that died meanwhile just drops
     * the reply (its client is gone) and reports success. */
    bool EnqueueReliable(WireFrame&& frame, std::uint64_t epoch) {
        std::lock_guard<std::mutex> lock(shared.mutex);
        if (!shared.sessionOpen || shared.sessionEpoch != epoch) {
            return true;
        }
        if (shared.reliable.size() >= config.reliableQueueFrames) {
            return false;
        }
        shared.reliable.push_back(std::move(frame));
        ++shared.repliesQueued;
        SetEvent(shared.wake);
        return true;
    }

    /* Queues an advisory frame: oldest dropped on overflow, counted. */
    void EnqueueAdvisory(std::uint16_t msgType,
                         std::vector<std::uint8_t>&& payload) {
        std::lock_guard<std::mutex> lock(shared.mutex);
        if (!shared.sessionOpen) {
            return;
        }
        if (shared.advisory.size() >= config.advisoryQueueFrames) {
            shared.advisory.pop_front();
            ++shared.advisoryDropCount;
        }
        WireFrame frame;
        frame.msgType = msgType;
        frame.flags = kFlagAdvisory;
        frame.sequence = ++shared.serverSequence;
        frame.payload = std::move(payload);
        shared.advisory.push_back(std::move(frame));
        // How deep this queue has ever got. Both sides hold shared.mutex, so
        // measuring at the push is equivalent to measuring at the drain and
        // needs no second lock; nothing here runs on the audio thread. The drop
        // count says frames were already lost, which is too late to size the
        // bound from -- this says how close it came while nothing was lost.
        const std::uint32_t depth =
            static_cast<std::uint32_t>(shared.advisory.size());
        if (depth > shared.advisoryHighWater) {
            shared.advisoryHighWater = depth;
        }
        SetEvent(shared.wake);
    }

    void RequestClose() {
        std::lock_guard<std::mutex> lock(shared.mutex);
        if (shared.sessionOpen) {
            shared.closeRequested = true;
        }
        SetEvent(shared.wake);
    }

    void PushStatus() {
        std::vector<std::uint8_t> payload;
        WireWriter writer(&payload);
        std::uint64_t drops = 0;
        std::uint64_t notifications = 0;
        std::uint64_t snapshots = 0;
        std::uint64_t commands = 0;
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            drops = shared.advisoryDropCount;
            notifications = shared.notificationsForwarded;
            snapshots = shared.snapshotsPushed;
            commands = shared.commandsExecuted;
        }
        writer.U64(drops);
        writer.U64(notifications);
        writer.U64(snapshots);
        writer.U64(commands);
        writer.U32(subscribePeriodMs);
        writer.U32(0);
        EnqueueAdvisory(kMsgServerStatus, std::move(payload));
        lastStatusDropCount = drops;
    }
};

RemoteServer::RemoteServer(const RemoteServerConfig& config) {
    RemoteServerConfig resolved = config;
    if (resolved.pipeName.empty()) {
        resolved.pipeName =
            std::string(kPipePrefix) + std::to_string(GetCurrentProcessId());
    }
    impl_.reset(new Impl(resolved));
    impl_->shared.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

RemoteServer::~RemoteServer() {
    Stop();
    if (impl_->shared.wake != nullptr) {
        CloseHandle(impl_->shared.wake);
        impl_->shared.wake = nullptr;
    }
}

bool RemoteServer::Start() {
    if (impl_->started) {
        return true;
    }
    if (impl_->shared.wake == nullptr) {
        return false;
    }
    if (!impl_->transport.Start()) {
        return false;
    }
    impl_->started = true;
    return true;
}

void RemoteServer::Stop() {
    if (!impl_->started) {
        return;
    }
    impl_->transport.Stop();
    impl_->started = false;
}

void RemoteServer::PumpLogic() {
    Impl& impl = *impl_;

    /* A session change voids the previous client's subscription. */
    {
        std::lock_guard<std::mutex> lock(impl.shared.mutex);
        if (impl.shared.sessionEpoch != impl.lastSeenEpoch) {
            impl.lastSeenEpoch = impl.shared.sessionEpoch;
            impl.subscribePeriodMs = 0;
            impl.lastSnapshotPushMs = 0;
        }
    }

    /* Execute queued requests strictly FIFO, bounded per pump. Each
     * request is popped, decoded fully, executed, and its reply queued
     * only after the API call returned — the no-half-executed-command
     * invariant is structural, not policed. */
    for (std::uint32_t executed = 0; executed < impl.config.maxCommandsPerPump;
         ++executed) {
        InboundRequest request;
        std::uint64_t currentEpoch = 0;
        {
            std::lock_guard<std::mutex> lock(impl.shared.mutex);
            if (impl.shared.inbound.empty()) {
                break;
            }
            request = std::move(impl.shared.inbound.front());
            impl.shared.inbound.pop_front();
            currentEpoch = impl.shared.sessionEpoch;
        }
        if (request.epoch != currentEpoch) {
            continue; // the session this arrived on is gone
        }

        WireFrame reply;
        if (request.msgType == kMsgCommand) {
            DispatchCommand(request.payload.data(), request.payload.size(),
                            request.sequence, &reply);
            {
                std::lock_guard<std::mutex> lock(impl.shared.mutex);
                ++impl.shared.commandsExecuted;
            }
        } else if (request.msgType == kMsgSubscribe) {
            if (request.payload.size() != sizeof(std::uint32_t)) {
                EncodeWireError(&reply, request.sequence,
                                kWireErrMalformedPayload, 0);
            } else {
                std::uint32_t periodMs = 0;
                std::memcpy(&periodMs, request.payload.data(), sizeof(periodMs));
                impl.subscribePeriodMs = periodMs;
                impl.lastSnapshotPushMs = 0; // first push due immediately
                reply.msgType = kMsgSubscribeAck;
                reply.flags = 0;
                reply.sequence = request.sequence;
                WireWriter writer(&reply.payload);
                writer.U32(periodMs);
            }
        } else {
            EncodeWireError(&reply, request.sequence, kWireErrUnknownMsgType, 0);
        }

        if (!impl.EnqueueReliable(std::move(reply), request.epoch)) {
            /* The reliable half is genuinely full: the read gate already
             * stopped admitting requests, so this is the burst tail of a
             * client that will not read. Close the session; the engine
             * state the executed call produced stands (the call
             * completed; only its reply has no reader). */
            impl.RequestClose();
            break;
        }
    }

    /* Snapshot subscription: capture on the pump cadence, push at most
     * every periodMs. The frame payload is the raw VnSnapshot image; its
     * leading structSize field is the vintage stamp. */
    if (impl.subscribePeriodMs > 0) {
        bool open = false;
        {
            std::lock_guard<std::mutex> lock(impl.shared.mutex);
            open = impl.shared.sessionOpen;
        }
        const std::uint64_t now = SteadyNowMs();
        if (open && (impl.lastSnapshotPushMs == 0 ||
                     now - impl.lastSnapshotPushMs >= impl.subscribePeriodMs)) {
            std::memset(&impl.snapshot, 0, sizeof(impl.snapshot));
            impl.snapshot.structSize = sizeof(VnSnapshot);
            if (Vn_CaptureSnapshot(&impl.snapshot) == VN_OK) {
                std::vector<std::uint8_t> payload(sizeof(VnSnapshot));
                std::memcpy(payload.data(), &impl.snapshot, sizeof(VnSnapshot));
                impl.EnqueueAdvisory(kMsgSnapshot, std::move(payload));
                {
                    std::lock_guard<std::mutex> lock(impl.shared.mutex);
                    ++impl.shared.snapshotsPushed;
                }
                impl.lastSnapshotPushMs = now;
                impl.PushStatus();
            }
        }
    }

    /* Advisory drops are surfaced promptly even without a subscription:
     * one status frame per pump at most. */
    {
        std::uint64_t drops = 0;
        {
            std::lock_guard<std::mutex> lock(impl.shared.mutex);
            drops = impl.shared.advisoryDropCount;
        }
        if (drops != impl.lastStatusDropCount) {
            impl.PushStatus();
        }
    }
}

void RemoteServer::NotificationBridge(const VnNotification* notification,
                                      void* userData) {
    if (notification == nullptr || userData == nullptr) {
        return;
    }
    RemoteServer* server = static_cast<RemoteServer*>(userData);
    Impl& impl = *server->impl_;
    {
        std::lock_guard<std::mutex> lock(impl.shared.mutex);
        if (!impl.shared.sessionOpen) {
            return;
        }
        ++impl.shared.notificationsForwarded;
    }
    std::vector<std::uint8_t> payload(sizeof(VnNotification));
    std::memcpy(payload.data(), notification, sizeof(VnNotification));
    impl.EnqueueAdvisory(kMsgNotification, std::move(payload));
}

RemoteServerStats RemoteServer::Stats() const {
    RemoteServerStats stats;
    std::lock_guard<std::mutex> lock(impl_->shared.mutex);
    stats.sessionsAccepted = impl_->shared.sessionsAccepted;
    stats.commandsExecuted = impl_->shared.commandsExecuted;
    stats.repliesQueued = impl_->shared.repliesQueued;
    stats.advisoryDropCount = impl_->shared.advisoryDropCount;
    stats.advisoryHighWater = impl_->shared.advisoryHighWater;
    stats.snapshotsPushed = impl_->shared.snapshotsPushed;
    stats.notificationsForwarded = impl_->shared.notificationsForwarded;
    stats.sessionsClosedByServer = impl_->shared.sessionsClosedByServer;
    stats.requestsDiscardedOnClose = impl_->shared.requestsDiscardedOnClose;
    return stats;
}

const std::string& RemoteServer::PipeName() const {
    return impl_->config.pipeName;
}

} // namespace harkenremote
