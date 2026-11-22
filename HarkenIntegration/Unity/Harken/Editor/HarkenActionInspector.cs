using System;
using System.Linq;
using UnityEditor;
using UnityEngine;

namespace Harken.Unity.Editor
{
    [CustomEditor(typeof(HarkenAction))]
    public sealed class HarkenActionInspector : UnityEditor.Editor
    {
        private readonly HarkenEditorCatalog catalog = new HarkenEditorCatalog();
        private readonly HarkenEditorPreview preview = new HarkenEditorPreview();
        private HarkenPackSource source;
        private TextAsset initJson;
        private string directory = string.Empty;
        private string platformName;
        private string language = string.Empty;
        private string commandDiagnostic = string.Empty;

        private void OnEnable()
        {
            var action = (HarkenAction)target;
            var runtime = serializedObject.FindProperty("runtime").objectReferenceValue as HarkenAudioSystem;
            if (runtime != null)
            {
                var runtimeProperties = new SerializedObject(runtime);
                language = runtimeProperties.FindProperty("language").stringValue;
                platformName = runtimeProperties.FindProperty("packPlatform").stringValue;
            }
            if (HarkenEditorSourceBinding.TryLoad(action.ProjectId, out var binding))
            {
                source = binding.LoadSource();
                initJson = binding.LoadInitJson();
                if (source == null) directory = binding.Directory;
                if (!string.IsNullOrEmpty(binding.Language)) language = binding.Language;
                if (!string.IsNullOrEmpty(binding.PlatformName)) platformName = binding.PlatformName;
            }
            Refresh(false);
        }

        public override void OnInspectorGUI()
        {
            serializedObject.Update();
            DrawPropertiesExcluding(serializedObject, "m_Script", "actionName", "projectId");
            EditorGUILayout.PropertyField(serializedObject.FindProperty("actionName"));
            using (new EditorGUI.DisabledScope(true))
                EditorGUILayout.PropertyField(serializedObject.FindProperty("projectId"));
            serializedObject.ApplyModifiedProperties();

            EditorGUILayout.Space();
            EditorGUI.BeginChangeCheck();
            source = (HarkenPackSource)EditorGUILayout.ObjectField("Pack source", source, typeof(HarkenPackSource), false);
            initJson = (TextAsset)EditorGUILayout.ObjectField("Init JSON", initJson, typeof(TextAsset), false);
            if (source == null) directory = EditorGUILayout.TextField("Pack directory", directory);
            language = EditorGUILayout.TextField("Language", language);
            if (EditorGUI.EndChangeCheck()) Refresh(true);
            EditorGUILayout.BeginHorizontal();
            if (GUILayout.Button(new GUIContent("Refresh"))) Refresh(true);
            if (GUILayout.Button(new GUIContent("Save binding"))) SaveBinding();
            EditorGUILayout.EndHorizontal();

            var action = (HarkenAction)target;
            if (catalog.Catalog != null)
            {
                string[] names = new[] { "Select Action" }.Concat(catalog.Catalog.Actions.Select(row => row.Name)).ToArray();
                int selected = Array.IndexOf(names, action.ActionName);
                EditorGUI.BeginChangeCheck();
                int choice = EditorGUILayout.Popup("Action", Math.Max(selected, 0), names);
                if (EditorGUI.EndChangeCheck() && choice > 0)
                    catalog.SelectAction(action, names[choice], false, out commandDiagnostic);
                if (action.ProjectId != catalog.Catalog.ProjectId &&
                    GUILayout.Button(string.IsNullOrEmpty(action.ProjectId) ? "Bind project" : "Rebind project"))
                    catalog.SelectAction(action, action.ActionName, true, out commandDiagnostic);
            }
            VnResult reference = catalog.CheckReference(action.ActionName, action.ProjectId, out string error);
            if (reference != VnResult.VN_OK)
            {
                MessageType type = catalog.Catalog == null && catalog.LastResult == VnResult.VN_CAPABILITY_UNAVAILABLE
                    ? MessageType.Info : MessageType.Error;
                EditorGUILayout.HelpBox(error, type);
            }
            bool playable = EditorApplication.isPlayingOrWillChangePlaymode;
            using (new EditorGUI.DisabledScope(playable))
            {
                bool catalogReady = catalog.Catalog != null && reference == VnResult.VN_OK;
                EditorGUILayout.BeginHorizontal();
                using (new EditorGUI.DisabledScope(!catalogReady || preview.IsPrepared))
                {
                    if (GUILayout.Button("Prepare preview")) PreparePreview(action);
                }
                using (new EditorGUI.DisabledScope(!preview.IsPrepared))
                {
                    if (GUILayout.Button(new GUIContent(EditorGUIUtility.IconContent("PlayButton").image, "Play preview")))
                    {
                        preview.Play(action.ActionName, action.ProjectId, out _);
                        commandDiagnostic = preview.Diagnostic;
                    }
                    if (GUILayout.Button(new GUIContent(EditorGUIUtility.IconContent("PreMatQuad").image, "Stop preview")))
                    {
                        preview.Stop();
                        commandDiagnostic = preview.Diagnostic;
                    }
                }
                EditorGUILayout.EndHorizontal();
            }
            if (!string.IsNullOrEmpty(commandDiagnostic)) EditorGUILayout.HelpBox(commandDiagnostic, MessageType.Warning);
        }

        private void Refresh(bool userRequested)
        {
            preview.Shutdown();
            if (source != null) catalog.Refresh(source, language, initJson);
            else if (!string.IsNullOrEmpty(directory))
                catalog.RefreshPlatform(directory, platformName,
                    HarkenAudioSystem.CurrentPackBasePlatform(), language);
            else if (userRequested)
                catalog.Refresh(string.Empty, language);
            commandDiagnostic = string.Empty;
            Repaint();
        }

        private void SaveBinding()
        {
            string projectId = catalog.Catalog?.ProjectId;
            string sourceGuid = source != null
                ? AssetDatabase.AssetPathToGUID(AssetDatabase.GetAssetPath(source)) : string.Empty;
            string initJsonGuid = initJson != null
                ? AssetDatabase.AssetPathToGUID(AssetDatabase.GetAssetPath(initJson)) : string.Empty;
            if (!HarkenEditorSourceBinding.Save(projectId, new HarkenEditorSourceBinding(
                sourceGuid, directory, initJsonGuid, language, platformName)))
            {
                commandDiagnostic = "Binding was not saved because no Pack directory is loaded.";
                return;
            }
            commandDiagnostic = "Binding was saved for this project.";
        }

        private void PreparePreview(HarkenAction action)
        {
            VnResult result = catalog.CheckReference(action.ActionName, action.ProjectId, out commandDiagnostic);
            if (result != VnResult.VN_OK) return;
            result = catalog.GetPreparationOrder(action.ActionName, out ulong[] ids, out commandDiagnostic);
            if (result != VnResult.VN_OK) return;
            preview.Prepare(catalog.PreviewContent, ids);
            commandDiagnostic = preview.Diagnostic;
        }

        private void OnDisable() => preview.Dispose();
    }
}
