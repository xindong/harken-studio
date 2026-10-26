using UnityEditor;
using UnityEngine;

namespace Harken.Unity.Editor
{
    [CustomEditor(typeof(HarkenAudioSystem), true)]
    public sealed class HarkenAudioSystemEditor : UnityEditor.Editor
    {
        public override void OnInspectorGUI()
        {
            serializedObject.Update();
            EditorGUILayout.PropertyField(serializedObject.FindProperty("autoInitialize"), new GUIContent("Auto Start"));
            EditorGUILayout.PropertyField(serializedObject.FindProperty("persistAcrossScenes"), new GUIContent("Keep Across Scenes"));
            EditorGUILayout.PropertyField(serializedObject.FindProperty("shutdownWhenDisabled"), new GUIContent("Shut Down When Disabled"));
            EditorGUILayout.PropertyField(serializedObject.FindProperty("ready"));
            EditorGUILayout.PropertyField(serializedObject.FindProperty("failed"));
            EditorGUILayout.PropertyField(serializedObject.FindProperty("resultObserved"));
            serializedObject.ApplyModifiedProperties();
        }
    }
}
