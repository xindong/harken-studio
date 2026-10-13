using System;
using NUnit.Framework;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;
using Object = UnityEngine.Object;

namespace Harken.Unity.Tests
{
    public sealed class HarkenComponentTests
    {
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
            AssertScriptGuid("8d1c4e7a2b9f4c0ea5d16f3b7c8e9012", typeof(HarkenVariableReference));
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
    }
}
