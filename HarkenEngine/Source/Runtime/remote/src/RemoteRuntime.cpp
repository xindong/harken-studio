#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#if defined(__ANDROID__)
#include <sys/ioctl.h>
#else
#include <ifaddrs.h>
#endif
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "Engine.h"
#include "MonitorCaptureHub.h"
#include "RemoteRuntime.h"
#include "harken_remote/Protocol.h"
#include <harken_buildinfo.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

namespace harken::remote {
namespace {

#if defined(_WIN32)
using Socket = SOCKET;
using AddressLength = int;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void CloseSocket(Socket value) {
    if (value != kInvalidSocket)
        closesocket(value);
}
bool WouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
bool SetNonBlocking(Socket value) {
    u_long enabled = 1;
    return ioctlsocket(value, FIONBIO, &enabled) == 0;
}
std::uint32_t ProcessId() { return GetCurrentProcessId(); }
#else
using Socket = int;
using AddressLength = socklen_t;
constexpr Socket kInvalidSocket = -1;
void CloseSocket(Socket value) {
    if (value != kInvalidSocket)
        close(value);
}
bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
bool SetNonBlocking(Socket value) {
#if defined(__APPLE__)
    int enabled = 1;
    if (setsockopt(value, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0)
        return false;
#endif
    const int flags = fcntl(value, F_GETFL, 0);
    return flags >= 0 && fcntl(value, F_SETFL, flags | O_NONBLOCK) == 0;
}
std::uint32_t ProcessId() { return static_cast<std::uint32_t>(getpid()); }
#endif

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::vector<in_addr> MulticastInterfaces(in_addr configured, [[maybe_unused]] Socket discovery) {
    if (configured.s_addr != htonl(INADDR_ANY))
        return {configured};
    std::vector<in_addr> addresses;
    auto append = [&](in_addr address) {
        if (address.s_addr == htonl(INADDR_ANY))
            return;
        const auto found = std::find_if(addresses.begin(), addresses.end(), [&](in_addr existing) {
            return existing.s_addr == address.s_addr;
        });
        if (found == addresses.end())
            addresses.push_back(address);
    };
#if defined(_WIN32)
    ULONG size = 16384;
    std::vector<std::uint8_t> buffer(size);
    ULONG result = GetAdaptersAddresses(
        AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()), &size);
    if (result == ERROR_BUFFER_OVERFLOW && size <= 1048576) {
        buffer.resize(size);
        result = GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()), &size);
    }
    if (result == NO_ERROR) {
        for (auto *adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
             adapter != nullptr; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp ||
                (adapter->Flags & IP_ADAPTER_NO_MULTICAST) != 0)
                continue;
            for (auto *entry = adapter->FirstUnicastAddress; entry != nullptr;
                 entry = entry->Next) {
                if (entry->Address.lpSockaddr != nullptr &&
                    entry->Address.lpSockaddr->sa_family == AF_INET) {
                    append(
                        reinterpret_cast<const sockaddr_in *>(entry->Address.lpSockaddr)->sin_addr);
                }
            }
        }
    }
#elif defined(__ANDROID__)
    std::vector<ifreq> interfaces(16);
    for (;;) {
        ifconf config{};
        config.ifc_len = static_cast<int>(interfaces.size() * sizeof(ifreq));
        config.ifc_req = interfaces.data();
        if (ioctl(discovery, SIOCGIFCONF, &config) != 0)
            break;
        if (static_cast<std::size_t>(config.ifc_len) == interfaces.size() * sizeof(ifreq)) {
            interfaces.resize(interfaces.size() * 2);
            continue;
        }
        const auto count = static_cast<std::size_t>(config.ifc_len) / sizeof(ifreq);
        for (std::size_t i = 0; i < count; ++i) {
            const auto &entry = interfaces[i];
            ifreq flags = entry;
            if (entry.ifr_addr.sa_family == AF_INET &&
                ioctl(discovery, SIOCGIFFLAGS, &flags) == 0 &&
                (flags.ifr_flags & IFF_UP) != 0 && (flags.ifr_flags & IFF_MULTICAST) != 0) {
                append(reinterpret_cast<const sockaddr_in *>(&entry.ifr_addr)->sin_addr);
            }
        }
        break;
    }
#else
    ifaddrs *interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        const std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> cleanup(interfaces, freeifaddrs);
        for (auto *entry = interfaces; entry != nullptr; entry = entry->ifa_next) {
            if (entry->ifa_addr != nullptr && entry->ifa_addr->sa_family == AF_INET &&
                (entry->ifa_flags & IFF_UP) != 0 && (entry->ifa_flags & IFF_MULTICAST) != 0) {
                append(reinterpret_cast<const sockaddr_in *>(entry->ifa_addr)->sin_addr);
            }
        }
    }
#endif
    in_addr loopback{};
    inet_pton(AF_INET, "127.0.0.1", &loopback);
    append(loopback);
    return addresses;
}

int WaitSockets(Socket listener, Socket discovery, Socket client, bool readClient, bool writeClient,
                std::uint32_t &events) {
    events = 0;
#if defined(_WIN32)
    fd_set reads, writes;
    FD_ZERO(&reads);
    FD_ZERO(&writes);
    FD_SET(listener, &reads);
    if (discovery != kInvalidSocket)
        FD_SET(discovery, &reads);
    if (client != kInvalidSocket) {
        if (readClient)
            FD_SET(client, &reads);
        if (writeClient)
            FD_SET(client, &writes);
    }
    timeval timeout{};
    timeout.tv_usec = 25000;
    const int result = select(0, &reads, &writes, nullptr, &timeout);
    if (result <= 0)
        return result;
    if (FD_ISSET(listener, &reads))
        events |= 1;
    if (discovery != kInvalidSocket && FD_ISSET(discovery, &reads))
        events |= 2;
    if (client != kInvalidSocket && FD_ISSET(client, &reads))
        events |= 4;
    if (client != kInvalidSocket && FD_ISSET(client, &writes))
        events |= 8;
    return result;
#else
    pollfd descriptors[3]{};
    descriptors[0] = {listener, POLLIN, 0};
    descriptors[1] = {discovery, POLLIN, 0};
    descriptors[2] = {
        client, static_cast<short>((readClient ? POLLIN : 0) | (writeClient ? POLLOUT : 0)), 0};
    const int result = poll(descriptors, 3, 25);
    if (result <= 0)
        return result;
    if ((descriptors[0].revents & (POLLERR | POLLNVAL)) != 0 ||
        (descriptors[1].revents & (POLLERR | POLLNVAL)) != 0) {
        errno = EBADF;
        return -1;
    }
    if ((descriptors[0].revents & POLLIN) != 0)
        events |= 1;
    if ((descriptors[1].revents & POLLIN) != 0)
        events |= 2;
    if ((descriptors[2].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0)
        events |= 4;
    if ((descriptors[2].revents & POLLOUT) != 0)
        events |= 8;
    return result;
#endif
}

std::uint64_t NewInstanceId() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto ticks =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    return (ticks ^ (static_cast<std::uint64_t>(ProcessId()) << 32) ^ ++sequence) | 1;
}

template <std::size_t N> void CopyText(char (&out)[N], const std::string &text) {
    const std::size_t size = std::min(text.size(), N - 1);
    std::memcpy(out, text.data(), size);
    out[size] = 0;
}

bool ConfigText(const char *value, std::size_t maximum) {
    if (value == nullptr)
        return true;
    for (std::size_t i = 0; i < maximum; ++i)
        if (value[i] == 0)
            return true;
    return false;
}

struct Request {
    Frame frame;
    std::uint64_t epoch = 0;
};
std::vector<VnActionFiringHandle> g_remoteFirings;

void ReapFirings() {
    g_remoteFirings.erase(
        std::remove_if(g_remoteFirings.begin(), g_remoteFirings.end(),
                       [](VnActionFiringHandle handle) {
                           VnActionFiringInfo info{};
                           info.structSize = sizeof(info);
                           const VnResult result = Vn_QueryActionFiring(handle, &info);
                           if (result == VN_STALE_HANDLE)
                               return true;
                           return result == VN_OK &&
                                  info.state == VN_ACTION_FIRING_TERMINAL_RETAINED &&
                                  Vn_ReleaseActionFiring(handle) == VN_OK;
                       }),
        g_remoteFirings.end());
}

void (*g_beforeCommandDispatchForTests)(void*) = nullptr;
void* g_beforeCommandDispatchContextForTests = nullptr;

class Server {
  public:
    VnRemoteConfig config{};
    Identity identity;
    VnRemoteStatus status{};
    std::string bindAddress;
    std::mutex mutex;
    std::deque<Request> inbound;
    std::deque<std::vector<std::uint8_t>> outbound;
    std::size_t inboundBytes = 0;
    std::size_t outboundBytes = 0;
    std::atomic<bool> stopping{false};
    bool closeRequested = false;
    bool welcomed = false;
    std::uint64_t epoch = 0;
    std::uint64_t nextEpoch = 1;
    std::uint64_t negotiatedCapabilities = 0;
    std::uint32_t observationAllowedMask = 0;
    Socket listener = kInvalidSocket;
    Socket discovery = kInvalidSocket;
    std::thread worker;
    bool socketsInitialized = false;
    std::uint64_t logicEpoch = 0;
    VnMonitorSessionHandle monitorSession = 0;
    std::uint64_t monitorEpoch = 0;
    std::uint64_t monitorLease = 0;
    std::uint32_t periodMs = 0;
    std::uint64_t lastPush = 0;
    std::uint64_t discoveryWindow = 0;
    std::uint32_t discoveryReplies = 0;
    std::vector<std::uint8_t> monitorBuffer;
    VnMonitorSessionHandle eventSession = 0;
    std::uint64_t eventLastPush = 0;
    std::vector<std::uint8_t> eventBuffer;
    bool eventV2 = false;
    std::uint32_t eventAllowedMask = 0;

    ~Server() { Stop(); }

    bool Start() {
#if defined(_WIN32)
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            return false;
        socketsInitialized = true;
#endif
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == kInvalidSocket || !SetNonBlocking(listener))
            return false;
#if defined(_WIN32)
        BOOL exclusive = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char *>(&exclusive), sizeof(exclusive));
#else
        int enabled = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(config.tcpPort));
        if (inet_pton(AF_INET, bindAddress.c_str(), &address.sin_addr) != 1 ||
            bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
            listen(listener, 4) != 0)
            return false;
        AddressLength length = sizeof(address);
        if (getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) != 0)
            return false;
        identity.tcpPort = ntohs(address.sin_port);
        status.tcpPort = identity.tcpPort;
        if ((config.flags & VN_REMOTE_DISCOVERY_ENABLED) != 0 && !StartDiscovery(address.sin_addr))
            return false;
        char hostname[128]{};
        if (gethostname(hostname, sizeof(hostname) - 1) == 0)
            identity.machineName = hostname;
        status.state = VN_REMOTE_LISTENING;
        worker = std::thread([this] {
            try {
                Run();
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex);
                status.lastResult = VN_DEVICE_ERROR;
                status.state = VN_REMOTE_FAILED;
                epoch = 0;
                welcomed = false;
                inbound.clear();
                inboundBytes = 0;
            }
        });
        return true;
    }

    bool StartDiscovery(in_addr bindInterface) {
        discovery = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (discovery == kInvalidSocket || !SetNonBlocking(discovery))
            return false;
        int enabled = 1;
        if (setsockopt(discovery, SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char *>(&enabled), sizeof(enabled)) != 0)
            return false;
#if defined(__APPLE__)
        if (setsockopt(discovery, SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0)
            return false;
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(config.discoveryPort));
        // address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(discovery, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
            return false;
        ip_mreq membership{};
        inet_pton(AF_INET, kDiscoveryGroup, &membership.imr_multiaddr);
        std::uint32_t joined = 0;
        for (const auto &address : MulticastInterfaces(bindInterface, discovery)) {
            membership.imr_interface = address;
            if (setsockopt(discovery, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                           reinterpret_cast<const char *>(&membership), sizeof(membership)) == 0)
                ++joined;
        }
        if (joined == 0)
            return false;
        status.discoveryPort = config.discoveryPort;
        return true;
    }

    void Stop() {
        stopping.store(true, std::memory_order_release);
        if (worker.joinable())
            worker.join();
        CloseSocket(listener);
        listener = kInvalidSocket;
        CloseSocket(discovery);
        discovery = kInvalidSocket;
#if defined(_WIN32)
        if (socketsInitialized) {
            WSACleanup();
            socketsInitialized = false;
        }
#endif
    }

    bool Queue(Frame frame, std::uint64_t expectedEpoch, bool advisory = false) {
        auto bytes = EncodeFrame(frame);
        if (bytes.empty())
            return false;
        std::lock_guard<std::mutex> lock(mutex);
        if (!welcomed || epoch != expectedEpoch)
            return false;
        if (outboundBytes + bytes.size() > config.queueBytes || outbound.size() >= 256) {
            if (advisory)
                ++status.droppedFrames;
            else
                closeRequested = true;
            return false;
        }
        outboundBytes += bytes.size();
        outbound.push_back(std::move(bytes));
        return true;
    }

    void Disconnect() {
        std::lock_guard<std::mutex> lock(mutex);
        epoch = 0;
        welcomed = false;
        closeRequested = false;
        negotiatedCapabilities = 0;
        observationAllowedMask = 0;
        inbound.clear();
        outbound.clear();
        inboundBytes = outboundBytes = 0;
        status.clientCount = 0;
        status.sessionId = 0;
        if (status.state != VN_REMOTE_FAILED)
            status.state = VN_REMOTE_LISTENING;
    }

    void DiscoveryReply() {
        std::array<std::uint8_t, 1024> bytes{};
        sockaddr_in sender{};
        AddressLength length = sizeof(sender);
        const int received = recvfrom(discovery, reinterpret_cast<char *>(bytes.data()),
                                      static_cast<int>(bytes.size()), 0,
                                      reinterpret_cast<sockaddr *>(&sender), &length);
        Header header;
        if (received != static_cast<int>(kHeaderBytes) ||
            !DecodeHeader(bytes.data(), received, header) || header.type != Message::Discover ||
            header.payloadBytes != 0)
            return;
        if (bindAddress == "127.0.0.1" && (ntohl(sender.sin_addr.s_addr) >> 24) != 127)
            return;
        const auto now = NowMs();
        if (now - discoveryWindow >= 1000) {
            discoveryWindow = now;
            discoveryReplies = 0;
        }
        if (discoveryReplies >= 64)
            return;
        ++discoveryReplies;
        Identity announced = identity;
        announced.sessionId = 0;
        const auto reply =
            EncodeFrame(MakeFrame(Message::Announcement, header.requestId, announced));
        sendto(discovery, reinterpret_cast<const char *>(reply.data()),
               static_cast<int>(reply.size()), 0, reinterpret_cast<const sockaddr *>(&sender),
               length);
    }

    bool ReceiveFrame(const Frame &frame, std::uint64_t connectionEpoch, bool &isWelcomed,
                      bool &closeAfterWrite) {
        if (!isWelcomed) {
            Hello hello;
            Identity welcome = identity;
            welcome.sessionId = connectionEpoch;
            if (frame.type != Message::Hello || frame.requestId == 0 ||
                !DecodePayload(frame, hello))
                welcome.result = VN_INVALID_ARG;
            else if (hello.major != kProtocolMajor)
                welcome.result = VN_MONITOR_INCOMPATIBLE;
            welcome.minor = std::min(hello.minor, kProtocolMinor);
            welcome.capabilities &= hello.capabilities;
#if defined(HARKEN_MONITOR_HUB_ABSENT)
            welcome.capabilities &= ~(kCapabilityEventCapture | kCapabilityExecutionObservation);
#endif
            auto bytes = EncodeFrame(MakeFrame(Message::Welcome, frame.requestId, welcome));
            std::lock_guard<std::mutex> lock(mutex);
            outboundBytes += bytes.size();
            outbound.push_back(std::move(bytes));
            if (welcome.result != VN_OK) {
                closeAfterWrite = true;
                return true;
            }
            isWelcomed = true;
            welcomed = true;
            epoch = connectionEpoch;
            negotiatedCapabilities = welcome.capabilities;
            // The explicitly started development transport grants observation only.
            // Client capabilities and request masks cannot extend this policy.
            observationAllowedMask = (welcome.capabilities & kCapabilityExecutionObservation) != 0
                ? (VN_MONITOR_PERM_OBSERVE_EVENTS | VN_MONITOR_PERM_OBSERVE_STATE |
                   VN_MONITOR_PERM_SYMBOLS) : 0;
            status.state = VN_REMOTE_CONNECTED;
            status.clientCount = 1;
            status.sessionId = connectionEpoch;
            return true;
        }
        if (frame.requestId == 0 || frame.type == Message::Hello)
            return false;
        if (frame.type == Message::Goodbye)
            return false;
        if (frame.type == Message::Ping) {
            if (!frame.payload.empty())
                return false;
            return Queue({Message::Pong, frame.requestId, {}}, connectionEpoch);
        }
        std::lock_guard<std::mutex> lock(mutex);
        const std::size_t cost = kHeaderBytes + frame.payload.size();
        if (inboundBytes + cost > config.queueBytes || inbound.size() >= 64)
            return false;
        inbound.push_back({frame, connectionEpoch});
        inboundBytes += cost;
        return true;
    }

    void Run() {
        Socket client = kInvalidSocket;
        Socket rejecting = kInvalidSocket;
        struct ClientCleanup {
            Socket &client;
            ~ClientCleanup() { CloseSocket(client); }
        } cleanup{client};
        ClientCleanup rejectionCleanup{rejecting};
        std::vector<std::uint8_t> rejectionBytes;
        std::size_t rejectionOffset = 0;
        std::uint64_t rejectionDeadline = 0;
        bool rejectionShutdown = false;
        std::vector<std::uint8_t> input;
        std::vector<std::uint8_t> writing;
        std::size_t writeOffset = 0;
        std::uint64_t connectedAt = 0, lastRead = 0, partialSince = 0, lastWrite = 0;
        std::uint64_t connectionEpoch = 0;
        std::uint32_t lastRequestId = 0;
        bool isWelcomed = false, closeAfterWrite = false;
        auto dropClient = [&] {
            CloseSocket(client);
            client = kInvalidSocket;
            input.clear();
            writing.clear();
            writeOffset = 0;
            isWelcomed = closeAfterWrite = false;
            Disconnect();
        };
        while (!stopping.load(std::memory_order_acquire)) {
            if (rejecting != kInvalidSocket) {
                bool closeRejection = NowMs() >= rejectionDeadline;
                if (!closeRejection && rejectionOffset < rejectionBytes.size()) {
#if defined(__linux__)
                    constexpr int flags = MSG_NOSIGNAL;
#else
                    constexpr int flags = 0;
#endif
                    const int count = send(
                        rejecting,
                        reinterpret_cast<const char *>(rejectionBytes.data() + rejectionOffset),
                        static_cast<int>(rejectionBytes.size() - rejectionOffset), flags);
                    if (count > 0)
                        rejectionOffset += count;
                    else if (count < 0 && !WouldBlock())
                        closeRejection = true;
                }
                if (!closeRejection && rejectionOffset == rejectionBytes.size() &&
                    !rejectionShutdown) {
#if defined(_WIN32)
                    shutdown(rejecting, SD_SEND);
#else
                    shutdown(rejecting, SHUT_WR);
#endif
                    rejectionShutdown = true;
                }
                if (!closeRejection) {
                    std::array<char, 2048> discarded{};
                    const int count =
                        recv(rejecting, discarded.data(), static_cast<int>(discarded.size()), 0);
                    closeRejection = count == 0 || (count < 0 && !WouldBlock());
                }
                if (closeRejection) {
                    CloseSocket(rejecting);
                    rejecting = kInvalidSocket;
                    rejectionBytes.clear();
                }
            }
            bool mustClose = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                mustClose = closeRequested;
                if (client != kInvalidSocket && writing.empty() && !outbound.empty()) {
                    writing = std::move(outbound.front());
                    outbound.pop_front();
                    outboundBytes -= writing.size();
                    writeOffset = 0;
                    lastWrite = NowMs();
                }
            }
            const std::uint64_t now = NowMs();
            if (client != kInvalidSocket &&
                (mustClose ||
                 (!isWelcomed && now - connectedAt > std::min(config.requestTimeoutMs, 5000u)) ||
                 (now - lastRead > config.requestTimeoutMs) ||
                 (partialSince != 0 && now - partialSince > config.requestTimeoutMs) ||
                 (!writing.empty() && now - lastWrite > config.requestTimeoutMs)))
                dropClient();
            if (client != kInvalidSocket && closeAfterWrite && writing.empty()) {
                dropClient();
            }
            std::uint32_t events = 0;
            const int selected = WaitSockets(listener, discovery, client, !closeAfterWrite,
                                             !writing.empty(), events);
            if (selected < 0) {
                if (WouldBlock())
                    continue;
                std::lock_guard<std::mutex> lock(mutex);
                status.state = VN_REMOTE_FAILED;
                status.lastResult = VN_DEVICE_ERROR;
                break;
            }
            if ((events & 2) != 0)
                DiscoveryReply();
            if (client != kInvalidSocket && (events & 4) != 0) {
                std::array<std::uint8_t, 16384> bytes{};
                const int count = recv(client, reinterpret_cast<char *>(bytes.data()),
                                       static_cast<int>(bytes.size()), 0);
                if (count == 0 || (count < 0 && !WouldBlock())) {
                    dropClient();
                    continue;
                }
                if (count > 0) {
                    if (input.size() + static_cast<std::size_t>(count) >
                        kMaxPayloadBytes + kHeaderBytes + bytes.size()) {
                        dropClient();
                        continue;
                    }
                    if (input.empty())
                        partialSince = NowMs();
                    input.insert(input.end(), bytes.begin(), bytes.begin() + count);
                    bool malformed = false;
                    while (input.size() >= kHeaderBytes) {
                        Header header;
                        if (!DecodeHeader(input.data(), input.size(), header)) {
                            malformed = true;
                            break;
                        }
                        const std::size_t total = kHeaderBytes + header.payloadBytes;
                        if (input.size() < total)
                            break;
                        Frame frame{header.type,
                                    header.requestId,
                                    {input.begin() + kHeaderBytes, input.begin() + total}};
                        input.erase(input.begin(), input.begin() + total);
                        if (frame.requestId <= lastRequestId) {
                            malformed = true;
                            break;
                        }
                        lastRequestId = frame.requestId;
                        if (!ReceiveFrame(frame, connectionEpoch, isWelcomed, closeAfterWrite)) {
                            malformed = true;
                            break;
                        }
                        lastRead = NowMs();
                        partialSince = input.empty() ? 0 : lastRead;
                        if (closeAfterWrite) {
                            input.clear();
                            break;
                        }
                    }
                    if (malformed) {
                        dropClient();
                        continue;
                    }
                }
            }
            if (client != kInvalidSocket && !writing.empty() && (events & 8) != 0) {
#if defined(__linux__)
                constexpr int flags = MSG_NOSIGNAL;
#else
                constexpr int flags = 0;
#endif
                const int count =
                    send(client, reinterpret_cast<const char *>(writing.data() + writeOffset),
                         static_cast<int>(writing.size() - writeOffset), flags);
                if (count < 0 && !WouldBlock()) {
                    dropClient();
                    continue;
                }
                if (count > 0) {
                    writeOffset += count;
                    if (writeOffset == writing.size()) {
                        writing.clear();
                        writeOffset = 0;
                        std::lock_guard<std::mutex> lock(mutex);
                        ++status.sentFrames;
                    }
                }
            }
            if ((events & 1) != 0) {
                if (client != kInvalidSocket) {
                    char probe = 0;
                    const int remaining = recv(client, &probe, 1, MSG_PEEK);
                    if (remaining == 0 || (remaining < 0 && !WouldBlock()))
                        dropClient();
                }
                Socket accepted = accept(listener, nullptr, nullptr);
                if (accepted != kInvalidSocket) {
                    if (!SetNonBlocking(accepted))
                        CloseSocket(accepted);
                    else if (client != kInvalidSocket) {
                        if (rejecting != kInvalidSocket)
                            CloseSocket(accepted);
                        else {
                            Writer busy;
                            busy.I32(VN_IN_USE);
                            rejectionBytes =
                                EncodeFrame({Message::Error, 0, std::move(busy.bytes)});
                            rejectionOffset = 0;
                            rejectionDeadline = NowMs() + 250;
                            rejectionShutdown = false;
                            rejecting = accepted;
                        }
                    } else {
                        client = accepted;
                        connectionEpoch = nextEpoch++;
                        lastRequestId = 0;
                        connectedAt = lastRead = NowMs();
                        partialSince = 0;
                    }
                }
            }
        }
        dropClient();
    }

    void CloseMonitor() {
        if (monitorSession != 0) {
            if (monitorLease != 0) {
                VnMonitorEffectiveCapturePlan stopped{};
                stopped.structSize = sizeof(stopped);
                Vn_MonitorStopCapture(monitorSession, monitorLease, &stopped);
            }
            Vn_MonitorCloseSession(monitorSession);
        }
        monitorSession = 0;
        monitorEpoch = 0;
        monitorLease = 0;
        periodMs = 0;
    }

    void CloseEventCapture() {
        if (eventSession != 0) Vn_MonitorCloseSession(eventSession);
        eventSession = 0;
        eventLastPush = 0;
    }

    EventCaptureStatusV2 EventStatus() {
        EventCaptureStatusV2 result;
        VnMonitorEventSessionStatusV2 current{};
        current.structSize = sizeof(current);
        result.base.result = Vn_MonitorGetEventSessionStatus(eventSession,
            reinterpret_cast<VnMonitorEventSessionStatus*>(&current));
        result.base.state = current.state;
        result.base.sessionEpoch = current.monitorSessionEpoch;
        result.base.startedTimestampNs = current.startedTimestampNs;
        result.base.stoppedTimestampNs = current.stoppedTimestampNs;
        result.base.sourceDroppedRecords = current.sourceDroppedRecords;
        result.base.deliveryDroppedRecords = current.deliveryDroppedRecords;
        result.acceptedExecutionFactVersion = current.acceptedExecutionFactVersion;
        result.baselineAvailability = current.baselineAvailability;
        std::copy(std::begin(current.supportedFactKinds), std::end(current.supportedFactKinds),
                  std::begin(result.supportedFactKinds));
        result.requestedPermissionMask = current.requestedPermissionMask;
        result.acceptedPermissionMask = current.acceptedPermissionMask;
        result.deniedPermissionMask = current.deniedPermissionMask;
        result.unsupportedPermissionMask = current.unsupportedPermissionMask;
        result.acceptedStreamKindMask = current.acceptedStreamKindMask;
        result.symbolsDecision = current.symbolsDecision;
        result.symbolAvailability = current.symbolAvailability;
        result.identityUnavailableCount = current.identityUnavailableCount;
        result.baselineStateCutId = current.baselineStateCutId;
        return result;
    }

    EventCaptureStatusV2 StartEventCapture(const EventCaptureRequestV2* request = nullptr) {
        EventCaptureStatusV2 result;
        if (eventSession != 0) {
            result.base.result = VN_IN_USE;
            return result;
        }
        VnMonitorSessionDescV2 desc{};
        desc.structSize = request ? sizeof(desc) : sizeof(VnMonitorSessionDesc);
        desc.requestedStreamKindMask = request ? request->requestedStreamKindMask : VN_MONITOR_STREAM_EVENT;
        desc.requestedPermissionMask = request ? request->requestedPermissionMask : VN_MONITOR_PERM_OBSERVE_EVENTS;
        desc.consumerQueueBytes = request ? request->consumerQueueBytes : 1048576;
        desc.requestSymbols = request ? request->requestSymbols : 0;
        desc.requestedExecutionFactVersion = request ? request->executionFactVersion : 0;
        result.base.result = monitor::OpenEventSession(
            reinterpret_cast<const VnMonitorSessionDesc*>(&desc), &eventSession, true,
            identity.instanceId ^ logicEpoch, eventAllowedMask);
        if (result.base.result != VN_OK) return result;
        eventV2 = request != nullptr;
        eventBuffer.resize(65536);
        eventLastPush = 0;
        return EventStatus();
    }

    bool QueueEventStatus(const EventCaptureStatusV2& status, std::uint32_t requestId,
                          std::uint64_t currentEpoch, bool v2) {
        return Queue(v2 ? MakeFrame(Message::EventStatusV2, requestId, status)
                        : MakeFrame(Message::EventStatus, requestId, status.base), currentEpoch);
    }

    void PumpEvents(std::uint64_t currentEpoch) {
        if (eventSession == 0 || NowMs() - eventLastPush < 50) return;
        eventLastPush = NowMs();
        auto status = EventStatus();
        VnMonitorPollDesc poll{};
        poll.structSize = sizeof(poll);
        poll.buffer = eventBuffer.data();
        poll.bufferBytes = eventBuffer.size();
        VnMonitorPollResult result{};
        result.structSize = sizeof(result);
        if (status.base.result == VN_OK) status.base.result = Vn_MonitorPoll(eventSession, &poll, &result);
        if (status.base.result == VN_OK && result.bytesWritten != 0) {
            MonitorRecords records;
            records.sessionEpoch = status.base.sessionEpoch;
            records.recordCount = result.recordsWritten;
            records.bytes.assign(eventBuffer.begin(), eventBuffer.begin() + result.bytesWritten);
            if (!Queue(MakeFrame(Message::EventRecords, 0, records), currentEpoch, true)) {
                status.base.result = VN_BACKPRESSURE;
                status.base.transportDroppedRecords = result.recordsWritten;
            }
        }
        if (status.base.result == VN_OK) status = EventStatus();
        const bool queued = QueueEventStatus(status, 0, currentEpoch, eventV2);
        if (!queued || status.base.result != VN_OK || status.base.state == VN_MONITOR_EVENT_SESSION_DRAINED)
            CloseEventCapture();
    }

    Snapshot Capture() {
        VnSnapshot value{};
        value.structSize = sizeof(value);
        Snapshot result;
        result.result = Vn_CaptureSnapshot(&value);
        if (result.result != VN_OK)
            return result;
        result.blockIndex = value.blockIndex;
        result.activeVoices = value.activeVoiceCount;
        result.busCount = value.activeBusCount;
        result.callbackCpuUs = value.audioCpuLastUs;
        result.updateCpuUs = value.updateCpuLastUs;
        result.commandBackpressure = value.backpressureCount;
        result.droppedNotifications =
            value.notificationDropCount + value.logicNotificationDropCount;
        for (std::size_t i = 0; i < VN_SNAPSHOT_MAX_VOICES; ++i) {
            if (value.voicePublicHandles[i] == 0)
                continue;
            result.voices.push_back({value.voicePublicHandles[i], {}, value.voices[i].state,
                                     value.voices[i].tapPostGain});
        }
        for (std::size_t i = 0; i < std::min<std::size_t>(value.busCount, VN_SNAPSHOT_MAX_BUSES);
             ++i) {
            if (value.busGenerations[i] == 0)
                continue;
            result.buses.push_back({value.busIds[i], value.busGenerations[i], value.busPeaks[i]});
        }
        return result;
    }

    Catalog CaptureCatalog(std::uint32_t offset) {
        Catalog result;
        auto *engine = detail::ActiveEngine();
        if (engine == nullptr) {
            result.result = VN_NOT_INITIALIZED;
            return result;
        }
        result.offset = offset;
        const auto *published = engine->packComposition.Published();
        result.generation = published != nullptr ? published->compositeGeneration
                                                 : engine->variables.CurrentWorldGeneration();
        std::size_t bytes = 24;
        bool pageComplete = false;
        auto append = [&](CatalogKind kind, VnObjectId id, std::string_view suppliedName) {
            const std::uint32_t index = result.totalCount++;
            if (index < offset || pageComplete || result.entries.size() >= 256)
                return;
            std::string name =
                suppliedName.empty() ? harken::ObjectIdString(id) : std::string(suppliedName);
            if (bytes + 24 + name.size() > kMaxPayloadBytes) {
                pageComplete = true;
                return;
            }
            bytes += 24 + name.size();
            result.entries.push_back({kind, id, std::move(name)});
        };
        if (published != nullptr && published->hasGameplayCatalog) {
            const auto &catalog = published->gameplayCatalog;
            for (const auto &row : catalog.active) {
                if (row.nameOffset > catalog.namePool.size() ||
                    row.nameLength > catalog.namePool.size() - row.nameOffset)
                    continue;
                append(CatalogKind::Action, row.actionStableId,
                       std::string_view(
                           reinterpret_cast<const char *>(catalog.namePool.data() + row.nameOffset),
                           row.nameLength));
            }
        } else {
            for (std::uint32_t i = 0; i < engine->variables.ActionProgramCount(); ++i) {
                const auto id = engine->variables.ActionProgramAt(i).actionStableId;
                append(CatalogKind::Action, id, {});
            }
        }
        for (std::uint32_t i = 0; i < engine->variables.DefinitionCount(); ++i) {
            const auto &definition = engine->variables.DefinitionAt(i);
            append(CatalogKind::Variable, definition.variableId, {});
        }
        const Snapshot snapshot = Capture();
        for (const auto &bus : snapshot.buses)
            append(CatalogKind::Bus, bus.id, bus.id == VN_BUS_MASTER ? "Master" : "");
        if (offset > result.totalCount)
            result.result = VN_INVALID_ARG;
        return result;
    }

    CommandResult Execute(const Command &command) {
        CommandResult result;
        result.operation = command.operation;
        const auto *engine = detail::ActiveEngine();
        const auto *published = engine->packComposition.Published();
        const auto generation = published != nullptr ? published->compositeGeneration
                                                     : engine->variables.CurrentWorldGeneration();
        if (command.expectedGeneration != 0 && command.expectedGeneration != generation) {
            result.result = VN_STALE_HANDLE;
            return result;
        }
        if (!std::isfinite(command.value) || command.durationMs > 600000) {
            result.result = VN_INVALID_ARG;
            return result;
        }
        const bool postsAction = command.operation == Operation::PostAction ||
                                 command.operation == Operation::PostActionByName;
        if (postsAction && g_remoteFirings.size() >= 1024) {
            result.result = VN_BACKPRESSURE;
            return result;
        }
        if (postsAction && g_remoteFirings.capacity() < 1024)
            g_remoteFirings.reserve(1024);
        switch (command.operation) {
        case Operation::PostAction: {
            VnPostActionDesc desc{};
            desc.structSize = sizeof(desc);
            desc.actionId = command.target;
            desc.emitter = command.secondary;
            result.result = Vn_PostAction(&desc, &result.handle);
            break;
        }
        case Operation::PostActionByName: {
            VnPostActionByNameDesc desc{};
            desc.structSize = sizeof(desc);
            desc.utf8Name = command.name.c_str();
            desc.emitter = command.secondary;
            result.result = Vn_PostActionByName(&desc, &result.handle);
            break;
        }
        case Operation::StopAll:
            result.result = Vn_StopAll(command.durationMs);
            break;
        case Operation::StopVoice:
            result.result = Vn_Stop(command.instanceHandle, command.durationMs);
            break;
        case Operation::StopAction:
            result.result = Vn_StopActionFiring(command.instanceHandle, command.durationMs);
            break;
        case Operation::SetGlobalVariable:
            result.result = Vn_SetValue(command.target, command.value);
            break;
        case Operation::SetBusVolume:
            result.result = Vn_SetBusVolume(command.target, command.value, command.durationMs);
            break;
        case Operation::SetVoiceVolume:
            result.result = Vn_SetVoiceVolume(command.instanceHandle, command.value, command.durationMs);
            break;
        default:
            result.result = VN_MONITOR_UNSUPPORTED;
            break;
        }
        if (postsAction && result.result == VN_OK && result.handle != 0)
            g_remoteFirings.push_back(result.handle);
        return result;
    }

    MonitorStatus StartMonitor(const MonitorRequest &request) {
        MonitorStatus status;
        if (request.streamMask == 0 || (request.streamMask & ~7u) != 0 || request.cadenceHz == 0 ||
            request.cadenceHz > 60 || request.bytesPerSecond < 4096 ||
            request.bytesPerSecond > 1048576) {
            status.result = VN_INVALID_ARG;
            return status;
        }
        if (monitorSession != 0) {
            status.result = VN_IN_USE;
            return status;
        }
        VnMonitorSessionDesc desc{};
        desc.structSize = sizeof(desc);
        desc.requestedStreamKindMask = request.streamMask;
        desc.requestedPermissionMask =
            VN_MONITOR_PERM_OBSERVE_STATE | VN_MONITOR_PERM_OBSERVE_EVENTS |
            VN_MONITOR_PERM_OBSERVE_MEASUREMENTS | VN_MONITOR_PERM_CAPTURE_CONTROL;
        desc.consumerQueueBytes = 1048576;
        status.result = monitor::OpenRemoteSession(
            &desc, desc.requestedPermissionMask, identity.instanceId ^ logicEpoch, &monitorSession);
        if (status.result != VN_OK)
            return status;
        VnMonitorCaptureControlStatus lease{};
        lease.structSize = sizeof(lease);
        status.result = Vn_MonitorAcquireCaptureControl(monitorSession, &lease);
        if (status.result != VN_OK) {
            CloseMonitor();
            return status;
        }
        monitorLease = lease.leaseGeneration;
        VnMonitorCapturePlan plan{};
        plan.structSize = sizeof(plan);
        plan.streamKindMask = request.streamMask;
        plan.cadenceHz = request.cadenceHz;
        plan.bytesPerSecondBudget = request.bytesPerSecond;
        VnMonitorEffectiveCapturePlan accepted{};
        accepted.structSize = sizeof(accepted);
        status.result = Vn_MonitorApplyCapturePlan(monitorSession, monitorLease, &plan, &accepted);
        if (status.result != VN_OK) {
            CloseMonitor();
            return status;
        }
        VnMonitorSubscriptionStatus subscription{};
        subscription.structSize = sizeof(subscription);
        VnMonitorStreamBaseline baselines[8]{};
        subscription.baselines = baselines;
        subscription.baselineCapacity = 8;
        status.result = Vn_MonitorGetSubscriptionStatus(monitorSession, &subscription);
        if (status.result != VN_OK) {
            CloseMonitor();
            return status;
        }
        monitorEpoch = subscription.monitorSessionEpoch;
        status.sessionEpoch = monitorEpoch;
        status.planGeneration = accepted.capturePlanGeneration;
        status.streamMask = accepted.accepted.streamKindMask;
        periodMs = std::max(1u, 1000u / request.cadenceHz);
        lastPush = 0;
        monitorBuffer.resize(kMaxPayloadBytes - 16);
        if ((request.streamMask & VN_MONITOR_STREAM_STATE) != 0) {
            VnMonitorStateCutDesc cut{};
            cut.structSize = sizeof(cut);
            std::uint64_t cutId = 0;
            Vn_MonitorRequestStateCut(monitorSession, &cut, &cutId);
        }
        return status;
    }

    void Error(std::uint32_t requestId, VnResult result, std::uint64_t expectedEpoch) {
        Writer writer;
        writer.I32(result);
        Queue({Message::Error, requestId, std::move(writer.bytes)}, expectedEpoch);
    }

    void PumpLogic() {
        std::uint64_t currentEpoch = 0, capabilities = 0;
        std::uint32_t allowedObservations = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            currentEpoch = epoch;
            capabilities = negotiatedCapabilities;
            allowedObservations = observationAllowedMask;
        }
        if (logicEpoch != currentEpoch) {
            CloseMonitor();
            CloseEventCapture();
            logicEpoch = currentEpoch;
            eventV2 = false;
            eventAllowedMask = allowedObservations;
        }
        if (currentEpoch == 0)
            return;
        if (auto hook = std::exchange(g_beforeCommandDispatchForTests, nullptr))
            hook(g_beforeCommandDispatchContextForTests);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
        for (std::uint32_t i = 0; i < config.maxCommandsPerUpdate; ++i) {
            if (i != 0 && std::chrono::steady_clock::now() >= deadline)
                break;
            Request request;
            {
                std::lock_guard<std::mutex> lock(mutex);
                // A new connection must first refresh its grant on the logic
                // thread. Leave its requests queued for that next update.
                if (epoch != currentEpoch) break;
                if (inbound.empty())
                    break;
                request = std::move(inbound.front());
                inbound.pop_front();
                inboundBytes -= kHeaderBytes + request.frame.payload.size();
                if (request.epoch != epoch)
                    continue;
            }
            const auto &frame = request.frame;
            if (frame.type == Message::SnapshotRequest && frame.payload.empty() &&
                (capabilities & kCapabilitySnapshot) != 0)
                Queue(MakeFrame(Message::Snapshot, frame.requestId, Capture()), request.epoch);
            else if (frame.type == Message::CatalogRequest &&
                     (capabilities & kCapabilityCatalog) != 0) {
                std::uint32_t offset = 0;
                Reader reader(frame.payload);
                if (!frame.payload.empty() && (!reader.U32(offset) || !reader.Done()))
                    Error(frame.requestId, VN_INVALID_ARG, request.epoch);
                else
                    Queue(MakeFrame(Message::Catalog, frame.requestId, CaptureCatalog(offset)),
                          request.epoch);
            } else if (frame.type == Message::Command && (capabilities & kCapabilityControl) != 0) {
                Command command;
                if (!DecodePayload(frame, command))
                    Error(frame.requestId, VN_INVALID_ARG, request.epoch);
                else {
                    const auto result = Execute(command);
                    Queue(MakeFrame(Message::CommandResult, frame.requestId, result),
                          request.epoch);
                    std::lock_guard<std::mutex> lock(mutex);
                    ++status.receivedCommands;
                }
            } else if (frame.type == Message::EventStart && frame.payload.empty() &&
                       (capabilities & kCapabilityEventCapture) != 0) {
                QueueEventStatus(StartEventCapture(), frame.requestId, request.epoch, false);
            } else if (frame.type == Message::EventStartV2) {
                EventCaptureRequestV2 value;
                EventCaptureStatusV2 result;
                if ((capabilities & kCapabilityExecutionObservation) == 0)
                    result.base.result = VN_MONITOR_UNSUPPORTED;
                else if (!DecodePayload(frame, value)) result.base.result = VN_INVALID_ARG;
                else result = StartEventCapture(&value);
                QueueEventStatus(result, frame.requestId, request.epoch, true);
            } else if (frame.type == Message::EventSeal &&
                       (capabilities & (kCapabilityEventCapture | kCapabilityExecutionObservation)) != 0) {
                Reader reader(frame.payload);
                std::uint64_t requestedSession = 0;
                auto result = EventStatus();
                const bool replyV2 = eventV2;
                if (!reader.U64(requestedSession) || !reader.Done()) result.base.result = VN_INVALID_ARG;
                else if (result.base.result == VN_OK && requestedSession != result.base.sessionEpoch)
                    result.base.result = VN_MONITOR_STALE_SESSION;
                else if (result.base.result == VN_OK) {
                    result.base.result = Vn_MonitorSealEventSession(eventSession);
                    if (result.base.result == VN_OK) result = EventStatus();
                }
                if (result.base.result == VN_OK && result.base.state == VN_MONITOR_EVENT_SESSION_DRAINED)
                    CloseEventCapture();
                QueueEventStatus(result, frame.requestId, request.epoch, replyV2);
            } else if (frame.type == Message::MonitorStart &&
                       (capabilities & kCapabilityMonitorRecords) != 0) {
                MonitorRequest requestValue;
                if (!DecodePayload(frame, requestValue))
                    Error(frame.requestId, VN_INVALID_ARG, request.epoch);
                else
                    Queue(MakeFrame(Message::MonitorStatus, frame.requestId,
                                    StartMonitor(requestValue)),
                          request.epoch);
            } else if (frame.type == Message::MonitorStop && frame.payload.empty()) {
                CloseMonitor();
                Queue(MakeFrame(Message::MonitorStatus, frame.requestId, MonitorStatus{}),
                      request.epoch);
            } else
                Error(frame.requestId, VN_MONITOR_UNSUPPORTED, request.epoch);
        }
        PumpEvents(currentEpoch);
        if (monitorSession != 0 && NowMs() - lastPush >= periodMs) {
            lastPush = NowMs();
            Queue(MakeFrame(Message::Snapshot, 0, Capture()), currentEpoch, true);
            VnMonitorPollDesc poll{};
            poll.structSize = sizeof(poll);
            poll.buffer = monitorBuffer.data();
            poll.bufferBytes = monitorBuffer.size();
            VnMonitorPollResult result{};
            result.structSize = sizeof(result);
            const VnResult polled = Vn_MonitorPoll(monitorSession, &poll, &result);
            if (polled != VN_OK) {
                MonitorStatus failed;
                failed.result = polled;
                CloseMonitor();
                Queue(MakeFrame(Message::MonitorStatus, 0, failed), currentEpoch);
            } else if (result.bytesWritten != 0) {
                MonitorRecords records;
                records.sessionEpoch = monitorEpoch;
                records.recordCount = result.recordsWritten;
                records.bytes.assign(monitorBuffer.begin(),
                                     monitorBuffer.begin() + result.bytesWritten);
                std::size_t offset = 0;
                while (offset + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES <= records.bytes.size()) {
                    Reader record(records.bytes.data() + offset + 16,
                                  records.bytes.size() - offset - 16);
                    std::uint64_t length = 0;
                    if (!record.U64(length) || length < VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES ||
                        length > records.bytes.size() - offset)
                        break;
                    std::fill_n(records.bytes.begin() + offset + 48, 8, 0);
                    offset += (length + 7u) & ~7ull;
                }
                if (!Queue(MakeFrame(Message::MonitorRecords, 0, records), currentEpoch, true)) {
                    MonitorStatus failed;
                    failed.result = VN_BACKPRESSURE;
                    CloseMonitor();
                    Queue(MakeFrame(Message::MonitorStatus, 0, failed), currentEpoch);
                }
            }
        }
    }
};

std::unique_ptr<Server> g_server;
VnRemoteStatus g_lastStatus{};

VnResult StartRuntime(const VnRemoteConfig *input) {
    if (!monitor::LogicThreadReady())
        return detail::ActiveEngine() == nullptr ? VN_NOT_INITIALIZED : VN_MONITOR_WRONG_THREAD;
    if (input == nullptr || input->structSize != sizeof(*input) || input->reserved0 != 0 ||
        input->tcpPort > 65535 || input->discoveryPort > 65535 ||
        (input->flags & ~VN_REMOTE_DISCOVERY_ENABLED) != 0 || !ConfigText(input->bindAddress, 64) ||
        !ConfigText(input->applicationName, 128) || !ConfigText(input->projectName, 128))
        return VN_INVALID_ARG;
    if (input->bindAddress != nullptr && input->bindAddress[0] != 0 &&
        std::string_view(input->bindAddress) != "127.0.0.1")
        return VN_INVALID_ARG;
    if (g_server)
        return VN_IN_USE;
    auto server = std::make_unique<Server>();
    server->config = *input;
    auto &config = server->config;
    if (config.discoveryPort == 0)
        config.discoveryPort = kDiscoveryPort;
    if (config.queueBytes == 0)
        config.queueBytes = 1048576;
    if (config.maxCommandsPerUpdate == 0)
        config.maxCommandsPerUpdate = 32;
    if (config.requestTimeoutMs == 0)
        config.requestTimeoutMs = 30000;
    if (config.queueBytes < kMaxPayloadBytes + kHeaderBytes || config.queueBytes > 16777216 ||
        config.maxCommandsPerUpdate > 256 || config.requestTimeoutMs < 100 ||
        config.requestTimeoutMs > 300000)
        return VN_INVALID_ARG;
    // server->bindAddress = input->bindAddress == nullptr || input->bindAddress[0] == 0
    //                           ? "0.0.0.0"
    //                           : input->bindAddress;
    server->bindAddress = "127.0.0.1";
    auto &identity = server->identity;
    identity.bindAddress = server->bindAddress;
    identity.instanceId = NewInstanceId();
    identity.processId = ProcessId();
    identity.applicationName = input->applicationName == nullptr || input->applicationName[0] == 0
                                   ? "Harken Application"
                                   : input->applicationName;
    identity.projectName = input->projectName == nullptr ? "" : input->projectName;
    identity.engineVersion = Harken::BuildInfo::Version;
    identity.buildId = Harken::BuildInfo::BuildId;
    identity.platform = Harken::BuildInfo::TargetPlatform;
    identity.sampleRate = detail::ActiveEngine()->config.mixRate.SampleRate();
    identity.blockFrames = detail::ActiveEngine()->config.blockFrames;
    auto &status = server->status;
    status.structSize = sizeof(status);
    status.protocolMajor = kProtocolMajor;
    status.protocolMinor = kProtocolMinor;
    status.instanceId = identity.instanceId;
    status.processId = identity.processId;
    status.capabilities = kCapabilities;
    CopyText(status.bindAddress, server->bindAddress);
    CopyText(status.applicationName, identity.applicationName);
    CopyText(status.projectName, identity.projectName);
    CopyText(status.engineVersion, identity.engineVersion);
    CopyText(status.buildId, identity.buildId);
    CopyText(status.platform, identity.platform);
    if (!server->Start()) {
        status.state = VN_REMOTE_FAILED;
        status.lastResult = VN_DEVICE_ERROR;
        status.tcpPort = 0;
        status.discoveryPort = 0;
        g_lastStatus = status;
        return VN_DEVICE_ERROR;
    }
    g_server = std::move(server);
    return VN_OK;
}

}

void Pump() {
    if (!g_remoteFirings.empty())
        ReapFirings();
    if (g_server) {
        try {
            g_server->PumpLogic();
        } catch (...) {
            std::lock_guard<std::mutex> lock(g_server->mutex);
            g_server->closeRequested = true;
            g_server->status.lastResult = VN_DEVICE_ERROR;
        }
    }
}

void SetBeforeCommandDispatchForTests(void (*hook)(void*), void* context) {
    g_beforeCommandDispatchForTests = hook;
    g_beforeCommandDispatchContextForTests = context;
}

unsigned PendingCommandsForTests() {
    if (!g_server) return 0;
    std::lock_guard<std::mutex> lock(g_server->mutex);
    return static_cast<unsigned>(g_server->inbound.size());
}

void Shutdown(bool engineTerminating) {
    g_beforeCommandDispatchForTests = nullptr;
    g_beforeCommandDispatchContextForTests = nullptr;
    if (g_server) {
        g_server->Stop();
        g_server->CloseMonitor();
        g_server->CloseEventCapture();
        g_lastStatus = g_server->status;
        g_server.reset();
    }
    g_lastStatus.structSize = sizeof(g_lastStatus);
    g_lastStatus.state = VN_REMOTE_STOPPED;
    g_lastStatus.clientCount = 0;
    g_lastStatus.sessionId = 0;
    g_lastStatus.tcpPort = 0;
    g_lastStatus.discoveryPort = 0;
    g_lastStatus.lastResult = VN_OK;
    if (engineTerminating)
        g_remoteFirings.clear();
}

}

extern "C" {

VnResult Vn_RemoteStart(const VnRemoteConfig *config) {
    VnResult result = VN_DEVICE_ERROR;
    try {
        result = harken::remote::StartRuntime(config);
    } catch (...) {
    }
    if (result != VN_OK && !harken::remote::g_server && result != VN_MONITOR_WRONG_THREAD) {
        harken::remote::g_lastStatus.structSize = sizeof(VnRemoteStatus);
        harken::remote::g_lastStatus.state = VN_REMOTE_FAILED;
        harken::remote::g_lastStatus.lastResult = result;
    }
    return result;
}

VnResult Vn_RemoteStop(void) {
    try {
        if (harken::detail::ActiveEngine() != nullptr && !harken::monitor::LogicThreadReady())
            return VN_MONITOR_WRONG_THREAD;
        harken::remote::Shutdown();
        return VN_OK;
    } catch (...) {
        return VN_DEVICE_ERROR;
    }
}

VnResult Vn_RemoteGetStatus(VnRemoteStatus *outStatus) {
    try {
        if (outStatus == nullptr || outStatus->structSize != sizeof(*outStatus))
            return VN_INVALID_ARG;
        if (harken::detail::ActiveEngine() != nullptr && !harken::monitor::LogicThreadReady())
            return VN_MONITOR_WRONG_THREAD;
        if (harken::remote::g_server) {
            std::lock_guard<std::mutex> lock(harken::remote::g_server->mutex);
            *outStatus = harken::remote::g_server->status;
        } else {
            *outStatus = harken::remote::g_lastStatus;
            outStatus->structSize = sizeof(*outStatus);
        }
        return VN_OK;
    } catch (...) {
        return VN_DEVICE_ERROR;
    }
}
}
