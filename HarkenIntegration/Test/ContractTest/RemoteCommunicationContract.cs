using System;
using System.Net;
using System.Net.Sockets;
using Harken;

namespace Harken.Test
{
    internal static class RemoteCommunicationContract
    {
        public static int RunNative()
        {
            bool initialized = false;
            try
            {
                VnConfig config = HarkenEngine.DefaultConfig();
                config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
                Expect(HarkenEngine.Init(config), VnResult.VN_OK, "Initialize");
                initialized = true;
                Expect(HarkenEngine.QueryRemoteCommunication(out VnRemoteStatus status), VnResult.VN_OK, "Default status");
                Require(status.state == (uint)VnRemoteState.VN_REMOTE_STOPPED && status.tcpPort == 0,
                    "Initialization must leave communication disabled.");
                Expect(HarkenEngine.StartRemoteCommunication("Invalid\0Identity"), VnResult.VN_INVALID_ARG,
                    "Embedded NUL identity");
                Expect(HarkenEngine.StartRemoteCommunication("SDK contract", port: 65536), VnResult.VN_INVALID_ARG,
                    "Port overflow");
                Expect(HarkenEngine.StartRemoteCommunication(new string('x', 128)), VnResult.VN_INVALID_ARG,
                    "Oversized application identity");

                int port;
                using (var occupied = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp))
                {
                    occupied.ExclusiveAddressUse = true;
                    occupied.Bind(new IPEndPoint(IPAddress.Loopback, 0));
                    occupied.Listen(1);
                    port = ((IPEndPoint)occupied.LocalEndPoint).Port;
                    VnResult refused = HarkenEngine.StartRemoteCommunication("SDK contract", "127.0.0.1", port, false);
                    Require(refused != VnResult.VN_OK, "An occupied port must refuse communication startup.");
                    Expect(HarkenEngine.QueryRemoteCommunication(out status), VnResult.VN_OK, "Failed status");
                    Require(status.state == (uint)VnRemoteState.VN_REMOTE_FAILED && status.lastResult == (int)refused,
                        "The separate communication status must retain its startup failure.");
                    Expect(HarkenEngine.Update(), VnResult.VN_OK, "Audio pump after communication failure");
                }

                Expect(HarkenEngine.StartRemoteCommunication("SDK contract", "127.0.0.1", port, false),
                    VnResult.VN_OK, "Retry after port release");
                Expect(HarkenEngine.QueryRemoteCommunication(out status), VnResult.VN_OK, "Listening status");
                Require(status.state == (uint)VnRemoteState.VN_REMOTE_LISTENING && status.tcpPort == (uint)port &&
                    status.discoveryPort == 0 && status.instanceId != 0 &&
                    status.processId == (uint)Environment.ProcessId,
                    "Listening status must identify the actual process and bound endpoint.");
                using (var client = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp))
                {
                    client.Connect(IPAddress.Loopback, port);
                    Require(client.Connected, "The SDK must start an actual TCP listener.");
                }
                Expect(HarkenEngine.StopRemoteCommunication(), VnResult.VN_OK, "Stop");
                Expect(HarkenEngine.QueryRemoteCommunication(out status), VnResult.VN_OK, "Stopped status");
                Require(status.state == (uint)VnRemoteState.VN_REMOTE_STOPPED && status.tcpPort == 0,
                    "Stop must retire the listening endpoint.");
                RequirePortClosed(port);

                Expect(HarkenEngine.StartRemoteCommunication("SDK contract", "127.0.0.1", 0, false),
                    VnResult.VN_OK, "Restart with ephemeral port");
                Expect(HarkenEngine.QueryRemoteCommunication(out status), VnResult.VN_OK, "Restart status");
                Require(status.tcpPort != 0, "Ephemeral startup must report the actual port.");
                port = (int)status.tcpPort;
                Expect(HarkenEngine.Term(), VnResult.VN_OK, "Terminate");
                initialized = false;
                Expect(HarkenEngine.QueryRemoteCommunication(out status), VnResult.VN_OK, "Terminated status");
                Require(status.state == (uint)VnRemoteState.VN_REMOTE_STOPPED && status.tcpPort == 0,
                    "Engine termination must stop communication without a separate Stop call.");
                RequirePortClosed(port);
                Expect(HarkenEngine.StartRemoteCommunication("SDK contract", "127.0.0.1", 0, false),
                    VnResult.VN_NOT_INITIALIZED, "Startup without an engine");
                Console.WriteLine("[Remote] default-off, bind refusal, preserved audio, retry, TCP, stop and termination PASS.");
                return 0;
            }
            catch (DllNotFoundException)
            {
                Console.WriteLine("[Remote] SKIP: native HarkenCore is unavailable.");
                return 2;
            }
            catch (BadImageFormatException)
            {
                Console.WriteLine("[Remote] SKIP: native HarkenCore architecture mismatch.");
                return 2;
            }
            catch (SocketException error)
            {
                Console.WriteLine("[Remote] FAIL: " + error.Message);
                return 1;
            }
            catch (InvalidOperationException error)
            {
                Console.WriteLine("[Remote] FAIL: " + error.Message);
                return 1;
            }
            finally
            {
                if (initialized) HarkenEngine.Term();
            }
        }

        private static void RequirePortClosed(int port)
        {
            using (var client = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp))
            {
                try { client.Connect(IPAddress.Loopback, port); }
                catch (SocketException error) when (error.SocketErrorCode == SocketError.ConnectionRefused) { return; }
                throw new InvalidOperationException("A stopped remote endpoint still accepts TCP connections.");
            }
        }

        private static void Expect(VnResult actual, VnResult expected, string operation)
            => Require(actual == expected, operation + ": " + actual + " (expected " + expected + ")");

        private static void Require(bool condition, string message)
        {
            if (!condition) throw new InvalidOperationException(message);
        }
    }
}
