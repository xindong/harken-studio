using System;
using UnityEditor;
using UnityEngine;

namespace Harken.Unity.Editor
{
    internal readonly struct HarkenEditorSourceBinding
    {
        public readonly string SourceGuid;
        public readonly string Directory;
        public readonly string InitJsonGuid;
        public readonly string Language;
        public readonly string PlatformName;

        public HarkenEditorSourceBinding(string sourceGuid, string directory, string initJsonGuid,
            string language, string platformName)
        {
            SourceGuid = sourceGuid ?? string.Empty;
            Directory = directory ?? string.Empty;
            InitJsonGuid = initJsonGuid ?? string.Empty;
            Language = language ?? string.Empty;
            PlatformName = platformName ?? string.Empty;
        }

        public static bool TryLoad(string projectId, out HarkenEditorSourceBinding binding)
        {
            binding = default;
            if (string.IsNullOrEmpty(projectId) || !PackPaths.IsCanonicalGuid(projectId)) return false;
            string json = EditorPrefs.GetString(Key(projectId), string.Empty);
            if (string.IsNullOrEmpty(json)) return false;
            var parsed = JsonUtility.FromJson<Serialized>(json);
            if (parsed == null) return false;
            binding = new HarkenEditorSourceBinding(parsed.sourceGuid, parsed.directory, parsed.initJsonGuid,
                parsed.language, parsed.platformName);
            return true;
        }

        public static bool Save(string projectId, HarkenEditorSourceBinding binding)
        {
            if (string.IsNullOrEmpty(projectId) || !PackPaths.IsCanonicalGuid(projectId)) return false;
            var parsed = new Serialized
            {
                sourceGuid = binding.SourceGuid,
                directory = binding.Directory,
                initJsonGuid = binding.InitJsonGuid,
                language = binding.Language,
                platformName = binding.PlatformName
            };
            EditorPrefs.SetString(Key(projectId), JsonUtility.ToJson(parsed));
            return true;
        }

        public HarkenPackSource LoadSource()
        {
            if (string.IsNullOrEmpty(SourceGuid)) return null;
            string path = AssetDatabase.GUIDToAssetPath(SourceGuid);
            return string.IsNullOrEmpty(path) ? null : AssetDatabase.LoadAssetAtPath<HarkenPackSource>(path);
        }

        public TextAsset LoadInitJson()
        {
            if (string.IsNullOrEmpty(InitJsonGuid)) return null;
            string path = AssetDatabase.GUIDToAssetPath(InitJsonGuid);
            return string.IsNullOrEmpty(path) ? null : AssetDatabase.LoadAssetAtPath<TextAsset>(path);
        }

        internal static string Key(string projectId)
            => "Harken.Editor.Source." + Application.dataPath + "." + projectId;

        [Serializable]
        private sealed class Serialized
        {
            public string sourceGuid;
            public string directory;
            public string initJsonGuid;
            public string language;
            public string platformName;
        }
    }
}
