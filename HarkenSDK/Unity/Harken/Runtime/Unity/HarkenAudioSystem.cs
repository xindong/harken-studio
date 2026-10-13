using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
#if UNITY_ANDROID && !UNITY_EDITOR
using System.Collections;
using System.Security.Cryptography;
using System.Text;
using UnityEngine.Networking;
#endif
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
    public class HarkenAudioSystem : MonoBehaviour
    {
        private const int SuccessMessageCacheCapacity = 64;
        [SerializeField] private bool autoInitialize = true;
        [SerializeField, HideInInspector] private string packRoot = "Audio/GeneratedPacks";
        [SerializeField, HideInInspector] private string initPack = "Init.pack";
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
        private string androidPackRoot;
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
            return ObserveRemoteResult("Start", HarkenProjectSettings.StartCommunication(session.Publication?.ProjectId));
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
            VnResult result = Actions.Resolve(actionName, projectId, out ActionResolved resolved);
            if (!owner.IsActive || !ReferenceEquals(owner, session))
                return ObserveResult(operation, VnResult.VN_NOT_INITIALIZED);
            if (result == VnResult.VN_OK)
            {
                result = owner.PrepareActions(new[] { actionName }, projectId, out ActionPreparation resources);
                slots[slot].Resources = resources;
            }
            if (!owner.IsActive || !ReferenceEquals(owner, session))
                return ObserveResult(operation, VnResult.VN_NOT_INITIALIZED);
            if (!IsReady || shuttingDown || !IsPreparationComplete)
            {
                ReleaseManagedResources(slots, slot);
                return ObserveResult(operation, !IsReady || shuttingDown
                    ? VnResult.VN_NOT_INITIALIZED : VnResult.VN_CONTENT_NOT_LOADED);
            }
            // Loading Packs changes the native publication generation. Bind only after preparation.
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
                (!string.IsNullOrEmpty(platformId) && !PackManifest.IsPlatformId(platformId)) ||
                (!string.IsNullOrEmpty(platformName) && !PackManifest.IsPlatformName(platformName)) ||
                (!string.IsNullOrEmpty(platformId) && !string.IsNullOrEmpty(platformName)))
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
            if (session != null || State == HarkenRuntimeState.Starting || source == null)
                return ObserveResult("HarkenAudioSystem.ConfigurePreparedSourceBeforeStartup", VnResult.VN_INVALID_ARG);
            preparedPackSource = source;
            preparedPacks = null;
            return ObserveResult("HarkenAudioSystem.ConfigurePreparedSourceBeforeStartup", VnResult.VN_OK);
        }

        public VnResult ConfigurePreparedBytesBeforeStartup(HarkenPreparedPacks source)
        {
            if (session != null || State == HarkenRuntimeState.Starting || source == null)
                return ObserveResult("HarkenAudioSystem.ConfigurePreparedBytesBeforeStartup", VnResult.VN_INVALID_ARG);
            preparedPacks = source;
            preparedPackSource = null;
            return ObserveResult("HarkenAudioSystem.ConfigurePreparedBytesBeforeStartup", VnResult.VN_OK);
        }

#if UNITY_ANDROID && !UNITY_EDITOR
        private sealed class PackagedFile
        {
            public PackagedFile(string sourcePath, string targetPath)
            {
                SourcePath = sourcePath;
                TargetPath = targetPath;
            }

            public string SourcePath { get; }
            public string TargetPath { get; }
        }
#endif

        public VnResult StartRuntime(string containmentRoot)
        {
            if (!TryBeginStartup(out VnResult rejection)) return rejection;
            try { return StartRuntimeCore(containmentRoot); }
            finally { starting = false; }
        }

        private VnResult StartRuntimeCore(string containmentRoot)
        {
            if (preparedPackSource != null)
            {
                VnResult captured = preparedPackSource.Capture(out HarkenPreparedPacks source,
                    out string error, language);
                LastStartupError = error ?? string.Empty;
                return captured == VnResult.VN_OK ? StartRuntimeCore(source)
                    : CompleteSessionStartup(captured, string.Empty);
            }
            if (preparedPacks != null) return StartRuntimeCore(preparedPacks);
            if (string.IsNullOrEmpty(containmentRoot) ||
                !IsSafeRelativePath(initPack))
            {
                LastStartupError = "A Pack root and a safe relative Init Pack path are required.";
                return CompleteSessionStartup(VnResult.VN_INVALID_ARG, string.Empty);
            }
            string platformRoot = ResolvePlatformRoot(containmentRoot);
            if (string.IsNullOrEmpty(platformRoot))
            {
                return CompleteSessionStartup(VnResult.VN_INVALID_ARG, string.Empty);
            }
            if (!PackManifest.TryReadManifest(platformRoot, language, out _,
                    out PackManifestEntry[] manifestPacks, out string selectedLanguage, out ulong initId))
            {
                LastStartupError = "Init Pack is not listed in the selected platform manifest.";
                return CompleteSessionStartup(VnResult.VN_INVALID_ARG, string.Empty);
            }
            PackManifestEntry selectedInit = Array.Find(manifestPacks, entry => entry.PackStableId == initId);
            if (!MatchesConfiguredInit(selectedInit.Path))
            {
                LastStartupError = "Configured Init Pack does not match the selected language publication.";
                return CompleteSessionStartup(VnResult.VN_PACK_STALE_REVISION, string.Empty);
            }
            if (Array.Exists(manifestPacks, entry => HarkenPackFiles.HasWrapper(Path.Combine(platformRoot, entry.Path))))
            {
                VnResult captured = HarkenPreparedPacks.Capture(PackManifest.ManifestFileName,
                    logical => ReadSelectedPack(platformRoot, logical), out HarkenPreparedPacks source,
                    out _, out string error, selectedLanguage);
                LastStartupError = error ?? string.Empty;
                if (captured != VnResult.VN_OK) return CompleteSessionStartup(captured, platformRoot);
                VnResult started = StartRuntimeCore(source);
                if (session != null && session.IsActive) PackRootPath = platformRoot;
                return started;
            }
            string initPath = Path.Combine(platformRoot, selectedInit.Path);
            VnConfig config = CreateStartupConfig();
            VnResult result = PackBootstrap.Start(
                initPath, platformRoot, startupPackStableIds, selectedLanguage,
                ref config, out session);
            return CompleteSessionStartup(result, platformRoot, config.maxActionFirings);
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
            => (!initPackConfigured && initPack == "Init.pack") || initPack == selectedPath;

        private VnResult StartRuntimeCore(HarkenPreparedPacks source)
        {
            if (source == null)
            {
                LastStartupError = "Prepared Pack bytes are required.";
                return CompleteSessionStartup(VnResult.VN_INVALID_ARG, string.Empty);
            }
            if (!MatchesConfiguredInit(source.Publication.InitPack.Path))
            {
                LastStartupError = "Configured Init Pack does not match the selected language publication.";
                return CompleteSessionStartup(VnResult.VN_PACK_STALE_REVISION, string.Empty);
            }
            if (!string.IsNullOrEmpty(language) && source.Publication.Language != language)
            {
                LastStartupError = "Prepared Pack language does not match the configured language.";
                return CompleteSessionStartup(VnResult.VN_PACK_STALE_REVISION, string.Empty);
            }
            if (!PackManifest.TrySelectPlatform(new[] { source.Publication.Platform }, packPlatformId,
                packPlatform, CurrentPackBasePlatform(), out _, out string error))
            {
                LastStartupError = error;
                return CompleteSessionStartup(VnResult.VN_INVALID_ARG, string.Empty);
            }
            VnConfig config = CreateStartupConfig();
            VnResult result = source.Start(startupPackStableIds, ref config, out session);
            return CompleteSessionStartup(result, string.Empty, config.maxActionFirings);
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
            if (!PackManifest.TryReadManifest(platformRoot, language, out _,
                    out PackManifestEntry[] manifestPacks, out string selectedLanguage, out ulong initId))
            {
                LastStartupError = "Init Pack is not listed in the selected platform manifest.";
                return CompleteSessionStartup(VnResult.VN_INVALID_ARG, string.Empty);
            }
            PackManifestEntry selectedInit = Array.Find(manifestPacks, entry => entry.PackStableId == initId);
            if (!MatchesConfiguredInit(selectedInit.Path))
            {
                LastStartupError = "Configured Init Pack does not match the selected language publication.";
                return CompleteSessionStartup(VnResult.VN_PACK_STALE_REVISION, string.Empty);
            }
            string relativeManifest = RelativePackPath(directory, Path.Combine(platformRoot, PackManifest.ManifestFileName));
            if (!PackManifest.IsSafeRelativePath(relativeManifest))
            {
                LastStartupError = "The selected GeneratedPacks directory does not contain a readable manifest.";
                return CompleteSessionStartup(VnResult.VN_INVALID_ARG, string.Empty);
            }
            VnResult captured = HarkenPreparedPacks.Capture(relativeManifest,
                logical => ReadSelectedPack(directory, logical),
                out HarkenPreparedPacks source, out _, out string captureError, selectedLanguage ?? language);
            LastStartupError = captureError ?? string.Empty;
            if (captured != VnResult.VN_OK)
                return CompleteSessionStartup(captured, string.Empty);
            VnResult started = StartRuntimeCore(source);
            if (session != null && session.IsActive)
                PackRootPath = platformRoot;
            return started;
        }

        private static byte[] ReadSelectedPack(string root, string logical)
        {
            if (!PackManifest.TryJoinContainedPath(root, logical, out string full))
                throw new IOException("Pack path leaves the selected directory.");
            return HarkenPackFiles.Read(full);
        }

        private static string RelativePackPath(string root, string fullPath)
        {
            string relative = Path.GetRelativePath(root, fullPath).Replace('\\', '/');
            return relative == "." ? PackManifest.ManifestFileName : relative;
        }

        private VnConfig CreateStartupConfig()
        {
            VnConfig config = HarkenEngine.DefaultConfig();
            config.sampleRate = 48000;
            config.mixChannelLayout = (uint)VnChannelLayout.VN_CHANNEL_LAYOUT_STEREO;
            config.deviceChannelLayout = (uint)VnChannelLayout.VN_CHANNEL_LAYOUT_STEREO;
            config.deviceMode = nullDevice
                ? (uint)VnDeviceMode.VN_DEVICE_NULL
                : (uint)VnDeviceMode.VN_DEVICE_REAL;
            HarkenProjectSettings.ApplyStartupCapacity(ref config);
            notificationRegistration = HarkenCallbacks.InstallNotification(
                ref config, HandleNotification, this);
            logRegistration = HarkenCallbacks.InstallLog(
                ref config, HandleLog, this);
            return config;
        }

        private VnResult CompleteSessionStartup(VnResult result, string platformRoot, uint firingCapacity = 0)
        {
            PackBootstrapSession completedSession = session;
            if (completedSession != null && completedSession.IsActive)
            {
                managedFirings = new ManagedFiring[checked((int)firingCapacity)];
                managedCallerSeed = 0;
                PackRootPath = platformRoot;
                State = HarkenRuntimeState.Ready;
                startupReadyPending = true;
                LastRemoteResult = VnResult.VN_OK;
                LastRemoteError = string.Empty;
                if (HarkenProjectSettings.CommunicationEnabled) RetryRemoteCommunication();
                ApplyApplicationPause();
            }
            else
            {
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
            if (preparedPackSource != null || preparedPacks != null)
            {
                StartRuntime((string)null);
                return;
            }
#if UNITY_ANDROID && !UNITY_EDITOR
            if (!HarkenProjectSettings.TryResolveStreamingPackDirectory(
                HarkenProjectSettings.ActiveForStartup().PackDirectory, Application.streamingAssetsPath,
                out androidPackRoot, out string directoryError))
            {
                if (!TryBeginStartup(out _)) return;
                try
                {
                    LastStartupError = string.IsNullOrEmpty(directoryError)
                        ? "The selected GeneratedPacks directory is not readable on this player."
                        : directoryError;
                    CompleteSessionStartup(VnResult.VN_INVALID_ARG, string.Empty);
                }
                finally { starting = false; }
                return;
            }
            if (TryBeginStartup(out _)) startupCoroutine = StartCoroutine(StartAndroidRuntime());
#else
            StartFromProjectPackDirectory();
#endif
        }


        private static bool IsSafeRelativePath(string value)
        {
            return PackManifest.IsSafeRelativePath(value);
        }

        private string ResolvePlatformRoot(string exportRoot)
        {
            if (string.IsNullOrEmpty(exportRoot))
            {
                return null;
            }
            if (PackManifest.TryReadPlatformDirectory(exportRoot, packPlatformId, packPlatform,
                    CurrentPackBasePlatform(), language, out string platformDirectory, out string error))
            {
                LastStartupError = string.Empty;
                return platformDirectory;
            }
            LastStartupError = error;
            return null;
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
            if (pack != null && !pack.IsLoaded &&
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

#if UNITY_ANDROID && !UNITY_EDITOR
        private IEnumerator StartAndroidRuntime()
        {
            try { yield return PrepareAndroidRuntime(); }
            finally
            {
                starting = false;
                startupCoroutine = null;
            }
        }

        private IEnumerator PrepareAndroidRuntime()
        {
            string sourceRoot = Application.streamingAssetsPath.TrimEnd('/', '\\');
            string normalizedPackRoot = androidPackRoot.Replace('\\', '/').Trim('/');
            byte[] indexBytes = null;
            yield return DownloadPackagedFile(sourceRoot,
                normalizedPackRoot + "/" + PackManifest.IndexFileName,
                value => indexBytes = value);
            if (indexBytes == null)
            {
                yield break;
            }
            if (!PackManifest.TryParseIndex(
                    Encoding.UTF8.GetString(indexBytes),
                    out PackIndexEntry[] platforms))
            {
                FailStartup("invalid Pack index");
                yield break;
            }
            if (!PackManifest.TrySelectPlatform(platforms, packPlatformId, packPlatform,
                    CurrentPackBasePlatform(), out PackIndexEntry selected, out string selectionError))
            {
                FailStartup(selectionError);
                yield break;
            }
            string platformRelative = normalizedPackRoot + "/" + selected.Path;
            if (!HarkenProjectSettings.PlayerIncludesExport(selected.Path, out string exportError))
            {
                FailStartup(exportError);
                yield break;
            }

            string manifestSourcePath =
                platformRelative + "/" + PackManifest.ManifestFileName;
            byte[] manifestBytes = null;
            yield return DownloadPackagedFile(
                sourceRoot, manifestSourcePath, value => manifestBytes = value);
            if (manifestBytes == null)
            {
                yield break;
            }
            string inventoryJson = Encoding.UTF8.GetString(manifestBytes);
            if (!PackManifest.TryParseManifest(inventoryJson,
                    out PackIndexEntry manifestPlatform, out PackManifestEntry[] packs) ||
                !PackManifest.MatchesPlatform(selected, manifestPlatform))
            {
                FailStartup("Pack index and manifest platform identities do not match");
                yield break;
            }

            if (!PackManifest.TryGetSelectedManifestPath(inventoryJson, language, out string selectedManifestPath))
            {
                FailStartup("Invalid selected publication language");
                yield break;
            }
            byte[] selectedManifestBytes = null;
            if (selectedManifestPath != null)
            {
                yield return DownloadPackagedFile(sourceRoot, platformRelative + "/" + selectedManifestPath,
                    value => selectedManifestBytes = value);
                if (selectedManifestBytes == null) yield break;
                if (!PackManifest.TryParseManifest(Encoding.UTF8.GetString(selectedManifestBytes),
                        out PackIndexEntry languagePlatform, out packs) ||
                    !PackManifest.MatchesPlatform(selected, languagePlatform))
                {
                    FailStartup("Selected publication platform does not match the Pack index");
                    yield break;
                }
            }

            if ((initPackConfigured || initPack != "Init.pack") && !Array.Exists(packs, entry => entry.Path == initPack))
            {
                FailStartup("Init Pack is not listed in the selected platform manifest");
                yield break;
            }
            var packagedFiles = new List<PackagedFile>();
            foreach (PackManifestEntry entry in packs)
            {
                if (!TryAddManifestPack(
                        packagedFiles, platformRelative, entry.Path))
                {
                    yield break;
                }
            }

            int selectedLength = selectedManifestBytes != null ? selectedManifestBytes.Length : 0;
            byte[] cacheIdentity = new byte[indexBytes.Length + manifestBytes.Length + selectedLength];
            Buffer.BlockCopy(indexBytes, 0, cacheIdentity, 0, indexBytes.Length);
            Buffer.BlockCopy(manifestBytes, 0, cacheIdentity, indexBytes.Length, manifestBytes.Length);
            if (selectedLength != 0)
                Buffer.BlockCopy(selectedManifestBytes, 0, cacheIdentity,
                    indexBytes.Length + manifestBytes.Length, selectedLength);
            string cacheKey = ComputeCacheKey(cacheIdentity);
            if (platformRelative.Length <= normalizedPackRoot.Length + 1)
            {
                FailStartup("Pack index path is not nested under Pack root");
                yield break;
            }
            string exportCacheRoot = Path.Combine(
                Application.persistentDataPath, "HarkenPackCache", cacheKey);
            string platformCacheRoot = Path.Combine(
                exportCacheRoot,
                platformRelative.Substring(normalizedPackRoot.Length + 1)
                    .Replace('/', Path.DirectorySeparatorChar));
            string readyPath = Path.Combine(platformCacheRoot, ".ready");
            if (!IsPreparedCache(
                    platformCacheRoot, readyPath, cacheKey, packagedFiles))
            {
                if (!TryResetDirectory(platformCacheRoot))
                {
                    yield break;
                }

                foreach (PackagedFile file in packagedFiles)
                {
                    byte[] packBytes = null;
                    yield return DownloadPackagedFile(
                        sourceRoot, file.SourcePath,
                        value => packBytes = value);
                    PackManifestEntry expected = Array.Find(packs, entry => entry.Path == file.TargetPath);
                    if (packBytes != null && !PackManifest.VerifyPackBytes(packBytes, expected))
                    {
                        FailStartup("Pack bytes do not match the selected platform manifest: " + file.TargetPath);
                        TryDeleteDirectory(platformCacheRoot);
                        yield break;
                    }
                    if (packBytes == null ||
                        !TryWriteContainedFile(
                            platformCacheRoot, file.TargetPath, packBytes))
                    {
                        TryDeleteDirectory(platformCacheRoot);
                        yield break;
                    }
                }
                if ((indexBytes != null &&
                     !TryWriteContainedFile(
                         exportCacheRoot, PackManifest.IndexFileName, indexBytes)) ||
                    !TryWriteContainedFile(
                        platformCacheRoot, PackManifest.ManifestFileName,
                        manifestBytes) ||
                    (selectedManifestBytes != null && !TryWriteContainedFile(
                        platformCacheRoot, selectedManifestPath, selectedManifestBytes)) ||
                    !TryWriteReadyFile(readyPath, cacheKey))
                {
                    TryDeleteDirectory(platformCacheRoot);
                    yield break;
                }
            }
            StartRuntimeCore(exportCacheRoot);
        }

        private IEnumerator DownloadPackagedFile(
            string sourceRoot, string relativePath, Action<byte[]> complete)
        {
            string url = sourceRoot.TrimEnd('/') + "/" +
                string.Join("/", Array.ConvertAll(relativePath.Replace('\\', '/').Split('/'), Uri.EscapeDataString));
            using (UnityWebRequest request = UnityWebRequest.Get(url))
            {
                yield return request.SendWebRequest();
                if (request.result != UnityWebRequest.Result.Success)
                {
                    FailStartup(
                        "cannot read packaged Pack " + relativePath + ": " +
                        request.error);
                    complete(null);
                    yield break;
                }
                complete(request.downloadHandler.data);
            }
        }

        private bool TryAddManifestPack(
            List<PackagedFile> files, string normalizedPackRoot,
            string manifestPath)
        {
            if (!IsSafeRelativePath(manifestPath))
            {
                FailStartup("invalid packaged Pack path");
                return false;
            }
            string normalizedPath = manifestPath.Replace('\\', '/');
            string prefix = normalizedPackRoot + "/";
            bool includesPackRoot = normalizedPath.StartsWith(
                prefix, StringComparison.Ordinal);
            string sourcePath = includesPackRoot
                ? normalizedPath
                : prefix + normalizedPath;
            string targetPath = includesPackRoot
                ? normalizedPath.Substring(prefix.Length)
                : normalizedPath;
            return TryAddPackagedFile(
                files, sourcePath, targetPath);
        }

        private bool TryAddPackagedFile(
            List<PackagedFile> files, string sourcePath, string targetPath)
        {
            if (!IsSafeRelativePath(sourcePath) ||
                !IsSafeRelativePath(targetPath))
            {
                FailStartup("invalid packaged Pack path");
                return false;
            }
            foreach (PackagedFile file in files)
            {
                if (!string.Equals(
                        file.TargetPath, targetPath, StringComparison.Ordinal))
                {
                    continue;
                }
                if (!string.Equals(
                        file.SourcePath, sourcePath, StringComparison.Ordinal))
                {
                    FailStartup("duplicate packaged Pack destination");
                    return false;
                }
                return true;
            }
            files.Add(new PackagedFile(sourcePath, targetPath));
            return true;
        }

        private static bool IsPreparedCache(
            string targetRoot, string readyPath, string cacheKey,
            List<PackagedFile> files)
        {
            try
            {
                if (!File.Exists(readyPath) ||
                    File.ReadAllText(readyPath, Encoding.ASCII) != cacheKey)
                {
                    return false;
                }
                foreach (PackagedFile file in files)
                {
                    if (!File.Exists(Path.Combine(targetRoot, file.TargetPath)))
                    {
                        return false;
                    }
                }
                return true;
            }
            catch
            {
                return false;
            }
        }

        private bool TryResetDirectory(string path)
        {
            try
            {
                if (Directory.Exists(path))
                {
                    Directory.Delete(path, true);
                }
                Directory.CreateDirectory(path);
                return true;
            }
            catch (Exception error)
            {
                FailStartup("cannot prepare Pack cache: " + error.Message);
                return false;
            }
        }

        private void TryDeleteDirectory(string path)
        {
            try
            {
                if (Directory.Exists(path))
                {
                    Directory.Delete(path, true);
                }
            }
            catch (Exception error)
            {
                Debug.LogError("Harken Pack cache cleanup failed: " + error.Message);
            }
        }

        private bool TryWriteContainedFile(
            string root, string relativePath, byte[] bytes)
        {
            try
            {
                string fullRoot = Path.GetFullPath(root).TrimEnd(
                    Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) +
                    Path.DirectorySeparatorChar;
                string destination = Path.GetFullPath(
                    Path.Combine(fullRoot, relativePath));
                StringComparison comparison = Path.DirectorySeparatorChar == '\\'
                    ? StringComparison.OrdinalIgnoreCase
                    : StringComparison.Ordinal;
                if (!destination.StartsWith(fullRoot, comparison))
                {
                    FailStartup("packaged Pack path escapes cache root");
                    return false;
                }
                Directory.CreateDirectory(Path.GetDirectoryName(destination));
                File.WriteAllBytes(destination, bytes);
                return true;
            }
            catch (Exception error)
            {
                FailStartup("cannot write Pack cache: " + error.Message);
                return false;
            }
        }

        private bool TryWriteReadyFile(string path, string cacheKey)
        {
            try
            {
                File.WriteAllText(path, cacheKey, Encoding.ASCII);
                return true;
            }
            catch (Exception error)
            {
                FailStartup("cannot publish Pack cache: " + error.Message);
                return false;
            }
        }

        private static string ComputeCacheKey(byte[] manifestBytes)
        {
            byte[] digest;
            using (SHA256 sha256 = SHA256.Create())
            {
                byte[] buildBytes = Encoding.UTF8.GetBytes(Application.buildGUID);
                sha256.TransformBlock(
                    buildBytes, 0, buildBytes.Length, buildBytes, 0);
                sha256.TransformFinalBlock(
                    manifestBytes, 0, manifestBytes.Length);
                digest = sha256.Hash;
            }
            var result = new StringBuilder(digest.Length * 2);
            foreach (byte value in digest)
            {
                result.Append(value.ToString("x2"));
            }
            return result.ToString();
        }
#endif

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
                if (next != null && next.isActiveAndEnabled)
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
            Debug.Log("[Harken " + level + "/" + code + "] " + text, this);
        }

        private void OnDisable()
        {
            disabling = true;
            try
            {
                // GameObject deactivation stops coroutines without guaranteeing iterator cleanup.
                // Disabling only this component leaves its coroutine running in Unity.
                if ((shutdownWhenDisabled && session != null && session.IsActive) ||
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
