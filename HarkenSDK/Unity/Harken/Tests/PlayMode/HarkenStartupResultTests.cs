using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Net;
using System.Net.Sockets;
using System.Text.RegularExpressions;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.Events;
using UnityEngine.TestTools;
using Object = UnityEngine.Object;

namespace Harken.Unity.Tests
{
    public sealed class HarkenStartupResultTests
    {
        [UnityTest]
        public IEnumerator ApplicationPauseWithoutSessionPreservesDiagnosticsAndExplicitErrors()
        {
            using (var fixture = new Fixture())
            {
                fixture.Activate();
                int observed = 0;
                fixture.Runtime.OperationCompleted += (_, __, ___) => ++observed;
                NotifyApplicationPause(fixture.Runtime, false);
                NotifyApplicationPause(fixture.Runtime, true);
                NotifyApplicationPause(fixture.Runtime, false);
                Assert.That(observed, Is.Zero);
                Assert.That(fixture.Runtime.State, Is.EqualTo(HarkenRuntimeState.Stopped));
                LogAssert.Expect(LogType.Error, new Regex("HarkenEngine.Suspend: VN_NOT_INITIALIZED"));
                Assert.That(fixture.Runtime.SetSuspended(true), Is.EqualTo(VnResult.VN_NOT_INITIALIZED));
                ExpectFailure(VnResult.VN_INVALID_ARG);
                Assert.That(fixture.Runtime.StartRuntime((PackSource)null), Is.EqualTo(VnResult.VN_INVALID_ARG));
                int failureObservations = observed;
                NotifyApplicationPause(fixture.Runtime, true);
                NotifyApplicationPause(fixture.Runtime, false);
                Assert.That(observed, Is.EqualTo(failureObservations));
                fixture.AssertFailed(VnResult.VN_INVALID_ARG, 1);
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.IsReady, Is.True);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator PausedStartupDefersReadinessAndPreservesPauseAcrossRestart()
        {
            using (var fixture = new Fixture())
            {
                fixture.Activate();
                var notifications = new List<string>();
                Get<UnityEvent>(fixture.Runtime, "ready").AddListener(() => notifications.Add("ready"));
                fixture.Runtime.BecameReady += _ => notifications.Add("BecameReady");
                int operations = 0;
                fixture.Runtime.OperationCompleted += (_, __, ___) => ++operations;
                NotifyApplicationPause(fixture.Runtime, true);
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.State, Is.EqualTo(HarkenRuntimeState.Suspended));
                Assert.That(fixture.Runtime.IsPreparationComplete, Is.False);
                Assert.That(notifications, Is.Empty);
                int startupOperations = operations;
                NotifyApplicationPause(fixture.Runtime, true);
                Assert.That(operations, Is.EqualTo(startupOperations));
                NotifyApplicationPause(fixture.Runtime, false);
                Assert.That(notifications, Is.EqualTo(new[] { "ready", "BecameReady" }));
                Assert.That(fixture.Runtime.IsPreparationComplete, Is.True);
                int resumedOperations = operations;
                NotifyApplicationPause(fixture.Runtime, false);
                Assert.That(operations, Is.EqualTo(resumedOperations));
                NotifyApplicationPause(fixture.Runtime, true);
                NotifyApplicationPause(fixture.Runtime, false);
                Assert.That(notifications, Is.EqualTo(new[] { "ready", "BecameReady", "BecameReady" }));
                NotifyApplicationPause(fixture.Runtime, true);
                Assert.That(fixture.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.State, Is.EqualTo(HarkenRuntimeState.Suspended));
                Assert.That(notifications.Count, Is.EqualTo(3));
                NotifyApplicationPause(fixture.Runtime, false);
                Assert.That(notifications, Is.EqualTo(new[] { "ready", "BecameReady", "BecameReady", "ready", "BecameReady" }));
            }
            yield return null;
        }



        [UnityTest]
        public IEnumerator ApplicationPauseDuringPreparationUsesLatestState()
        {
            foreach (bool pausedAtCompletion in new[] { false, true })
            {
                using (var fixture = new Fixture())
                {
                    fixture.Activate();
                    bool readStarted = false;
                    Assert.That(fixture.Runtime.StartRuntime(PackSource.FromReader(path =>
                    {
                        Assert.That(fixture.Runtime.State, Is.EqualTo(HarkenRuntimeState.Starting));
                        readStarted = true;
                        NotifyApplicationPause(fixture.Runtime, true);
                        NotifyApplicationPause(fixture.Runtime, false);
                        NotifyApplicationPause(fixture.Runtime, pausedAtCompletion);
                        return File.ReadAllBytes(Path.Combine(fixture.Root, path));
                    })), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(readStarted, Is.True);
                    Assert.That(fixture.Runtime.State, Is.EqualTo(pausedAtCompletion
                        ? HarkenRuntimeState.Suspended : HarkenRuntimeState.Ready));
                    Assert.That(fixture.ReadyCount, Is.EqualTo(pausedAtCompletion ? 0 : 1));
                    NotifyApplicationPause(fixture.Runtime, false);
                    Assert.That(fixture.StartupReadyCount, Is.EqualTo(1));
                    Assert.That(fixture.ReadyCount, Is.EqualTo(1));
                }
                yield return null;
            }
        }

        [UnityTest]
        public IEnumerator EarlySuspendObserverCannotPublishReadyBeforeStartupResult()
        {
            using (var fixture = new Fixture())
            {
                fixture.Activate();
                NotifyApplicationPause(fixture.Runtime, true);
                var notifications = new List<string>();
                fixture.Runtime.OperationCompleted += (owner, operation, result) =>
                {
                    notifications.Add(operation);
                    if (operation == "HarkenEngine.Suspend")
                        Assert.That(owner.SetSuspended(false), Is.EqualTo(VnResult.VN_OK));
                };
                Get<UnityEvent>(fixture.Runtime, "ready").AddListener(() => notifications.Add("ready"));
                fixture.Runtime.BecameReady += _ => notifications.Add("BecameReady");
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(notifications, Is.EqualTo(new[]
                {
                    "HarkenEngine.Suspend", "HarkenEngine.Resume", "PackBootstrap.Start", "ready", "BecameReady"
                }));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator EarlySuspendObserverCannotReenterAfterCancelingOwnedPreparation()
        {
            using (var fixture = new Fixture())
            {
                fixture.Activate();
                NotifyApplicationPause(fixture.Runtime, true);
                bool canceled = false;
                VnResult nestedResult = VnResult.VN_OK;
                fixture.Runtime.OperationCompleted += (owner, operation, result) =>
                {
                    if (operation != "HarkenEngine.Suspend" || canceled) return;
                    canceled = true;
                    Set(owner, "startupCoroutine", owner.StartCoroutine(WaitWithoutCleanup(() => { })));
                    Assert.That(owner.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(Get<bool>(owner, "starting"), Is.False);
                    nestedResult = owner.StartRuntime(fixture.Prepared);
                };
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(canceled, Is.True);
                Assert.That(nestedResult, Is.EqualTo(VnResult.VN_ALREADY_INITIALIZED));
                Assert.That(fixture.Runtime.Session, Is.Null);
                Assert.That(fixture.StartupReadyCount, Is.Zero);
                Assert.That(fixture.ReadyCount, Is.Zero);
                NotifyApplicationPause(fixture.Runtime, false);
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.StartupReadyCount, Is.EqualTo(1));
                Assert.That(fixture.ReadyCount, Is.EqualTo(1));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator DeferredReadyCanPauseAndResumeWithoutDuplicateNotifications()
        {
            using (var fixture = new Fixture())
            {
                fixture.Activate();
                NotifyApplicationPause(fixture.Runtime, true);
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                Get<UnityEvent>(fixture.Runtime, "ready").AddListener(() =>
                {
                    NotifyApplicationPause(fixture.Runtime, true);
                    NotifyApplicationPause(fixture.Runtime, false);
                });
                NotifyApplicationPause(fixture.Runtime, false);
                Assert.That(fixture.Runtime.IsReady, Is.True);
                Assert.That(fixture.StartupReadyCount, Is.EqualTo(1));
                Assert.That(fixture.ReadyCount, Is.EqualTo(1));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator DeferredStartupNotificationsRetainSessionAcquisitionGuard()
        {
            foreach (bool useBecameReady in new[] { false, true })
            {
                using (var fixture = new Fixture())
                {
                    fixture.Activate();
                    NotifyApplicationPause(fixture.Runtime, true);
                    Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                    bool called = false;
                    System.Action stopAndRetry = () =>
                    {
                        called = true;
                        Assert.That(fixture.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                        Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_ALREADY_INITIALIZED));
                    };
                    if (useBecameReady) fixture.Runtime.BecameReady += _ => stopAndRetry();
                    else Get<UnityEvent>(fixture.Runtime, "ready").AddListener(() => stopAndRetry());
                    NotifyApplicationPause(fixture.Runtime, false);
                    Assert.That(called, Is.True);
                    Assert.That(fixture.Runtime.Session, Is.Null);
                    Assert.That(fixture.Runtime.State, Is.EqualTo(HarkenRuntimeState.Stopped));
                    Assert.That(fixture.StartupReadyCount, Is.EqualTo(1));
                    Assert.That(fixture.ReadyCount, Is.EqualTo(useBecameReady ? 1 : 0));
                }
                yield return null;
            }
        }

        [UnityTest]
        public IEnumerator ResumeNotificationCanStartAReplacementSessionWithItsOwnReadiness()
        {
            using (var fixture = new Fixture())
            {
                fixture.Activate();
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                PackBootstrapSession previous = fixture.Runtime.Session;
                bool restarted = false;
                fixture.Runtime.BecameReady += owner =>
                {
                    if (restarted) return;
                    restarted = true;
                    Assert.That(owner.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(owner.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                };
                NotifyApplicationPause(fixture.Runtime, true);
                NotifyApplicationPause(fixture.Runtime, false);
                Assert.That(restarted, Is.True);
                Assert.That(previous.IsActive, Is.False);
                Assert.That(fixture.Runtime.Session, Is.Not.SameAs(previous));
                Assert.That(fixture.Runtime.IsReady, Is.True);
                Assert.That(fixture.StartupReadyCount, Is.EqualTo(2));
                Assert.That(fixture.ReadyCount, Is.EqualTo(3));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator RemoteCommunicationFollowsProjectPolicyAndAudioLifecycle()
        {
            using (var fixture = new Fixture())
            {
                fixture.Activate();
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.QueryRemoteCommunication(out VnRemoteStatus status), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.IsReady, Is.True);
                if (HarkenProjectSettings.CommunicationEnabled)
                {
                    Assert.That(status.state, Is.EqualTo((uint)VnRemoteState.VN_REMOTE_LISTENING));
                    Assert.That(status.tcpPort, Is.GreaterThan(0));
                    using (var client = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp))
                    {
                        client.Connect(IPAddress.Loopback, (int)status.tcpPort);
                        Assert.That(client.Connected, Is.True);
                    }
                }
                else
                {
                    Assert.That(status.state, Is.EqualTo((uint)VnRemoteState.VN_REMOTE_STOPPED));
                    Assert.That(status.tcpPort, Is.Zero);
                    LogAssert.Expect(LogType.Warning, new Regex("Harken remote Start: "));
                    Assert.That(fixture.Runtime.RetryRemoteCommunication(), Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
                }
                Assert.That(fixture.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.QueryRemoteCommunication(out status), Is.EqualTo(VnResult.VN_OK));
                Assert.That(status.state, Is.EqualTo((uint)VnRemoteState.VN_REMOTE_STOPPED));
                Assert.That(status.tcpPort, Is.Zero);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator RemoteBindFailurePreservesAudioStartupAndAllowsExplicitRetry()
        {
            using (var settings = new RemoteSettings())
            using (var fixture = new Fixture())
            {
                using (var occupied = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp))
                {
                    occupied.ExclusiveAddressUse = true;
                    occupied.Bind(new IPEndPoint(IPAddress.Loopback, 0));
                    occupied.Listen(1);
                    int port = ((IPEndPoint)occupied.LocalEndPoint).Port;
                    settings.Configure("127.0.0.1", port);
                    fixture.Activate();
                    LogAssert.Expect(LogType.Warning, new Regex("Harken remote Start: "));
                    Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(fixture.Runtime.IsReady, Is.True);
                    Assert.That(fixture.ReadyCount, Is.EqualTo(1));
                    Assert.That(fixture.FailedCount, Is.Zero);
                    Assert.That(fixture.Runtime.LastResult, Is.EqualTo(VnResult.VN_OK));
                    Assert.That(fixture.Runtime.LastStartupError, Is.Empty);
                    Assert.That(fixture.Runtime.LastRemoteResult, Is.Not.EqualTo(VnResult.VN_OK));
                    Assert.That(fixture.Runtime.LastRemoteError, Is.Not.Empty);
                    Assert.That(fixture.Runtime.QueryRemoteCommunication(out VnRemoteStatus status), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(status.state, Is.EqualTo((uint)VnRemoteState.VN_REMOTE_FAILED));
                    Assert.That(status.lastResult, Is.EqualTo((int)fixture.Runtime.LastRemoteResult));
                }
                Assert.That(fixture.Runtime.RetryRemoteCommunication(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.LastRemoteError, Is.Empty);
                Assert.That(fixture.Runtime.QueryRemoteCommunication(out VnRemoteStatus listening), Is.EqualTo(VnResult.VN_OK));
                Assert.That(listening.state, Is.EqualTo((uint)VnRemoteState.VN_REMOTE_LISTENING));
                Assert.That(fixture.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.QueryRemoteCommunication(out VnRemoteStatus stopped), Is.EqualTo(VnResult.VN_OK));
                Assert.That(stopped.state, Is.EqualTo((uint)VnRemoteState.VN_REMOTE_STOPPED));
                Assert.That(stopped.tcpPort, Is.Zero);
            }
            yield return null;
        }

        private sealed class RemoteSettings : IDisposable
        {
            private readonly HarkenProjectSettings settings = HarkenProjectSettings.Load();
            private readonly string original;

            public RemoteSettings()
            {
                Assert.That(settings, Is.Not.Null, "Prepare HarkenUnityFixtureAssets before this test.");
                original = JsonUtility.ToJson(settings);
            }

            public void Configure(string address, int port)
            {
                Set(settings, "enableCommunicationInBuilds", true);
                Set(settings, "remoteBindAddress", address);
                Set(settings, "remotePort", port);
                Set(settings, "enableRemoteDiscovery", false);
            }

            public void Dispose() => JsonUtility.FromJsonOverwrite(original, settings);
        }

        [UnityTest]
        public IEnumerator ManualInvalidInputPublishesFailureAndWaitsForExplicitCorrection()
        {
            using (var fixture = new Fixture())
            {
                fixture.Activate();
                ExpectFailure(VnResult.VN_INVALID_ARG);
                Assert.That(fixture.Runtime.StartRuntime((string)null), Is.EqualTo(VnResult.VN_INVALID_ARG));
                fixture.AssertFailed(VnResult.VN_INVALID_ARG, 1);
                for (int i = 0; i < 3; ++i) yield return null;
                fixture.AssertFailed(VnResult.VN_INVALID_ARG, 1);
                Assert.That(fixture.ReadyCount, Is.Zero);
                ExpectFailure(VnResult.VN_INVALID_ARG);
                Assert.That(fixture.Runtime.StartRuntime((PackSource)null), Is.EqualTo(VnResult.VN_INVALID_ARG));
                fixture.AssertFailed(VnResult.VN_INVALID_ARG, 2);
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.IsReady, Is.True);
                Assert.That(fixture.ReadyCount, Is.EqualTo(1));
                Assert.That(fixture.FailedCount, Is.EqualTo(2));
                Assert.That(fixture.Runtime.LastStartupError, Is.Empty);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator ManualPlatformAndLanguageFailuresPublishTheirResults()
        {
            using (var fixture = new Fixture())
            {
                fixture.Activate();
                Assert.That(fixture.Runtime.ConfigurePlatformBeforeStartup(null, "MissingPlatform"), Is.EqualTo(VnResult.VN_OK));
                ExpectFailure(VnResult.VN_INVALID_ARG);
                Assert.That(fixture.Runtime.StartRuntime(fixture.Root), Is.EqualTo(VnResult.VN_INVALID_ARG));
                fixture.AssertFailed(VnResult.VN_INVALID_ARG, 1);
                Assert.That(fixture.Runtime.ConfigurePlatformBeforeStartup(null), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.ConfigureLanguageBeforeStartup("UnpublishedLanguage"), Is.EqualTo(VnResult.VN_OK));
                ExpectFailure(VnResult.VN_PACK_STALE_REVISION);
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_PACK_STALE_REVISION));
                fixture.AssertFailed(VnResult.VN_PACK_STALE_REVISION, 2);
                Assert.That(fixture.Runtime.ConfigureLanguageBeforeStartup(null), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.IsPreparationComplete, Is.True);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator BootstrapRefusalRetiresRegistrationsAndAllowsExplicitRetry()
        {
            using (var fixture = new Fixture())
            {
                fixture.Activate();
                HarkenProjectSettings.ActiveForStartup().SetMaxVoices(20000);
                ExpectFailure(VnResult.VN_INVALID_CONFIG);
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_INVALID_CONFIG));
                fixture.AssertFailed(VnResult.VN_INVALID_CONFIG, 1);
                for (int i = 0; i < 3; ++i) yield return null;
                fixture.AssertFailed(VnResult.VN_INVALID_CONFIG, 1);
                HarkenProjectSettings.ActiveForStartup().SetMaxVoices(HarkenProjectSettings.DefaultMaxVoices);
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.IsReady, Is.True);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator RepeatedStartPreservesReadyAndSuspendedSessions()
        {
            using (var fixture = new Fixture())
            {
                fixture.Activate();
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                PackBootstrapSession session = fixture.Runtime.Session;
                ExpectFailure(VnResult.VN_ALREADY_INITIALIZED);
                Assert.That(fixture.Runtime.StartRuntime((string)null), Is.EqualTo(VnResult.VN_ALREADY_INITIALIZED));
                Assert.That(fixture.Runtime.State, Is.EqualTo(HarkenRuntimeState.Ready));
                Assert.That(fixture.Runtime.Session, Is.SameAs(session));
                Assert.That(fixture.Runtime.SetSuspended(true), Is.EqualTo(VnResult.VN_OK));
                ExpectFailure(VnResult.VN_ALREADY_INITIALIZED);
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_ALREADY_INITIALIZED));
                Assert.That(fixture.Runtime.State, Is.EqualTo(HarkenRuntimeState.Suspended));
                Assert.That(fixture.Runtime.Session, Is.SameAs(session));
                Assert.That(session.IsActive, Is.True);
                Assert.That(fixture.Runtime.LastResult, Is.EqualTo(VnResult.VN_ALREADY_INITIALIZED));
                Assert.That(fixture.FailedCount, Is.EqualTo(2));
                Assert.That(fixture.ReadyCount, Is.EqualTo(1));
                Assert.That(fixture.Runtime.SetSuspended(false), Is.EqualTo(VnResult.VN_OK));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator StartupAndShutdownCallbacksCannotAcquireAnotherSession()
        {
            using (var fixture = new Fixture())
            {
                fixture.Activate();
                UnityAction<string> retry = _ =>
                {
                    Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_ALREADY_INITIALIZED));
                    Assert.That(fixture.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_ALREADY_INITIALIZED));
                };
                Get<HarkenRuntimeResultEvent>(fixture.Runtime, "failed").AddListener(retry);
                ExpectFailure(VnResult.VN_INVALID_ARG);
                Assert.That(fixture.Runtime.StartRuntime((PackSource)null), Is.EqualTo(VnResult.VN_INVALID_ARG));
                Assert.That(fixture.FailedCount, Is.EqualTo(1));
                Assert.That(fixture.Runtime.Session, Is.Null);
                Get<HarkenRuntimeResultEvent>(fixture.Runtime, "failed").RemoveListener(retry);

                bool readyCallback = false;
                UnityAction stopDuringReady = () =>
                {
                    readyCallback = true;
                    Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_ALREADY_INITIALIZED));
                    Assert.That(fixture.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_ALREADY_INITIALIZED));
                };
                Get<UnityEvent>(fixture.Runtime, "ready").AddListener(stopDuringReady);
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(readyCallback, Is.True);
                Assert.That(fixture.Runtime.State, Is.EqualTo(HarkenRuntimeState.Stopped));
                Assert.That(fixture.Runtime.Session, Is.Null);
                Assert.That(fixture.ReadyCount, Is.Zero);
                Get<UnityEvent>(fixture.Runtime, "ready").RemoveListener(stopDuringReady);
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                bool shutdownCallback = false;
                fixture.Runtime.OperationCompleted += (owner, operation, result) =>
                {
                    if (operation != "PackBootstrapSession.Shutdown") return;
                    shutdownCallback = true;
                    Assert.That(owner.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_ALREADY_INITIALIZED));
                };
                Assert.That(fixture.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(shutdownCallback, Is.True);
                Assert.That(fixture.Runtime.Session, Is.Null);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator CanceledPreparationCannotReenterEitherReadyNotification()
        {
            using (var fixture = new Fixture())
            {
                fixture.Activate();
                foreach (bool useBecameReady in new[] { false, true })
                {
                    int notifications = 0;
                    VnResult nestedResult = VnResult.VN_OK;
                    System.Action cancelAndRetry = () =>
                    {
                        if (++notifications != 1) return;
                        // Stand in for Android's owned coroutine while its synchronous completion callback runs.
                        Set(fixture.Runtime, "startupCoroutine",
                            fixture.Runtime.StartCoroutine(WaitWithoutCleanup(() => { })));
                        Assert.That(fixture.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                        // Use production cancellation; this is not Android resource extraction or device execution.
                        Assert.That(Get<bool>(fixture.Runtime, "starting"), Is.False);
                        nestedResult = fixture.Runtime.StartRuntime(fixture.Prepared);
                    };
                    UnityAction readyListener = () => cancelAndRetry();
                    System.Action<HarkenAudioSystem> becameReadyListener = _ => cancelAndRetry();
                    UnityEvent ready = Get<UnityEvent>(fixture.Runtime, "ready");
                    if (useBecameReady) fixture.Runtime.BecameReady += becameReadyListener;
                    else ready.AddListener(readyListener);
                    try
                    {
                        Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                        Assert.That(nestedResult, Is.EqualTo(VnResult.VN_ALREADY_INITIALIZED));
                        Assert.That(notifications, Is.EqualTo(1));
                        Assert.That(fixture.Runtime.Session, Is.Null);
                        Assert.That(fixture.Runtime.State, Is.EqualTo(HarkenRuntimeState.Stopped));
                        Assert.That(fixture.FailedCount, Is.Zero);
                    }
                    finally
                    {
                        ready.RemoveListener(readyListener);
                        fixture.Runtime.BecameReady -= becameReadyListener;
                    }
                    Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(fixture.Runtime.IsReady, Is.True);
                    Assert.That(fixture.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                }
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator GameObjectDeactivationCancelsOwnedStartupWithoutEnumeratorCleanup()
        {
            using (var fixture = new Fixture())
            {
                fixture.Activate();
                Set(fixture.Runtime, "autoInitialize", true);
                int progress = 0;
                OwnWaitingStartup(fixture.Runtime, () => ++progress);
                yield return null;
                Assert.That(progress, Is.GreaterThan(0));
                int beforeDisable = progress;
                fixture.Owner.SetActive(false);
                AssertCanceledStartup(fixture.Runtime);
                fixture.Owner.SetActive(true);
                for (int i = 0; i < 3; ++i) yield return null;
                AssertCanceledStartup(fixture.Runtime);
                Assert.That(progress, Is.EqualTo(beforeDisable));
                Assert.That(fixture.ReadyCount, Is.Zero);
                Assert.That(fixture.FailedCount, Is.Zero);
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.IsReady, Is.True);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator DisabledComponentKeepsOwnedPreparationUntilExplicitShutdown()
        {
            using (var fixture = new Fixture())
            {
                fixture.Activate();
                int progress = 0;
                Coroutine owned = OwnWaitingStartup(fixture.Runtime, () => ++progress);
                int beforeDisable = progress;
                fixture.Runtime.enabled = false;
                for (int i = 0; i < 3; ++i) yield return null;
                Assert.That(progress, Is.GreaterThan(beforeDisable));
                Assert.That(Get<Coroutine>(fixture.Runtime, "startupCoroutine"), Is.SameAs(owned));
                Assert.That(Get<bool>(fixture.Runtime, "starting"), Is.True);
                Assert.That(fixture.Runtime.State, Is.EqualTo(HarkenRuntimeState.Starting));
                Assert.That(fixture.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                AssertCanceledStartup(fixture.Runtime);
                int afterShutdown = progress;
                fixture.Runtime.enabled = true;
                for (int i = 0; i < 3; ++i) yield return null;
                Assert.That(progress, Is.EqualTo(afterShutdown));
                AssertCanceledStartup(fixture.Runtime);
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.IsReady, Is.True);
            }
            yield return null;
        }



        [UnityTest]
        public IEnumerator AutomaticInvalidRootNotifiesOnceAndAllowsExplicitPreparedRetry()
        {
            using (var fixture = new Fixture())
            {
                Set(fixture.Runtime, "autoInitialize", true);
                Set(fixture.Runtime, "packRoot", "../invalid");
                HarkenProjectSettings.ActiveForStartup().SetPackDirectory(string.Empty);
                ExpectFailure(VnResult.VN_INVALID_ARG);
                fixture.Activate();
                fixture.AssertFailed(VnResult.VN_INVALID_ARG, 1);
                for (int i = 0; i < 3; ++i) yield return null;
                fixture.AssertFailed(VnResult.VN_INVALID_ARG, 1);
                Assert.That(fixture.Runtime.enabled, Is.True);
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.IsReady, Is.True);
            }
            yield return null;
        }




        [UnityTest]
        public IEnumerator ProjectVoiceLimitReachesInitAndIgnoresTheSceneValue()
        {
            using (var fixture = new Fixture())
            {
                HarkenProjectSettings.ActiveForStartup().SetMaxVoices(20000);
                Set(fixture.Runtime, "maxVoices", 32u);
                fixture.Activate();
                ExpectFailure(VnResult.VN_INVALID_CONFIG);
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_INVALID_CONFIG));
                HarkenProjectSettings.ActiveForStartup().SetMaxVoices(HarkenProjectSettings.DefaultMaxVoices);
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.IsReady, Is.True);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator AutomaticStartFailsWhenNoPackDirectoryIsSelected()
        {
            var owner = new GameObject("Unselected pack directory");
            owner.SetActive(false);
            var runtime = owner.AddComponent<HarkenAudioSystem>();
            using (var lease = HarkenProjectSettingsLease.Acquire())
            {
                lease.Settings.SetPackDirectory(string.Empty);
                Set(runtime, "packRoot", "Audio/GeneratedPacks");
                LogAssert.Expect(LogType.Error, new Regex("Harken operation failed: PackBootstrap.Start: " + VnResult.VN_INVALID_ARG));
                owner.SetActive(true);
                Assert.That(runtime.State, Is.EqualTo(HarkenRuntimeState.Failed));
                Assert.That(runtime.IsReady, Is.False);
                Assert.That(runtime.LastStartupError, Does.Not.Contain("StreamingAssets"));
                Assert.That(runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
            }
            Object.Destroy(owner);
            yield return null;
        }

        [UnityTest]
        public IEnumerator ProjectSuppressAudioAppliesOnlyWithoutAStartupCall()
        {
            using (var fixture = new Fixture())
            {
                HarkenProjectSettings.ActiveForStartup().SetSuppressUnityAudio(true);
                Set(fixture.Runtime, "suppressUnityAudio", false);
                fixture.Activate();
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Get<bool>(fixture.Runtime, "suppressUnityAudio"), Is.True);
                Assert.That(fixture.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.ConfigureLifecycleBeforeStartup(false, false), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.StartRuntime(fixture.Prepared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Get<bool>(fixture.Runtime, "suppressUnityAudio"), Is.False);
            }
            yield return null;
        }

        private sealed class Fixture : IDisposable
        {
            public readonly GameObject Owner = new GameObject("Startup result audio system");
            public readonly HarkenAudioSystem Runtime;
            public readonly PackSource Prepared;
            public readonly CompanionTestAction Action;
            private readonly HarkenProjectSettingsLease settingsLease = HarkenProjectSettingsLease.Acquire();
            public readonly string Root = Path.Combine(Application.streamingAssetsPath, "Audio/U4Original");
            public int FailedCount { get; private set; }
            public int ReadyCount { get; private set; }
            public int StartupReadyCount { get; private set; }
            private VnResult observedFailure;
            private HarkenRuntimeState failureState;

            public Fixture()
            {
                Owner.SetActive(false);
                Runtime = Owner.AddComponent<HarkenAudioSystem>();
                Set(Runtime, "autoInitialize", false);
                HarkenPackSource source = Resources.Load<HarkenPackSource>("HarkenSdkFixture/Source");
                Assert.That(source, Is.Not.Null, "Prepare HarkenUnityFixtureAssets before this test.");
                Assert.That(source.CaptureCompanion(Resources.Load<TextAsset>("HarkenSdkFixture/SourceInit"), out Prepared, out string error), Is.EqualTo(VnResult.VN_OK), error);
                Assert.That(CompanionTestPublication.Read(Root, null, out CompanionTestPublication publication, out error), Is.EqualTo(VnResult.VN_OK), error);
                Assert.That(publication.TryFindAction("Play_U4_A", out Action), Is.True);
                Assert.That(Runtime.ConfigureBeforeStartup("UnusedStartupResultPath", "Init.bytes",
                    Array.Empty<ulong>(), true, false), Is.EqualTo(VnResult.VN_OK));
                Get<HarkenRuntimeResultEvent>(Runtime, "failed").AddListener(_ =>
                {
                    ++FailedCount;
                    observedFailure = Runtime.LastResult;
                    failureState = Runtime.State;
                });
                Runtime.BecameReady += _ => ++ReadyCount;
                Get<UnityEvent>(Runtime, "ready").AddListener(() => ++StartupReadyCount);
            }

            public void Activate() => Owner.SetActive(true);

            public void AssertFailed(VnResult result, int count)
            {
                Assert.That(Runtime.State, Is.EqualTo(HarkenRuntimeState.Failed));
                Assert.That(Runtime.LastOperation, Is.EqualTo("PackBootstrap.Start"));
                Assert.That(Runtime.LastResult, Is.EqualTo(result));
                Assert.That(Runtime.LastStartupError, Is.Not.Empty);
                Assert.That(Runtime.Session, Is.Null);
                Assert.That(FailedCount, Is.EqualTo(count));
                Assert.That(observedFailure, Is.EqualTo(result));
                Assert.That(failureState, Is.EqualTo(HarkenRuntimeState.Failed));
                Assert.That(Get<object>(Runtime, "notificationRegistration"), Is.Null);
                Assert.That(Get<object>(Runtime, "logRegistration"), Is.Null);
            }

            public void Dispose()
            {
                try
                {
                    Assert.That(Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                    Object.Destroy(Owner);
                }
                finally { settingsLease.Dispose(); }
            }
        }

        private static Coroutine OwnWaitingStartup(HarkenAudioSystem runtime, System.Action progress)
        {
            // Replace only Android resource extraction with a real Unity wait; production owns and cancels it.
            object[] arguments = { VnResult.VN_OK };
            Assert.That(typeof(HarkenAudioSystem).GetMethod("TryBeginStartup",
                BindingFlags.Instance | BindingFlags.NonPublic).Invoke(runtime, arguments), Is.EqualTo(true));
            Coroutine owned = runtime.StartCoroutine(WaitWithoutCleanup(progress));
            Set(runtime, "startupCoroutine", owned);
            return owned;
        }

        private static IEnumerator WaitWithoutCleanup(System.Action progress)
        {
            while (true)
            {
                progress();
                yield return null;
            }
        }

        private static void AssertCanceledStartup(HarkenAudioSystem runtime)
        {
            Assert.That(Get<Coroutine>(runtime, "startupCoroutine"), Is.Null);
            Assert.That(Get<bool>(runtime, "starting"), Is.False);
            Assert.That(runtime.State, Is.EqualTo(HarkenRuntimeState.Stopped));
            Assert.That(runtime.LastOperation, Is.EqualTo("PackBootstrapSession.Shutdown"));
            Assert.That(runtime.LastResult, Is.EqualTo(VnResult.VN_OK));
            Assert.That(runtime.Session, Is.Null);
        }

        private static void ExpectFailure(VnResult result)
            => LogAssert.Expect(LogType.Error, new Regex("Harken operation failed: PackBootstrap.Start: " + result));

        private static void NotifyApplicationPause(HarkenAudioSystem runtime, bool paused)
        {
#if UNITY_EDITOR
            const string callback = "HandleApplicationPause";
#else
            const string callback = "OnApplicationPause";
#endif
            typeof(HarkenAudioSystem).GetMethod(callback, BindingFlags.Instance | BindingFlags.NonPublic)
                .Invoke(runtime, new object[] { paused });
        }

        private static T Get<T>(object target, string name)
            => (T)target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).GetValue(target);

        private static void Set(object target, string name, object value)
            => target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).SetValue(target, value);
    }
}
