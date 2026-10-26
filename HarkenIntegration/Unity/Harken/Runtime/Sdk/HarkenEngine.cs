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
        internal static bool IsUpdating => updateDepth != 0;
        internal static event Action OutermostUpdateReturned;

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

        public static VnResult Init(in VnConfig config)
        {
            VnResult support = Emitter.CheckStateOnlySupport();
            if (support != VnResult.VN_OK) return support;
            VnResult result = HarkenNative.Vn_Init(config);
            if (result == VnResult.VN_OK) VariableGameplay.NotifySessionChanged();
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
        /// Explicitly enables optional communication for the initialized host.
        /// Call on the externally synchronized engine thread. Failure leaves
        /// audio running; the host may retry without creating another session.
        /// </summary>
        public static VnResult StartRemoteCommunication(
            // string applicationName, string bindAddress = "0.0.0.0", int port = 0,
            string applicationName, string bindAddress = "127.0.0.1", int port = 0,
            bool enableDiscovery = true, int discoveryPort = 0, string projectName = null)
            => RemoteCommunication.Start(applicationName, bindAddress, port,
                enableDiscovery, discoveryPort, projectName);

        /// <summary>Stops optional communication without terminating audio.</summary>
        public static VnResult StopRemoteCommunication() => RemoteCommunication.Stop();

        /// <summary>Reads communication state, including the last startup failure.</summary>
        public static VnResult QueryRemoteCommunication(out VnRemoteStatus status)
            => RemoteCommunication.GetStatus(out status);

        public static VnResult Term()
        {
            VnResult result = HarkenNative.Vn_Term();
            if (result == VnResult.VN_OK) VariableGameplay.NotifySessionChanged();
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

        /// <summary>A <see cref="VnSnapshot"/> with <c>structSize</c> preset; pass
        /// by ref to <see cref="Capture"/>.</summary>
        internal static VnSnapshot DefaultSnapshot()
        {
            VnSnapshot s = default;
            s.structSize = (uint)Marshal.SizeOf<VnSnapshot>();
            return s;
        }

        internal static VnResult Capture(ref VnSnapshot snapshot)
            => HarkenNative.Vn_CaptureSnapshot(ref snapshot);

        /// <summary>
        /// Applies one complete owner-scoped Bus and Send snapshot. The arrays
        /// are pinned only for the native call and are safe for Mono and IL2CPP.
        /// Retry the same revision after <see cref="VnResult.VN_BACKPRESSURE"/>.
        /// </summary>
        internal static unsafe VnResult ApplyBusTopology(
            ulong ownerId, ulong baseRevision, ulong revision,
            VnBusTopologyBus[] buses, VnBusTopologySend[] sends)
        {
            buses = buses ?? System.Array.Empty<VnBusTopologyBus>();
            sends = sends ?? System.Array.Empty<VnBusTopologySend>();
            uint busSize = (uint)Marshal.SizeOf<VnBusTopologyBus>();
            uint sendSize = (uint)Marshal.SizeOf<VnBusTopologySend>();
            for (int i = 0; i < buses.Length; ++i)
                buses[i].structSize = busSize;
            for (int i = 0; i < sends.Length; ++i)
                sends[i].structSize = sendSize;

            fixed (VnBusTopologyBus* busRows = buses)
            fixed (VnBusTopologySend* sendRows = sends)
            {
                VnBusTopologyDesc topology = default;
                topology.structSize =
                    (uint)Marshal.SizeOf<VnBusTopologyDesc>();
                topology.ownerId = ownerId;
                topology.baseRevision = baseRevision;
                topology.revision = revision;
                topology.busCount = (uint)buses.Length;
                topology.busStructSize = busSize;
                topology.buses = (System.IntPtr)busRows;
                topology.sendCount = (uint)sends.Length;
                topology.sendStructSize = sendSize;
                topology.sends = (System.IntPtr)sendRows;
                return HarkenNative.Vn_ApplyBusTopology(topology);
            }
        }

        /// <summary>Creates a bus under an existing parent.</summary>
        internal static VnResult CreateBus(VnObjectId busId, VnObjectId parentId)
            => HarkenNative.Vn_CreateBus(busId, parentId);

        /// <summary>Sets a bus volume (hot-path-safe: blittable args, no allocation).</summary>
        public static VnResult SetBusVolume(VnObjectId busId, float volumeDb, uint rampMs)
            => HarkenNative.Vn_SetBusVolume(busId, volumeDb, rampMs);

        /// <summary>Sets the independent gain on a bus's main output.</summary>
        public static VnResult SetBusOutputVolume(VnObjectId busId, float volumeDb, uint rampMs)
            => HarkenNative.Vn_SetBusOutputVolume(busId, volumeDb, rampMs);

        /// <summary>Sets a bus mix-wide lowpass (0) or highpass (1) filter.</summary>
        internal static VnResult SetBusFilter(VnObjectId busId, uint filterKind,
            float cutoffHz, float q, uint enable01, uint rampMs)
            => HarkenNative.Vn_SetBusFilter(busId, filterKind, cutoffHz, q,
                enable01, rampMs);

        /// <summary>Sets a bus Direct/output-only lowpass (0) or highpass (1).</summary>
        internal static VnResult SetBusOutputFilter(VnObjectId busId, uint filterKind,
            float cutoffHz, float q, uint enable01, uint rampMs)
            => HarkenNative.Vn_SetBusOutputFilter(busId, filterKind, cutoffHz, q,
                enable01, rampMs);

        /// <summary>Sets a live voice's lowpass (0) or highpass (1) tone filter.</summary>
        internal static VnResult SetVoiceFilter(ulong handle, uint filterKind,
            float cutoffHz, float q, uint enable01, uint rampMs)
            => HarkenNative.Vn_SetVoiceFilter(handle, filterKind, cutoffHz, q,
                enable01, rampMs);

        /// <summary>Mutes or unmutes a bus; composes with (never overwrites) its volume state.</summary>
        public static VnResult SetBusMute(VnObjectId busId, uint mute01, uint fadeMs)
            => HarkenNative.Vn_SetBusMute(busId, mute01, fadeMs);

        /// <summary>Solos or unsolos a bus (tree-path audibility scoping).</summary>
        internal static VnResult SetBusSolo(VnObjectId busId, uint solo01, uint fadeMs)
            => HarkenNative.Vn_SetBusSolo(busId, solo01, fadeMs);

        /// <summary>Moves a live bus (with its subtree) under a new parent, atomically at a block boundary.</summary>
        internal static VnResult ReparentBus(VnObjectId busId, VnObjectId newParentId)
            => HarkenNative.Vn_ReparentBus(busId, newParentId);

        /// <summary>Destroys a bus and its whole subtree; routed voices migrate to the surviving parent.</summary>
        internal static VnResult DestroyBus(VnObjectId busId)
            => HarkenNative.Vn_DestroyBus(busId);
    }
}
