using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEngine;

namespace Harken.Unity.Editor
{
    public sealed class HarkenEditorPreview : IDisposable
    {
        private static HarkenEditorPreview current;
        private readonly Dictionary<ulong, FiringOwner> firings = new Dictionary<ulong, FiringOwner>();
        private readonly List<Scope> scopes = new List<Scope>();
        private PackBootstrapSession session;
        private HarkenPreparedPacks scopedSource;
        private bool closeWhenIdle;
        private bool stoppingAll;
        private bool ticking;
        private bool scopeBusy;
        private bool shutdownRequested;
        private bool forceNullDevice;
        private ulong seed;
        private Emitter sessionEmitter;
        private Scope listenerOwner;

        private sealed class FiringOwner
        {
            public ActionFiring Firing;
            public string Name;
            public bool StopRequested;
            public bool StopAccepted;
            public uint FadeMs;
            public Scope Owner;
            public HarkenSpatialSource Source;
        }

        public PackBootstrapSession Session => session;
        public bool IsPrepared => session != null && session.IsActive;
        public VnResult LastResult { get; private set; } = VnResult.VN_OK;
        public string Diagnostic { get; private set; } = string.Empty;
        public VnResult LastRemoteResult { get; private set; } = VnResult.VN_OK;
        public int FiringCount => firings.Count;

        public Scope CreateScope()
        {
            var scope = new Scope(this);
            scopes.Add(scope);
            return scope;
        }

        public sealed class Scope : IDisposable
        {
            internal readonly HarkenEditorPreview preview;
            internal PackBootstrapSession capturedSession;
            internal HarkenPreparedPacks source;
            internal string[] preparedNames;
            internal ActionPreparation preparation;
            internal string[] pendingNames;
            internal HarkenPreparedPacks pendingSource;
            internal Emitter stateEmitter;
            internal bool disposed;
            internal bool transitioning;
            internal bool failedResidual;
            internal readonly HarkenSpatialSources spatialSources = new HarkenSpatialSources();
            internal Transform listener;
            internal bool releaseListener;

            internal Scope(HarkenEditorPreview preview) => this.preview = preview;

            public bool IsPrepared { get; internal set; }
            public VnResult LastResult { get; private set; } = VnResult.VN_OK;
            public string Diagnostic { get; private set; } = string.Empty;

            public VnResult SetListener(Transform target)
            {
                if (disposed || !IsPrepared || capturedSession == null || !capturedSession.IsActive)
                    return Observe(VnResult.VN_NOT_INITIALIZED, "<listener>");
                if (preview.ticking || preview.scopeBusy || preview.shutdownRequested ||
                    (preview.listenerOwner != null && !ReferenceEquals(preview.listenerOwner, this)))
                    return Observe(VnResult.VN_IN_USE, "<listener>");
                VnResult result = UnitySpatial.SetListener(target);
                if (result == VnResult.VN_OK)
                {
                    preview.listenerOwner = this;
                    listener = target;
                    releaseListener = false;
                }
                return Observe(result, "<listener>");
            }

            public VnResult CreateSpatialSource(Transform target, out HarkenSpatialSource spatialSource)
            {
                spatialSource = null;
                if (disposed || !IsPrepared || !ReferenceEquals(preview.session, capturedSession))
                    return Observe(VnResult.VN_NOT_INITIALIZED, "<source>");
                if (preview.ticking || preview.scopeBusy || preview.shutdownRequested || transitioning ||
                    !ReferenceEquals(preview.listenerOwner, this) || releaseListener)
                    return Observe(VnResult.VN_IN_USE, "<source>");
                return Observe(spatialSources.Create(this, capturedSession, target, out spatialSource), "<source>");
            }

            public VnResult Prepare(HarkenPreparedPacks source, string[] actionNames)
            {
                // Temporarily block direct API callers as well as the Inspector until current Pack companions are supported.
                if (!HarkenEditorPublicationSupport.IsAvailable)
                    return Observe(VnResult.VN_CAPABILITY_UNAVAILABLE, "<prepare>", HarkenEditorPublicationSupport.Diagnostic);
                if (disposed) return Observe(VnResult.VN_NOT_INITIALIZED, "<prepare>");
                if (preview.shutdownRequested) return Observe(VnResult.VN_NOT_INITIALIZED, "<prepare>");
                if (preview.ticking || preview.scopeBusy) return Observe(VnResult.VN_IN_USE, "<prepare>");
                preview.scopeBusy = true;
                try { return PrepareGuarded(source, actionNames); }
                finally { preview.scopeBusy = false; }
            }

            private VnResult PrepareGuarded(HarkenPreparedPacks source, string[] actionNames)
            {
                if (disposed || preview.shutdownRequested) return Observe(VnResult.VN_NOT_INITIALIZED, "<prepare>");
                VnResult copied = CopyNames(actionNames, out string[] names);
                if (copied != VnResult.VN_OK) return Observe(copied, "<prepare>");
                if (source == null) return Observe(VnResult.VN_CONTENT_NOT_LOADED, "<prepare>");
                if (source.Publication == null) return Observe(VnResult.VN_CAPABILITY_UNAVAILABLE, "<prepare>");
                VnResult valid = ValidateNames(source, names);
                if (valid != VnResult.VN_OK)
                {
                    pendingNames = null;
                    pendingSource = null;
                    IsPrepared = false;
                    preparedNames = null;
                    transitioning = true;
                    preview.StopOwnedFirings(this, 50);
                    return Observe(valid, "<prepare>");
                }
                if (preview.session != null && preview.scopedSource == null)
                    return Observe(VnResult.VN_PACK_STALE_REVISION, "<prepare>");
                if (preview.session != null && preview.scopedSource != null &&
                    !ReferenceEquals(source, preview.scopedSource))
                    return Observe(VnResult.VN_PACK_STALE_REVISION, "<prepare>");
                DropEndedSession();
                if (IsPrepared && !transitioning && pendingNames == null &&
                    ReferenceEquals(source, this.source) && SameNames(names, preparedNames))
                    return Observe(VnResult.VN_OK, "<prepare>");
                if (pendingNames != null && ReferenceEquals(pendingSource, source) && SameNames(names, pendingNames))
                    return Observe(LastResult == VnResult.VN_OK ? VnResult.VN_IN_USE : LastResult, "<prepare>");
                pendingSource = source;
                pendingNames = names;
                this.source = source;
                bool changing = preparedNames != null && !SameNames(preparedNames, names);
                if (changing || (names.Length == 0 && preparation != null))
                {
                    transitioning = true;
                    IsPrepared = false;
                    preview.StopOwnedFirings(this, 50);
                }
                if (preview.HasOwnedFiring(this)) return Observe(VnResult.VN_IN_USE, "<prepare>");
                return preview.PrepareCore(this, names, allowStart: true);
            }

            public VnResult Play(string name, out ActionFiring firing)
                => PlayCore(name, null, out firing);

            public VnResult Play(string name, HarkenSpatialSource spatialSource, out ActionFiring firing)
            {
                firing = default;
                if (spatialSource == null) return Observe(VnResult.VN_INVALID_ARG, name);
                return PlayCore(name, spatialSource, out firing);
            }

            private VnResult PlayCore(string name, HarkenSpatialSource spatialSource, out ActionFiring firing)
            {
                firing = default;
                if (disposed || !IsPrepared || capturedSession == null || !capturedSession.IsActive)
                    return Observe(VnResult.VN_NOT_INITIALIZED, name);
                if (preview.shutdownRequested || preview.stoppingAll || transitioning || pendingNames != null)
                    return Observe(VnResult.VN_IN_USE, name);
                if (preview.ticking || preview.scopeBusy) return Observe(VnResult.VN_IN_USE, name);
                if (!ContainsName(preparedNames, name)) return Observe(VnResult.VN_UNKNOWN_ID, name);
                preview.scopeBusy = true;
                try
                {
                    PackBootstrapSession playingSession = capturedSession;
                    VnResult result = preview.Maintain(allowIdleShutdown: false);
                    if (disposed || preview.shutdownRequested || transitioning || pendingNames != null)
                        return Observe(VnResult.VN_IN_USE, name);
                    if (!playingSession.IsActive || !ReferenceEquals(preview.session, playingSession) ||
                        !ReferenceEquals(capturedSession, playingSession) || !ReferenceEquals(current, preview))
                        return Observe(VnResult.VN_NOT_INITIALIZED, name);
                    if (preview.stoppingAll) return Observe(VnResult.VN_IN_USE, name);
                    if (result != VnResult.VN_OK) return Observe(result, name);
                    if (spatialSource != null && (!ReferenceEquals(preview.listenerOwner, this) || releaseListener))
                        return Observe(VnResult.VN_IN_USE, name);
                    if (spatialSource != null)
                    {
                        result = UnitySpatial.SetListener(listener);
                        if (result != VnResult.VN_OK) return Observe(result, name);
                    }
                    result = Actions.Resolve(name, source.Publication.ProjectId, out ActionResolved resolved);
                    bool retained = false;
                    Emitter emitter = stateEmitter;
                    if (result == VnResult.VN_OK && spatialSource != null)
                    {
                        result = spatialSource.Retain(this, playingSession);
                        retained = result == VnResult.VN_OK;
                        if (retained) result = spatialSource.Validate(this, playingSession, out emitter);
                    }
                    if (result == VnResult.VN_OK)
                    {
                        result = Actions.Post(in resolved, emitter, preview.seed++, out firing);
                        if (result == VnResult.VN_OK)
                            preview.firings.Add(firing.Handle, new FiringOwner
                            {
                                Firing = firing,
                                Name = name,
                                Owner = this,
                                Source = spatialSource
                            });
                    }
                    if (result != VnResult.VN_OK && retained) spatialSource.Release();
                    return Observe(result, name);
                }
                finally { preview.scopeBusy = false; }
            }

            public VnResult Stop(ActionFiring firing, uint fadeMs = 50)
            {
                if (disposed && !preview.HasOwnedFiring(this) && !firing.IsValid)
                    return Observe(VnResult.VN_OK, "<stop>");
                if (preview.ticking || preview.scopeBusy) return Observe(VnResult.VN_IN_USE, "<stop>");
                preview.scopeBusy = true;
                try { return preview.StopOwned(this, firing, fadeMs); }
                finally { preview.scopeBusy = false; }
            }

            public VnResult StopAll(uint fadeMs = 50)
            {
                if (preview.ticking || preview.scopeBusy) return Observe(VnResult.VN_IN_USE, "<stop>");
                preview.scopeBusy = true;
                try
                {
                    spatialSources.EndAll();
                    releaseListener = true;
                    VnResult firstFailure = VnResult.VN_OK;
                    foreach (FiringOwner owner in preview.firings.Values.ToArray())
                    {
                        if (!ReferenceEquals(owner.Owner, this)) continue;
                        VnResult result = preview.RequestStop(owner, fadeMs, closeWhenIdle: false);
                        if (firstFailure == VnResult.VN_OK && result != VnResult.VN_OK) firstFailure = result;
                    }
                    return Observe(firstFailure, "<stop>");
                }
                finally { preview.scopeBusy = false; }
            }

            public void Dispose()
            {
                disposed = true;
                spatialSources.EndAll();
                releaseListener = true;
                pendingNames = null;
                pendingSource = null;
                IsPrepared = false;
                transitioning = true;
                if (preview.ticking || preview.scopeBusy) return;
                preview.scopeBusy = true;
                try
                {
                    preview.StopOwnedFirings(this, 50);
                    if (!preview.HasOwnedFiring(this) && preparation != null && !preparation.IsReleased)
                    {
                        VnResult released = preparation.Release();
                        if (released != VnResult.VN_OK) Observe(released, "<dispose>");
                    }
                }
                finally { preview.scopeBusy = false; }
            }

            internal void DropEndedSession()
            {
                if (capturedSession == null) return;
                if (preview.session != null && capturedSession.IsActive &&
                    ReferenceEquals(capturedSession, preview.session)) return;
                spatialSources.ClearEndedSession();
                stateEmitter = Emitter.None;
                if (ReferenceEquals(preview.listenerOwner, this)) preview.listenerOwner = null;
                listener = null;
                capturedSession = null;
                preparation = null;
                IsPrepared = false;
                preparedNames = null;
                pendingNames = null;
                pendingSource = null;
                transitioning = false;
                failedResidual = false;
            }

            internal VnResult Observe(VnResult result, string name, string detail = null)
            {
                LastResult = result;
                Diagnostic = FormatDiagnostic(result, name, detail);
                return result;
            }
        }

        public VnResult Prepare(string directory, string language, ulong[] startupIds, bool nullDevice = false)
        {
            if (!HarkenEditorPublicationSupport.IsAvailable)
                return Observe(VnResult.VN_CAPABILITY_UNAVAILABLE, "<prepare>", HarkenEditorPublicationSupport.Diagnostic);
            VnResult allowed = CanPrepare();
            if (allowed != VnResult.VN_OK) return Observe(allowed, "<prepare>");
            var catalog = new HarkenEditorCatalog();
            VnResult result = catalog.Refresh(directory, language);
            if (result != VnResult.VN_OK) return Observe(result, "<prepare>", catalog.Diagnostic);
            if (catalog.PreparedPacks != null) return Prepare(catalog.PreparedPacks, startupIds, nullDevice);
            VnConfig config = CreateConfig(nullDevice);
            result = PackBootstrap.Start(Path.Combine(directory, catalog.Publication.InitPack.Path), directory,
                startupIds, language, ref config, out session);
            return Adopt(result);
        }

        public VnResult Prepare(HarkenPackSource source, ulong[] startupIds, bool nullDevice = false)
        {
            if (!HarkenEditorPublicationSupport.IsAvailable)
                return Observe(VnResult.VN_CAPABILITY_UNAVAILABLE, "<prepare>", HarkenEditorPublicationSupport.Diagnostic);
            VnResult allowed = CanPrepare();
            if (allowed != VnResult.VN_OK) return Observe(allowed, "<prepare>");
            if (source == null) return Observe(VnResult.VN_CONTENT_NOT_LOADED, "<prepare>");
            VnResult result = source.Capture(out HarkenPreparedPacks prepared, out string error);
            return result == VnResult.VN_OK ? Prepare(prepared, startupIds, nullDevice) : Observe(result, "<prepare>", error);
        }

        public VnResult Prepare(HarkenPreparedPacks source, ulong[] startupIds, bool nullDevice = false)
        {
            if (!HarkenEditorPublicationSupport.IsAvailable)
                return Observe(VnResult.VN_CAPABILITY_UNAVAILABLE, "<prepare>", HarkenEditorPublicationSupport.Diagnostic);
            VnResult allowed = CanPrepare();
            if (allowed != VnResult.VN_OK) return Observe(allowed, "<prepare>");
            if (source == null) return Observe(VnResult.VN_CONTENT_NOT_LOADED, "<prepare>");
            VnConfig config = CreateConfig(nullDevice);
            return Adopt(source.Start(startupIds, ref config, out session));
        }

        private VnResult CanPrepare()
            => EditorApplication.isPlayingOrWillChangePlaymode || IsPrepared || (current != null && current.IsPrepared)
                ? VnResult.VN_ALREADY_INITIALIZED : VnResult.VN_OK;

        private static VnConfig CreateConfig(bool nullDevice)
        {
            VnConfig config = HarkenEngine.DefaultConfig();
            config.sampleRate = 48000;
            config.mixChannelLayout = (uint)VnChannelLayout.VN_CHANNEL_LAYOUT_STEREO;
            config.deviceChannelLayout = (uint)VnChannelLayout.VN_CHANNEL_LAYOUT_STEREO;
            config.deviceMode = nullDevice ? (uint)VnDeviceMode.VN_DEVICE_NULL : (uint)VnDeviceMode.VN_DEVICE_REAL;
            return config;
        }

        private VnResult Adopt(VnResult result)
        {
            if (IsPrepared)
            {
                current = this;
                closeWhenIdle = false;
                stoppingAll = false;
                shutdownRequested = false;
                EditorApplication.update -= OnEditorUpdate;
                EditorApplication.update += OnEditorUpdate;
                if (session.Publication == null)
                {
                    Shutdown();
                    return Observe(VnResult.VN_CAPABILITY_UNAVAILABLE, "<prepare>");
                }
                LastRemoteResult = HarkenProjectSettings.StartCommunication(session.Publication.ProjectId);
                if (LastRemoteResult != VnResult.VN_OK)
                    UnityEngine.Debug.LogWarning("Harken remote Start: " + LastRemoteResult);
            }
            return Observe(result, "<prepare>");
        }

        public VnResult PrepareStartupPacks()
            => Observe(IsPrepared ? session.PrepareStartupPacks() : VnResult.VN_NOT_INITIALIZED, "<prepare>");

        public VnResult Play(string name, string projectId, out ActionFiring firing)
        {
            firing = default;
            if (!IsPrepared || !ReferenceEquals(current, this)) return Observe(VnResult.VN_NOT_INITIALIZED, name);
            if (stoppingAll || shutdownRequested || ticking) return Observe(VnResult.VN_IN_USE, name);
            PackBootstrapSession playingSession = session;
            VnResult result = Maintain(allowIdleShutdown: false);
            if (!playingSession.IsActive || !ReferenceEquals(session, playingSession) || !ReferenceEquals(current, this))
                return Observe(VnResult.VN_NOT_INITIALIZED, name);
            if (stoppingAll || shutdownRequested) return Observe(VnResult.VN_IN_USE, name);
            if (result != VnResult.VN_OK) return Observe(result, name);
            result = Actions.Resolve(name, projectId, out ActionResolved resolved);
            if (result == VnResult.VN_OK)
            {
                if (!sessionEmitter.IsValid) result = Emitter.CreateStateOnly(out sessionEmitter);
                if (result == VnResult.VN_OK)
                    result = Actions.Post(in resolved, sessionEmitter, seed++, out firing);
                if (result == VnResult.VN_OK)
                    firings.Add(firing.Handle, new FiringOwner { Firing = firing, Name = name });
            }
            return Observe(result, name);
        }

        public VnResult Stop(ActionFiring firing, uint fadeMs = 50)
        {
            if (!IsPrepared) return Observe(VnResult.VN_NOT_INITIALIZED, "<stop>");
            if (!firings.TryGetValue(firing.Handle, out FiringOwner owner)) return Observe(VnResult.VN_STALE_HANDLE, "<stop>");
            return RequestStop(owner, fadeMs, closeWhenIdle: !HasUndisposedScope());
        }

        public VnResult Stop(uint fadeMs = 50)
        {
            if (!IsPrepared) return VnResult.VN_OK;
            stoppingAll = true;
            closeWhenIdle = true;
            shutdownRequested = true;
            CancelAllPending();
            VnResult firstFailure = VnResult.VN_OK;
            foreach (FiringOwner owner in firings.Values.ToArray())
            {
                VnResult result = RequestStop(owner, fadeMs, closeWhenIdle: true);
                if (firstFailure == VnResult.VN_OK && result != VnResult.VN_OK) firstFailure = result;
            }
            if (firings.Count == 0) return Shutdown();
            return firstFailure;
        }

        public VnResult Tick() => Maintain(allowIdleShutdown: true);

        private VnResult Maintain(bool allowIdleShutdown)
        {
            if (!IsPrepared || !ReferenceEquals(current, this)) return VnResult.VN_NOT_INITIALIZED;
            if (ticking) return VnResult.VN_IN_USE;
            PackBootstrapSession tickingSession = session;
            ticking = true;
            try
            {
                MaintainSpatial();
                VnResult updated = HarkenEngine.Update();
                if (!tickingSession.IsActive || !ReferenceEquals(session, tickingSession) || !ReferenceEquals(current, this))
                    return Observe(VnResult.VN_NOT_INITIALIZED, "<update>");
                if (updated != VnResult.VN_OK) return Observe(updated, "<update>");
                foreach (FiringOwner owner in firings.Values.ToArray())
                {
                    VnResult result = owner.Firing.Query(out VnActionFiringInfo info);
                    if (result == VnResult.VN_STALE_HANDLE) { RemoveFiring(owner); continue; }
                    if (result != VnResult.VN_OK) { Observe(result, owner.Name); continue; }
                    if (info.state == (uint)VnActionFiringState.VN_ACTION_FIRING_TERMINAL_RETAINED)
                    {
                        result = owner.Firing.Release();
                        if (result == VnResult.VN_OK) RemoveFiring(owner);
                        else Observe(result, owner.Name);
                    }
                    else if (owner.StopRequested && !owner.StopAccepted)
                    {
                        result = owner.Firing.Stop(owner.FadeMs);
                        owner.StopAccepted = result == VnResult.VN_OK;
                        if (result != VnResult.VN_OK) Observe(result, owner.Name);
                    }
                }
                ApplyDeferredScopeStops();
                ReleaseDueScopes();
                AdvancePendingPrepares();
                RemoveFinishedDisposedScopes();
                if (shutdownRequested) return TrySessionShutdown();
                if (allowIdleShutdown && closeWhenIdle && firings.Count == 0 && !HasUndisposedScope())
                    return Shutdown();
                if (allowIdleShutdown && scopedSource != null && !HasUndisposedScope() &&
                    !HasStandaloneFiring() && AllHeldScopesReleased())
                    return Shutdown();
                return VnResult.VN_OK;
            }
            finally { ticking = false; }
        }

        private void RemoveFiring(FiringOwner owner)
        {
            owner.Source?.Release();
            firings.Remove(owner.Firing.Handle);
        }

        private void MaintainSpatial()
        {
            foreach (Scope scope in scopes)
            {
                if (scope.disposed || shutdownRequested || scope.transitioning)
                {
                    scope.spatialSources.EndAll();
                    scope.releaseListener = true;
                }
                VnResult result = scope.spatialSources.Maintain();
                if (result != VnResult.VN_OK) scope.Observe(result, "<source>");
                if (!ReferenceEquals(listenerOwner, scope)) continue;
                if (scope.releaseListener && !HasOwnedFiring(scope) && scope.spatialSources.Count == 0)
                {
                    listenerOwner = null;
                    scope.listener = null;
                    continue;
                }
                result = UnitySpatial.SetListener(scope.listener);
                if (result != VnResult.VN_OK) scope.Observe(result, "<listener>");
            }
        }

        public VnResult Shutdown()
        {
            shutdownRequested = true;
            CancelAllPending();
            foreach (FiringOwner owner in firings.Values.ToArray())
                RequestStop(owner, owner.StopRequested ? owner.FadeMs : 50, closeWhenIdle: false);
            return TrySessionShutdown();
        }

        public static void ShutdownOwnedSession() => current?.Shutdown();
        private void OnEditorUpdate() => Tick();
        public void Dispose() => Shutdown();

        private VnResult PrepareCore(Scope scope, string[] names, bool allowStart)
        {
            if (scope.disposed || shutdownRequested)
            {
                scope.pendingNames = null;
                scope.pendingSource = null;
                scope.IsPrepared = false;
                if (scope.preparation != null && !scope.preparation.IsReleased) scope.failedResidual = true;
                return scope.Observe(VnResult.VN_NOT_INITIALIZED, "<prepare>");
            }
            if (scope.source == null) return scope.Observe(VnResult.VN_CONTENT_NOT_LOADED, "<prepare>");
            if (session == null || !session.IsActive)
            {
                if (!allowStart)
                {
                    scope.pendingNames = null;
                    scope.pendingSource = null;
                    return scope.Observe(VnResult.VN_NOT_INITIALIZED, "<prepare>");
                }
                if (names.Length == 0)
                {
                    scope.preparedNames = names;
                    scope.IsPrepared = true;
                    scope.pendingNames = null;
                    scope.pendingSource = null;
                    scope.transitioning = false;
                    return scope.Observe(VnResult.VN_OK, "<prepare>");
                }
                VnResult started = Prepare(scope.source, Array.Empty<ulong>(), forceNullDevice);
                if (started != VnResult.VN_OK || !IsPrepared)
                {
                    scope.pendingNames = null;
                    scope.pendingSource = null;
                    return scope.Observe(started, "<prepare>", Diagnostic);
                }
                scopedSource = scope.source;
                scope.capturedSession = session;
            }
            else if (scopedSource == null)
                return scope.Observe(VnResult.VN_PACK_STALE_REVISION, "<prepare>");
            else if (!ReferenceEquals(scope.source, scopedSource))
                return scope.Observe(VnResult.VN_PACK_STALE_REVISION, "<prepare>");
            else scope.capturedSession = session;
            if (scope.disposed || shutdownRequested || session == null || !session.IsActive)
            {
                scope.pendingNames = null;
                scope.pendingSource = null;
                if (scope.preparation != null && !scope.preparation.IsReleased) scope.failedResidual = true;
                scope.IsPrepared = false;
                return scope.Observe(VnResult.VN_NOT_INITIALIZED, "<prepare>");
            }
            if (!scope.stateEmitter.IsValid)
            {
                VnResult created = Emitter.CreateStateOnly(out scope.stateEmitter);
                if (created != VnResult.VN_OK)
                {
                    scope.IsPrepared = false;
                    scope.pendingNames = null;
                    scope.pendingSource = null;
                    return scope.Observe(created, "<prepare>");
                }
            }
            if (HasOwnedFiring(scope)) return scope.Observe(VnResult.VN_IN_USE, "<prepare>");
            if (NeedsRelease(scope) && scope.preparation != null && !scope.preparation.IsReleased)
            {
                VnResult released = scope.preparation.Release();
                if (released != VnResult.VN_OK) return scope.Observe(released, "<prepare>");
                scope.preparation = null;
                scope.failedResidual = false;
            }
            if (names.Length == 0)
            {
                scope.preparedNames = names;
                scope.IsPrepared = true;
                scope.pendingNames = null;
                scope.pendingSource = null;
                scope.transitioning = false;
                scope.failedResidual = false;
                return scope.Observe(VnResult.VN_OK, "<prepare>");
            }
            VnResult result = session.PrepareActions(names, scope.source.Publication.ProjectId,
                out ActionPreparation preparation);
            if (preparation != null) scope.preparation = preparation;
            if (scope.disposed || shutdownRequested || !ReferenceEquals(session, scope.capturedSession) ||
                !session.IsActive)
            {
                scope.IsPrepared = false;
                scope.pendingNames = null;
                scope.pendingSource = null;
                if (scope.preparation != null && !scope.preparation.IsReleased) scope.failedResidual = true;
                return scope.Observe(VnResult.VN_NOT_INITIALIZED, "<prepare>");
            }
            if (result == VnResult.VN_OK)
            {
                scope.preparedNames = names;
                scope.IsPrepared = true;
                scope.pendingNames = null;
                scope.pendingSource = null;
                scope.transitioning = false;
                scope.failedResidual = false;
            }
            else
            {
                scope.IsPrepared = false;
                scope.pendingNames = null;
                scope.pendingSource = null;
                scope.transitioning = false;
                scope.failedResidual = scope.preparation != null && !scope.preparation.IsReleased;
            }
            return scope.Observe(result, "<prepare>");
        }

        private void ApplyDeferredScopeStops()
        {
            foreach (Scope scope in scopes.ToArray())
            {
                if (scope.disposed || shutdownRequested) StopOwnedFirings(scope, 50);
            }
        }

        private void ReleaseDueScopes()
        {
            foreach (Scope scope in scopes.ToArray())
            {
                if (!NeedsRelease(scope) || HasOwnedFiring(scope)) continue;
                if (scope.preparation != null && !scope.preparation.IsReleased)
                {
                    VnResult released = scope.preparation.Release();
                    if (released != VnResult.VN_OK)
                    {
                        scope.Observe(released, "<release>");
                        continue;
                    }
                    scope.failedResidual = false;
                    if (scope.disposed || shutdownRequested) scope.preparation = null;
                }
                if ((scope.disposed || shutdownRequested) && scope.stateEmitter.IsValid)
                {
                    VnResult destroyed = scope.stateEmitter.Destroy();
                    if (destroyed == VnResult.VN_OK || destroyed == VnResult.VN_STALE_HANDLE)
                        scope.stateEmitter = Emitter.None;
                    else scope.Observe(destroyed, "<release-context>");
                }
            }
        }

        private void AdvancePendingPrepares()
        {
            if (shutdownRequested) return;
            foreach (Scope scope in scopes.ToArray())
            {
                if (scope.disposed || scope.pendingNames == null) continue;
                if (session == null || !session.IsActive)
                {
                    scope.pendingNames = null;
                    scope.pendingSource = null;
                    continue;
                }
                if (HasOwnedFiring(scope)) continue;
                if (NeedsRelease(scope) && scope.preparation != null && !scope.preparation.IsReleased) continue;
                PrepareCore(scope, scope.pendingNames, allowStart: false);
            }
        }

        private void RemoveFinishedDisposedScopes()
        {
            for (int i = scopes.Count - 1; i >= 0; --i)
            {
                Scope scope = scopes[i];
                if (!scope.disposed || scope.stateEmitter.IsValid || HasOwnedFiring(scope) || scope.spatialSources.Count != 0 ||
                    ReferenceEquals(listenerOwner, scope)) continue;
                if (scope.preparation != null && !scope.preparation.IsReleased) continue;
                scopes.RemoveAt(i);
            }
        }

        private bool AllHeldScopesReleased()
        {
            foreach (Scope scope in scopes)
            {
                if (scope.spatialSources.Count != 0 || scope.stateEmitter.IsValid) return false;
                if (HasOwnedFiring(scope)) return false;
                if (scope.preparation != null && !scope.preparation.IsReleased) return false;
            }
            return true;
        }

        private bool HasStandaloneFiring()
        {
            foreach (FiringOwner owner in firings.Values)
                if (owner.Owner == null) return true;
            return false;
        }

        private bool HasUndisposedScope()
        {
            foreach (Scope scope in scopes)
                if (!scope.disposed) return true;
            return false;
        }

        private bool HasOwnedFiring(Scope scope)
        {
            foreach (FiringOwner owner in firings.Values)
                if (ReferenceEquals(owner.Owner, scope)) return true;
            return false;
        }

        private static bool NeedsRelease(Scope scope)
            => scope.disposed || scope.preview.shutdownRequested || scope.transitioning || scope.failedResidual ||
                (scope.pendingNames != null && (scope.pendingNames.Length == 0 ||
                    !SameNames(scope.pendingNames, scope.preparedNames)));

        private void StopOwnedFirings(Scope scope, uint fadeMs)
        {
            foreach (FiringOwner owner in firings.Values.ToArray())
            {
                if (!ReferenceEquals(owner.Owner, scope)) continue;
                RequestStop(owner, fadeMs, closeWhenIdle: false);
            }
        }

        private VnResult StopOwned(Scope scope, ActionFiring firing, uint fadeMs)
        {
            if (!firings.TryGetValue(firing.Handle, out FiringOwner owner))
                return scope.Observe(firing.IsValid ? VnResult.VN_STALE_HANDLE : VnResult.VN_OK, "<stop>");
            if (!ReferenceEquals(owner.Owner, scope)) return scope.Observe(VnResult.VN_STALE_HANDLE, "<stop>");
            return scope.Observe(RequestStop(owner, fadeMs, closeWhenIdle: false), owner.Name);
        }

        private VnResult RequestStop(FiringOwner owner, uint fadeMs, bool closeWhenIdle)
        {
            if (closeWhenIdle) this.closeWhenIdle = true;
            owner.StopRequested = true;
            owner.FadeMs = fadeMs;
            VnResult result = owner.Firing.Stop(fadeMs);
            owner.StopAccepted = result == VnResult.VN_OK;
            return Observe(result, owner.Name);
        }

        private void CancelAllPending()
        {
            foreach (Scope scope in scopes)
            {
                scope.pendingNames = null;
                scope.pendingSource = null;
                scope.IsPrepared = false;
            }
        }

        private VnResult TrySessionShutdown()
        {
            if (IsPrepared && firings.Count == 0)
            {
                ReleaseDueScopes();
                if (sessionEmitter.IsValid)
                {
                    VnResult destroyed = sessionEmitter.Destroy();
                    if (destroyed != VnResult.VN_OK && destroyed != VnResult.VN_STALE_HANDLE)
                        return Observe(destroyed, "<release-context>");
                    sessionEmitter = Emitter.None;
                }
            }
            VnResult result = IsPrepared ? session.Shutdown() : VnResult.VN_OK;
            if (!IsPrepared)
            {
                FinishClose();
            }
            return Observe(result, "<shutdown>");
        }

        private void FinishClose()
        {
            session = null;
            sessionEmitter = Emitter.None;
            scopedSource = null;
            firings.Clear();
            foreach (Scope scope in scopes)
            {
                scope.spatialSources.ClearEndedSession();
                scope.stateEmitter = Emitter.None;
                scope.listener = null;
                scope.capturedSession = null;
                scope.preparation = null;
                scope.IsPrepared = false;
                scope.preparedNames = null;
                scope.pendingNames = null;
                scope.pendingSource = null;
                scope.transitioning = false;
                scope.failedResidual = false;
            }
            listenerOwner = null;
            if (ReferenceEquals(current, this)) current = null;
            EditorApplication.update -= OnEditorUpdate;
            stoppingAll = false;
            closeWhenIdle = false;
            shutdownRequested = false;
        }

        private static VnResult CopyNames(string[] actionNames, out string[] copy)
        {
            copy = null;
            if (actionNames == null) return VnResult.VN_INVALID_ARG;
            for (int i = 0; i < actionNames.Length; ++i)
                if (string.IsNullOrEmpty(actionNames[i])) return VnResult.VN_INVALID_ARG;
            copy = (string[])actionNames.Clone();
            return VnResult.VN_OK;
        }

        private static VnResult ValidateNames(HarkenPreparedPacks source, string[] names)
        {
            if (names.Length == 0) return VnResult.VN_OK;
            return source.Publication.GetActionPreparationOrder(names, source.Publication.ProjectId, out _);
        }

        private static bool SameNames(string[] left, string[] right)
        {
            if (left == null || right == null || left.Length != right.Length) return false;
            for (int i = 0; i < left.Length; ++i)
                if (left[i] != right[i]) return false;
            return true;
        }

        private static bool ContainsName(string[] names, string name)
        {
            if (names == null || name == null) return false;
            for (int i = 0; i < names.Length; ++i)
                if (names[i] == name) return true;
            return false;
        }

        internal static string FormatDiagnostic(VnResult result, string name, string detail = null)
        {
            if (result == VnResult.VN_OK) return string.Empty;
            bool lifecycle = !string.IsNullOrEmpty(name) && (name[0] == '<' ||
                result == VnResult.VN_NOT_INITIALIZED || result == VnResult.VN_ALREADY_INITIALIZED ||
                result == VnResult.VN_IN_USE);
            string text = lifecycle
                ? "Preview " + (name[0] == '<' ? name.Trim('<', '>') : "play") +
                    " was refused (" + result + "/" + (int)result + ")."
                : Actions.Describe(result, name);
            return string.IsNullOrEmpty(detail) ? text : text + " " + detail;
        }

        private VnResult Observe(VnResult result, string name, string detail = null)
        {
            LastResult = result;
            Diagnostic = FormatDiagnostic(result, name, detail);
            return result;
        }
    }
}
