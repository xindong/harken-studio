using System;
using UnityEngine;

namespace Harken.Unity
{
    public sealed class HarkenAction : MonoBehaviour, IHarkenRuntimeParticipant
    {
        [SerializeField] private HarkenAudioSystem runtime;
        [SerializeField] private HarkenEmitter emitter;
        [SerializeField] private string actionName = string.Empty;
        [SerializeField] private string projectId = string.Empty;
        [SerializeField] private ulong callerSeed;
        [SerializeField] private bool playOnEnable;
        [SerializeField] private uint stopFadeMs = 50;

        private ActionFiring firing;
        private PackBootstrapSession firingSession;
        private HarkenAudioSystem firingRuntime;
        private ActionPreparation resources;
        private ActionPreparation cachedPreparation;
        private PackBootstrapSession preparationSession;
        private string preparationName;
        private string preparationProject;
        private bool preparing;
        private uint enableGeneration;
        private bool releaseObserved;
        private VnResult lastReleaseResult;
        private enum AutomaticPlayState { Idle, Armed, Consumed }

        private AutomaticPlayState automaticPlay;
        private bool runtimeBound;
        private ActionResolved cachedResolved;
        private bool cacheValid;
        private string cachedActionName;
        private string cachedProjectId;
        private PackBootstrapSession cachedSession;
        private ulong cachedPackVersion;
        private string diagnosticName;
        private VnResult diagnosticResult;
        private string diagnostic;

        public int ShutdownOrder => 300;
        public string ActionName => actionName;
        public string ProjectId => projectId;
        public ActionFiring Firing => firing;
        public bool IsPlaying => firing.IsValid;
        public VnResult LastResult { get; private set; } = VnResult.VN_OK;
        public VnActionFiringInfo LastFiringInfo { get; private set; }

        private void OnEnable()
        {
            ++enableGeneration;
            automaticPlay = playOnEnable
                ? AutomaticPlayState.Armed : AutomaticPlayState.Idle;
            TryBindRuntime();
        }

        private void Update()
        {
            if (!TryBindRuntime())
            {
                return;
            }
            TryAutomaticPlay();
            ForgetTerminatedFiring();
            if (!firing.IsValid && resources != null) ReleaseResources();
            if (!firing.IsValid || firingRuntime == null ||
                !ReferenceEquals(firingSession, firingRuntime.Session))
            {
                return;
            }
            ActionFiring observed = firing;
            PackBootstrapSession observedSession = firingSession;
            VnResult result = observed.Query(out VnActionFiringInfo info);
            firingRuntime.ObserveResult("ActionFiring.Query", result);
            if (result != VnResult.VN_OK || !firing.Equals(observed) ||
                !ReferenceEquals(firingSession, observedSession))
            {
                return;
            }
            LastFiringInfo = info;
            if (info.state ==
                (uint)VnActionFiringState.VN_ACTION_FIRING_TERMINAL_RETAINED)
            {
                ReleaseTerminalFiring();
            }
        }

        private bool TryBindRuntime()
        {
            if (runtimeBound && runtime != null)
            {
                return true;
            }
            runtimeBound = false;
            runtime = runtime != null ? runtime : HarkenAudioSystem.Instance;
            if (runtime == null)
            {
                LastResult = VnResult.VN_NOT_INITIALIZED;
                return false;
            }
            runtime.RegisterParticipant(this);
            runtime.BecameReady += HandleRuntimeReady;
            runtimeBound = true;
            return true;
        }

        private void TryAutomaticPlay()
        {
            if (automaticPlay != AutomaticPlayState.Armed || runtime == null ||
                !runtime.IsPreparationComplete || emitter == null ||
                !emitter.IsCreated)
            {
                return;
            }
            automaticPlay = AutomaticPlayState.Consumed;
            Play();
        }

        public VnResult Play()
        {
            automaticPlay = AutomaticPlayState.Consumed;
            ForgetTerminatedFiring();
            if (runtime == null || !runtime.IsReady ||
                emitter == null || !emitter.IsCreated || firing.IsValid || preparing ||
                resources != null || !isActiveAndEnabled)
            {
                return ObserveAction(VnResult.VN_INVALID_ARG);
            }
            VnResult result;
            preparing = true;
            try { result = PrepareAndPost(); }
            finally { preparing = false; }
            return ObserveAction(result);
        }

        private VnResult PrepareAndPost()
        {
            HarkenAudioSystem host = runtime;
            PackBootstrapSession owner = host.Session;
            HarkenEmitter source = emitter;
            Emitter target = source.Handle;
            uint generation = enableGeneration;
            string name = actionName;
            string project = projectId;

            if (!ReferenceEquals(cachedSession, runtime.Session) ||
                cachedPackVersion != Packs.ChangeVersion ||
                !string.Equals(cachedActionName, actionName, StringComparison.Ordinal) ||
                !string.Equals(cachedProjectId, projectId, StringComparison.Ordinal))
            {
                cacheValid = false;
            }
            VnResult result;
            ActionPreparation prepared;
            if (cachedPreparation != null && ReferenceEquals(preparationSession, owner) &&
                preparationName == name && preparationProject == project)
            {
                prepared = cachedPreparation;
                result = prepared.Prepare();
            }
            else result = owner.PrepareActions(new[] { name }, project, out prepared);
            if (!this || !isActiveAndEnabled || enableGeneration != generation || !owner.IsActive ||
                runtime != host || !ReferenceEquals(host.Session, owner) || emitter != source ||
                source == null || !source.IsCreated || source.Handle.Handle != target.Handle ||
                actionName != name || projectId != project || !host.IsReady)
            {
                host.TakeFiringCleanup(default, owner, stopFadeMs, prepared);
                cacheValid = false;
                return VnResult.VN_NOT_INITIALIZED;
            }
            resources = prepared;
            cachedPreparation = prepared;
            preparationSession = owner;
            preparationName = name;
            preparationProject = project;
            firingSession = owner;
            firingRuntime = host;
            releaseObserved = false;
            if (result == VnResult.VN_OK && (!cacheValid || cachedPackVersion != Packs.ChangeVersion))
            {
                result = Actions.Resolve(name, project, out cachedResolved);
                cacheValid = result == VnResult.VN_OK;
                cachedPackVersion = Packs.ChangeVersion;
                cachedActionName = name;
                cachedProjectId = project;
                cachedSession = owner;
            }
            if (result == VnResult.VN_OK)
                result = Actions.Post(in cachedResolved, target, callerSeed++, out firing);
            // Direct native mutations can bypass the SDK stamp. Refuse once, then refresh on a new request.
            if (result == VnResult.VN_STALE_HANDLE) cacheValid = false;
            if (result != VnResult.VN_OK) ReleaseResources();
            return result;
        }

        public VnResult Stop()
        {
            ForgetTerminatedFiring();
            return !firing.IsValid
                ? Observe("ActionFiring.Stop", VnResult.VN_OK)
                : Observe("ActionFiring.Stop", firing.Stop(stopFadeMs));
        }

        public void BeforeRuntimeShutdown()
        {
            TransferFiringCleanup();
            cacheValid = false;
        }

        private void ForgetTerminatedFiring()
        {
            if (firingSession == null || !firingSession.IsActive)
            {
                firing = default;
                resources = null;
                firingSession = null;
                firingRuntime = null;
            }
        }

        private void TransferFiringCleanup()
        {
            ActionFiring detached = firing;
            PackBootstrapSession owner = firingSession;
            HarkenAudioSystem host = firingRuntime;
            ActionPreparation detachedResources = resources;
            cachedPreparation = null;
            firing = default;
            resources = null;
            firingSession = null;
            firingRuntime = null;
            if (owner != null && owner.IsActive && host != null)
                host.TakeFiringCleanup(detached, owner, stopFadeMs, detachedResources);
        }

        private void ReleaseTerminalFiring()
        {
            VnResult result = firing.Release();
            HarkenAudioSystem owner = firingRuntime;
            if (result == VnResult.VN_OK || result == VnResult.VN_STALE_HANDLE)
            {
                firing = default;
                ReleaseResources();
            }
            owner.ObserveResult("ActionFiring.Release", result);
        }

        private void ReleaseResources()
        {
            HarkenAudioSystem owner = firingRuntime;
            VnResult result = resources?.Release() ?? VnResult.VN_OK;
            if (result == VnResult.VN_OK)
            {
                resources = null;
                firingSession = null;
                firingRuntime = null;
            }
            else
            {
                bool changed = !releaseObserved || lastReleaseResult != result;
                releaseObserved = true;
                lastReleaseResult = result;
                if (changed && owner != null) owner.ObserveResult("ActionPreparation.Release", result);
            }
        }

        private void HandleRuntimeReady(HarkenAudioSystem owner)
        {
            owner.RegisterParticipant(this);
            cacheValid = false;
        }

        private VnResult ObserveAction(VnResult result)
        {
            if (result == VnResult.VN_OK)
                return Observe("Actions.PostResolved", result);
            if (diagnostic == null || diagnosticResult != result ||
                !string.Equals(diagnosticName, actionName, StringComparison.Ordinal))
            {
                diagnosticName = actionName;
                diagnosticResult = result;
                diagnostic = Actions.Describe(result, actionName);
            }
            return Observe(diagnostic, result);
        }

        private void OnValidate()
        {
            cacheValid = false;
        }

        private VnResult Observe(string operation, VnResult result)
        {
            LastResult = result;
            return runtime != null
                ? runtime.ObserveResult(operation, result)
                : result;
        }

        private void OnDisable()
        {
            ++enableGeneration;
            TransferFiringCleanup();
            if (!runtimeBound || runtime == null)
            {
                runtimeBound = false;
                automaticPlay = AutomaticPlayState.Idle;
                cacheValid = false;
                return;
            }
            runtime.BecameReady -= HandleRuntimeReady;
            runtime.UnregisterParticipant(this);
            runtimeBound = false;
            automaticPlay = AutomaticPlayState.Idle;
            cacheValid = false;
        }
    }
}
