using System;
using System.Reflection;
using NUnit.Framework;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;
using UnityEngine.TestTools;
using Object = UnityEngine.Object;

namespace Harken.Unity.Tests
{
    public sealed class HarkenComponentTests
    {
        [Test]
        public void ManagedPlaybackPreservesLiveHandlesAcrossGrowthAndSlotReuse()
        {
            var table = new HarkenAudioSystem.ManagedFiringTable();
            for (int i = 0; i < 257; ++i)
            {
                int slot = table.Reserve();
                table[slot].Firing = new ActionFiring((ulong)i + 1);
            }
            Assert.That(table.Count, Is.EqualTo(257));
            var returned = new System.Collections.Generic.HashSet<int>();
            for (int slot = table.First; slot >= 0;)
            {
                ref var row = ref table[slot];
                int next = row.Next;
                if (row.Firing.Handle % 2 != 0)
                {
                    returned.Add(slot);
                    table.Return(slot);
                }
                slot = next;
            }
            int reused = table.Reserve();
            Assert.That(returned, Does.Contain(reused));
            Assert.That(table[reused].Firing.IsValid, Is.False);
            table[reused].Firing = new ActionFiring(258);
            int current = table.First;
            for (ulong handle = 2; handle <= 258; handle += 2)
            {
                Assert.That(current, Is.GreaterThanOrEqualTo(0));
                Assert.That(table[current].Firing.Handle, Is.EqualTo(handle));
                current = table[current].Next;
            }
            Assert.That(current, Is.EqualTo(-1));
            Assert.That(table.Last, Is.EqualTo(reused));
            Assert.That(table.Count, Is.EqualTo(129));
        }

        [Test]
        public void ComponentsAttachToOrdinaryGameObjects()
        {
            GameObject owner = new GameObject("Runtime");
            GameObject source = new GameObject("Emitter");
            owner.SetActive(false);
            source.SetActive(false);
            try
            {
                Assert.That(owner.AddComponent<HarkenAudioSystem>(), Is.Not.Null);
                Assert.That(owner.AddComponent<HarkenPack>(), Is.Not.Null);
                Assert.That(source.AddComponent<HarkenEmitter>(), Is.Not.Null);
                Assert.That(source.AddComponent<HarkenListener>(), Is.Not.Null);
                Assert.That(source.AddComponent<HarkenAction>(), Is.Not.Null);
                Assert.That(source.AddComponent<HarkenVariable>(), Is.Not.Null);
            }
            finally
            {
                Object.DestroyImmediate(owner);
                Object.DestroyImmediate(source);
            }
        }

        [TestCase((uint)VnLogLevel.VN_LOG_INFO, LogType.Log)]
        [TestCase((uint)VnLogLevel.VN_LOG_WARN, LogType.Warning)]
        [TestCase((uint)VnLogLevel.VN_LOG_ERROR, LogType.Error)]
        [TestCase(99u, LogType.Log)]
        public void NativeLogSeverityReachesUnityConsole(uint level, LogType expected)
        {
            var owner = new GameObject("Native log routing");
            owner.SetActive(false);
            try
            {
                var runtime = owner.AddComponent<HarkenAudioSystem>();
                LogAssert.Expect(expected, "[Harken " + level + "/42] native diagnostic");
                typeof(HarkenAudioSystem).GetMethod("HandleLog",
                    BindingFlags.Instance | BindingFlags.NonPublic)
                    .Invoke(runtime, new object[] { level, 42u, "native diagnostic", null });
                LogAssert.NoUnexpectedReceived();
            }
            finally { Object.DestroyImmediate(owner); }
        }

        [Test]
        public void RuntimeFaultReportsOneConsoleErrorAndOneOwnerEvent()
        {
            var owner = new GameObject("Runtime fault routing");
            owner.SetActive(false);
            var runtime = owner.AddComponent<HarkenAudioSystem>();
            var handler = (Action<RuntimeFaultInfo>)Delegate.CreateDelegate(typeof(Action<RuntimeFaultInfo>),
                runtime, typeof(HarkenAudioSystem).GetMethod("HandleRuntimeFault",
                    BindingFlags.Instance | BindingFlags.NonPublic));
            int events = 0;
            runtime.RuntimeRecreationRequired += (sender, info) =>
            {
                Assert.That(sender, Is.SameAs(runtime));
                ++events;
                RuntimeFaultDiagnostics.ObserveResult(VnResult.VN_RUNTIME_RECREATE_REQUIRED);
            };
            RuntimeFaultDiagnostics.Reset();
            RuntimeFaultDiagnostics.Reported += handler;
            try
            {
                LogAssert.Expect(LogType.Error, "Runtime recreation required; fault details unavailable.");
                RuntimeFaultDiagnostics.ObserveResult(VnResult.VN_RUNTIME_RECREATE_REQUIRED);
                typeof(HarkenAudioSystem).GetMethod("HandleLog", BindingFlags.Instance | BindingFlags.NonPublic)
                    .Invoke(runtime, new object[] { (uint)VnLogLevel.VN_LOG_ERROR,
                        VnConstants.VN_LOG_RUNTIME_RECREATE_REQUIRED, "duplicate", null });
                RuntimeFaultDiagnostics.ObserveResult(VnResult.VN_RUNTIME_RECREATE_REQUIRED);
                Assert.That(events, Is.EqualTo(1));
                LogAssert.NoUnexpectedReceived();
            }
            finally
            {
                RuntimeFaultDiagnostics.Reported -= handler;
                RuntimeFaultDiagnostics.Reset();
                Object.DestroyImmediate(owner);
            }
        }

        [Test]
        public void PreservedScriptGuidsResolveToRenamedTypes()
        {
            AssertScriptGuid("36331721422512843a2073fdb8a0a5b7", typeof(HarkenAudioSystem));
            AssertScriptGuid("86dc2ba3718492d4d81ac658c4d37948", typeof(HarkenEmitter));
            AssertScriptGuid("085a7acf12a2c55459f6bf39baa74f37", typeof(HarkenListener));
            AssertScriptGuid("2ef3314cf6c38e04d8e2928103934f08", typeof(HarkenAction));
            AssertScriptGuid("cea760cd59c04124ea8e13425b7e572d", typeof(HarkenPack));
            AssertScriptGuid("93afe7f51ef944b48c2b367b71da7449", typeof(HarkenPackSource));
            AssertScriptGuid("287bcdb1554757942a41316f40863746", typeof(HarkenPackStartup));
            AssertScriptGuid("a16a98bff1bb7844eb17fbb89499c665", typeof(HarkenVariable));
            AssertScriptGuid("e2504957c026421881d6e090940d852c", typeof(HarkenProjectSettings));
        }

        [Test]
        public void CoordinateConversionPreservesUnityHandednessContract()
        {
            VnVec3 value = UnityCoordinates.ToHarken(2.0f, 3.0f, 4.0f);
            Assert.That(value.x, Is.EqualTo(2.0f));
            Assert.That(value.y, Is.EqualTo(3.0f));
            Assert.That(value.z, Is.EqualTo(-4.0f));
        }

        private static void AssertScriptGuid(string guid, Type expected)
        {
            string path = AssetDatabase.GUIDToAssetPath(guid);
            Assert.That(path, Is.Not.Empty, guid);
            MonoScript script = AssetDatabase.LoadAssetAtPath<MonoScript>(path);
            Assert.That(script, Is.Not.Null, path);
            Assert.That(script.GetClass(), Is.EqualTo(expected), path);
        }

        [Test]
        public void ManualInitializationSettingSurvivesPrefabAndSceneReload()
        {
            string folder = "Assets/HarkenAudioSystemPersistence_" + Guid.NewGuid().ToString("N");
            AssetDatabase.CreateFolder("Assets", folder.Substring("Assets/".Length));
            Scene scene = default;
            try
            {
                scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
                var owner = new GameObject("Manual audio system");
                owner.SetActive(false);
                SceneManager.MoveGameObjectToScene(owner, scene);
                var system = owner.AddComponent<HarkenAudioSystem>();
                var serialized = new SerializedObject(system);
                Assert.That(serialized.FindProperty("autoInitialize").boolValue, Is.True);
                serialized.FindProperty("autoInitialize").boolValue = false;
                serialized.ApplyModifiedPropertiesWithoutUndo();
                PrefabUtility.SaveAsPrefabAsset(owner, folder + "/AudioSystem.prefab");
                Object.DestroyImmediate(owner);
                var instance = (GameObject)PrefabUtility.InstantiatePrefab(
                    AssetDatabase.LoadAssetAtPath<GameObject>(folder + "/AudioSystem.prefab"), scene);
                serialized = new SerializedObject(instance.GetComponent<HarkenAudioSystem>());
                Assert.That(serialized.FindProperty("autoInitialize").boolValue, Is.False);
                Assert.That(EditorSceneManager.SaveScene(scene, folder + "/AudioSystem.unity"), Is.True);
                EditorSceneManager.CloseScene(scene, true);
                scene = EditorSceneManager.OpenScene(folder + "/AudioSystem.unity", OpenSceneMode.Additive);
                serialized = new SerializedObject(scene.GetRootGameObjects()[0].GetComponent<HarkenAudioSystem>());
                Assert.That(serialized.FindProperty("autoInitialize").boolValue, Is.False);
            }
            finally
            {
                if (scene.IsValid() && scene.isLoaded)
                    EditorSceneManager.CloseScene(scene, true);
                AssetDatabase.DeleteAsset(folder);
            }
        }

        [Test]
        public void ActionNameAndProjectSurvivePrefabSceneAndUndo()
        {
            string folder = "Assets/HarkenActionPersistence_" + Guid.NewGuid().ToString("N");
            AssetDatabase.CreateFolder("Assets", folder.Substring("Assets/".Length));
            Scene scene = default;
            try
            {
                scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
                GameObject source = new GameObject("Named Action");
                source.SetActive(false);
                SceneManager.MoveGameObjectToScene(source, scene);
                HarkenAction action = source.AddComponent<HarkenAction>();
                var serialized = new SerializedObject(action);
                serialized.FindProperty("actionName").stringValue = "Play_Exact_Name";
                serialized.FindProperty("projectId").stringValue = "01234567-89ab-cdef-8123-456789abcdef";
                serialized.ApplyModifiedPropertiesWithoutUndo();
                PrefabUtility.SaveAsPrefabAsset(source, folder + "/Action.prefab");
                Object.DestroyImmediate(source);
                GameObject instance = (GameObject)PrefabUtility.InstantiatePrefab(
                    AssetDatabase.LoadAssetAtPath<GameObject>(folder + "/Action.prefab"), scene);
                action = instance.GetComponent<HarkenAction>();
                serialized = new SerializedObject(action);
                Undo.RecordObject(action, "Change Action name");
                serialized.FindProperty("actionName").stringValue = "Play_Changed";
                serialized.ApplyModifiedProperties();
                Undo.FlushUndoRecordObjects();
                Undo.PerformUndo();
                Assert.That(action.ActionName, Is.EqualTo("Play_Exact_Name"));
                Assert.That(action.ProjectId, Is.EqualTo("01234567-89ab-cdef-8123-456789abcdef"));
                Assert.That(EditorSceneManager.SaveScene(scene, folder + "/Action.unity"), Is.True);
                EditorSceneManager.CloseScene(scene, true);
                scene = EditorSceneManager.OpenScene(folder + "/Action.unity", OpenSceneMode.Additive);
                action = scene.GetRootGameObjects()[0].GetComponent<HarkenAction>();
                Assert.That(action.ActionName, Is.EqualTo("Play_Exact_Name"));
                Assert.That(action.ProjectId, Is.EqualTo("01234567-89ab-cdef-8123-456789abcdef"));
                serialized = new SerializedObject(action);
                Assert.That(serialized.FindProperty("cachedResolved"), Is.Null);
                Assert.That(serialized.FindProperty("cacheValid"), Is.Null);
            }
            finally
            {
                if (scene.IsValid() && scene.isLoaded)
                    EditorSceneManager.CloseScene(scene, true);
                AssetDatabase.DeleteAsset(folder);
            }
        }

        [TestCase("")]
        [TestCase(null)]
        [TestCase("not-a-project")]
        [TestCase("00000000-0000-0000-0000-000000000000")]
        public void ProjectBoundResolveRejectsUnboundOrInvalidIdentity(string projectId)
        {
            Assert.That(Actions.Resolve("Play_Exact_Name", projectId, out ActionResolved resolved),
                Is.EqualTo(VnResult.VN_INVALID_ARG));
            Assert.That(resolved.ActionId.IsNull, Is.True);
        }

        [Test]
        public void DestroyedEmitterComponentNeverBecomesGlobalWrite()
        {
            var owner = new GameObject("VariableEmitterScope");
            owner.SetActive(false);
            try
            {
                var component = owner.AddComponent<HarkenVariable>();
                var emitter = owner.AddComponent<HarkenEmitter>();
                typeof(HarkenVariable).GetField("emitter", BindingFlags.Instance | BindingFlags.NonPublic)
                    .SetValue(component, emitter);
                Object.DestroyImmediate(emitter);
                Assert.That(ReferenceEquals(emitter, null), Is.False);
                Assert.That(emitter == null, Is.True);
                Assert.That(component.Apply(), Is.EqualTo(VnResult.VN_STALE_HANDLE));
            }
            finally
            {
                Object.DestroyImmediate(owner);
            }
        }

        [Test]
        public void LocalClearAndPluralSettersAreAbsentAndScalarSettersRemainPublic()
        {
            Assert.That(typeof(Emitter).GetMethod("ClearValue"), Is.Null);
            Assert.That(typeof(Emitter).GetMethod("ClearValues"), Is.Null);
            Assert.That(typeof(HarkenEmitter).GetMethod("ResetVariables"), Is.Null);
            Assert.That(typeof(Emitter).GetMethod("SetValue"), Is.Null);
            Assert.That(typeof(Emitter).GetMethod("SetValues"), Is.Null);
            Assert.That(typeof(Values).GetMethod("SetValues"), Is.Null);
            Assert.That(typeof(Values).GetMethod("ResetValues"), Is.Null);
            MethodInfo scalar = typeof(Values).GetMethod("SetValue",
                new[] { typeof(VnObjectId), typeof(float), typeof(Emitter?) });
            Assert.That(scalar, Is.Not.Null);
            Assert.That(scalar.GetParameters()[2].ParameterType, Is.EqualTo(typeof(Emitter?)));
            Assert.That(scalar.GetParameters()[2].IsOptional, Is.True);
            MethodInfo named = typeof(Values).GetMethod("SetValue",
                new[] { typeof(string), typeof(string), typeof(float), typeof(Emitter?) });
            Assert.That(named, Is.Not.Null);
            Assert.That(named.GetParameters()[3].IsOptional, Is.True);
            Assert.That(typeof(Actions).GetMethod("Post",
                new[] { typeof(string), typeof(Emitter), typeof(ActionFiring).MakeByRefType() }), Is.Not.Null);
        }
    }
}
