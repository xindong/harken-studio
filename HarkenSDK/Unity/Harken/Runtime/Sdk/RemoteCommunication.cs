using System;
using System.Runtime.InteropServices;

namespace Harken
{
    internal static class RemoteCommunication
    {
        private static readonly uint ConfigSize = (uint)Marshal.SizeOf<VnRemoteConfig>();
        private static readonly uint StatusSize = (uint)Marshal.SizeOf<VnRemoteStatus>();

        public static unsafe VnResult Start(
            // string applicationName, string bindAddress = "0.0.0.0", int port = 0,
            string applicationName, string bindAddress = "127.0.0.1", int port = 0,
            bool enableDiscovery = true, int discoveryPort = 0, string projectName = null)
        {
            if (port < 0 || port > ushort.MaxValue || discoveryPort < 0 || discoveryPort > ushort.MaxValue ||
                applicationName?.IndexOf('\0') >= 0 || bindAddress?.IndexOf('\0') >= 0 ||
                projectName?.IndexOf('\0') >= 0)
                return VnResult.VN_INVALID_ARG;
            Span<byte> address = stackalloc byte[64];
            Span<byte> application = stackalloc byte[128];
            Span<byte> project = stackalloc byte[128];
            if (!Utf8.TryEncode(bindAddress ?? string.Empty, address, out _) ||
                !Utf8.TryEncode(applicationName ?? string.Empty, application, out _) ||
                !Utf8.TryEncode(projectName ?? string.Empty, project, out _))
                return VnResult.VN_INVALID_ARG;
            fixed (byte* addressPointer = address)
            fixed (byte* applicationPointer = application)
            fixed (byte* projectPointer = project)
            {
                VnRemoteConfig config = default;
                config.structSize = ConfigSize;
                config.tcpPort = (uint)port;
                config.discoveryPort = (uint)discoveryPort;
                config.flags = enableDiscovery ? (uint)VnRemoteFlags.VN_REMOTE_DISCOVERY_ENABLED : 0;
                config.bindAddress = (IntPtr)addressPointer;
                config.applicationName = (IntPtr)applicationPointer;
                config.projectName = (IntPtr)projectPointer;
                return HarkenNative.Vn_RemoteStart(in config);
            }
        }

        public static VnResult Stop() => HarkenNative.Vn_RemoteStop();

        public static VnResult GetStatus(out VnRemoteStatus status)
        {
            status = default;
            status.structSize = StatusSize;
            return HarkenNative.Vn_RemoteGetStatus(ref status);
        }
    }
}
