using System;
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using Harken.Samples.PlayableDemo;
using Harken.Unity;
using Harken.Unity.Editor;
using Object = UnityEngine.Object;

namespace Harken.Samples.PlayableDemo.Editor
{
    public static class HarkenDemoSceneBuilder
    {
        [Serializable]
        private sealed class VariableManifest
        {
            public VariableCatalog variableCatalog;
        }

        [Serializable]
        private sealed class VariableCatalog
        {
            public VariableEntry[] variables;
        }

        [Serializable]
        private sealed class VariableEntry
        {
            public string codeName;
            public string variableId;
        }

        private static readonly string[] ActionNames =
        {
            "Play_Random_Draw", "Play_Shuffle", "Play_Random_Queue",
            "Play_Sequence", "Play_Sequence_Queue", "Play_Variable_Control"
        };
        private static readonly string[] Labels =
        {
            "Random", "Shuffle", "Random Queue", "Sequence",
            "Sequence Queue", "Variable Control"
        };

        [MenuItem("Harken/Playable Demo/Rebuild Scene and Prefabs")]
        public static void Rebuild() => RebuildAtPath(FindSampleRoot());

        public static void RebuildAtPath(string root)
        {
            if (EditorApplication.isPlayingOrWillChangePlaymode)
                throw new InvalidOperationException("Rebuild the sample outside Play Mode.");
            if (string.IsNullOrEmpty(root))
            {
                throw new DirectoryNotFoundException(
                    "Import the Playable Audio Demo sample first.");
            }
            var catalog = new HarkenEditorCatalog();
            VnResult result = catalog.RefreshPlatform(Path.Combine(root, "PackSource"), null, null,
                HarkenAudioSystem.CurrentPackBasePlatform());
            if (result != VnResult.VN_OK) throw new InvalidDataException(catalog.Diagnostic);
            if (!PackManifest.TryLoadManifest(catalog.DirectoryPath, catalog.Publication.Language, out _, out _))
                throw new InvalidDataException("The sample publication has missing or mismatched Pack files.");
            string manifestJson = File.ReadAllText(Path.Combine(catalog.DirectoryPath, "HarkenPackManifest.json"));
            result = HarkenVariableCatalogGenerator.ValidatePublication(manifestJson, catalog.DirectoryPath, out string variableError);
            if (result != VnResult.VN_OK) throw new InvalidDataException(variableError);
            VariableManifest manifest = JsonUtility.FromJson<VariableManifest>(manifestJson);
            VnObjectId choiceId = default;
            if (manifest?.variableCatalog?.variables != null)
                foreach (VariableEntry entry in manifest.variableCatalog.variables)
                    if (entry.codeName == "Demo_Choice")
                        choiceId = VnObjectId.Parse(entry.variableId);
            if (choiceId.IsNull) throw new InvalidDataException("The sample publication has no Demo_Choice variable.");
            var startupIds = new List<ulong>();
            var prepared = new HashSet<ulong>();
            foreach (string name in ActionNames)
            {
                result = catalog.GetPreparationOrder(name, out ulong[] required, out string error);
                if (result != VnResult.VN_OK) throw new InvalidDataException(error);
                foreach (ulong id in required)
                    if (prepared.Add(id)) startupIds.Add(id);
            }
            string prefabRoot = root + "/Prefabs";
            string sceneRoot = root + "/Scenes";
            Directory.CreateDirectory(prefabRoot);
            Directory.CreateDirectory(sceneRoot);

            GameObject runtimeTemplate = new GameObject("Harken Runtime");
            HarkenAudioSystem runtimeComponent =
                runtimeTemplate.AddComponent<HarkenAudioSystem>();
            ConfigureRuntime(runtimeComponent, catalog.Publication, startupIds);
            string runtimePrefabPath = prefabRoot + "/HarkenAudioSystem.prefab";
            PrefabUtility.SaveAsPrefabAsset(runtimeTemplate, runtimePrefabPath);
            Object.DestroyImmediate(runtimeTemplate);

            GameObject emitterTemplate = new GameObject("Harken Emitter");
            emitterTemplate.AddComponent<HarkenEmitter>();
            string emitterPrefabPath = prefabRoot + "/HarkenEmitter.prefab";
            PrefabUtility.SaveAsPrefabAsset(emitterTemplate, emitterPrefabPath);
            Object.DestroyImmediate(emitterTemplate);

            var scene = EditorSceneManager.NewScene(
                NewSceneSetup.EmptyScene, NewSceneMode.Single);
            GameObject runtimeObject = (GameObject)PrefabUtility.InstantiatePrefab(
                AssetDatabase.LoadAssetAtPath<GameObject>(runtimePrefabPath));
            HarkenAudioSystem runtime = runtimeObject.GetComponent<HarkenAudioSystem>();

            GameObject emitterObject = (GameObject)PrefabUtility.InstantiatePrefab(
                AssetDatabase.LoadAssetAtPath<GameObject>(emitterPrefabPath));
            HarkenEmitter emitter = emitterObject.GetComponent<HarkenEmitter>();
            var emitterSerialized = new SerializedObject(emitter);
            emitterSerialized.FindProperty("runtime").objectReferenceValue = runtime;
            emitterSerialized.ApplyModifiedPropertiesWithoutUndo();
            emitterObject.transform.position = new Vector3(0.0f, 1.0f, 0.0f);

            var actions = new HarkenAction[ActionNames.Length];
            for (int i = 0; i < ActionNames.Length; ++i)
            {
                GameObject actionObject = new GameObject(Labels[i]);
                actionObject.transform.SetParent(emitterObject.transform, false);
                actions[i] = actionObject.AddComponent<HarkenAction>();
                SerializedObject serialized = new SerializedObject(actions[i]);
                serialized.FindProperty("runtime").objectReferenceValue = runtime;
                serialized.FindProperty("emitter").objectReferenceValue = emitter;
                serialized.ApplyModifiedPropertiesWithoutUndo();
                result = catalog.SelectAction(actions[i], ActionNames[i], false, out string error);
                if (result != VnResult.VN_OK) throw new InvalidDataException(error);
            }

            HarkenVariable variable = emitterObject.AddComponent<HarkenVariable>();
            SerializedObject variableSerialized = new SerializedObject(variable);
            variableSerialized.FindProperty("runtime").objectReferenceValue = runtime;
            SerializedProperty variableId = variableSerialized.FindProperty("variable");
            variableId.FindPropertyRelative("stableId").FindPropertyRelative("high").ulongValue = choiceId.high;
            variableId.FindPropertyRelative("stableId").FindPropertyRelative("low").ulongValue = choiceId.low;
            variableId.FindPropertyRelative("codeName").stringValue = "Demo_Choice";
            variableSerialized.FindProperty("variableName").stringValue = "Demo_Choice";
            variableSerialized.FindProperty("projectId").stringValue = catalog.Publication.ProjectId;
            variableSerialized.FindProperty("value").floatValue = 0.0f;
            variableSerialized.ApplyModifiedPropertiesWithoutUndo();

            GameObject cameraObject = new GameObject("Main Camera");
            cameraObject.tag = "MainCamera";
            cameraObject.transform.position = new Vector3(0.0f, 1.0f, -10.0f);
            cameraObject.AddComponent<Camera>();
            cameraObject.AddComponent<AudioListener>();
            cameraObject.AddComponent<HarkenListener>();

            GameObject lightObject = new GameObject("Directional Light");
            lightObject.transform.rotation = Quaternion.Euler(50.0f, -30.0f, 0.0f);
            Light light = lightObject.AddComponent<Light>();
            light.type = LightType.Directional;

            GameObject controllerObject = new GameObject("Demo UI");
            HarkenPlayableDemoController controller =
                controllerObject.AddComponent<HarkenPlayableDemoController>();
            SerializedObject controllerSerialized = new SerializedObject(controller);
            controllerSerialized.FindProperty("runtime").objectReferenceValue = runtime;
            SerializedProperty actionArray =
                controllerSerialized.FindProperty("actions");
            actionArray.arraySize = actions.Length;
            SerializedProperty labelArray =
                controllerSerialized.FindProperty("labels");
            labelArray.arraySize = Labels.Length;
            for (int i = 0; i < actions.Length; ++i)
            {
                actionArray.GetArrayElementAtIndex(i).objectReferenceValue = actions[i];
                labelArray.GetArrayElementAtIndex(i).stringValue = Labels[i];
            }
            controllerSerialized.FindProperty("variable").objectReferenceValue = variable;
            controllerSerialized.ApplyModifiedPropertiesWithoutUndo();

            string scenePath = sceneRoot + "/HarkenPlayableDemo.unity";
            EditorSceneManager.SaveScene(scene, scenePath, true);
            AssetDatabase.SaveAssets();
            AssetDatabase.Refresh();
            HarkenDemoPackInstaller.InstallIfImported();
            Debug.Log("Harken playable sample rebuilt at " + scenePath);
        }

        private static void ConfigureRuntime(HarkenAudioSystem runtime, GameplayPublication publication, List<ulong> startupIds)
        {
            SerializedObject serialized = new SerializedObject(runtime);
            SerializedProperty packs =
                serialized.FindProperty("startupPackStableIds");
            packs.arraySize = startupIds.Count;
            for (int i = 0; i < startupIds.Count; ++i)
                packs.GetArrayElementAtIndex(i).ulongValue = startupIds[i];
            serialized.FindProperty("packRoot").stringValue = "Audio/GeneratedPacks";
            serialized.FindProperty("initPack").stringValue = publication.InitPack.Path;
            serialized.FindProperty("packPlatformId").stringValue = publication.Platform.Id;
            serialized.FindProperty("packPlatform").stringValue = string.Empty;
            serialized.FindProperty("language").stringValue = publication.Language;
            serialized.FindProperty("nullDevice").boolValue = false;
            serialized.FindProperty("persistAcrossScenes").boolValue = false;
            serialized.ApplyModifiedPropertiesWithoutUndo();
        }

        private static string FindSampleRoot()
        {
            string[] scripts = AssetDatabase.FindAssets(
                "HarkenDemoSceneBuilder t:MonoScript");
            foreach (string guid in scripts)
            {
                string path = AssetDatabase.GUIDToAssetPath(guid);
                if (path.StartsWith("Assets/"))
                {
                    return Path.GetDirectoryName(
                        Path.GetDirectoryName(path)).Replace('\\', '/');
                }
            }
            return string.Empty;
        }
    }
}
