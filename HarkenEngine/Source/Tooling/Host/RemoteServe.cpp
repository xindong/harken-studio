// remote-serve: initialize the engine, start the remote-control channel
// server on a named pipe, and pump the engine while external clients
// drive it. The host owns the engine lifecycle and the pump cadence; the
// wire mirror executes between Vn_Update pumps on this (logic) thread.
//
// The run is wall-clock paced at a fixed control tick in both device
// modes: in null mode the host is the clock and pumps the block count the
// elapsed wall time implies, so subscription cadence and notification
// timing behave like a live session without audio hardware (which is what
// the acceptance driver and CI exercise).
//
// Exit codes: 0 = served and shut down cleanly (duration elapsed);
// 1 = usage error; 3 = engine init or pipe failure.
#include "RemoteServe.h"

#include <harken/Harken.h>

#include "DemoOptions.h"
#include "Engine.h" // internal: null-mode block pump (test-clock precedent)
#include "harken_remote/RemoteServer.h"
#include <harken_buildinfo.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace harkenhost {

namespace {

struct RemoteServeOptions {
    std::string pipeName;       // empty = default \\.\pipe\HarkenRemote.<pid>
    double seconds = 0.0;       // 0 = serve until the process is terminated
    bool nullMode = false;
    std::uint32_t sampleRate = 0;  // 0 = engine default
    std::uint32_t blockFrames = 0; // 0 = engine default
    std::uint32_t readTimeoutMs = harkenremote::kDefaultReadTimeoutMs;
    std::uint32_t inboundFrames = 64;
    std::uint32_t reliableFrames = 256;
    std::uint32_t advisoryFrames = 64;
    std::uint32_t closeLingerMs = 100;
    std::string tracePath;      // empty = no session trace
    bool parkEnabled = false;
    float parkThresholdDb = 0.0f;  // 0 = engine default (-60 dB)
    std::uint32_t maxParkedVoices = 0; // 0 = engine default
};

constexpr double kServeTickSeconds = 0.010;

bool ParseU32(const char* text, std::uint32_t* out) {
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value > 0xFFFFFFFFul) {
        return false;
    }
    *out = static_cast<std::uint32_t>(value);
    return true;
}

bool ParseRemoteServeOptions(int argc, char** argv, RemoteServeOptions* out) {
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--pipe" && i + 1 < argc) {
            out->pipeName = argv[++i];
            // A bare name is convenience; the transport needs the full
            // local pipe namespace path.
            if (out->pipeName.rfind("\\\\.\\pipe\\", 0) != 0) {
                out->pipeName = std::string("\\\\.\\pipe\\") + out->pipeName;
            }
        } else if (arg == "--seconds" && i + 1 < argc) {
            if (!ParseSeconds(argv[++i], &out->seconds)) {
                std::fprintf(stderr,
                             "error: --seconds must be a finite number in "
                             "(0, %.0f]\n",
                             kMaxDemoSeconds);
                return false;
            }
        } else if (arg == "--null") {
            out->nullMode = true;
        } else if (arg == "--sample-rate" && i + 1 < argc) {
            if (!ParseU32(argv[++i], &out->sampleRate)) {
                return false;
            }
        } else if (arg == "--block-frames" && i + 1 < argc) {
            if (!ParseU32(argv[++i], &out->blockFrames)) {
                return false;
            }
        } else if (arg == "--read-timeout-ms" && i + 1 < argc) {
            if (!ParseU32(argv[++i], &out->readTimeoutMs) ||
                out->readTimeoutMs == 0) {
                return false;
            }
        } else if (arg == "--inbound-frames" && i + 1 < argc) {
            if (!ParseU32(argv[++i], &out->inboundFrames) ||
                out->inboundFrames == 0) {
                return false;
            }
        } else if (arg == "--reliable-frames" && i + 1 < argc) {
            if (!ParseU32(argv[++i], &out->reliableFrames) ||
                out->reliableFrames == 0) {
                return false;
            }
        } else if (arg == "--advisory-frames" && i + 1 < argc) {
            if (!ParseU32(argv[++i], &out->advisoryFrames) ||
                out->advisoryFrames == 0) {
                return false;
            }
        } else if (arg == "--close-linger-ms" && i + 1 < argc) {
            if (!ParseU32(argv[++i], &out->closeLingerMs)) {
                return false;
            }
        } else if (arg == "--trace" && i + 1 < argc) {
            out->tracePath = argv[++i];
        } else if (arg == "--park") {
            out->parkEnabled = true;
        } else if (arg == "--park-threshold-db" && i + 1 < argc) {
            char* end = nullptr;
            out->parkThresholdDb =
                static_cast<float>(std::strtod(argv[++i], &end));
            if (end == argv[i] || *end != '\0') {
                return false;
            }
            out->parkEnabled = true;
        } else if (arg == "--max-parked" && i + 1 < argc) {
            if (!ParseU32(argv[++i], &out->maxParkedVoices)) {
                return false;
            }
        } else {
            std::fprintf(stderr, "error: unknown argument '%s'\n", arg.c_str());
            return false;
        }
    }
    return true;
}

} // namespace

void PrintRemoteServeUsage() {
    std::fprintf(
        stderr,
        "\nremote-serve hosts the engine behind the remote-control channel:\n"
        "a local named pipe (default \\\\.\\pipe\\HarkenRemote.<pid>) carrying\n"
        "the versioned command mirror plus snapshot/notification streams.\n"
        "  --pipe <name>          pipe name or full \\\\.\\pipe\\ path\n"
        "  --seconds N            serve duration; absent = until terminated\n"
        "  --null                 headless: null device, host-clocked blocks\n"
        "  --sample-rate N        engine sample rate (0 = default)\n"
        "  --block-frames N       engine block frames (0 = default)\n"
        "  --read-timeout-ms N    half-frame stall window (default 10000)\n"
        "  --inbound-frames N     request queue bound (default 64)\n"
        "  --reliable-frames N    reply queue bound (default 256)\n"
        "  --advisory-frames N    snapshot/notification queue bound (64)\n"
        "  --close-linger-ms N    grace before server-initiated disconnect\n"
        "  --trace <path>         journal the session (Vn_TraceStart before\n"
        "                         serving; stopped at clean shutdown) for\n"
        "                         deterministic trace-replay\n"
        "  --park                 enable audibility parking (engine defaults)\n"
        "  --park-threshold-db N  park below this level (implies --park)\n"
        "  --max-parked N         parked-voice pool capacity (0 = default)\n");
}

int RunRemoteServe(int argc, char** argv) {
    RemoteServeOptions options;
    if (!ParseRemoteServeOptions(argc, argv, &options)) {
        PrintRemoteServeUsage();
        return 1;
    }

    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.sampleRate = options.sampleRate;
    config.blockFrames = options.blockFrames;
    config.deviceMode = options.nullMode ? VN_DEVICE_NULL : VN_DEVICE_REAL;
    config.audibilityParkEnabled = options.parkEnabled ? 1u : 0u;
    config.parkThresholdDb = options.parkThresholdDb;
    config.maxParkedVoices = options.maxParkedVoices;

    harkenremote::RemoteServerConfig serverConfig;
    serverConfig.pipeName = options.pipeName;
    serverConfig.version = std::string(Harken::BuildInfo::Version);
    serverConfig.sampleRate = options.sampleRate == 0 ? 48000 : options.sampleRate;
    serverConfig.blockFrames = options.blockFrames == 0 ? 256 : options.blockFrames;
    serverConfig.configStructSize = sizeof(VnConfig);
    serverConfig.readTimeoutMs = options.readTimeoutMs;
    serverConfig.inboundQueueFrames = options.inboundFrames;
    serverConfig.reliableQueueFrames = options.reliableFrames;
    serverConfig.advisoryQueueFrames = options.advisoryFrames;
    serverConfig.closeLingerMs = options.closeLingerMs;

    harkenremote::RemoteServer server(serverConfig);
    config.onNotification = &harkenremote::RemoteServer::NotificationBridge;
    config.notificationUserData = &server;

    const VnResult initResult = Vn_Init(&config);
    if (initResult != VN_OK) {
        std::fprintf(stderr, "error: Vn_Init failed (%d)\n", initResult);
        return 3;
    }
    if (!options.tracePath.empty()) {
        // Started before any stateful call, so the start gate is satisfied
        // by construction: the journal begins at the engine's pristine
        // post-init origin and every client command lands inside it.
        const VnResult traceResult = Vn_TraceStart(options.tracePath.c_str());
        if (traceResult != VN_OK) {
            std::fprintf(stderr, "error: Vn_TraceStart('%s') failed (%d)\n",
                         options.tracePath.c_str(), traceResult);
            Vn_Term();
            return 3;
        }
    }
    if (!server.Start()) {
        std::fprintf(stderr, "error: cannot create pipe %s\n",
                     server.PipeName().c_str());
        Vn_Term();
        return 3;
    }

    // The ready line is the driver/test synchronization point: clients
    // connect only after it appears.
    std::printf("remote-serve: listening on %s\n", server.PipeName().c_str());
    std::fflush(stdout);

    harken::Engine* engine = harken::detail::ActiveEngine();
    const double engineRate =
        static_cast<double>(engine->config.mixRate.SampleRate());
    const double engineBlock = static_cast<double>(engine->config.blockFrames);

    const auto start = std::chrono::steady_clock::now();
    auto nextTick = start;
    double pendingFrames = 0.0;
    for (;;) {
        nextTick += std::chrono::milliseconds(
            static_cast<int>(kServeTickSeconds * 1000.0));
        std::this_thread::sleep_until(nextTick);

        if (options.nullMode) {
            // The host is the clock: render the block count this tick's
            // wall time implies, carrying the fractional remainder, and
            // give streams a deterministic service pass per block.
            pendingFrames += kServeTickSeconds * engineRate;
            while (pendingFrames >= engineBlock) {
                engine->nullBackend.Pump(1);
                engine->streams.SyncServe();
                pendingFrames -= engineBlock;
            }
        }

        Vn_Update();
        server.PumpLogic();

        if (options.seconds > 0.0) {
            const std::chrono::duration<double> elapsed =
                std::chrono::steady_clock::now() - start;
            if (elapsed.count() >= options.seconds) {
                break;
            }
        }
    }

    server.Stop();
    if (!options.tracePath.empty()) {
        // A clean shutdown seals the journal with its end marker; a killed
        // process leaves a truncated journal, which trace-replay reports.
        (void)Vn_TraceStop();
    }
    Vn_Term();

    const harkenremote::RemoteServerStats stats = server.Stats();
    std::printf(
        "remote-serve: done — sessions %llu, commands %llu, replies %llu, "
        "snapshots %llu, notifications %llu, advisory drops %llu, "
        "server closes %llu\n",
        static_cast<unsigned long long>(stats.sessionsAccepted),
        static_cast<unsigned long long>(stats.commandsExecuted),
        static_cast<unsigned long long>(stats.repliesQueued),
        static_cast<unsigned long long>(stats.snapshotsPushed),
        static_cast<unsigned long long>(stats.notificationsForwarded),
        static_cast<unsigned long long>(stats.advisoryDropCount),
        static_cast<unsigned long long>(stats.sessionsClosedByServer));
    return 0;
}

} // namespace harkenhost
