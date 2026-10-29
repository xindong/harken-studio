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
        private TextAsset companion;
        private string directory = string.Empty;
        private string platformId;
        private string platformName;
        private string language = string.Empty;
        private string commandDiagnostic = string.Empty;

        private void OnEnable()
        {
            // Temporarily avoid automatic manifest capture until publication tooling supports current Pack companions.
            if (!HarkenEditorPublicationSupport.CompanionAvailable) return;
            var action = (HarkenAction)target;
            var runtime = serializedObject.FindProperty("runtime").objectReferenceValue as HarkenAudioSystem;
            if (runtime != null)
            {
                var runtimeProperties = new SerializedObject(runtime);
                source = runtimeProperties.FindProperty("preparedPackSource").objectReferenceValue as HarkenPackSource;
                language = runtimeProperties.FindProperty("language").stringValue;
                platformId = runtimeProperties.FindProperty("packPlatformId").stringValue;
                platformName = runtimeProperties.FindProperty("packPlatform").stringValue;
            }
            if (source == null && HarkenEditorPublicationBinding.TryLoad(action.ProjectId, out var binding))
            {
                source = binding.LoadSource();
                companion = binding.LoadCompanion();
                if (source == null) directory = binding.Directory;
                if (!string.IsNullOrEmpty(binding.Language)) language = binding.Language;
                if (!string.IsNullOrEmpty(binding.PlatformId)) platformId = binding.PlatformId;
                if (!string.IsNullOrEmpty(binding.PlatformName)) platformName = binding.PlatformName;
            }
            Refresh(false);
        }

        private void Refresh() => Refresh(true);

        public override void OnInspectorGUI()
        {
            serializedObject.Update();
            DrawPropertiesExcluding(serializedObject, "m_Script", "actionName", "projectId");
            EditorGUILayout.PropertyField(serializedObject.FindProperty("actionName"));
            using (new EditorGUI.DisabledScope(HarkenEditorPublicationSupport.CompanionAvailable))
                EditorGUILayout.PropertyField(serializedObject.FindProperty("projectId"));
            serializedObject.ApplyModifiedProperties();

            // Keep manual runtime configuration available while catalog binding and Editor preview are suspended.
            if (!HarkenEditorPublicationSupport.CompanionAvailable)
            {
                EditorGUILayout.HelpBox(HarkenEditorPublicationSupport.Diagnostic, MessageType.Info);
                return;
            }

            EditorGUILayout.Space();
            EditorGUI.BeginChangeCheck();
            source = (HarkenPackSource)EditorGUILayout.ObjectField("Pack source", source, typeof(HarkenPackSource), false);
            companion = (TextAsset)EditorGUILayout.ObjectField("Init JSON", companion, typeof(TextAsset), false);
            if (source == null) directory = EditorGUILayout.TextField("Publication directory", directory);
            language = EditorGUILayout.TextField("Language", language);
            if (EditorGUI.EndChangeCheck()) Refresh(true);
            EditorGUILayout.BeginHorizontal();
            if (GUILayout.Button(new GUIContent("Refresh publication"))) Refresh(true);
            if (GUILayout.Button(new GUIContent("Save publication binding"))) SaveBinding();
            EditorGUILayout.EndHorizontal();

            var action = (HarkenAction)target;
            if (catalog.Publication != null)
            {
                string[] names = new[] { "Select Action" }.Concat(catalog.Publication.Actions.Select(row => row.Name)).ToArray();
                int selected = Array.IndexOf(names, action.ActionName);
                EditorGUI.BeginChangeCheck();
                int choice = EditorGUILayout.Popup("Published Action", Math.Max(selected, 0), names);
                if (EditorGUI.EndChangeCheck() && choice > 0)
                    catalog.SelectAction(action, names[choice], false, out commandDiagnostic);
                if (action.ProjectId != catalog.Publication.ProjectId &&
                    GUILayout.Button(string.IsNullOrEmpty(action.ProjectId) ? "Bind project" : "Rebind project"))
                    catalog.SelectAction(action, action.ActionName, true, out commandDiagnostic);
            }
            VnResult reference = catalog.CheckReference(action.ActionName, action.ProjectId, out string error);
            if (reference != VnResult.VN_OK)
            {
                MessageType type = catalog.Publication == null && catalog.LastResult == VnResult.VN_CAPABILITY_UNAVAILABLE
                    ? MessageType.Info : MessageType.Error;
                EditorGUILayout.HelpBox(error, type);
            }
            bool playable = EditorApplication.isPlayingOrWillChangePlaymode;
            using (new EditorGUI.DisabledScope(playable))
            {
                bool catalogReady = catalog.Publication != null && reference == VnResult.VN_OK;
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
            if (source != null) catalog.Refresh(source, language, companion);
            else if (!string.IsNullOrEmpty(directory))
                catalog.RefreshPlatform(directory, platformId, platformName,
                    HarkenAudioSystem.CurrentPackBasePlatform(), language);
            else if (userRequested)
                catalog.Refresh(string.Empty, language);
            commandDiagnostic = string.Empty;
            Repaint();
        }

        private void SaveBinding()
        {
            string projectId = catalog.Publication?.ProjectId;
            string sourceGuid = source != null
                ? AssetDatabase.AssetPathToGUID(AssetDatabase.GetAssetPath(source)) : string.Empty;
            string companionGuid = companion != null
                ? AssetDatabase.AssetPathToGUID(AssetDatabase.GetAssetPath(companion)) : string.Empty;
            if (!HarkenEditorPublicationBinding.Save(projectId, new HarkenEditorPublicationBinding(
                sourceGuid, directory, companionGuid, language, platformId, platformName)))
            {
                commandDiagnostic = "Publication binding was not saved because no paired gameplay publication is selected.";
                return;
            }
            commandDiagnostic = "Publication binding was saved for this project.";
        }

        private void PreparePreview(HarkenAction action)
        {
            VnResult result = catalog.CheckReference(action.ActionName, action.ProjectId, out commandDiagnostic);
            if (result != VnResult.VN_OK) return;
            result = catalog.GetPreparationOrder(action.ActionName, out ulong[] ids, out commandDiagnostic);
            if (result != VnResult.VN_OK) return;
            if (catalog.PreparedPacks != null) preview.Prepare(catalog.PreparedPacks, ids);
            else preview.Prepare(catalog.DirectoryPath, catalog.Publication.Language, ids);
            commandDiagnostic = preview.Diagnostic;
        }

        private void OnDisable() => preview.Dispose();
    }
}
