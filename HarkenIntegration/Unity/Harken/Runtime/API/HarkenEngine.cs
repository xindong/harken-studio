#if HARKEN_NATIVE_DEVELOPMENT || (!HARKEN_NATIVE_RELEASE && (UNITY_EDITOR || DEVELOPMENT_BUILD))
#define HARKEN_ENGINE_DEV
#endif
using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

[assembly: InternalsVisibleTo("Harken.Unity")]
[assembly: InternalsVisibleTo("Harken.Unity.Editor")]
[assembly: InternalsVisibleTo("Harken.Unity.EditModeTests")]
[assembly: InternalsVisibleTo("Harken.Unity.PlayModeTests")]

namespace Harken
{
    /// <summary>
    /// Engine lifecycle, host integration and supported runtime mix controls.
    /// The host owns the native session. Wrappers also track session changes and
    /// the outermost Update return boundary for managed cleanup. The per-frame
    /// <see cref="Update"/> is on the zero-GC hot path.
    /// </summary>
    public static class HarkenEngine
    {
        private static int updateDepth;
        internal static uint NativeAbiMinor { get; private set; }
        internal static bool IsUpdating => updateDepth != 0;
        internal static event Action OutermostUpdateReturned;
        internal static event Action Initialized;

        /// <summary>
        /// A <see cref="VnConfig"/> with <c>structSize</c> preset to the current
        /// mirror's size and every other field zero. Each numeric zero selects
        /// the documented engine default (the vintage contract), so a caller
        /// built against an older package sends its shorter struct and the engine
        /// reads defaults for the tail — exactly as a C caller would. Fill the
        /// fields you need, then <see cref="Init"/>.
        /// </summary>
        public static VnConfig DefaultConfig()
        {
            VnConfig c = default;
            c.structSize = (uint)Marshal.SizeOf<VnConfig>();
            return c;
        }

        internal static bool TryGetAbi(out VnAbiVersionInfo abi)
        {
            abi = default;
            abi.structSize = (uint)Marshal.SizeOf<VnAbiVersionInfo>();
            try
            {
                return HarkenNative.Vn_QueryAbiVersion(ref abi) == VnResult.VN_OK &&
                    abi.major == VnConstants.VN_ABI_VERSION_MAJOR;
            }
            catch (EntryPointNotFoundException) { return false; }
        }

        internal static ulong BeginOutputPreparation(in VnConfig config)
        {
#if UNITY_STANDALONE_WIN && !UNITY_EDITOR
            try
            {
                if (TryGetAbi(out var abi) && abi.minor >= 5 &&
                    HarkenNative.Vn_BeginOutputPreparation(config, out ulong ticket) == VnResult.VN_OK)
                    return ticket;
            }
            catch (DllNotFoundException) { }
            catch (EntryPointNotFoundException) { }
            catch (BadImageFormatException) { }
#endif
            return 0;
        }

        internal static void CancelOutputPreparation(ulong ticket)
        {
#if UNITY_STANDALONE_WIN && !UNITY_EDITOR
            if (ticket != 0) HarkenNative.Vn_CancelOutputPreparation(ticket);
#endif
        }

        public static VnResult Init(in VnConfig config) => Init(config, 0);

        internal static VnResult Init(in VnConfig config, ulong ticket)
        {
            if (!TryGetAbi(out var abi)) return VnResult.VN_CAPABILITY_UNAVAILABLE;
            VnResult support = Emitter.CheckStateOnlySupport();
            if (support != VnResult.VN_OK) return support;
            VnResult result;
#if UNITY_STANDALONE_WIN && !UNITY_EDITOR
            if (ticket != 0)
            {
                result = HarkenNative.Vn_InitWithOutputPreparation(config, ticket);
                if (result == VnResult.VN_INVALID_CONFIG &&
                    HarkenNative.Vn_CancelOutputPreparation(ticket) == VnResult.VN_OK)
                    result = HarkenNative.Vn_Init(config);
            }
            else
#endif
                result = HarkenNative.Vn_Init(config);
            if (result != VnResult.VN_OK) return result;
            NativeAbiMinor = abi.minor;
            VariableGameplay.NotifySessionChanged();
            Initialized?.Invoke();
            return result;
        }

        /// <summary>
        /// Releases the platform device while preserving engine state. Call from
        /// the host's externally synchronized application-lifecycle callback.
        /// </summary>
        public static VnResult Suspend() => HarkenNative.Vn_Suspend();

        /// <summary>
        /// Rebuilds the platform device after <see cref="Suspend"/>. A device
        /// error leaves the engine suspended so the host can retry.
        /// </summary>
        public static VnResult Resume() => HarkenNative.Vn_Resume();

        /// <summary>
        /// Silences or restores the platform device output for the host, such as
        /// an editor mute toggle. Mixing, metering and audio time are unaffected.
        /// </summary>
        public static VnResult SetHostOutputMute(uint mute01, uint fadeMs)
            => HarkenNative.Vn_SetHostOutputMute(mute01, fadeMs);

#if HARKEN_ENGINE_DEV
        /// <summary>
        /// Explicitly enables optional communication for the initialized host.
        /// Call on the externally synchronized engine thread. Failure leaves
        /// audio running; the host may retry without creating another session.
        /// </summary>
        public static VnResult StartRemoteCommunication(
            string applicationName, string bindAddress = "127.0.0.1", int port = 0,
            bool enableDiscovery = true, int discoveryPort = 0, string projectName = null)
            => RemoteCommunication.Start(applicationName, bindAddress, port,
                enableDiscovery, discoveryPort, projectName);

        /// <summary>Stops optional communication without terminating audio.</summary>
        public static VnResult StopRemoteCommunication() => RemoteCommunication.Stop();

        /// <summary>Reads communication state, including the last startup failure.</summary>
        public static VnResult QueryRemoteCommunication(out VnRemoteStatus status)
            => RemoteCommunication.GetStatus(out status);
#endif

        public static VnResult Term()
        {
            VnResult result = HarkenNative.Vn_Term();
            if (result == VnResult.VN_OK)
            {
                NameTable.Clear();
                VariableGameplay.NotifySessionChanged();
            }
            return result;
        }

        /// <summary>
        /// Pumps the engine on the calling thread (drains notifications, invokes
        /// callbacks, flushes commits). Per-frame hot path — zero managed
        /// allocation in the binding. Unity session cleanup waits for the
        /// outermost native call to return, including nested callback updates.
        /// </summary>
        public static VnResult Update()
        {
            ++updateDepth;
            try { return HarkenNative.Vn_Update(); }
            finally
            {
                if (--updateDepth == 0) OutermostUpdateReturned?.Invoke();
            }
        }

        /// <summary>Sets a bus volume (hot-path-safe: blittable args, no allocation).</summary>
        public static VnResult SetBusVolume(VnObjectId busId, float volumeDb, uint rampMs)
            => HarkenNative.Vn_SetBusVolume(busId, volumeDb, rampMs);

        /// <summary>Sets the independent gain on a bus's output.</summary>
        public static VnResult SetBusOutputVolume(VnObjectId busId, float volumeDb, uint rampMs)
            => HarkenNative.Vn_SetBusOutputVolume(busId, volumeDb, rampMs);

        /// <summary>Mutes or unmutes a bus; composes with (never overwrites) its volume state.</summary>
        public static VnResult SetBusMute(VnObjectId busId, uint mute01, uint fadeMs)
            => HarkenNative.Vn_SetBusMute(busId, mute01, fadeMs);
    }
}
