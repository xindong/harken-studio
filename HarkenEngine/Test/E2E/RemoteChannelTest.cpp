#include "Common/ObjectIdTestSupport.h"
// Remote channel end to end: harken_host remote-serve runs as a real
// subprocess (the packc-subprocess precedent) serving a null-backend
// engine on a uniquely named local pipe, and these cases drive it as a
// real client — handshake and version gate, malformed-frame answers,
// framing violations that close the session, disconnect containment,
// snapshot subscription cadence, advisory-versus-reliable delivery under
// pressure, and the stop-reading backpressure disconnect. Every wait is
// deadline-bounded so a wedged server fails the test instead of hanging
// CI; no audio hardware is touched.
#include "E2E/PipeClient.h"

#include <harken/Harken.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <string>

#ifndef HARKEN_HOST_PATH
#error "HARKEN_HOST_PATH must point at the harken_host executable"
#endif

namespace {

using namespace harkenremote;
using harkentest::HostProcess;
using harkentest::PipeClient;

std::atomic<int> g_pipeSerial{0};

// Each test gets its own pipe name: parallel test processes and retried
// runs must never collide on the single-instance pipe.
std::string UniquePipeName() {
    return "\\\\.\\pipe\\HarkenRemoteTest." +
           std::to_string(GetCurrentProcessId()) + "." +
           std::to_string(++g_pipeSerial);
}

std::string ServeCommand(const std::string& pipeName,
                         const std::string& extraFlags = std::string()) {
    std::string cmd = std::string("\"") + HARKEN_HOST_PATH +
                      "\" remote-serve --null --seconds 120 --pipe " +
                      pipeName;
    if (!extraFlags.empty()) {
        cmd += " " + extraFlags;
    }
    return cmd;
}

WireFrame HelloFrame(std::uint16_t major, std::uint16_t minor,
                     std::uint32_t sequence = 1) {
    WireFrame frame;
    frame.msgType = kMsgHello;
    frame.sequence = sequence;
    WireWriter writer(&frame.payload);
    writer.U16(major);
    writer.U16(minor);
    return frame;
}

WireFrame CommandFrame(std::uint16_t opcode, std::uint32_t sequence) {
    WireFrame frame;
    frame.msgType = kMsgCommand;
    frame.sequence = sequence;
    WireWriter writer(&frame.payload);
    writer.U16(opcode);
    writer.U16(0);
    return frame;
}

struct WelcomeFacts {
    std::uint16_t major = 0;
    std::uint16_t minor = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t blockFrames = 0;
    std::uint32_t configStructSize = 0;
    std::string version;
};

bool ParseWelcome(const WireFrame& frame, WelcomeFacts* out) {
    if (frame.msgType != kMsgWelcome) {
        return false;
    }
    WireReader reader(frame.payload.data(), frame.payload.size());
    std::uint32_t versionLength = 0;
    if (!reader.ReadU16(&out->major) || !reader.ReadU16(&out->minor) ||
        !reader.ReadU32(&out->sampleRate) ||
        !reader.ReadU32(&out->blockFrames) ||
        !reader.ReadU32(&out->configStructSize) ||
        !reader.ReadU32(&versionLength)) {
        return false;
    }
    if (versionLength == kWireNullLength || versionLength > 4096) {
        return false;
    }
    const std::uint8_t* bytes = reader.ReadBytes(versionLength);
    if (bytes == nullptr) {
        return false;
    }
    out->version.assign(reinterpret_cast<const char*>(bytes), versionLength);
    return true;
}

// Connects and completes a protocol-1.0 handshake, asserting on failure.
void ConnectAndHandshake(PipeClient* client, const std::string& pipeName,
                         WelcomeFacts* facts = nullptr) {
    ASSERT_TRUE(client->Connect(pipeName, 15000)) << "server never listened";
    ASSERT_TRUE(client->WriteFrame(HelloFrame(kProtocolMajor, kProtocolMinor)));
    WireFrame welcome;
    ASSERT_TRUE(client->ReadFrame(&welcome, 5000));
    WelcomeFacts parsed;
    ASSERT_TRUE(ParseWelcome(welcome, &parsed));
    if (facts != nullptr) {
        *facts = parsed;
    }
}

// Reads frames until a Reply/Error with the wanted sequence arrives,
// skipping advisory frames (snapshots, notifications, status). Returns
// false when the deadline passes or the pipe closes first.
bool ReadReplyFor(PipeClient* client, std::uint32_t sequence, WireFrame* out,
                  int timeoutMs = 5000) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            return false;
        }
        WireFrame frame;
        if (!client->ReadFrame(&frame, static_cast<int>(deadline - now))) {
            return false;
        }
        if ((frame.flags & kFlagAdvisory) != 0) {
            continue;
        }
        if (frame.sequence == sequence &&
            (frame.msgType == kMsgReply || frame.msgType == kMsgError)) {
            *out = frame;
            return true;
        }
    }
}

std::int32_t ReplyResult(const WireFrame& reply) {
    WireReader reader(reply.payload.data(), reply.payload.size());
    std::uint16_t opcode = 0;
    std::uint16_t reserved = 0;
    std::int32_t result = 0;
    EXPECT_TRUE(reader.ReadU16(&opcode));
    EXPECT_TRUE(reader.ReadU16(&reserved));
    EXPECT_TRUE(reader.ReadI32(&result));
    return result;
}

std::uint16_t ErrorCode(const WireFrame& error) {
    WireReader reader(error.payload.data(), error.payload.size());
    std::uint16_t code = 0;
    EXPECT_TRUE(reader.ReadU16(&code));
    return code;
}

TEST(RemoteChannelE2E, HandshakeReportsRuntimeFactsAndCommandsRoundTrip) {
    const std::string pipeName = UniquePipeName();
    HostProcess host;
    ASSERT_TRUE(host.Start(ServeCommand(pipeName)));

    PipeClient client;
    WelcomeFacts facts;
    ASSERT_NO_FATAL_FAILURE(ConnectAndHandshake(&client, pipeName, &facts));
    EXPECT_EQ(kProtocolMajor, facts.major);
    EXPECT_EQ(kProtocolMinor, facts.minor);
    EXPECT_EQ(48000u, facts.sampleRate);
    EXPECT_EQ(256u, facts.blockFrames);
    EXPECT_EQ(sizeof(VnConfig), facts.configStructSize);
    EXPECT_FALSE(facts.version.empty());

    WireFrame create = CommandFrame(kOpVn_CreateBus, 10);
    WireWriter writer(&create.payload);
    writer.ObjectId(harkentest::ObjectId(7));
    writer.ObjectId(VN_BUS_MASTER);
    ASSERT_TRUE(client.WriteFrame(create));
    WireFrame reply;
    ASSERT_TRUE(ReadReplyFor(&client, 10, &reply));
    ASSERT_EQ(kMsgReply, reply.msgType);
    EXPECT_EQ(VN_OK, ReplyResult(reply));
}

TEST(RemoteChannelE2E, MajorMismatchIsRejectedThenNextClientServed) {
    const std::string pipeName = UniquePipeName();
    HostProcess host;
    ASSERT_TRUE(host.Start(ServeCommand(pipeName)));

    PipeClient wrong;
    ASSERT_TRUE(wrong.Connect(pipeName, 15000));
    ASSERT_TRUE(wrong.WriteFrame(HelloFrame(kProtocolMajor + 1, 0)));
    WireFrame reject;
    ASSERT_TRUE(wrong.ReadFrame(&reject, 5000));
    ASSERT_EQ(kMsgHelloReject, reject.msgType);
    {
        WireReader reader(reject.payload.data(), reject.payload.size());
        std::uint16_t reason = 0;
        std::uint16_t serverMajor = 0;
        ASSERT_TRUE(reader.ReadU16(&reason));
        ASSERT_TRUE(reader.ReadU16(&serverMajor));
        EXPECT_EQ(kHelloRejectMajorMismatch, reason);
        EXPECT_EQ(kProtocolMajor, serverMajor);
    }
    EXPECT_TRUE(wrong.WaitForClose(5000)); // refusal reply, then close
    wrong.Close();

    // The engine survived the refusal and the server accepts the next
    // client at the correct major.
    PipeClient right;
    ASSERT_NO_FATAL_FAILURE(ConnectAndHandshake(&right, pipeName));
}

TEST(RemoteChannelE2E, MalformedFramesAnswerInOrderWithoutKillingSession) {
    const std::string pipeName = UniquePipeName();
    HostProcess host;
    ASSERT_TRUE(host.Start(ServeCommand(pipeName)));

    PipeClient client;
    ASSERT_NO_FATAL_FAILURE(ConnectAndHandshake(&client, pipeName));

    // Unknown message type: answered with a wire error, session lives.
    WireFrame weird;
    weird.msgType = 0x777;
    weird.sequence = 20;
    ASSERT_TRUE(client.WriteFrame(weird));
    WireFrame answer;
    ASSERT_TRUE(ReadReplyFor(&client, 20, &answer));
    ASSERT_EQ(kMsgError, answer.msgType);
    EXPECT_EQ(kWireErrUnknownMsgType, ErrorCode(answer));

    // Unknown opcode inside a well-framed command.
    ASSERT_TRUE(client.WriteFrame(CommandFrame(999, 21)));
    ASSERT_TRUE(ReadReplyFor(&client, 21, &answer));
    ASSERT_EQ(kMsgError, answer.msgType);
    EXPECT_EQ(kWireErrUnknownOpcode, ErrorCode(answer));

    // Short payload for a known opcode.
    WireFrame shortStop = CommandFrame(kOpVn_Stop, 22);
    WireWriter writer(&shortStop.payload);
    writer.U32(7); // Vn_Stop wants u64 + u32
    ASSERT_TRUE(client.WriteFrame(shortStop));
    ASSERT_TRUE(ReadReplyFor(&client, 22, &answer));
    ASSERT_EQ(kMsgError, answer.msgType);
    EXPECT_EQ(kWireErrMalformedPayload, ErrorCode(answer));

    // The session still executes real commands afterwards.
    WireFrame create = CommandFrame(kOpVn_CreateBus, 23);
    WireWriter createWriter(&create.payload);
    createWriter.ObjectId(harkentest::ObjectId(9));
    createWriter.ObjectId(VN_BUS_MASTER);
    ASSERT_TRUE(client.WriteFrame(create));
    ASSERT_TRUE(ReadReplyFor(&client, 23, &answer));
    ASSERT_EQ(kMsgReply, answer.msgType);
    EXPECT_EQ(VN_OK, ReplyResult(answer));
}

TEST(RemoteChannelE2E, OversizedLengthPrefixClosesSessionEngineSurvives) {
    const std::string pipeName = UniquePipeName();
    HostProcess host;
    ASSERT_TRUE(host.Start(ServeCommand(pipeName)));

    PipeClient client;
    ASSERT_NO_FATAL_FAILURE(ConnectAndHandshake(&client, pipeName));

    // A length prefix beyond maxFrameBytes: the framing cannot be
    // trusted past it, so the session closes without a reply.
    std::uint8_t header[kFrameHeaderBytes] = {};
    const std::uint32_t huge = kMaxFrameBytes + 1;
    std::memcpy(header, &huge, sizeof(huge));
    const std::uint16_t msgType = kMsgCommand;
    std::memcpy(header + 4, &msgType, sizeof(msgType));
    ASSERT_TRUE(client.WriteBytes(header, sizeof(header), 5000));
    EXPECT_TRUE(client.WaitForClose(5000));
    client.Close();

    PipeClient next;
    ASSERT_NO_FATAL_FAILURE(ConnectAndHandshake(&next, pipeName));
}

TEST(RemoteChannelE2E, HalfFrameStallPastReadTimeoutClosesSession) {
    const std::string pipeName = UniquePipeName();
    HostProcess host;
    ASSERT_TRUE(host.Start(ServeCommand(pipeName, "--read-timeout-ms 300")));

    PipeClient client;
    ASSERT_NO_FATAL_FAILURE(ConnectAndHandshake(&client, pipeName));

    // Six bytes of a twelve-byte header, then silence: the half-frame
    // clock must close the session soon after the configured window.
    const std::uint8_t partial[6] = {8, 0, 0, 0, 4, 0};
    ASSERT_TRUE(client.WriteBytes(partial, sizeof(partial), 5000));
    EXPECT_TRUE(client.WaitForClose(5000));
    client.Close();

    PipeClient next;
    ASSERT_NO_FATAL_FAILURE(ConnectAndHandshake(&next, pipeName));
}

TEST(RemoteChannelE2E, ClientDropMidSessionLeavesServerServingNext) {
    const std::string pipeName = UniquePipeName();
    HostProcess host;
    ASSERT_TRUE(host.Start(ServeCommand(pipeName)));

    {
        PipeClient dying;
        ASSERT_NO_FATAL_FAILURE(ConnectAndHandshake(&dying, pipeName));
        // Commands in flight, replies never read, then an abrupt local
        // close — the pipe-break path a killed client process produces.
        for (std::uint32_t i = 0; i < 8; ++i) {
            WireFrame set = CommandFrame(kOpVn_SetValue, 40 + i);
            WireWriter writer(&set.payload);
            writer.U64(1);
            writer.F32(static_cast<float>(i));
            ASSERT_TRUE(dying.WriteFrame(set));
        }
        dying.Close();
    }

    PipeClient next;
    ASSERT_NO_FATAL_FAILURE(ConnectAndHandshake(&next, pipeName));
    WireFrame create = CommandFrame(kOpVn_CreateBus, 50);
    WireWriter writer(&create.payload);
    writer.ObjectId(harkentest::ObjectId(11));
    writer.ObjectId(VN_BUS_MASTER);
    ASSERT_TRUE(next.WriteFrame(create));
    WireFrame reply;
    ASSERT_TRUE(ReadReplyFor(&next, 50, &reply));
    ASSERT_EQ(kMsgReply, reply.msgType);
    EXPECT_EQ(VN_OK, ReplyResult(reply));
}

TEST(RemoteChannelE2E, SecondConnectRefusedWhileFirstClientServed) {
    const std::string pipeName = UniquePipeName();
    HostProcess host;
    ASSERT_TRUE(host.Start(ServeCommand(pipeName)));

    PipeClient first;
    ASSERT_NO_FATAL_FAILURE(ConnectAndHandshake(&first, pipeName));

    PipeClient second;
    DWORD lastError = 0;
    EXPECT_FALSE(second.Connect(pipeName, 500, /*retryBusy=*/false,
                                &lastError));
    EXPECT_EQ(static_cast<DWORD>(ERROR_PIPE_BUSY), lastError);

    first.Close();
    // With the first client gone the server accepts the next connect.
    ASSERT_NO_FATAL_FAILURE(ConnectAndHandshake(&second, pipeName));
}

TEST(RemoteChannelE2E, SnapshotSubscriptionDeliversAtRequestedCadence) {
    const std::string pipeName = UniquePipeName();
    HostProcess host;
    ASSERT_TRUE(host.Start(ServeCommand(pipeName)));

    PipeClient client;
    ASSERT_NO_FATAL_FAILURE(ConnectAndHandshake(&client, pipeName));

    WireFrame subscribe;
    subscribe.msgType = kMsgSubscribe;
    subscribe.sequence = 60;
    WireWriter writer(&subscribe.payload);
    writer.U32(100); // periodMs
    ASSERT_TRUE(client.WriteFrame(subscribe));

    // The SubscribeAck and the snapshot stream are validated inside one
    // collection loop: the ack is a reliable frame with its own message
    // type, snapshots are advisory frames carrying the raw vintage-
    // stamped VnSnapshot image.
    int snapshots = 0;
    bool sawAck = false;
    const ULONGLONG deadline = GetTickCount64() + 1200;
    while (GetTickCount64() < deadline) {
        WireFrame frame;
        const ULONGLONG now = GetTickCount64();
        if (!client.ReadFrame(&frame,
                              static_cast<int>(deadline > now ? deadline - now
                                                              : 1))) {
            break;
        }
        if (frame.msgType == kMsgSubscribeAck) {
            sawAck = true;
            WireReader reader(frame.payload.data(), frame.payload.size());
            std::uint32_t period = 0;
            ASSERT_TRUE(reader.ReadU32(&period));
            EXPECT_EQ(100u, period);
        } else if (frame.msgType == kMsgSnapshot) {
            EXPECT_NE(0u, frame.flags & kFlagAdvisory);
            ASSERT_EQ(sizeof(VnSnapshot), frame.payload.size());
            std::uint32_t stamp = 0;
            std::memcpy(&stamp, frame.payload.data(), sizeof(stamp));
            EXPECT_EQ(sizeof(VnSnapshot), stamp); // vintage-stamped image
            ++snapshots;
        }
    }
    EXPECT_TRUE(sawAck);
    // 1.2 s at 100 ms nominal: exact scheduling is the host tick's, so
    // accept a wide-but-meaningful band — far more than zero, far fewer
    // than an unthrottled flood.
    EXPECT_GE(snapshots, 6);
    EXPECT_LE(snapshots, 18);

    // Unsubscribe stops the stream.
    WireFrame unsubscribe;
    unsubscribe.msgType = kMsgSubscribe;
    unsubscribe.sequence = 61;
    WireWriter unsubWriter(&unsubscribe.payload);
    unsubWriter.U32(0);
    ASSERT_TRUE(client.WriteFrame(unsubscribe));
    // Drain until quiet: after the ack and any in-flight advisory
    // frames, no further snapshot may arrive for a full period window.
    int tail = 0;
    const ULONGLONG quietDeadline = GetTickCount64() + 600;
    bool pastAck = false;
    while (GetTickCount64() < quietDeadline) {
        WireFrame frame;
        if (!client.ReadFrame(&frame, 150)) {
            if (pastAck) {
                break; // a full quiet window after the ack: stream is off
            }
            continue;
        }
        if (frame.msgType == kMsgSubscribeAck) {
            pastAck = true;
            tail = 0;
        } else if (frame.msgType == kMsgSnapshot && pastAck) {
            ++tail;
        }
    }
    EXPECT_TRUE(pastAck);
    EXPECT_EQ(0, tail) << "snapshots kept flowing after unsubscribe";
}

TEST(RemoteChannelE2E, AdvisoryOverflowDropsSnapshotsNeverReplies) {
    const std::string pipeName = UniquePipeName();
    HostProcess host;
    // A tiny advisory buffer plus a fast subscription forces overflow
    // while the client deliberately does not read.
    ASSERT_TRUE(host.Start(ServeCommand(pipeName, "--advisory-frames 4")));

    PipeClient client;
    ASSERT_NO_FATAL_FAILURE(ConnectAndHandshake(&client, pipeName));

    WireFrame subscribe;
    subscribe.msgType = kMsgSubscribe;
    subscribe.sequence = 70;
    WireWriter writer(&subscribe.payload);
    writer.U32(10);
    ASSERT_TRUE(client.WriteFrame(subscribe));

    Sleep(800); // let the advisory half overflow unread

    // A command issued through the pressure must still get its reply.
    WireFrame create = CommandFrame(kOpVn_CreateBus, 71);
    WireWriter createWriter(&create.payload);
    createWriter.ObjectId(harkentest::ObjectId(13));
    createWriter.ObjectId(VN_BUS_MASTER);
    ASSERT_TRUE(client.WriteFrame(create));
    WireFrame reply;
    ASSERT_TRUE(ReadReplyFor(&client, 71, &reply, 8000));
    ASSERT_EQ(kMsgReply, reply.msgType);
    EXPECT_EQ(VN_OK, ReplyResult(reply));

    // The drops are visible in a server status frame.
    std::uint64_t reportedDrops = 0;
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        WireFrame frame;
        if (!client.ReadFrame(&frame, 1000)) {
            break;
        }
        if (frame.msgType == kMsgServerStatus) {
            WireReader reader(frame.payload.data(), frame.payload.size());
            ASSERT_TRUE(reader.ReadU64(&reportedDrops));
            if (reportedDrops > 0) {
                break;
            }
        }
    }
    EXPECT_GT(reportedDrops, 0u);
}

TEST(RemoteChannelE2E, CommandFloodWithoutReadingBackpressuresThenCloses) {
    const std::string pipeName = UniquePipeName();
    HostProcess host;
    // Small queues make the contract observable at test scale: the read
    // gate stops consuming first; a reply that still cannot queue (one
    // read chunk can carry many frames past the gate) closes the session.
    ASSERT_TRUE(host.Start(ServeCommand(
        pipeName,
        "--inbound-frames 4 --reliable-frames 8 --close-linger-ms 10")));

    PipeClient client;
    ASSERT_NO_FATAL_FAILURE(ConnectAndHandshake(&client, pipeName));

    // Flood tiny commands, never reading. Either the pipe backpressures
    // the writes (server stopped consuming) or the server closes; both
    // terminate the loop.
    bool writeStalled = false;
    for (std::uint32_t i = 0; i < 20000; ++i) {
        WireFrame set = CommandFrame(kOpVn_SetValue, 1000 + i);
        WireWriter writer(&set.payload);
        writer.U64(2);
        writer.F32(1.0f);
        if (!client.WriteFrame(set, 250)) {
            writeStalled = true;
            break;
        }
    }
    // Whichever way the flood ended, the server must have closed the
    // session (replies could not all queue) — and stayed alive.
    EXPECT_TRUE(client.WaitForClose(10000));
    client.Close();
    EXPECT_TRUE(host.Running());
    (void)writeStalled;

    // Containment after the backpressure close: no mid-command engine
    // corruption (every dispatched call ran to completion before its
    // reply was considered), and the next session round-trips a command
    // against the same, still-healthy engine instance.
    PipeClient next;
    ASSERT_NO_FATAL_FAILURE(ConnectAndHandshake(&next, pipeName));
    WireFrame create = CommandFrame(kOpVn_CreateBus, 90);
    WireWriter writer(&create.payload);
    writer.ObjectId(harkentest::ObjectId(17));
    writer.ObjectId(VN_BUS_MASTER);
    ASSERT_TRUE(next.WriteFrame(create));
    WireFrame reply;
    ASSERT_TRUE(ReadReplyFor(&next, 90, &reply));
    ASSERT_EQ(kMsgReply, reply.msgType);
    EXPECT_EQ(VN_OK, ReplyResult(reply));
}

} // namespace
