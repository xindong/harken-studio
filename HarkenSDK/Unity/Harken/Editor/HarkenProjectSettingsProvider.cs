using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEngine;

namespace Harken.Unity.Editor
{
    public static class HarkenProjectSettingsProvider
    {
        public const string AssetPath = "Assets/Harken/Resources/HarkenProjectSettings.asset";

        [SettingsProvider]
        public static SettingsProvider CreateSettingsProvider()
        {
            return new SettingsProvider("Project/Harken", SettingsScope.Project)
            {
                label = "Harken",
                keywords = new HashSet<string> { "Harken", "Project", "Communication", "Voices", "Packs" },
                guiHandler = _ => EditorGUILayout.HelpBox(
                    "Startup limits, the audio project, and GeneratedPacks are on the Project page. Remote sessions are on the Communication page.",
                    MessageType.Info)
            };
        }

        private sealed class ProjectPageState
        {
            public SerializedObject Project;
            public SerializedObject EditorProject;
        }

        [SettingsProvider]
        public static SettingsProvider CreateProjectSettingsProvider()
        {
            var state = new ProjectPageState();
            return new SettingsProvider("Project/Harken/Project", SettingsScope.Project)
            {
                label = "Project",
                keywords = new HashSet<string>
                {
                    "Harken", "Voices", "Packs", "Language", "Platform", "GeneratedPacks", "Audio"
                },
                activateHandler = (_, __) =>
                {
                    state.Project = new SerializedObject(GetOrCreateSettings());
                    state.EditorProject = new SerializedObject(HarkenEditorProjectSettings.GetOrCreate());
                },
                guiHandler = _ => DrawProjectPage(state)
            };
        }

        [SettingsProvider]
        public static SettingsProvider CreateCommunicationSettingsProvider()
        {
            SerializedObject serialized = null;
            return new SettingsProvider("Project/Harken/Communication", SettingsScope.Project)
            {
                label = "Communication",
                keywords = new HashSet<string> { "Harken", "Communication", "Remote", "Build", "Discovery", "Port" },
                activateHandler = (_, __) => serialized = new SerializedObject(GetOrCreateSettings()),
                guiHandler = _ =>
                {
                    if (serialized == null || serialized.targetObject == null)
                        serialized = new SerializedObject(GetOrCreateSettings());
                    serialized.Update();
                    EditorGUILayout.PropertyField(serialized.FindProperty("enableCommunicationInBuilds"),
                        new GUIContent("Enable Communication in Builds"));
                    // EditorGUILayout.PropertyField(serialized.FindProperty("remoteBindAddress"), new GUIContent("Bind Address"));
                    EditorGUILayout.LabelField("Bind Address", "127.0.0.1");
                    EditorGUILayout.PropertyField(serialized.FindProperty("remotePort"), new GUIContent("Port"));
                    EditorGUILayout.PropertyField(serialized.FindProperty("enableRemoteDiscovery"), new GUIContent("Auto Discover"));
                    EditorGUILayout.PropertyField(serialized.FindProperty("remoteDiscoveryPort"), new GUIContent("Discovery Port"));
                    EditorGUILayout.PropertyField(serialized.FindProperty("remoteApplicationName"), new GUIContent("Application Name"));
                    if (serialized.ApplyModifiedProperties())
                        AssetDatabase.SaveAssetIfDirty(serialized.targetObject);
                }
            };
        }

        private static void DrawProjectPage(ProjectPageState state)
        {
            SerializedObject project = state.Project;
            SerializedObject editorProject = state.EditorProject;
            if (project == null || project.targetObject == null)
                state.Project = project = new SerializedObject(GetOrCreateSettings());
            if (editorProject == null || editorProject.targetObject == null)
                state.EditorProject = editorProject = new SerializedObject(HarkenEditorProjectSettings.GetOrCreate());
            project.Update();
            editorProject.Update();
            EditorGUILayout.LabelField("Audio project", EditorStyles.boldLabel);
            DrawFolder(editorProject.FindProperty("audioProjectDirectory"), "Audio project directory");
            EditorGUILayout.LabelField("Generated packs", EditorStyles.boldLabel);
            DrawFolder(project.FindProperty("packDirectory"), "GeneratedPacks directory");
            bool editorChanged = editorProject.ApplyModifiedProperties();
            bool projectChanged = project.ApplyModifiedProperties();
            if (editorChanged || projectChanged)
            {
                AssetDatabase.SaveAssetIfDirty(editorProject.targetObject);
                AssetDatabase.SaveAssetIfDirty(project.targetObject);
            }
            if (!HarkenProjectSettings.TryResolvePackDirectory(out string packRoot, out string packError))
                EditorGUILayout.HelpBox(packError, MessageType.Warning);
            else
                DrawPlayerExports(project, packRoot);
            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Startup limits", EditorStyles.boldLabel);
            project.Update();
            DrawCapacity(project, "maxVoices", "Max Voices");
            DrawCapacity(project, "maxPacks", "Max Packs");
            DrawCapacity(project, "maxActions", "Max Actions");
            DrawCapacity(project, "maxActionFirings", "Max Simultaneous Playbacks");
            DrawCapacity(project, "maxEmitters", "Max Emitters");
            DrawCapacity(project, "emitterVariableSlots", "Emitter Variable Slots");
            EditorGUILayout.PropertyField(project.FindProperty("language"), new GUIContent("Language"));
            EditorGUILayout.PropertyField(project.FindProperty("packPlatformId"), new GUIContent("Platform Id"));
            EditorGUILayout.PropertyField(project.FindProperty("packPlatform"), new GUIContent("Platform Name"));
            EditorGUILayout.PropertyField(project.FindProperty("initPack"), new GUIContent("Init Pack"));
            EditorGUILayout.PropertyField(project.FindProperty("suppressUnityAudio"), new GUIContent("Suppress Unity Audio"));
            EditorGUILayout.HelpBox("Suppress Unity Audio stays off unless this page is enabled. A startup call can still override it.", MessageType.None);
            if (project.ApplyModifiedProperties())
                AssetDatabase.SaveAssetIfDirty(project.targetObject);
        }

        private static void DrawCapacity(SerializedObject project, string property, string label)
        {
            SerializedProperty field = project.FindProperty(property);
            EditorGUILayout.PropertyField(field, new GUIContent(label));
        }

        private static void DrawFolder(SerializedProperty property, string label)
        {
            EditorGUILayout.BeginHorizontal();
            EditorGUILayout.PropertyField(property, new GUIContent(label));
            if (GUILayout.Button("Select", GUILayout.Width(70)))
            {
                string picked = EditorUtility.OpenFolderPanel(label, Application.dataPath, string.Empty);
                if (!string.IsNullOrEmpty(picked))
                    property.stringValue = StoreProjectPath(picked);
            }
            EditorGUILayout.EndHorizontal();
        }

        private static string StoreProjectPath(string absolute)
        {
            string projectRoot = HarkenProjectSettings.ProjectRoot();
            if (!HarkenProjectSettings.TryResolveProjectPath(absolute, projectRoot, out string full))
                return absolute.Replace('\\', '/');
            if (!HarkenProjectSettings.IsInside(projectRoot, full))
                return full.Replace('\\', '/');
            return Path.GetRelativePath(projectRoot, full).Replace('\\', '/');
        }

        private static void DrawPlayerExports(SerializedObject project, string packRoot)
        {
            project.Update();
            SerializedProperty exports = project.FindProperty("playerExports");
            var selected = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            for (int i = 0; i < exports.arraySize; ++i)
                selected.Add(exports.GetArrayElementAtIndex(i).stringValue);
            EditorGUILayout.LabelField("Exports included in a player");
            bool changed = false;
            foreach (string folder in Directory.GetDirectories(packRoot).OrderBy(path => path, StringComparer.OrdinalIgnoreCase))
            {
                string name = Path.GetFileName(folder);
                if (string.IsNullOrEmpty(name) || name.StartsWith(".", StringComparison.Ordinal)) continue;
                bool included = selected.Contains(name);
                bool next = EditorGUILayout.ToggleLeft(name, included);
                if (next == included) continue;
                changed = true;
                if (next) selected.Add(name);
                else selected.Remove(name);
            }
            if (!changed) return;
            string[] values = selected.Where(name => !string.IsNullOrEmpty(name)).OrderBy(name => name, StringComparer.OrdinalIgnoreCase).ToArray();
            exports.arraySize = values.Length;
            for (int i = 0; i < values.Length; ++i)
                exports.GetArrayElementAtIndex(i).stringValue = values[i];
            if (project.ApplyModifiedProperties())
                AssetDatabase.SaveAssetIfDirty(project.targetObject);
        }

        public static HarkenProjectSettings GetOrCreateSettings()
        {
            ValidateSettingsAssets();
            var settings = HarkenProjectSettings.Load();
            if (settings != null) return settings;
            if (AssetDatabase.LoadMainAssetAtPath(AssetPath) != null || File.Exists(AssetPath))
                throw new InvalidOperationException("The Harken settings path contains a different asset: " + AssetPath);
            if (!AssetDatabase.IsValidFolder("Assets/Harken"))
                AssetDatabase.CreateFolder("Assets", "Harken");
            if (!AssetDatabase.IsValidFolder("Assets/Harken/Resources"))
                AssetDatabase.CreateFolder("Assets/Harken", "Resources");
            settings = ScriptableObject.CreateInstance<HarkenProjectSettings>();
            AssetDatabase.CreateAsset(settings, AssetPath);
            AssetDatabase.SaveAssetIfDirty(settings);
            return settings;
        }

        public static void ValidateSettingsAssets()
        {
            string settingsPath = null;
            foreach (string guid in AssetDatabase.FindAssets(HarkenProjectSettings.ResourceName))
            {
                string path = AssetDatabase.GUIDToAssetPath(guid);
                int resources = path.LastIndexOf("/Resources/", StringComparison.OrdinalIgnoreCase);
                if (resources < 0 || !string.Equals(
                    path.Substring(resources + "/Resources/".Length),
                    HarkenProjectSettings.ResourceName + Path.GetExtension(path), StringComparison.OrdinalIgnoreCase))
                    continue;
                if (settingsPath != null || AssetDatabase.LoadAssetAtPath<HarkenProjectSettings>(path) == null)
                    throw new BuildFailedException("Conflicting Harken settings resource: " + path +
                        ". Keep exactly one HarkenProjectSettings asset at Resources/" +
                        HarkenProjectSettings.ResourceName + ".asset.");
                settingsPath = path;
            }
        }
    }

    internal sealed class HarkenProjectSettingsBuildValidator : IPreprocessBuildWithReport
    {
        public int callbackOrder => 0;
        public void OnPreprocessBuild(BuildReport report)
        {
            HarkenProjectSettingsProvider.ValidateSettingsAssets();
            HarkenEditorProjectSettings.ValidateNotInResources();
        }
    }
}
