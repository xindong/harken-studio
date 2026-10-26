using System;
using System.Collections.Generic;
using System.Reflection;
using UnityEditor;
using UnityEngine;
using Harken.Generated;

namespace Harken.Unity.Editor
{
    [CustomPropertyDrawer(typeof(HarkenVariableId))]
    public sealed class HarkenVariableIdDrawer : PropertyDrawer
    {
        public override void OnGUI(Rect position, SerializedProperty property,
            GUIContent label)
        {
            SerializedProperty id = property.FindPropertyRelative("stableId");
            SerializedProperty name = property.FindPropertyRelative("codeName");
            Collect(out string[] labels, out VnObjectId[] ids);
            int selected = 0;
            VnObjectId current = new VnObjectId(id.FindPropertyRelative("high").ulongValue,
                id.FindPropertyRelative("low").ulongValue);
            for (int i = 1; i < ids.Length; ++i)
            {
                if (ids[i] == current)
                {
                    selected = i;
                    break;
                }
            }
            if (!current.IsNull && selected == 0)
            {
                var extraLabels = new string[labels.Length + 1];
                var extraIds = new VnObjectId[ids.Length + 1];
                extraLabels[0] = labels[0];
                extraIds[0] = ids[0];
                extraLabels[1] = "Unresolved (" + (string.IsNullOrEmpty(name.stringValue) ? "id" : name.stringValue) +
                    " / " + current + ")";
                extraIds[1] = current;
                System.Array.Copy(labels, 1, extraLabels, 2, labels.Length - 1);
                System.Array.Copy(ids, 1, extraIds, 2, ids.Length - 1);
                labels = extraLabels;
                ids = extraIds;
                selected = 1;
            }
            EditorGUI.BeginProperty(position, label, property);
            int next = EditorGUI.Popup(position, label.text, selected, labels);
            if (next != selected)
            {
                id.FindPropertyRelative("high").ulongValue = ids[next].high;
                id.FindPropertyRelative("low").ulongValue = ids[next].low;
                name.stringValue = next == 0 ? string.Empty : FieldName(labels[next]);
                if (property.propertyPath == "variable" && property.serializedObject.targetObject is HarkenVariable)
                {
                    property.serializedObject.FindProperty("variableName").stringValue = string.Empty;
                    property.serializedObject.FindProperty("projectId").stringValue = string.Empty;
                }
            }
            EditorGUI.EndProperty();
        }

        public static bool TryResolve(string codeName, out VnObjectId stableId)
        {
            stableId = default;
            if (string.IsNullOrEmpty(codeName))
            {
                return false;
            }
            FieldInfo field = typeof(HarkenVariables).GetField(VariableCatalogEmitter.SymbolName(codeName),
                BindingFlags.Public | BindingFlags.Static);
            if (field == null || field.FieldType != typeof(VnObjectId) ||
                IsFingerprint(field.Name))
            {
                return false;
            }
            stableId = (VnObjectId)field.GetValue(null);
            return true;
        }

        private static void Collect(out string[] labels, out VnObjectId[] ids)
        {
            var names = new List<string> { "(None)" };
            var values = new List<VnObjectId> { default };
            foreach (FieldInfo field in typeof(HarkenVariables).GetFields(
                BindingFlags.Public | BindingFlags.Static))
            {
                if (field.FieldType != typeof(VnObjectId) || IsFingerprint(field.Name))
                {
                    continue;
                }
                VnObjectId id = (VnObjectId)field.GetValue(null);
                names.Add(VariableCatalogEmitter.AuthoredName(field.Name) + " (" + id + ")");
                values.Add(id);
            }
            labels = names.ToArray();
            ids = values.ToArray();
        }

        private static bool IsFingerprint(string fieldName)
        {
            return fieldName == "ExecutionCatalogFingerprint" ||
                fieldName == "CatalogFingerprint";
        }

        private static string FieldName(string label)
        {
            int split = label.IndexOf(" (", StringComparison.Ordinal);
            return split <= 0 ? label : label.Substring(0, split);
        }
    }
}
