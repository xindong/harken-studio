using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using UnityEngine;
using UnityEngine.Events;
using Harken;

namespace Harken.Unity
{
    public enum HarkenRuntimeState
    {
        Stopped,
        Starting,
        Ready,
        Suspended,
        Failed,
        ShuttingDown
    }

    public interface IHarkenRuntimeParticipant
    {
        int ShutdownOrder { get; }
        void BeforeRuntimeShutdown();
    }

    [Serializable]
    public sealed class HarkenRuntimeResultEvent : UnityEvent<string>
    {
    }

    [DefaultExecutionOrder(-10000)]
    public partial class HarkenAudioSystem : MonoBehaviour
    {
        private const int SuccessMessageCacheCapacity = 64;
        [SerializeField] private bool autoInitialize = true;
        [SerializeField, HideInInspector] private string packRoot = "Audio/GeneratedPacks";
        [SerializeField, HideInInspector] private string initPack = "Init.bytes";
        [SerializeField, HideInInspector] private string packPlatform = string.Empty;
        [SerializeField, HideInInspector] private string packPlatformId = string.Empty;
        [SerializeField, HideInInspector] private string language = string.Empty;
        [SerializeField, HideInInspector] private ulong[] startupPackStableIds = Array.Empty<ulong>();
        [SerializeField, HideInInspector] private HarkenPackSource preparedPackSource;
        private HarkenPreparedPacks preparedPacks;
        private bool lifecycleConfigured;
        private bool languageConfigured;
        private bool platformConfigured;
        private bool initPackConfigured;
        [SerializeField, HideInInspector] private bool nullDevice;
        [SerializeField] private bool persistAcrossScenes = true;
        [SerializeField, HideInInspector] private uint maxVoices = 32;
        [SerializeField, HideInInspector] private uint maxPacks = 16;
        [SerializeField, HideInInspector] private uint maxActions = 64;
        [SerializeField, HideInInspector] private uint maxActionFirings = 64;
        [SerializeField, HideInInspector] private bool suppressUnityAudio;
        [SerializeField] private bool shutdownWhenDisabled;
        [SerializeField, HideInInspector] private bool unityAudioCaptured;
        [SerializeField, HideInInspector] private bool previousUnityAudioPause;
        [SerializeField, HideInInspector] private float previousUnityAudioVolume;
        [SerializeField, HideInInspector] private uint maxEmitters;
        [SerializeField, HideInInspector] private uint emitterVariableSlots;
        [SerializeField] private UnityEvent ready = new UnityEvent();
        [SerializeField] private HarkenRuntimeResultEvent failed =
            new HarkenRuntimeResultEvent();
        [SerializeField] private HarkenRuntimeResultEvent resultObserved =
            new HarkenRuntimeResultEvent();

        private PackBootstrapSession session;
        private HarkenCallbacks.Registration notificationRegistration;
        private HarkenCallbacks.Registration logRegistration;
        private readonly List<IHarkenRuntimeParticipant> participants =
            new List<IHarkenRuntimeParticipant>();
        private readonly List<HarkenPack> pendingPackLoads =
            new List<HarkenPack>();
        private bool shuttingDown;
        private bool disabling;
        private bool hostExiting;
        private bool hasPendingShutdown;
        private ShutdownCompletion pendingShutdown;
        private bool starting;
        private bool publishingStartupResult;
        private bool applicationPaused;
        private bool startupReadyPending;
        private PackBootstrapSession publishingReadySession;
        private Coroutine startupCoroutine;
        private readonly Dictionary<string, string> successMessages =
            new Dictionary<string, string>(SuccessMessageCacheCapacity, StringComparer.Ordinal);
        private readonly List<FiringCleanup> firingCleanup = new List<FiringCleanup>();
        private bool collectingFirings;
        private readonly List<PackCleanup> packCleanup = new List<PackCleanup>();
        private bool collectingPacks;
        private readonly List<EmitterCleanup> emitterCleanup = new List<EmitterCleanup>();
        private bool collectingEmitters;
        private ManagedFiring[] managedFirings = Array.Empty<ManagedFiring>();
        private bool collectingManagedFirings;
        private ulong managedCallerSeed;
        private readonly HarkenSpatialSources spatialSources = new HarkenSpatialSources();
        private string runtimeFailureOperation;
        private VnResult runtimeFailureResult;
        private int gameThreadId;

        private struct ShutdownCompletion
        {
            public PackBootstrapSession Session;
            public HarkenRuntimeState State;
            public string Operation;
            public VnResult Result;
            public VnResult TermResult;
            public bool RuntimeFailed;
            public bool PublishFailure;
        }

        private struct ManagedFiring
        {
            public ActionFiring Firing;
            public HarkenSpatialSource Source;
            public ActionPreparation Resources;
            public bool Reserved;
            public bool Observed;
            public VnResult LastResult;
            public bool LastWasRelease;
            public bool ResourcesObserved;
            public VnResult LastResourcesResult;
        }
        private struct EmitterCleanup
        {
            public Emitter Emitter;
            public PackBootstrapSession Session;
            public uint FadeMs;
            public bool StopAccepted;
            public bool Observed;
            public VnResult LastResult;
        }

        private struct PackCleanup
        {
            public Pack Pack;
            public PackBootstrapSession Session;
            public ulong StableId;
            public VnResult LastResult;
        }

        private struct FiringCleanup
        {
            public ActionFiring Firing;
            public ActionPreparation Resources;
            public PackBootstrapSession Session;
            public uint FadeMs;
            public bool StopAccepted;
            public bool StopObserved;
            public VnResult LastStopResult;
            public bool ResourcesObserved;
            public VnResult LastResourcesResult;
        }

        public static HarkenAudioSystem Instance { get; private set; }
        public HarkenRuntimeState State { get; private set; }
        public bool IsReady => State == HarkenRuntimeState.Ready;
        public VnResult LastResult { get; private set; } = VnResult.VN_OK;
        public string LastOperation { get; private set; } = string.Empty;
        public string PackRootPath { get; private set; } = string.Empty;
        public string LastStartupError { get; private set; } = string.Empty;
        public VnResult LastRemoteResult { get; private set; } = VnResult.VN_OK;
        public string LastRemoteError { get; private set; } = string.Empty;
        public PackBootstrapSession Session => session;
        public string Language => session != null ? session.Language : language;
        public int PendingPackLoadCount => pendingPackLoads.Count;
        public bool IsPreparationComplete
        {
            get
            {
                if (!IsReady || session == null || !session.IsStartupComplete ||
                    pendingPackLoads.Count != 0)
                    return false;
                foreach (IHarkenRuntimeParticipant participant in participants)
                {
                    if (participant is HarkenPack pack && pack &&
                        pack.isActiveAndEnabled && pack.LoadOnReady && !pack.IsLoaded)
                        return false;
                }
                return true;
            }
        }

        public event Action<HarkenAudioSystem> BecameReady;
        public event Action<HarkenAudioSystem, string, VnResult> OperationCompleted;
        public event HarkenCallbacks.NotificationHandler NotificationReceived;
        public event Action<HarkenAudioSystem, HarkenRuntimeState, string, VnResult> RuntimeEnded;

        public VnResult ConfigureLifecycleBeforeStartup(bool suppressAudio, bool stopWhenDisabled)
        {
            if (session != null || starting || shuttingDown)
                return ObserveResult("HarkenAudioSystem.ConfigureLifecycleBeforeStartup", VnResult.VN_INVALID_ARG);
            suppressUnityAudio = suppressAudio;
            shutdownWhenDisabled = stopWhenDisabled;
            lifecycleConfigured = true;
            return ObserveResult("HarkenAudioSystem.ConfigureLifecycleBeforeStartup", VnResult.VN_OK);
        }

        public VnResult RetryRemoteCommunication()
        {
            if (!IsGameplayThread())
                return ObserveRemoteResult("Start", VnResult.VN_INVALID_ARG);
            if (Instance != this || session == null || !session.IsActive || shuttingDown || hostExiting)
                return ObserveRemoteResult("Start", VnResult.VN_NOT_INITIALIZED);
            return ObserveRemoteResult("Start", HarkenProjectSettings.StartCommunication(session.ProjectId));
        }

        public VnResult QueryRemoteCommunication(out VnRemoteStatus status)
            => RemoteCommunication.GetStatus(out status);

        private VnResult ObserveRemoteResult(string operation, VnResult result)
        {
            LastRemoteResult = result;
            LastRemoteError = result == VnResult.VN_OK
                ? string.Empty : "Harken remote " + operation + ": " + result;
            if (result != VnResult.VN_OK) Debug.LogWarning(LastRemoteError, this);
            return result;
        }

        public VnResult SetMasterVolume(float volume)
        {
            const string operation = "HarkenAudioSystem.SetMasterVolume";
            PackBootstrapSession owner = session;
            if (!IsReady || owner == null || !owner.IsActive || shuttingDown)
                return ObserveResult(operation, VnResult.VN_NOT_INITIALIZED);
            if (float.IsNaN(volume) || float.IsInfinity(volume))
                return ObserveResult(operation, VnResult.VN_INVALID_ARG);
            volume = Mathf.Clamp01(volume);
            float volumeDb = volume == 0f ? -960f : 20f * Mathf.Log10(volume);
            return ObserveResult(operation,
                HarkenEngine.SetBusOutputVolume(VnConstants.VN_BUS_MASTER, volumeDb, 0));
        }

        /// <summary>
        /// Prepares the Action resource closure synchronously and posts once. SDK ownership lasts
        /// until natural completion or session shutdown; preload with Session.PrepareActions to avoid cold I/O.
        /// The returned handle may be queried or stopped; callers must not release it.
        /// </summary>
        public VnResult PlayAction(string actionName, string projectId, out ActionFiring firing)
            => PlayAction(actionName, projectId, managedCallerSeed++, out firing);

        public VnResult PlayAction(string actionName, string projectId, ulong callerSeed, out ActionFiring firing)
            => PlayActionCore(actionName, projectId, callerSeed, null, out firing);

        public VnResult CreateSpatialSource(Transform target, out HarkenSpatialSource source)
        {
            source = null;
            if (!IsReady || shuttingDown) return VnResult.VN_NOT_INITIALIZED;
            return spatialSources.Create(this, session, target, out source);
        }

        public VnResult PlayAction(string actionName, string projectId, HarkenSpatialSource source, out ActionFiring firing)
        {
            firing = default;
            if (source == null) return ObserveResult("HarkenAudioSystem.PlayAction", VnResult.VN_INVALID_ARG);
            return PlayActionCore(actionName, projectId, managedCallerSeed++, source, out firing);
        }

        private VnResult PlayActionCore(string actionName, string projectId, ulong callerSeed,
            HarkenSpatialSource source, out ActionFiring firing)
        {
            const string operation = "HarkenAudioSystem.PlayAction";
            firing = default;
            PackBootstrapSession owner = session;
            if (!IsReady || owner == null || !owner.IsActive || shuttingDown)
                return ObserveResult(operation, VnResult.VN_NOT_INITIALIZED);
            if (!IsPreparationComplete) return ObserveResult(operation, VnResult.VN_CONTENT_NOT_LOADED);
            CollectManagedFirings();
            if (!IsReady || !owner.IsActive || !ReferenceEquals(owner, session))
                return ObserveResult(operation, VnResult.VN_NOT_INITIALIZED);
            if (!IsPreparationComplete) return ObserveResult(operation, VnResult.VN_CONTENT_NOT_LOADED);
            CaptureUnityAudio();
            ManagedFiring[] slots = managedFirings;
            int slot = -1;
            for (int i = 0; i < slots.Length; ++i)
                if (!slots[i].Reserved) { slot = i; break; }
            if (slot < 0) return ObserveResult(operation, VnResult.VN_NO_ACTION);
            if (source != null)
            {
                VnResult retained = source.Retain(this, owner);
                if (retained != VnResult.VN_OK) return ObserveResult(operation, retained);
            }
            slots[slot].Reserved = true;
            slots[slot].Source = source;
            VnResult result = owner.PrepareActions(new[] { actionName }, projectId, out ActionPreparation resources);
            slots[slot].Resources = resources;
            if (!owner.IsActive || !ReferenceEquals(owner, session))
                return ObserveResult(operation, VnResult.VN_NOT_INITIALIZED);
            if (!IsReady || shuttingDown || !IsPreparationComplete)
            {
                ReleaseManagedResources(slots, slot);
                return ObserveResult(operation, !IsReady || shuttingDown
                    ? VnResult.VN_NOT_INITIALIZED : VnResult.VN_CONTENT_NOT_LOADED);
            }
            // Loading Packs changes the native publication generation. Bind only after preparation.
            ActionResolved resolved = default;
            if (result == VnResult.VN_OK)
                result = Actions.Resolve(actionName, projectId, out resolved);
            Emitter emitter = Emitter.None;
            if (result == VnResult.VN_OK && source != null)
                result = source.Validate(this, owner, out emitter);
            if (result == VnResult.VN_OK)
                result = Actions.Post(in resolved, emitter, callerSeed, out firing);
            if (!owner.IsActive || !ReferenceEquals(owner, session))
            {
                firing = default;
                return ObserveResult(operation, VnResult.VN_NOT_INITIALIZED);
            }
            if (result == VnResult.VN_OK) slots[slot].Firing = firing;
            else ReleaseManagedResources(slots, slot);
            return ObserveResult(operation, result);
        }

        private void ReleaseManagedResources(ManagedFiring[] slots, int index)
        {
            ManagedFiring pending = slots[index];
            VnResult result = pending.Resources?.Release() ?? VnResult.VN_OK;
            if (result == VnResult.VN_OK)
            {
                pending.Source?.Release();
                slots[index] = default;
            }
            else
            {
                slots[index].ResourcesObserved = true;
                slots[index].LastResourcesResult = result;
            }
            if (result != VnResult.VN_OK &&
                (!pending.ResourcesObserved || pending.LastResourcesResult != result))
                ObserveResult("ActionPreparation.Release(managed)", result);
        }

        private void CollectManagedFirings()
        {
            if (collectingManagedFirings || session == null || !session.IsActive) return;
            collectingManagedFirings = true;
            PackBootstrapSession owner = session;
            ManagedFiring[] slots = managedFirings;
            try
            {
                for (int i = 0; i < slots.Length; ++i)
                {
                    ManagedFiring pending = slots[i];
                    if (!pending.Firing.IsValid)
                    {
                        // A reserved slot without a scope is still inside preparation/provider work.
                        if (pending.Resources != null) ReleaseManagedResources(slots, i);
                        if (!owner.IsActive || !ReferenceEquals(owner, session)) break;
                        continue;
                    }
                    VnResult result = pending.Firing.Query(out VnActionFiringInfo info);
                    if (result == VnResult.VN_OK &&
                        info.state != (uint)VnActionFiringState.VN_ACTION_FIRING_TERMINAL_RETAINED)
                        continue;
                    bool released = result == VnResult.VN_OK;
                    if (released) result = pending.Firing.Release();
                    if (result == VnResult.VN_OK || result == VnResult.VN_STALE_HANDLE)
                    {
                        slots[i].Firing = default;
                        ReleaseManagedResources(slots, i);
                        if (!owner.IsActive || !ReferenceEquals(owner, session)) break;
                    }
                    else
                    {
                        slots[i].Observed = true;
                        slots[i].LastResult = result;
                        slots[i].LastWasRelease = released;
                    }
                    if (result != VnResult.VN_STALE_HANDLE &&
                        (!pending.Observed || pending.LastResult != result || pending.LastWasRelease != released))
                        ObserveResult(released ? "ActionFiring.Release(managed)" : "ActionFiring.Query(managed)", result);
                    // A result listener can end this session and start another one.
                    if (!owner.IsActive || !ReferenceEquals(owner, session)) break;
                }
            }
            finally { collectingManagedFirings = false; }
        }

        private void CaptureUnityAudio()
        {
            if (!suppressUnityAudio || unityAudioCaptured) return;
            previousUnityAudioPause = AudioListener.pause;
            previousUnityAudioVolume = AudioListener.volume;
            unityAudioCaptured = true;
            AudioListener.pause = true;
            AudioListener.volume = 0;
        }

        private void RestoreUnityAudio()
        {
            if (!unityAudioCaptured) return;
            AudioListener.pause = previousUnityAudioPause;
            AudioListener.volume = previousUnityAudioVolume;
            unityAudioCaptured = false;
        }

        public VnResult ConfigureBeforeStartup(
            string relativePackRoot, string relativeInitPack,
            ulong[] startupPacks, bool useNullDevice,
            bool keepAcrossScenes)
        {
            if (session != null || State == HarkenRuntimeState.Starting ||
                State == HarkenRuntimeState.Ready ||
                !IsSafeRelativePath(relativePackRoot) ||
                !IsSafeRelativePath(relativeInitPack) || startupPacks == null)
            {
                return ObserveResult(
                    "HarkenAudioSystem.ConfigureBeforeStartup",
                    VnResult.VN_INVALID_ARG);
            }
            packRoot = relativePackRoot;
            initPack = relativeInitPack;
            initPackConfigured = true;
            startupPackStableIds = (ulong[])startupPacks.Clone();
            nullDevice = useNullDevice;
            persistAcrossScenes = keepAcrossScenes;
            return ObserveResult(
                "HarkenAudioSystem.ConfigureBeforeStartup", VnResult.VN_OK);
        }

        public VnResult ConfigurePlatformBeforeStartup(string platformId, string platformName = null)
        {
            if (session != null || State == HarkenRuntimeState.Starting || State == HarkenRuntimeState.Ready ||
                !string.IsNullOrEmpty(platformId) ||
                (!string.IsNullOrEmpty(platformName) && !PackPaths.IsPlatformName(platformName)))
                return ObserveResult("HarkenAudioSystem.ConfigurePlatformBeforeStartup", VnResult.VN_INVALID_ARG);
            packPlatformId = platformId ?? string.Empty;
            packPlatform = platformName ?? string.Empty;
            platformConfigured = true;
            return ObserveResult("HarkenAudioSystem.ConfigurePlatformBeforeStartup", VnResult.VN_OK);
        }

        public VnResult ConfigureLanguageBeforeStartup(string selectedLanguage)
        {
            if (session != null || State == HarkenRuntimeState.Starting || State == HarkenRuntimeState.Ready ||
                (!string.IsNullOrEmpty(selectedLanguage) &&
                 (!IsSafeRelativePath(selectedLanguage) || selectedLanguage.IndexOfAny(new[] { '/', '\\' }) >= 0)))
                return ObserveResult("HarkenAudioSystem.ConfigureLanguageBeforeStartup", VnResult.VN_INVALID_ARG);
            language = selectedLanguage ?? string.Empty;
            languageConfigured = true;
            return ObserveResult("HarkenAudioSystem.ConfigureLanguageBeforeStartup", VnResult.VN_OK);
        }

        public VnResult ConfigurePreparedSourceBeforeStartup(HarkenPackSource source)
        {
            return ObserveResult("HarkenAudioSystem.ConfigurePreparedSourceBeforeStartup", VnResult.VN_PACK_INCOMPATIBLE);
        }

        public VnResult ConfigurePreparedBytesBeforeStartup(HarkenPreparedPacks source)
        {
            return ObserveResult("HarkenAudioSystem.ConfigurePreparedBytesBeforeStartup", VnResult.VN_PACK_INCOMPATIBLE);
        }

        public VnResult StartRuntime(string containmentRoot)
        {
            if (!TryBeginStartup(out VnResult rejection)) return rejection;
            try { return StartRuntimeCore(containmentRoot); }
            finally { starting = false; }
        }

        private VnResult StartRuntimeCore(string containmentRoot)
        {
            if (preparedPackSource != null || preparedPacks != null)
            {
                LastStartupError = HarkenPreparedPacks.Diagnostic;
                return CompleteSessionStartup(VnResult.VN_PACK_INCOMPATIBLE, string.Empty);
            }
            if (string.IsNullOrEmpty(containmentRoot) || !IsSafeRelativePath(initPack))
            {
                LastStartupError = "A Pack root and Init.bytes are required.";
                return CompleteSessionStartup(VnResult.VN_INVALID_ARG, string.Empty);
            }
            return StartResolvedDirectory(containmentRoot);
        }

        public VnResult StartRuntime(HarkenPreparedPacks source)
        {
            if (!TryBeginStartup(out VnResult rejection)) return rejection;
            try { return StartRuntimeCore(source); }
            finally { starting = false; }
        }

        /// <summary>Starts a named publication using the component's device and lifecycle settings.</summary>
        public VnResult StartRuntime(string manifestPath, Func<string, byte[]> readBytes, string selectedLanguage = null)
        {
            if (!TryBeginStartup(out VnResult rejection)) return rejection;
            try
            {
                VnResult result = HarkenPreparedPacks.Capture(manifestPath, readBytes,
                    out HarkenPreparedPacks source, out _, out string error, selectedLanguage ?? language);
                LastStartupError = error ?? string.Empty;
                if (result != VnResult.VN_OK) return CompleteSessionStartup(result, string.Empty);
                return StartRuntimeCore(source);
            }
            finally { starting = false; }
        }

        private bool MatchesConfiguredInit(string selectedPath)
            => initPack == selectedPath;

        private VnResult StartRuntimeCore(HarkenPreparedPacks source)
        {
            LastStartupError = HarkenPreparedPacks.Diagnostic;
            return CompleteSessionStartup(VnResult.VN_PACK_INCOMPATIBLE, string.Empty);
        }

        private bool TryBeginStartup(out VnResult rejection)
        {
            rejection = VnResult.VN_ALREADY_INITIALIZED;
            // A nested request must not recursively publish the same failed event or acquire a second session.
            if (starting || publishingStartupResult || shuttingDown || hasPendingShutdown ||
                hostExiting || disabling || !this) return false;
            if (Instance != this || (session != null && session.IsActive))
            {
                if (session == null || !session.IsActive) State = HarkenRuntimeState.Failed;
                LastStartupError = "This audio system cannot acquire another engine session.";
                PublishStartupResult(rejection);
                return false;
            }
            if (!isActiveAndEnabled) return false;
            starting = true;
            State = HarkenRuntimeState.Starting;
            LastStartupError = string.Empty;
            ApplyProjectStartupPolicy();
            return true;
        }

        private void ApplyProjectStartupPolicy()
        {
            HarkenProjectSettings settings = HarkenProjectSettings.ActiveForStartup();
            if (!lifecycleConfigured)
                suppressUnityAudio = settings.SuppressUnityAudio;
            if (!languageConfigured)
                language = settings.Language;
            if (!platformConfigured)
            {
                packPlatformId = settings.PlatformId;
                packPlatform = settings.PlatformName;
            }
            if (!initPackConfigured)
                initPack = settings.InitPack;
        }

        private VnResult StartFromProjectPackDirectory()
        {
            if (!TryBeginStartup(out VnResult rejection)) return rejection;
            try
            {
                if (!HarkenProjectSettings.TryResolvePackDirectory(out string directory, out string error))
                {
                    LastStartupError = error ?? string.Empty;
                    return CompleteSessionStartup(VnResult.VN_INVALID_ARG, string.Empty);
                }
#if !UNITY_EDITOR
                string platformRoot = ResolvePlatformRoot(directory);
                if (string.IsNullOrEmpty(platformRoot))
                    return CompleteSessionStartup(VnResult.VN_INVALID_ARG, string.Empty);
                if (!HarkenProjectSettings.PlayerIncludesExport(platformRoot, out string exportError))
                {
                    LastStartupError = exportError ?? string.Empty;
                    return CompleteSessionStartup(VnResult.VN_INVALID_ARG, string.Empty);
                }
#endif
                return StartResolvedDirectory(directory);
            }
            finally { starting = false; }
        }

        private VnResult StartResolvedDirectory(string directory)
        {
            string platformRoot = ResolvePlatformRoot(directory);
            if (string.IsNullOrEmpty(platformRoot))
                return CompleteSessionStartup(VnResult.VN_INVALID_ARG, string.Empty);
            return StartCompanionDirectory(platformRoot);
        }

        private VnResult StartCompanionDirectory(string platformRoot)
        {
            if (!MatchesConfiguredInit("Init.bytes"))
            {
                LastStartupError = "A companion directory uses Init.bytes. Update the explicitly configured Init path.";
                return CompleteSessionStartup(VnResult.VN_PACK_STALE_REVISION, string.Empty);
            }
            if (startupPackStableIds != null && startupPackStableIds.Length != 0)
            {
                LastStartupError = "A companion directory starts with Init only. Preload Actions by name with Session.PrepareActions.";
                return CompleteSessionStartup(VnResult.VN_INVALID_ARG, string.Empty);
            }
            VnResult result = CreateStartupConfig(out VnConfig config);
            if (result != VnResult.VN_OK) return CompleteSessionStartup(result, platformRoot);
            result = PackBootstrap.StartDirectory(platformRoot, language, ref config, out session);
            if (result == VnResult.VN_CONTENT_NOT_LOADED && session == null)
                LastStartupError = "The selected directory requires the current Init.bytes and Init.json pair. Re-export Packs with the current Studio.";
            else if (result == VnResult.VN_BAD_PACK || result == VnResult.VN_PACK_INCOMPATIBLE)
                LastStartupError = "Invalid or unsupported Pack publication. Re-export with the current Studio and provide the paired .bytes and .json files.";
            return CompleteSessionStartup(result, platformRoot);
        }

        private VnResult CreateStartupConfig(out VnConfig config)
        {
            config = HarkenEngine.DefaultConfig();
            config.sampleRate = 48000;
            config.mixChannelLayout = (uint)VnChannelLayout.VN_CHANNEL_LAYOUT_STEREO;
            config.deviceChannelLayout = (uint)VnChannelLayout.VN_CHANNEL_LAYOUT_STEREO;
            config.deviceMode = nullDevice
                ? (uint)VnDeviceMode.VN_DEVICE_NULL
                : (uint)VnDeviceMode.VN_DEVICE_REAL;
            HarkenProjectSettings.ApplyStartupCapacity(ref config);
            try
            {
                // Bootstrap recommendations do not change firing capacity. Allocate before native startup.
                managedFirings = new ManagedFiring[checked((int)config.maxActionFirings)];
                notificationRegistration = HarkenCallbacks.InstallNotification(
                    ref config, HandleNotification, this);
                logRegistration = HarkenCallbacks.InstallLog(
                    ref config, HandleLog, this);
                return VnResult.VN_OK;
            }
            catch (OverflowException) { return VnResult.VN_INVALID_CONFIG; }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
        }

        private VnResult CompleteSessionStartup(VnResult result, string platformRoot)
        {
            PackBootstrapSession completedSession = session;
            if (completedSession != null && completedSession.IsActive)
            {
                managedCallerSeed = 0;
                PackRootPath = platformRoot;
                State = HarkenRuntimeState.Ready;
                startupReadyPending = true;
                LastRemoteResult = VnResult.VN_OK;
                LastRemoteError = string.Empty;
                if (HarkenProjectSettings.CommunicationEnabled) RetryRemoteCommunication();
            }
            else
            {
                managedFirings = Array.Empty<ManagedFiring>();
                State = HarkenRuntimeState.Failed;
                DisposeCallbackRegistrations();
            }
            if (result == VnResult.VN_OK) LastStartupError = string.Empty;
            else if (string.IsNullOrEmpty(LastStartupError)) LastStartupError = result.ToString();
            PublishStartupResult(result, completedSession);
            if (completedSession != null && completedSession.IsActive &&
                ReferenceEquals(session, completedSession) && IsPreparationComplete)
                CaptureUnityAudio();
            return result;
        }

        private void PublishStartupResult(VnResult result, PackBootstrapSession completedSession = null)
        {
            publishingStartupResult = true;
            string error = LastStartupError;
            try
            {
                if (completedSession != null && completedSession.IsActive &&
                    ReferenceEquals(session, completedSession)) ApplyApplicationPause();
                ObserveResult("PackBootstrap.Start", result);
                if (result != VnResult.VN_OK) failed.Invoke(error);
                // Canceling asynchronous preparation cannot release this synchronous notification scope.
                PublishReady(completedSession);
            }
            finally { publishingStartupResult = false; }
        }

        private void PublishReady(PackBootstrapSession readySession)
        {
            if (readySession == null || !readySession.IsActive ||
                !ReferenceEquals(session, readySession) || !IsReady ||
                ReferenceEquals(publishingReadySession, readySession)) return;
            PackBootstrapSession previousReadySession = publishingReadySession;
            bool wasPublishingStartupResult = publishingStartupResult;
            publishingReadySession = readySession;
            try
            {
                if (startupReadyPending)
                {
                    startupReadyPending = false;
                    publishingStartupResult = true;
                    ready.Invoke();
                }
                if (readySession.IsActive && ReferenceEquals(session, readySession) && IsReady)
                    BecameReady?.Invoke(this);
            }
            finally
            {
                publishingReadySession = previousReadySession;
                publishingStartupResult = wasPublishingStartupResult;
            }
        }

        private void Awake()
        {
            if (Instance != null && Instance != this)
            {
                State = HarkenRuntimeState.Failed;
                LastOperation = "HarkenAudioSystem singleton ownership";
                LastResult = VnResult.VN_ALREADY_INITIALIZED;
                Debug.LogError("Only one active HarkenAudioSystem is allowed.", this);
                enabled = false;
                return;
            }
            Instance = this;
            gameThreadId = Thread.CurrentThread.ManagedThreadId;
            VariableGameplay.GameThreadProbe = IsGameplayThread;
            VariableGameplay.NotifySessionChanged();
            State = HarkenRuntimeState.Stopped;
            if (persistAcrossScenes)
            {
                DontDestroyOnLoad(gameObject);
            }
            if (!autoInitialize) return;
            if (configuredCompanionSource != null)
            {
                StartConfiguredCompanion();
                return;
            }
            if (preparedPackSource != null || preparedPacks != null)
            {
                StartRuntime((string)null);
                return;
            }
#if UNITY_ANDROID && !UNITY_EDITOR
            // Restore Android directory auto-start after it consumes current-schema companions without a legacy inventory.
            if (!TryBeginStartup(out _)) return;
            try
            {
                LastStartupError = "Android directory auto-start is temporarily unavailable. " +
                    "Use HarkenStreamingPackSource with StartRuntimeAsync or configure a companion source before startup.";
                CompleteSessionStartup(VnResult.VN_CAPABILITY_UNAVAILABLE, string.Empty);
            }
            finally { starting = false; }
#else
            StartFromProjectPackDirectory();
#endif
        }

        private static bool IsSafeRelativePath(string value)
        {
            return PackPaths.IsSafeRelativePath(value);
        }

        private string ResolvePlatformRoot(string exportRoot)
        {
            if (string.IsNullOrEmpty(exportRoot)) return null;
            if (File.Exists(Path.Combine(exportRoot, "Init.json"))) return SelectCompanionDirectory(exportRoot);
            if (!string.IsNullOrEmpty(packPlatform) && PackPaths.IsPlatformName(packPlatform))
            {
                string selected = Path.Combine(exportRoot, packPlatform);
                if (Directory.Exists(selected)) return SelectCompanionDirectory(selected);
            }
            return SelectCompanionDirectory(exportRoot);
        }

        private string SelectCompanionDirectory(string directory)
        {
            if (!string.IsNullOrEmpty(packPlatformId))
            {
                LastStartupError = "A companion directory has no platform ID inventory. Clear the legacy platform ID and select its directory by name.";
                return null;
            }
            if (!string.IsNullOrEmpty(packPlatform) && (!PackPaths.IsPlatformName(packPlatform) ||
                !string.Equals(Path.GetFileName(directory.TrimEnd(Path.DirectorySeparatorChar,
                    Path.AltDirectorySeparatorChar)), packPlatform, StringComparison.Ordinal)))
            {
                LastStartupError = "The companion directory does not match the explicitly selected platform name.";
                return null;
            }
            return directory;
        }

        public static string CurrentPackBasePlatform()
        {
            RuntimePlatform platform = Application.platform;
            if (platform == RuntimePlatform.WindowsPlayer ||
                platform == RuntimePlatform.WindowsEditor)
            {
                return "Windows";
            }
            if (platform == RuntimePlatform.OSXPlayer ||
                platform == RuntimePlatform.OSXEditor)
            {
                return "macOS";
            }
            if (platform == RuntimePlatform.LinuxPlayer || platform == RuntimePlatform.LinuxEditor)
            {
                return "Linux";
            }
            if (platform == RuntimePlatform.Android)
            {
                return "Android";
            }
            if (platform == RuntimePlatform.IPhonePlayer)
            {
                return "iOS";
            }
            return null;
        }

        private void FailStartup(string reason)
        {
            LastStartupError = reason;
            CompleteSessionStartup(VnResult.VN_INVALID_ARG, string.Empty);
        }

        public void RegisterParticipant(IHarkenRuntimeParticipant participant)
        {
            if (participant != null && !participants.Contains(participant))
            {
                participants.Add(participant);
            }
        }

        public void UnregisterParticipant(IHarkenRuntimeParticipant participant)
        {
            participants.Remove(participant);
        }

        internal void QueuePackLoad(HarkenPack pack)
        {
            if (pack != null && pack.LoadOnReady && !pack.IsLoaded &&
                !pendingPackLoads.Contains(pack))
            {
                pendingPackLoads.Add(pack);
            }
        }

        internal void CancelPackLoad(HarkenPack pack)
        {
            pendingPackLoads.Remove(pack);
        }

        internal void TakePackCleanup(Pack pack, PackBootstrapSession owner, ulong stableId, VnResult result)
        {
            if (!pack.IsValid || owner == null || !owner.IsActive || !ReferenceEquals(owner, session)) return;
            // Requests are distinct even when the session shares one native Pack handle.
            packCleanup.Add(new PackCleanup { Pack = pack, Session = owner, StableId = stableId, LastResult = result });
        }

        private void CollectPackCleanup()
        {
            if (collectingPacks) return;
            collectingPacks = true;
            try
            {
                for (int i = packCleanup.Count - 1; i >= 0; --i)
                {
                    PackCleanup pending = packCleanup[i];
                    if (!pending.Session.IsActive)
                    {
                        packCleanup.RemoveAt(i);
                        continue;
                    }
                    if (!ReferenceEquals(pending.Session, session)) continue;
                    VnResult result = pending.Session.ReleasePack(pending.StableId, pending.Pack);
                    if (result == VnResult.VN_OK || result == VnResult.VN_STALE_HANDLE)
                        packCleanup.RemoveAt(i);
                    else
                    {
                        PackCleanup updated = pending;
                        updated.LastResult = result;
                        packCleanup[i] = updated;
                    }
                    if (result != pending.LastResult) ObserveResult("Packs.UnloadPack(cleanup)", result);
                    if (session == null || !session.IsActive || !ReferenceEquals(session, pending.Session)) break;
                }
            }
            finally { collectingPacks = false; }
        }

        internal void TakeFiringCleanup(ActionFiring firing, PackBootstrapSession owner, uint fadeMs,
            ActionPreparation resources = null)
        {
            CollectFiringCleanup();
            if ((!firing.IsValid && resources == null) || owner == null || !owner.IsActive) return;
            foreach (FiringCleanup pending in firingCleanup)
                if (ReferenceEquals(pending.Session, owner) && (firing.IsValid
                    ? pending.Firing.Equals(firing) : ReferenceEquals(pending.Resources, resources))) return;
            // A detached component transfers both owners, including a failed preparation's remaining references.
            firingCleanup.Add(new FiringCleanup { Firing = firing, Session = owner, FadeMs = fadeMs, Resources = resources });
            CollectFiringCleanup();
        }

        private void ReleaseFiringCleanupResources(int index, FiringCleanup pending)
        {
            VnResult result = pending.Resources?.Release() ?? VnResult.VN_OK;
            if (result == VnResult.VN_OK) firingCleanup.RemoveAt(index);
            else
            {
                bool changed = !pending.ResourcesObserved || pending.LastResourcesResult != result;
                pending.ResourcesObserved = true;
                pending.LastResourcesResult = result;
                firingCleanup[index] = pending;
                if (changed) ObserveResult("ActionPreparation.Release(cleanup)", result);
            }
        }

        internal void TakeEmitterCleanup(Emitter emitter, PackBootstrapSession owner, uint fadeMs)
        {
            if (!emitter.IsValid || owner == null || !owner.IsActive || !ReferenceEquals(owner, session)) return;
            foreach (EmitterCleanup pending in emitterCleanup)
                if (ReferenceEquals(pending.Session, owner) && pending.Emitter.Handle == emitter.Handle) return;
            emitterCleanup.Add(new EmitterCleanup { Emitter = emitter, Session = owner, FadeMs = fadeMs });
            CollectEmitterCleanup();
        }

        private void CollectEmitterCleanup()
        {
            if (collectingEmitters) return;
            collectingEmitters = true;
            try
            {
                for (int i = emitterCleanup.Count - 1; i >= 0; --i)
                {
                    EmitterCleanup pending = emitterCleanup[i];
                    if (!pending.Session.IsActive) { emitterCleanup.RemoveAt(i); continue; }
                    if (!ReferenceEquals(pending.Session, session)) continue;
                    VnResult result = pending.StopAccepted ? VnResult.VN_OK : pending.Emitter.Stop(pending.FadeMs);
                    pending.StopAccepted = result == VnResult.VN_OK;
                    if (pending.StopAccepted) result = pending.Emitter.Destroy();
                    bool changed = !pending.Observed || result != pending.LastResult;
                    if (result == VnResult.VN_OK || result == VnResult.VN_STALE_HANDLE)
                        emitterCleanup.RemoveAt(i);
                    else
                    {
                        pending.Observed = true;
                        pending.LastResult = result;
                        emitterCleanup[i] = pending;
                    }
                    if (changed) ObserveResult("Emitter.Destroy(cleanup)", result);
                    if (session == null || !session.IsActive || !ReferenceEquals(session, pending.Session)) break;
                }
            }
            finally { collectingEmitters = false; }
        }

        private void CollectFiringCleanup()
        {
            if (collectingFirings) return;
            collectingFirings = true;
            try
            {
                for (int i = firingCleanup.Count - 1; i >= 0; --i)
                {
                    FiringCleanup pending = firingCleanup[i];
                    if (!pending.Session.IsActive)
                    {
                        firingCleanup.RemoveAt(i);
                        continue;
                    }
                    if (!ReferenceEquals(pending.Session, session)) continue;
                    if (!pending.Firing.IsValid)
                    {
                        ReleaseFiringCleanupResources(i, pending);
                        if (session == null || !session.IsActive || !ReferenceEquals(session, pending.Session)) break;
                        continue;
                    }
                    VnResult result = pending.Firing.Query(out VnActionFiringInfo info);
                    if (result == VnResult.VN_STALE_HANDLE)
                    {
                        pending.Firing = default;
                        ReleaseFiringCleanupResources(i, pending);
                        if (session == null || !session.IsActive || !ReferenceEquals(session, pending.Session)) break;
                        continue;
                    }
                    if (result != VnResult.VN_OK) continue;
                    if (info.state == (uint)VnActionFiringState.VN_ACTION_FIRING_TERMINAL_RETAINED)
                    {
                        result = pending.Firing.Release();
                        if (result == VnResult.VN_OK || result == VnResult.VN_STALE_HANDLE)
                        {
                            pending.Firing = default;
                            ReleaseFiringCleanupResources(i, pending);
                            if (session == null || !session.IsActive || !ReferenceEquals(session, pending.Session)) break;
                        }
                        ObserveResult("ActionFiring.Release(cleanup)", result);
                    }
                    else if (!pending.StopAccepted)
                    {
                        result = pending.Firing.Stop(pending.FadeMs);
                        bool changed = !pending.StopObserved || pending.LastStopResult != result;
                        pending.StopAccepted = result == VnResult.VN_OK;
                        pending.StopObserved = true;
                        pending.LastStopResult = result;
                        firingCleanup[i] = pending;
                        if (changed) ObserveResult("ActionFiring.Stop(cleanup)", result);
                    }
                    // Result listeners may shut down this session and clear its pending ownership.
                    if (session == null || !session.IsActive || !ReferenceEquals(session, pending.Session)) break;
                }
            }
            finally { collectingFirings = false; }
        }

        public VnResult ObserveResult(string operation, VnResult result)
        {
            string observedOperation = operation ?? string.Empty;
            LastOperation = observedOperation;
            LastResult = result;
            string message;
            if (result == VnResult.VN_OK)
            {
                if (!successMessages.TryGetValue(observedOperation, out message))
                {
                    if (successMessages.Count == SuccessMessageCacheCapacity) successMessages.Clear();
                    message = observedOperation + ": VN_OK";
                    successMessages.Add(observedOperation, message);
                }
            }
            else message = observedOperation + ": " + result;
            resultObserved.Invoke(message);
            OperationCompleted?.Invoke(this, observedOperation, result);
            if (result != VnResult.VN_OK)
            {
                Debug.LogError("Harken operation failed: " + message, this);
            }
            return result;
        }

        private static bool IsGameplayThread()
        {
            HarkenAudioSystem owner = Instance;
            return owner != null &&
                Thread.CurrentThread.ManagedThreadId == owner.gameThreadId;
        }

        private void Update()
        {
            PackBootstrapSession updatingSession = session;
            if (updatingSession == null || !updatingSession.IsActive || !ReferenceEquals(session, updatingSession))
            {
                return;
            }
            VnResult spatial = spatialSources.Maintain();
            if (spatial != VnResult.VN_OK) ObserveResult("SpatialSource.Update", spatial);
            if (!updatingSession.IsActive || !ReferenceEquals(session, updatingSession)) return;
            VnResult result = HarkenEngine.Update();
            if (!updatingSession.IsActive || !ReferenceEquals(session, updatingSession)) return;
            if (result != VnResult.VN_OK)
            {
                FailRuntime("HarkenEngine.Update", result);
                return;
            }
            ObserveResult("HarkenEngine.Update", result);
            if (!updatingSession.IsActive || !ReferenceEquals(session, updatingSession)) return;
            if (IsPreparationComplete) CaptureUnityAudio();
            CollectManagedFirings();
            if (!updatingSession.IsActive || !ReferenceEquals(session, updatingSession)) return;
            CollectFiringCleanup();
            if (!updatingSession.IsActive || !ReferenceEquals(session, updatingSession)) return;
            CollectEmitterCleanup();
            if (!updatingSession.IsActive || !ReferenceEquals(session, updatingSession)) return;
            CollectPackCleanup();
            if (!updatingSession.IsActive || !ReferenceEquals(session, updatingSession)) return;
            if (IsReady && pendingPackLoads.Count != 0)
            {
                HarkenPack next = pendingPackLoads[0];
                pendingPackLoads.RemoveAt(0);
                if (next != null && next.isActiveAndEnabled && next.LoadOnReady)
                {
                    next.Load();
                }
            }
        }

        private void OnApplicationPause(bool paused)
        {
#if UNITY_EDITOR
            return;
#else
            HandleApplicationPause(paused);
#endif
        }

        private void HandleApplicationPause(bool paused)
        {
            if (Instance != this || hostExiting) return;
            applicationPaused = paused;
            ApplyApplicationPause();
        }

        private void ApplyApplicationPause()
        {
            if (shuttingDown || session == null || !session.IsActive) return;
            if (applicationPaused == (State == HarkenRuntimeState.Suspended)) return;
            SetSuspended(applicationPaused);
        }

        public VnResult SetSuspended(bool suspended)
        {
            string operation = suspended
                ? "HarkenEngine.Suspend"
                : "HarkenEngine.Resume";
            if (session == null || !session.IsActive)
            {
                return ObserveResult(operation, VnResult.VN_NOT_INITIALIZED);
            }
            bool wasSuspended = State == HarkenRuntimeState.Suspended;
            PackBootstrapSession suspendingSession = session;
            VnResult result = suspended
                ? HarkenEngine.Suspend()
                : HarkenEngine.Resume();
            State = suspended || result != VnResult.VN_OK
                ? HarkenRuntimeState.Suspended
                : HarkenRuntimeState.Ready;
            ObserveResult(operation, result);
            if (wasSuspended && !suspended && result == VnResult.VN_OK &&
                !starting && !publishingStartupResult)
            {
                PublishReady(suspendingSession);
            }
            return result;
        }

        public VnResult ShutdownRuntime()
            => ShutdownRuntime(null, VnResult.VN_OK);

        private VnResult ShutdownRuntime(string failureOperation, VnResult failureResult)
        {
            if (shuttingDown)
            {
                return hasPendingShutdown ? pendingShutdown.TermResult : LastResult;
            }
            shuttingDown = true;
            VariableGameplay.NotifySessionChanged();
            if (failureOperation != null)
            {
                runtimeFailureOperation = failureOperation;
                runtimeFailureResult = failureResult;
            }
            PackBootstrapSession endingSession = session;
            VnResult result;
            try
            {
                CancelCompanionStartup();
                if (startupCoroutine != null)
                {
                    StopCoroutine(startupCoroutine);
                    startupCoroutine = null;
                    starting = false;
                }
                State = HarkenRuntimeState.ShuttingDown;
                participants.Sort((left, right) =>
                    right.ShutdownOrder.CompareTo(left.ShutdownOrder));
                IHarkenRuntimeParticipant[] shutdownParticipants = participants.ToArray();
                foreach (IHarkenRuntimeParticipant participant in shutdownParticipants)
                    participant?.BeforeRuntimeShutdown();
                pendingPackLoads.Clear();

                result = session != null && session.IsActive ? session.Shutdown() : VnResult.VN_OK;
                var completion = new ShutdownCompletion
                {
                    Session = endingSession,
                    State = runtimeFailureOperation != null || result != VnResult.VN_OK
                        ? HarkenRuntimeState.Failed : HarkenRuntimeState.Stopped,
                    Operation = runtimeFailureOperation ?? "PackBootstrapSession.Shutdown",
                    Result = runtimeFailureOperation != null ? runtimeFailureResult : result,
                    TermResult = result,
                    RuntimeFailed = runtimeFailureOperation != null,
                    PublishFailure = failureOperation != null
                };
                if (session != null && session.IsActive)
                {
                    PublishShutdownResult(in completion);
                    return result;
                }
                // Native Term defers destruction while any notification callback is on its stack.
                if (endingSession != null && HarkenEngine.IsUpdating)
                {
                    pendingShutdown = completion;
                    hasPendingShutdown = true;
                    HarkenEngine.OutermostUpdateReturned += CompletePendingShutdown;
                    return result;
                }
                CompleteShutdown(in completion);
            }
            finally { if (!hasPendingShutdown) shuttingDown = false; }
            return result;
        }

        private void CompletePendingShutdown()
        {
            HarkenEngine.OutermostUpdateReturned -= CompletePendingShutdown;
            ShutdownCompletion completion = pendingShutdown;
            pendingShutdown = default;
            hasPendingShutdown = false;
            // A destroyed Unity host still owns these managed resources until native Update returns.
            CompleteShutdown(in completion);
        }

        private void CompleteShutdown(in ShutdownCompletion completion)
        {
            try
            {
                managedFirings = Array.Empty<ManagedFiring>();
                spatialSources.ClearEndedSession();
                firingCleanup.Clear();
                emitterCleanup.Clear();
                packCleanup.Clear();
                session = null;
                startupReadyPending = false;
                DisposeCallbackRegistrations();
                RestoreUnityAudio();
                runtimeFailureOperation = null;
                PublishShutdownResult(in completion);
            }
            finally { shuttingDown = false; }
            if (completion.Session != null)
                RuntimeEnded?.Invoke(this, completion.State, completion.Operation, completion.Result);
        }

        private void PublishShutdownResult(in ShutdownCompletion completion)
        {
            State = completion.State;
            ObserveResult("PackBootstrapSession.Shutdown", completion.TermResult);
            if (!completion.RuntimeFailed) return;
            if (this) enabled = false;
            ObserveResult(completion.Operation, completion.Result);
            if (completion.PublishFailure) failed.Invoke(completion.Operation + ": " + completion.Result);
        }

        private void FailRuntime(string operation, VnResult result)
        {
            ShutdownRuntime(operation, result);
        }

        private void DisposeCallbackRegistrations()
        {
            notificationRegistration?.Dispose();
            notificationRegistration = null;
            logRegistration?.Dispose();
            logRegistration = null;
        }

        private void HandleNotification(
            in VnNotification notification, object state)
        {
            NotificationReceived?.Invoke(in notification, state);
        }

        private void HandleLog(uint level, uint code, string text, object state)
        {
            string message = "[Harken " + level + "/" + code + "] " + text;
            switch ((VnLogLevel)level)
            {
                case VnLogLevel.VN_LOG_WARN:
                    Debug.LogWarning(message, this);
                    break;
                case VnLogLevel.VN_LOG_ERROR:
                    Debug.LogError(message, this);
                    break;
                default:
                    Debug.Log(message, this);
                    break;
            }
        }

        private void OnDisable()
        {
            disabling = true;
            try
            {
                // GameObject deactivation stops coroutines without guaranteeing iterator cleanup.
                // Disabling only this component leaves its coroutine running in Unity.
                if (companionStartup != null || (shutdownWhenDisabled && session != null && session.IsActive) ||
                    (!gameObject.activeInHierarchy && startupCoroutine != null))
                    ShutdownRuntime();
            }
            finally { disabling = false; }
        }

        private void OnDestroy()
        {
            hostExiting = true;
            if (Instance == this)
            {
                ShutdownRuntime();
                VariableGameplay.GameThreadProbe = null;
                Instance = null;
            }
        }

        private void OnApplicationQuit()
        {
            hostExiting = true;
            if (Instance == this)
            {
                ShutdownRuntime();
            }
        }
    }
}
