using System;
using System.Collections;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text.RegularExpressions;
using NUnit.Framework;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;
using UnityEngine.TestTools;
using Harken.Unity.Editor;
using Object = UnityEngine.Object;

namespace Harken.Unity.Tests
{
    public sealed class HarkenEditorLifecycleTests
    {
        private static readonly string DomainToken = Guid.NewGuid().ToString("N");
        [SerializeField] private string folder;
        [SerializeField] private string scenePath;
        [SerializeField] private string priorScene;
        [SerializeField] private string beforeReload;
        [SerializeField] private string savedName;
        [SerializeField] private string savedProject;
        [SerializeField] private bool originalOptionsEnabled;
        [SerializeField] private EnterPlayModeOptions originalOptions;
        [SerializeField] private bool settingsSaved;
        [SerializeField] private bool ownedEngine;
        [SerializeField] private string projectSettingsSnapshot;

        [UnitySetUp]
        public IEnumerator SetUp()
        {
            if (!string.IsNullOrEmpty(folder)) yield break;
            originalOptionsEnabled = EditorSettings.enterPlayModeOptionsEnabled;
            originalOptions = EditorSettings.enterPlayModeOptions;
            settingsSaved = true;
            EditorSettings.enterPlayModeOptionsEnabled = false;
            priorScene = SceneManager.GetActiveScene().path;
            folder = "Assets/HarkenLifecycleFixture_" + Guid.NewGuid().ToString("N");
            AssetDatabase.CreateFolder("Assets", Path.GetFileName(folder));
            scenePath = folder + "/Fixture.unity";
            if (string.IsNullOrEmpty(projectSettingsSnapshot))
            {
                var projectSettings = HarkenProjectSettingsProvider.GetOrCreateSettings();
                projectSettingsSnapshot = JsonUtility.ToJson(projectSettings);
            }
            EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
            yield break;
        }

        [UnityTearDown]
        public IEnumerator TearDown()
        {
            VnResult shutdownResult = VnResult.VN_OK;
            try
            {
                if (EditorApplication.isPlaying) yield return new ExitPlayMode();
                try
                {
                    if (ownedEngine)
                    {
                        try
                        {
                            if (HarkenAudioSystem.Instance != null)
                                shutdownResult = HarkenAudioSystem.Instance.ShutdownRuntime();
                        }
                        finally
                        {
                            HarkenEditorPreview.ShutdownOwnedSession();
                            HarkenEngine.Term();
                        }
                    }
                }
                finally
                {
                    if (settingsSaved)
                    {
                        EditorSettings.enterPlayModeOptionsEnabled = originalOptionsEnabled;
                        EditorSettings.enterPlayModeOptions = originalOptions;
                    }
                    try
                    {
                        if (!string.IsNullOrEmpty(priorScene) && File.Exists(priorScene))
                            EditorSceneManager.OpenScene(priorScene, OpenSceneMode.Single);
                        else EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
                    }
                    finally
                    {
                        if (!string.IsNullOrEmpty(projectSettingsSnapshot))
                        {
                            var projectSettings = HarkenProjectSettingsProvider.GetOrCreateSettings();
                            JsonUtility.FromJsonOverwrite(projectSettingsSnapshot, projectSettings);
                            EditorUtility.SetDirty(projectSettings);
                            AssetDatabase.SaveAssetIfDirty(projectSettings);
                        }
                        if (!string.IsNullOrEmpty(folder)) AssetDatabase.DeleteAsset(folder);
                    }
                }
            }
            finally
            {
                folder = null;
                scenePath = null;
                ownedEngine = false;
                settingsSaved = false;
            }
            Assert.That(shutdownResult, Is.EqualTo(VnResult.VN_OK));
        }

        [UnityTest]
        public IEnumerator SuspendedSampleDoesNotClaimTheRuntimeSingleton()
        {
            string sampleScene = AssetDatabase.GUIDToAssetPath("df1e14712a3bfa646afce84f432ea5c9");
            Assert.That(sampleScene, Is.Not.Empty, "Import PlayableDemo before running the sample suspension contract.");
            EditorSceneManager.OpenScene(sampleScene, OpenSceneMode.Single);
            yield return new EnterPlayMode();
            HarkenAudioSystem sampleRuntime = Find<HarkenAudioSystem>();
            Assert.That(sampleRuntime.gameObject.activeSelf, Is.False);
            Assert.That(sampleRuntime.Session, Is.Null);
            Assert.That(HarkenAudioSystem.Instance, Is.Null);
            Assert.That(FindAll<HarkenAction>().All(action => !action.isActiveAndEnabled), Is.True);
            Assert.That(Find<HarkenVariable>().isActiveAndEnabled, Is.False);
            Assert.That(Find<HarkenEmitter>().isActiveAndEnabled, Is.False);
            Assert.That(Find<HarkenListener>().enabled, Is.False);
            yield return null;
            var owner = new GameObject("Independent Runtime");
            owner.SetActive(false);
            try
            {
                var runtime = owner.AddComponent<HarkenAudioSystem>();
                var serialized = new SerializedObject(runtime);
                serialized.FindProperty("autoInitialize").boolValue = false;
                serialized.FindProperty("persistAcrossScenes").boolValue = false;
                serialized.ApplyModifiedPropertiesWithoutUndo();
                owner.SetActive(true);
                Assert.That(HarkenAudioSystem.Instance, Is.SameAs(runtime));
                Assert.That(runtime.State, Is.EqualTo(HarkenRuntimeState.Stopped));
                Assert.That(runtime.Session, Is.Null);
            }
            finally { Object.DestroyImmediate(owner); }
            yield return new ExitPlayMode();
        }

        [UnityTest]
        public IEnumerator PrefabUndoRedoAndPreviewOwnershipSurviveActualScriptAndPlayModeReloads()
        {
            HarkenEditorServiceTests.RequirePublicationTooling();
            HarkenEditorCatalog catalog = Catalog("U4Original");
            var original = new GameObject("Persistent Action");
            original.SetActive(false);
            var action = original.AddComponent<HarkenAction>();
            Assert.That(catalog.SelectAction(action, "Play_U4_A", false, out _), Is.EqualTo(VnResult.VN_OK));
            string prefabPath = folder + "/Action.prefab";
            PrefabUtility.SaveAsPrefabAsset(original, prefabPath);
            Object.DestroyImmediate(original);
            var instance = (GameObject)PrefabUtility.InstantiatePrefab(AssetDatabase.LoadAssetAtPath<GameObject>(prefabPath));
            action = instance.GetComponent<HarkenAction>();
            Undo.IncrementCurrentGroup();
            Assert.That(catalog.SelectAction(action, "Play_U4_B", false, out _), Is.EqualTo(VnResult.VN_OK));
            Undo.FlushUndoRecordObjects();
            Undo.PerformUndo();
            Assert.That(action.ActionName, Is.EqualTo("Play_U4_A"));
            Undo.PerformRedo();
            Assert.That(action.ActionName, Is.EqualTo("Play_U4_B"));
            savedName = action.ActionName;
            savedProject = action.ProjectId;
            AssertOverride(action);
            Assert.That(EditorSceneManager.SaveScene(SceneManager.GetActiveScene(), scenePath), Is.True);
            PreparePreview();
            beforeReload = DomainToken;
            EditorUtility.RequestScriptReload();
            yield return new WaitForDomainReload();
            AssertReload();
            EditorSceneManager.OpenScene(scenePath, OpenSceneMode.Single);
            AssertSavedAction();
            PreparePreview();
            beforeReload = DomainToken;
            yield return new EnterPlayMode();
            AssertReload();
            AssertSavedAction(checkPrefabMetadata: false);
            VnConfig config = HarkenEngine.DefaultConfig();
            config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
            Assert.That(HarkenEngine.Init(in config), Is.EqualTo(VnResult.VN_OK),
                "Entering Play Mode must Term the prior Editor preview session.");
            Assert.That(HarkenEngine.Term(), Is.EqualTo(VnResult.VN_OK));
            yield return new ExitPlayMode();
            Assert.That(EditorApplication.isPlaying, Is.False);
            Assert.That(EditorApplication.isPlayingOrWillChangePlaymode, Is.False);
            AssertSavedAction();
            PreparePreview();
        }

        [UnityTest]
        public IEnumerator RenamedPublicationDoesNotRewriteTheSavedNameAcrossARealSceneReload()
        {
            CreateComponentScene("U4Original", "Play_U4_A");
            yield return new EnterPlayMode();
            HarkenAudioSystem runtime = Find<HarkenAudioSystem>();
            HarkenAction action = Find<HarkenAction>();
            ownedEngine = runtime.IsReady;
            Assert.That(runtime.IsPreparationComplete, Is.True, runtime.LastStartupError);
            Post(action);
            Assert.That(Get<bool>(action, "cacheValid"), Is.True);
            savedName = action.ActionName;
            savedProject = action.ProjectId;
            Assert.That(runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
            ConfigureRuntime(runtime, "U4Renamed", "Play_U4_Renamed");
            Assert.That(runtime.StartRuntime(PublicationRoot("U4Renamed")), Is.EqualTo(VnResult.VN_OK));
            RefuseOldName(action);
            Assert.That(action.ActionName, Is.EqualTo(savedName));
            yield return new ExitPlayMode();
            runtime = Find<HarkenAudioSystem>();
            action = Find<HarkenAction>();
            ConfigureRuntime(runtime, "U4Renamed", "Play_U4_Renamed");
            SetString(action, "actionName", savedName);
            SetString(action, "projectId", savedProject);
            AssertNoSerializedCache(action);
            Assert.That(EditorSceneManager.SaveScene(SceneManager.GetActiveScene(), scenePath), Is.True);
            int previousInstance = action.GetInstanceID();
            EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
            EditorSceneManager.OpenScene(scenePath, OpenSceneMode.Single);
            Assert.That(Find<HarkenAction>().GetInstanceID(), Is.Not.EqualTo(previousInstance));
            Assert.That(Find<HarkenAction>().ActionName, Is.EqualTo(savedName));
            Assert.That(Find<HarkenAction>().ProjectId, Is.EqualTo(savedProject));
            yield return new EnterPlayMode();
            action = Find<HarkenAction>();
            Assert.That(Find<HarkenAudioSystem>().IsPreparationComplete, Is.True);
            RefuseOldName(action);
            Assert.That(action.ActionName, Is.EqualTo("Play_U4_A"));
            SetString(action, "actionName", "Play_U4_Renamed");
            Post(action);
            Assert.That(action.Firing.Query(out VnActionFiringInfo info), Is.EqualTo(VnResult.VN_OK));
            Assert.That(info.actionId, Is.EqualTo(ReadAction("U4Renamed", "Play_U4_Renamed").Names.RootId));
            yield return new ExitPlayMode();
        }

        private void PreparePreview()
        {
            var preview = new HarkenEditorPreview();
            HarkenEditorCatalog catalog = Catalog("U4Original");
            Assert.That(catalog.GetPreparationOrder("Play_U4_B", out ulong[] ids, out _), Is.EqualTo(VnResult.VN_OK));
            Assert.That(preview.Prepare(PublicationRoot("U4Original"), null, ids, true), Is.EqualTo(VnResult.VN_OK),
                "A new preview must initialize after the prior session was terminated.");
            ownedEngine = true;
            Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
            Assert.That(preview.Play("Play_U4_B", catalog.Publication.ProjectId, out _), Is.EqualTo(VnResult.VN_OK));
        }

        private void AssertReload() => Assert.That(DomainToken, Is.Not.EqualTo(beforeReload),
            "The test must observe an actual managed-domain/assembly reload.");

        private void AssertSavedAction(bool checkPrefabMetadata = true)
        {
            HarkenAction action = Find<HarkenAction>();
            Assert.That(action.ActionName, Is.EqualTo(savedName));
            Assert.That(action.ProjectId, Is.EqualTo(savedProject));
            if (checkPrefabMetadata) AssertOverride(action);
            AssertNoSerializedCache(action);
        }

        private static void AssertOverride(HarkenAction action)
        {
            PropertyModification[] modifications = PrefabUtility.GetPropertyModifications(action);
            Assert.That(modifications, Is.Not.Null, "Editor prefab override metadata must survive reload.");
            Assert.That(modifications.Any(row => row.propertyPath == "actionName" && row.value == "Play_U4_B"), Is.True);
        }

        private static void AssertNoSerializedCache(HarkenAction action)
        {
            var serialized = new SerializedObject(action);
            Assert.That(serialized.FindProperty("cachedResolved"), Is.Null);
            Assert.That(serialized.FindProperty("cacheValid"), Is.Null);
        }

        private void CreateComponentScene(string directory, string name)
        {
            var owner = new GameObject("Reload Runtime");
            var source = new GameObject("Reload Action");
            owner.SetActive(false);
            source.SetActive(false);
            var runtime = owner.AddComponent<HarkenAudioSystem>();
            var emitter = source.AddComponent<HarkenEmitter>();
            var action = source.AddComponent<HarkenAction>();
            ConfigureRuntime(runtime, directory, name);
            SetReference(emitter, "runtime", runtime);
            SetReference(action, "runtime", runtime);
            SetReference(action, "emitter", emitter);
            SetString(action, "actionName", name);
            SetString(action, "projectId", ReadInit(directory).Facts.projectId.ToString());
            owner.SetActive(true);
            source.SetActive(true);
            Assert.That(EditorSceneManager.SaveScene(SceneManager.GetActiveScene(), scenePath), Is.True);
        }

        private static void ConfigureRuntime(HarkenAudioSystem runtime, string directory, string name)
        {
            Assert.That(runtime.ConfigureBeforeStartup("Audio/" + directory, "Init.bytes", Array.Empty<ulong>(), true, false),
                Is.EqualTo(VnResult.VN_OK));
            var projectSettings = HarkenProjectSettingsProvider.GetOrCreateSettings();
            var serializedSettings = new SerializedObject(projectSettings);
            serializedSettings.FindProperty("initPack").stringValue = "Init.bytes";
            serializedSettings.FindProperty("packDirectory").stringValue =
                Path.Combine(Application.streamingAssetsPath, "Audio", directory);
            serializedSettings.ApplyModifiedPropertiesWithoutUndo();
            AssetDatabase.SaveAssetIfDirty(projectSettings);
            Assert.That(runtime.ConfigurePlatformBeforeStartup(null), Is.EqualTo(VnResult.VN_OK));
        }

        private static void RefuseOldName(HarkenAction action)
        {
            Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
            LogAssert.Expect(LogType.Error, new Regex("Harken operation failed: Action .*VN_CONTENT_NOT_LOADED"));
            Assert.That(action.Play(), Is.EqualTo(VnResult.VN_CONTENT_NOT_LOADED));
            Assert.That(action.Firing.IsValid, Is.False);
        }

        private static void Post(HarkenAction action)
        {
            Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
            Assert.That(action.Play(), Is.EqualTo(VnResult.VN_OK));
            Assert.That(action.Firing.IsValid, Is.True);
        }

        private static HarkenEditorCatalog Catalog(string directory)
        {
            var catalog = new HarkenEditorCatalog();
            Assert.That(catalog.Refresh(PublicationRoot(directory)), Is.EqualTo(VnResult.VN_OK), catalog.Diagnostic);
            return catalog;
        }

        private static CompanionInput ReadInit(string directory)
        {
            var source = new PackDirectorySource(PublicationRoot(directory), null);
            Assert.That(source.Capture(null, default, null, out CompanionInput input), Is.EqualTo(VnResult.VN_OK));
            return input;
        }

        private static CompanionInput ReadAction(string directory, string name)
        {
            CompanionInput init = ReadInit(directory);
            var source = new PackDirectorySource(PublicationRoot(directory), null);
            Assert.That(source.Capture(name, init.Facts.projectId, init.Bytes, out CompanionInput input), Is.EqualTo(VnResult.VN_OK));
            return input;
        }

        private static string PublicationRoot(string name) => Path.Combine(Application.streamingAssetsPath, "Audio", name);
        private static T[] FindAll<T>() where T : Component => SceneManager.GetActiveScene().GetRootGameObjects()
            .SelectMany(root => root.GetComponentsInChildren<T>(true)).ToArray();
        private static T Find<T>() where T : Component => FindAll<T>().Single();
        private static T Get<T>(object target, string name)
            => (T)target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).GetValue(target);

        private static void SetString(Object target, string name, string value)
        {
            var serialized = new SerializedObject(target);
            serialized.FindProperty(name).stringValue = value;
            serialized.ApplyModifiedPropertiesWithoutUndo();
        }

        private static void SetReference(Object target, string name, Object value)
        {
            var serialized = new SerializedObject(target);
            serialized.FindProperty(name).objectReferenceValue = value;
            serialized.ApplyModifiedPropertiesWithoutUndo();
        }
    }
}
