using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Text.RegularExpressions;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;
using Object = UnityEngine.Object;

namespace Harken.Unity.Tests
{
    public sealed class HarkenPreparedRuntimeTests
    {
        [UnityTest]
        public IEnumerator ThrowingStartupProviderCleansCallbacksAndAllowsExplicitRetry()
        {
            string root = Path.Combine(Application.streamingAssetsPath, "Audio", "U4Original");
            Assert.That(CompanionTestPublication.Read(root, null, out CompanionTestPublication publication, out string error),
                Is.EqualTo(VnResult.VN_OK), error);
            Assert.That(publication.TryFindAction("Play_U4_A", out CompanionTestAction row), Is.True);
            bool throwProvider = true;
            PackSource prepared = PackSource.FromReader(path =>
            {
                if (throwProvider) throw new IOException("Expected startup provider failure.");
                string file = Path.Combine(root, path);
                return File.Exists(file) ? File.ReadAllBytes(file) : null;
            });
            var owner = new GameObject("Throwing startup provider");
            owner.SetActive(false);
            var runtime = owner.AddComponent<HarkenAudioSystem>();
            Set(runtime, "autoInitialize", false);
            Set(runtime, "persistAcrossScenes", false);
            try
            {
                Assert.That(runtime.ConfigureBeforeStartup("Unused", publication.InitPack.Path, Array.Empty<ulong>(), true, false),
                    Is.EqualTo(VnResult.VN_OK));
                owner.SetActive(true);
                ExpectStartupFailure(VnResult.VN_PACK_PROVIDER_FAILED);
                Assert.That(runtime.StartRuntime(prepared), Is.EqualTo(VnResult.VN_PACK_PROVIDER_FAILED));
                Assert.That(runtime.State, Is.EqualTo(HarkenRuntimeState.Failed));
                Assert.That(runtime.Session, Is.Null);
                Assert.That(Get<object>(runtime, "notificationRegistration"), Is.Null);
                Assert.That(Get<object>(runtime, "logRegistration"), Is.Null);
                throwProvider = false;
                Assert.That(runtime.StartRuntime(prepared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(runtime.IsPreparationComplete, Is.True);
                Assert.That(runtime.PlayAction(row.Name, publication.ProjectId, out ActionFiring firing), Is.EqualTo(VnResult.VN_OK));
                Assert.That(firing.IsValid, Is.True);
            }
            finally
            {
                runtime.ShutdownRuntime();
                Object.Destroy(owner);
            }
            yield return null;
        }

#if HARKEN_SDK_TEST_NULL_PUMP
        [System.Runtime.InteropServices.DllImport(HarkenNative.LibraryName,
            CallingConvention = System.Runtime.InteropServices.CallingConvention.Cdecl)]
        private static extern VnResult HarkenSdkTest_PumpNull(uint blocks);

        [UnityTest]
        public IEnumerator VariableComponentRejectsStaleCatalogAndPreservesMatchingNameBinding()
        {
            string root = Path.Combine(Application.streamingAssetsPath, "Audio", "VariableNames");
            PackSource prepared = PackSource.FromDirectory(root);
            var owner = new GameObject("Variable binding runtime");
            owner.SetActive(false);
            var runtime = owner.AddComponent<HarkenAudioSystem>();
            Set(runtime, "autoInitialize", false);
            Set(runtime, "persistAcrossScenes", false);
            try
            {
                Assert.That(runtime.ConfigureBeforeStartup("Unused", "Init.bytes", Array.Empty<ulong>(), true, false),
                    Is.EqualTo(VnResult.VN_OK));
                owner.SetActive(true);
                Assert.That(runtime.StartRuntime(prepared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Values.Resolve("Sdk_Value", out VnObjectId id), Is.EqualTo(VnResult.VN_OK));
                var component = owner.AddComponent<HarkenVariable>();
                Set(component, "setOnEnable", false);
                Set(component, "variableName", "OldName");
                Set(component, "projectId", runtime.Session.ProjectId);
                component.Variable = HarkenVariableId.FromGenerated("Sdk_Value", id);
                Assert.That(component.VariableName, Is.Empty);
                Assert.That(component.ProjectId, Is.Empty);
                LogAssert.Expect(LogType.Error, new Regex("HarkenVariable.SetValue.*VN_VARIABLE_CATALOG_MISMATCH"));
                Assert.That(component.SetValue(1), Is.EqualTo(VnResult.VN_VARIABLE_CATALOG_MISMATCH));
                AssertVariableValue(id, 0);
                Set(component, "variableName", "Sdk_Value");
                Set(component, "projectId", runtime.Session.ProjectId);
                Assert.That(component.SetValue(1), Is.EqualTo(VnResult.VN_OK));
                AssertVariableValue(id, 1);
                Set(component, "variable", HarkenVariableId.FromGenerated("Other", new VnObjectId(1, 2)));
                Set(component, "projectId", "00000000-0000-4000-8000-000000000001");
                LogAssert.Expect(LogType.Error, new Regex("HarkenVariable.SetValue.*VN_GAMEPLAY_WRONG_PROJECT"));
                Assert.That(component.SetValue(1), Is.EqualTo(VnResult.VN_GAMEPLAY_WRONG_PROJECT));
                AssertVariableValue(id, 1);
                Set(component, "projectId", runtime.Session.ProjectId);
                LogAssert.Expect(LogType.Error, new Regex("HarkenVariable.SetValue.*VN_INVALID_ARG"));
                Assert.That(component.SetValue(1), Is.EqualTo(VnResult.VN_INVALID_ARG));
                AssertVariableValue(id, 1);
            }
            finally
            {
                runtime.ShutdownRuntime();
                Object.Destroy(owner);
            }
            yield return null;
        }

        private static void AssertVariableValue(VnObjectId id, float expected)
        {
            for (int i = 0; i < 32; ++i)
            {
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(HarkenSdkTest_PumpNull(1), Is.EqualTo(VnResult.VN_OK));
            }
            Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
            var query = new VnVariableQueryDescV1
            {
                structSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf<VnVariableQueryDescV1>(), variableId = id
            };
            var value = new VnVariableQueryResultV1
            {
                structSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf<VnVariableQueryResultV1>()
            };
            Assert.That(Values.QueryVariable(in query, ref value), Is.EqualTo(VnResult.VN_OK));
            Assert.That(value.pendingCount, Is.Zero);
            Assert.That(value.currentValue, Is.EqualTo(expected));
        }

        [UnityTest]
        public IEnumerator TextAssetAndPublicationStartupResolveCurrentVariableNames()
        {
            HarkenPackSource asset = Resources.Load<HarkenPackSource>("HarkenSdkFixture/VariableSource");
            Assert.That(asset, Is.Not.Null, "Run HarkenUnityFixtureAssets.PrepareVariableSource before this test.");
            PackSource captured = CaptureSource(asset, "VariableSourceInit");
            var owner = new GameObject("Named variable Runtime");
            owner.SetActive(false);
            var runtime = owner.AddComponent<HarkenAudioSystem>();
            Set(runtime, "autoInitialize", false);
            Set(runtime, "persistAcrossScenes", false);
            try
            {
                Assert.That(runtime.ConfigureBeforeStartup("UnusedTextAssetPath", "Init.bytes",
                    Array.Empty<ulong>(), true, false), Is.EqualTo(VnResult.VN_OK));
                Assert.That(runtime.ConfigureCompanionSourceBeforeStartup(captured), Is.EqualTo(VnResult.VN_OK));
                owner.SetActive(true);
                Assert.That(runtime.StartRuntime(captured), Is.EqualTo(VnResult.VN_OK), runtime.LastStartupError);
                AssertNamedWrite(runtime.Session.ProjectId);
                Assert.That(runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));

                string root = Path.Combine(Application.streamingAssetsPath, "Audio/VariableNames");
                PackSource prepared = PackSource.FromDirectory(root);
                Assert.That(runtime.StartRuntime(prepared), Is.EqualTo(VnResult.VN_OK), runtime.LastStartupError);
                AssertNamedWrite(runtime.Session.ProjectId);
            }
            finally
            {
                runtime.ShutdownRuntime();
                Object.Destroy(owner);
            }
            yield return null;
        }

        private static void AssertNamedWrite(string projectId)
        {
            Assert.That(Values.Resolve("Sdk_Value", out VnObjectId variableId), Is.EqualTo(VnResult.VN_OK));
            var query = new VnVariableQueryDescV1
            {
                structSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf<VnVariableQueryDescV1>(),
                variableId = variableId
            };
            var value = new VnVariableQueryResultV1
            {
                structSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf<VnVariableQueryResultV1>()
            };
            Assert.That(Values.QueryVariable(in query, ref value), Is.EqualTo(VnResult.VN_OK));
            Assert.That(value.currentValue, Is.Zero);
            Assert.That(value.pendingCount, Is.Zero);
            Assert.That(Values.SetValue("Sdk_Value", projectId, 1), Is.EqualTo(VnResult.VN_OK));
            for (int i = 0; i < 32; ++i)
            {
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(HarkenSdkTest_PumpNull(1), Is.EqualTo(VnResult.VN_OK));
            }
            Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
            Assert.That(Values.QueryVariable(in query, ref value), Is.EqualTo(VnResult.VN_OK));
            Assert.That(value.pendingCount, Is.Zero);
            Assert.That(value.currentValue, Is.EqualTo(1));
            Assert.That(Values.SetValue("Sdk_Value", "00000000-0000-4000-8000-000000000001", 0),
                Is.EqualTo(VnResult.VN_GAMEPLAY_WRONG_PROJECT));
            Assert.That(Values.QueryVariable(in query, ref value), Is.EqualTo(VnResult.VN_OK));
            Assert.That(value.currentValue, Is.EqualTo(1));
        }
#endif

        [UnityTest]
        public IEnumerator ManualInitializationWaitsAcrossFramesThenStartsOnceFromSuppliedBytes()
        {
            var owner = new GameObject("Manual audio system");
            var duplicateOwner = new GameObject("Duplicate audio system");
            var source = new GameObject("Manual Action");
            owner.SetActive(false);
            duplicateOwner.SetActive(false);
            source.SetActive(false);
            var runtime = owner.AddComponent<HarkenAudioSystem>();
            var duplicate = duplicateOwner.AddComponent<HarkenAudioSystem>();
            Set(runtime, "autoInitialize", false);
            Set(runtime, "persistAcrossScenes", false);
            Set(runtime, "packRoot", "../must-not-read");
            Set(duplicate, "autoInitialize", false);
            int readyCount = 0;
            runtime.BecameReady += _ => ++readyCount;
            try
            {
                owner.SetActive(true);
                for (int frame = 0; frame < 3; ++frame)
                {
                    yield return null;
                    Assert.That(HarkenAudioSystem.Instance, Is.SameAs(runtime));
                    Assert.That(runtime.State, Is.EqualTo(HarkenRuntimeState.Stopped));
                    Assert.That(runtime.Session, Is.Null);
                    Assert.That(runtime.LastStartupError, Is.Empty);
                    Assert.That(Get<object>(runtime, "notificationRegistration"), Is.Null);
                    Assert.That(Get<object>(runtime, "logRegistration"), Is.Null);
                    Assert.That(readyCount, Is.Zero);
                }

                LogAssert.Expect(LogType.Error, "Only one active HarkenAudioSystem is allowed.");
                duplicateOwner.SetActive(true);
                Assert.That(duplicate.State, Is.EqualTo(HarkenRuntimeState.Failed));
                Assert.That(duplicate.enabled, Is.False);
                ExpectStartupFailure(VnResult.VN_ALREADY_INITIALIZED);
                Assert.That(duplicate.StartRuntime("unused"), Is.EqualTo(VnResult.VN_ALREADY_INITIALIZED));
                ExpectStartupFailure(VnResult.VN_ALREADY_INITIALIZED);
                Assert.That(duplicate.StartRuntime((PackSource)null), Is.EqualTo(VnResult.VN_ALREADY_INITIALIZED));
                Assert.That(duplicate.Session, Is.Null);
                ExpectStartupFailure(VnResult.VN_INVALID_ARG);
                Assert.That(runtime.StartRuntime((PackSource)null), Is.EqualTo(VnResult.VN_INVALID_ARG));
                Assert.That(runtime.Session, Is.Null);

                HarkenPackSource asset = Resources.Load<HarkenPackSource>("HarkenSdkFixture/Source");
                Assert.That(asset, Is.Not.Null, "Prepare HarkenUnityFixtureAssets before running this test.");
                PackSource prepared = CaptureSource(asset, "SourceInit");
                string root = Path.Combine(Application.streamingAssetsPath, "Audio/U4Original");
                Assert.That(CompanionTestPublication.Read(root, null, out CompanionTestPublication publication, out string error),
                    Is.EqualTo(VnResult.VN_OK), error);
                Assert.That(publication.TryFindAction("Play_U4_A", out CompanionTestAction row), Is.True);
                Assert.That(runtime.ConfigureBeforeStartup("UnusedSuppliedBytes", "Init.bytes", Array.Empty<ulong>(),
                    true, false), Is.EqualTo(VnResult.VN_OK));
                Assert.That(runtime.ConfigureLanguageBeforeStartup(prepared.Language), Is.EqualTo(VnResult.VN_OK));
                var emitter = source.AddComponent<HarkenEmitter>();
                var action = source.AddComponent<HarkenAction>();
                Set(emitter, "runtime", runtime);
                Set(action, "runtime", runtime);
                Set(action, "emitter", emitter);
                Set(action, "actionName", row.Name);
                Set(action, "projectId", publication.ProjectId);
                source.SetActive(true);
                Assert.That(emitter.IsCreated, Is.False);

                Assert.That(runtime.StartRuntime(prepared), Is.EqualTo(VnResult.VN_OK));
                PackBootstrapSession session = runtime.Session;
                Assert.That(runtime.IsPreparationComplete, Is.True, runtime.LastStartupError);
                Assert.That(runtime.PackRootPath, Is.Empty);
                Assert.That(emitter.IsCreated, Is.True);
                Assert.That(readyCount, Is.EqualTo(1));
                ExpectStartupFailure(VnResult.VN_ALREADY_INITIALIZED);
                Assert.That(runtime.StartRuntime(prepared), Is.EqualTo(VnResult.VN_ALREADY_INITIALIZED));
                ExpectStartupFailure(VnResult.VN_ALREADY_INITIALIZED);
                Assert.That(runtime.StartRuntime("unused"), Is.EqualTo(VnResult.VN_ALREADY_INITIALIZED));
                ExpectStartupFailure(VnResult.VN_ALREADY_INITIALIZED);
                Assert.That(duplicate.StartRuntime(prepared), Is.EqualTo(VnResult.VN_ALREADY_INITIALIZED));
                Assert.That(runtime.Session, Is.SameAs(session));
                Assert.That(duplicate.Session, Is.Null);
                Assert.That(readyCount, Is.EqualTo(1));
                yield return null;
                Assert.That(action.Play(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(action.Firing.Query(out VnActionFiringInfo info), Is.EqualTo(VnResult.VN_OK));
                Assert.That(info.actionId, Is.EqualTo(row.ActionStableId));
            }
            finally
            {
                runtime.ShutdownRuntime();
                Object.Destroy(source);
                Object.Destroy(duplicateOwner);
                Object.Destroy(owner);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator SerializedTextAssetSourceStartsTheRuntimeInEditorAndPlayer()
        {
            HarkenPackSource asset = Resources.Load<HarkenPackSource>("HarkenSdkFixture/Source");
            Assert.That(asset, Is.Not.Null,
                "Run Harken.Unity.Tests.HarkenUnityFixtureAssets.Prepare before Editor or Player tests.");
            PackSource captured = CaptureSource(asset, "SourceInit");
            string root = Path.Combine(Application.streamingAssetsPath, "Audio/U4Original");
            Assert.That(CompanionTestPublication.Read(root, null, out CompanionTestPublication publication, out string error),
                Is.EqualTo(VnResult.VN_OK), error);
            Assert.That(publication.TryFindAction("Play_U4_A", out CompanionTestAction row), Is.True);
            var owner = new GameObject("Serialized prepared Runtime");
            var source = new GameObject("Serialized prepared Action");
            owner.SetActive(false);
            source.SetActive(false);
            var runtime = owner.AddComponent<HarkenAudioSystem>();
            var emitter = source.AddComponent<HarkenEmitter>();
            var action = source.AddComponent<HarkenAction>();
            Set(emitter, "runtime", runtime);
            Set(action, "runtime", runtime);
            Set(action, "emitter", emitter);
            Set(action, "actionName", row.Name);
            Set(action, "projectId", publication.ProjectId);
            try
            {
                Assert.That(runtime.ConfigureBeforeStartup("UnusedTextAssetPath", "Init.bytes",
                    Array.Empty<ulong>(), true, false), Is.EqualTo(VnResult.VN_OK));
                Assert.That(runtime.ConfigureCompanionSourceBeforeStartup(captured), Is.EqualTo(VnResult.VN_OK));
                owner.SetActive(true);
                source.SetActive(true);
                Assert.That(runtime.IsPreparationComplete, Is.True, runtime.LastStartupError);
                Assert.That(runtime.PackRootPath, Is.Empty);
                Assert.That(runtime.Session.ProjectId, Is.EqualTo(publication.ProjectId));
                Assert.That(runtime.Session.StartupPacks, Is.Empty);
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(action.Play(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(action.Firing.Query(out VnActionFiringInfo info), Is.EqualTo(VnResult.VN_OK));
                Assert.That(info.actionId, Is.EqualTo(row.ActionStableId));
            }
            finally
            {
                runtime.ShutdownRuntime();
                Object.Destroy(source);
                Object.Destroy(owner);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator CapturedBytesUseSharedPreparationWithoutReplayingARefusedAutomaticTrigger()
        {
            string root = Path.Combine(Application.streamingAssetsPath, "Audio/U4Original");
            Assert.That(CompanionTestPublication.Read(root, null, out CompanionTestPublication publication, out string error),
                Is.EqualTo(VnResult.VN_OK), error);
            Assert.That(publication.TryFindAction("Play_U4_A", out CompanionTestAction row), Is.True);
            Assert.That(publication.TryFindPack(row.OwnerPackStableId, out CompanionTestPack pack), Is.True);
            bool supplied = false;
            ulong revision = 0;
            PackSource prepared = PackSource.FromReader(path =>
            {
                if (!supplied && (path == "Play_U4_A.bytes" || path == "Play_U4_A.json")) return null;
                string file = Path.Combine(root, path);
                return File.Exists(file) ? File.ReadAllBytes(file) : null;
            }, revision: () => revision);
            var owner = new GameObject("Prepared Runtime");
            var source = new GameObject("Prepared Action");
            owner.SetActive(false);
            source.SetActive(false);
            var runtime = owner.AddComponent<HarkenAudioSystem>();
            var emitter = source.AddComponent<HarkenEmitter>();
            var action = source.AddComponent<HarkenAction>();
            Set(emitter, "runtime", runtime);
            Set(action, "runtime", runtime);
            Set(action, "emitter", emitter);
            Set(action, "actionName", row.Name);
            Set(action, "projectId", publication.ProjectId);
            Set(action, "playOnEnable", true);
            try
            {
                Assert.That(runtime.ConfigureBeforeStartup("UnusedPreparedPath", publication.InitPack.Path, Array.Empty<ulong>(), true, false),
                    Is.EqualTo(VnResult.VN_OK));
                Assert.That(runtime.ConfigureCompanionSourceBeforeStartup(prepared), Is.EqualTo(VnResult.VN_OK));
                owner.SetActive(true);
                Assert.That(runtime.IsReady, Is.True, runtime.LastStartupError);
                Assert.That(runtime.PackRootPath, Is.Empty);
                LogAssert.Expect(LogType.Error, new Regex("Harken operation failed: Action .*VN_CONTENT_NOT_LOADED"));
                source.SetActive(true);
                yield return null;
                yield return null;
                Assert.That(action.LastResult, Is.EqualTo(VnResult.VN_CONTENT_NOT_LOADED));
                Assert.That(action.Firing.IsValid, Is.False);
                ulong attemptedSeed = Get<ulong>(action, "callerSeed");
                ++revision;
                supplied = true;
                Assert.That(runtime.Session.PrepareActions(new[] { row.Name }, publication.ProjectId,
                    out ActionPreparation ready), Is.EqualTo(VnResult.VN_OK));
                Assert.That(runtime.Session.AcquirePack(pack.PackStableId, out Pack first), Is.EqualTo(VnResult.VN_OK));
                Assert.That(runtime.Session.AcquirePack(pack.PackStableId, out Pack shared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(shared.Handle, Is.EqualTo(first.Handle));
                Assert.That(runtime.Session.ReleasePack(pack.PackStableId, first), Is.EqualTo(VnResult.VN_OK));
                for (int i = 0; i < 3; ++i) yield return null;
                Assert.That(Get<ulong>(action, "callerSeed"), Is.EqualTo(attemptedSeed));
                Assert.That(action.LastResult, Is.EqualTo(VnResult.VN_CONTENT_NOT_LOADED));
                Assert.That(action.Firing.IsValid, Is.False);
                Assert.That(action.Play(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(action.Firing.IsValid, Is.True);
            }
            finally
            {
                runtime.ShutdownRuntime();
                Object.Destroy(source);
                Object.Destroy(owner);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator ReaderStartRuntimeSucceedsOnceWithoutRegressingFilesystemTextAssetOrPreparedEntries()
        {
            const string logicalRoot = "Assets/Audio/U4Original";
            Dictionary<string, byte[]> files = LogicalFiles(logicalRoot, "U4Original");
            PackSource captured = PackSource.FromReader(path => MemoryReader(files)(logicalRoot + "/" + path));
            string root = Path.Combine(Application.streamingAssetsPath, "Audio/U4Original");
            Assert.That(CompanionTestPublication.Read(root, null, out CompanionTestPublication publication, out string error),
                Is.EqualTo(VnResult.VN_OK), error);
            Assert.That(publication.TryFindAction("Play_U4_A", out CompanionTestAction row), Is.True);
            HarkenPackSource asset = Resources.Load<HarkenPackSource>("HarkenSdkFixture/Source");
            Assert.That(asset, Is.Not.Null, "Prepare HarkenUnityFixtureAssets before running this test.");
            PackSource textAssetPrepared = CaptureSource(asset, "SourceInit");
            var owner = new GameObject("Reader Runtime");
            owner.SetActive(false);
            var runtime = owner.AddComponent<HarkenAudioSystem>();
            Set(runtime, "autoInitialize", false);
            Set(runtime, "persistAcrossScenes", false);
            try
            {
                Assert.That(runtime.ConfigureBeforeStartup("UnusedReaderPath", "Init.bytes",
                    Array.Empty<ulong>(), true, false), Is.EqualTo(VnResult.VN_OK));
                owner.SetActive(true);
                Assert.That(runtime.StartRuntime(captured), Is.EqualTo(VnResult.VN_OK),
                    runtime.LastStartupError);
                Assert.That(runtime.IsReady, Is.True);
                Assert.That(runtime.IsPreparationComplete, Is.True, runtime.LastStartupError);
                Assert.That(runtime.Session.PrepareActions(new[] { row.Name }, runtime.Session.ProjectId,
                    out ActionPreparation ready), Is.EqualTo(VnResult.VN_OK));
                Assert.That(runtime.Session.AcquirePack(row.OwnerPackStableId, out Pack demanded), Is.EqualTo(VnResult.VN_OK));
                Assert.That(demanded.IsValid, Is.True);
                Assert.That(runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));

                Assert.That(runtime.StartRuntime(Path.Combine(Application.streamingAssetsPath, "Audio/U4Original")),
                    Is.EqualTo(VnResult.VN_OK), runtime.LastStartupError);
                Assert.That(runtime.IsReady, Is.True);
                Assert.That(runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));

                Assert.That(runtime.StartRuntime(textAssetPrepared), Is.EqualTo(VnResult.VN_OK), runtime.LastStartupError);
                Assert.That(runtime.IsReady, Is.True);
                Assert.That(runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));

            }
            finally
            {
                runtime.ShutdownRuntime();
                Object.Destroy(owner);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator SetMasterVolumeAcceptsZeroPositiveOneAndClampAndRejectsNaN()
        {
            HarkenPackSource asset = Resources.Load<HarkenPackSource>("HarkenSdkFixture/Source");
            Assert.That(asset, Is.Not.Null, "Prepare HarkenUnityFixtureAssets before running this test.");
            PackSource prepared = CaptureSource(asset, "SourceInit");
            var owner = new GameObject("Master volume Runtime");
            owner.SetActive(false);
            var runtime = owner.AddComponent<HarkenAudioSystem>();
            Set(runtime, "autoInitialize", false);
            Set(runtime, "persistAcrossScenes", false);
            try
            {
                Assert.That(runtime.ConfigureBeforeStartup("UnusedMasterVolumePath", "Init.bytes",
                    Array.Empty<ulong>(), true, false), Is.EqualTo(VnResult.VN_OK));
                owner.SetActive(true);
                ExpectVolumeFailure(VnResult.VN_NOT_INITIALIZED);
                Assert.That(runtime.SetMasterVolume(1f), Is.EqualTo(VnResult.VN_NOT_INITIALIZED));
                Assert.That(runtime.StartRuntime(prepared), Is.EqualTo(VnResult.VN_OK), runtime.LastStartupError);
                Assert.That(runtime.IsReady, Is.True);
                Assert.That(HarkenEngine.SetBusVolume(VnConstants.VN_BUS_MASTER, 0f, 0),
                    Is.EqualTo(VnResult.VN_IN_USE));
                Assert.That(runtime.SetMasterVolume(0f), Is.EqualTo(VnResult.VN_OK));
                Assert.That(runtime.SetMasterVolume(0.5f), Is.EqualTo(VnResult.VN_OK));
                Assert.That(runtime.SetMasterVolume(1f), Is.EqualTo(VnResult.VN_OK));
                Assert.That(runtime.SetMasterVolume(2f), Is.EqualTo(VnResult.VN_OK));
                Assert.That(runtime.SetMasterVolume(-1f), Is.EqualTo(VnResult.VN_OK));
                ExpectVolumeFailure(VnResult.VN_INVALID_ARG);
                Assert.That(runtime.SetMasterVolume(float.NaN), Is.EqualTo(VnResult.VN_INVALID_ARG));
                ExpectVolumeFailure(VnResult.VN_INVALID_ARG);
                Assert.That(runtime.SetMasterVolume(float.PositiveInfinity), Is.EqualTo(VnResult.VN_INVALID_ARG));
                ExpectVolumeFailure(VnResult.VN_INVALID_ARG);
                Assert.That(runtime.SetMasterVolume(float.NegativeInfinity), Is.EqualTo(VnResult.VN_INVALID_ARG));
            }
            finally
            {
                runtime.ShutdownRuntime();
                Object.Destroy(owner);
            }
            yield return null;
        }

        private static PackSource CaptureSource(HarkenPackSource asset, string initName)
        {
            TextAsset init = Resources.Load<TextAsset>("HarkenSdkFixture/" + initName);
            Assert.That(init, Is.Not.Null);
            Assert.That(asset.CaptureCompanion(init, out PackSource source, out string error),
                Is.EqualTo(VnResult.VN_OK), error);
            return source;
        }

        private static void ExpectStartupFailure(VnResult result)
            => LogAssert.Expect(LogType.Error, new Regex("Harken operation failed: PackBootstrap.Start: " + result));

        private static void ExpectVolumeFailure(VnResult result)
            => LogAssert.Expect(LogType.Error,
                new Regex("Harken operation failed: HarkenAudioSystem.SetMasterVolume: " + result));

        private static Dictionary<string, byte[]> LogicalFiles(string logicalRoot, string name)
        {
            string disk = Path.Combine(Application.streamingAssetsPath, "Audio", name);
            var files = new Dictionary<string, byte[]>(StringComparer.Ordinal);
            foreach (string path in Directory.GetFiles(disk, "*", SearchOption.AllDirectories))
            {
                string relative = path.Substring(disk.Length)
                    .TrimStart(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
                    .Replace('\\', '/');
                files[string.IsNullOrEmpty(logicalRoot) ? relative : logicalRoot + "/" + relative] =
                    File.ReadAllBytes(path);
            }
            return files;
        }

        private static Func<string, byte[]> MemoryReader(Dictionary<string, byte[]> files)
            => path => files.TryGetValue(path, out byte[] bytes) ? (byte[])bytes.Clone() : null;

        private static T Get<T>(object target, string name)
            => (T)target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).GetValue(target);

        private static void Set(object target, string name, object value)
            => target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).SetValue(target, value);
    }
}
