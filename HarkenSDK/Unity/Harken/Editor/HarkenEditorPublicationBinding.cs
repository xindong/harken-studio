using System;
using UnityEditor;
using UnityEngine;

namespace Harken.Unity.Editor
{
    internal readonly struct HarkenEditorPublicationBinding
    {
        public readonly string SourceGuid;
        public readonly string Directory;
        public readonly string CompanionGuid;
        public readonly string Language;
        public readonly string PlatformId;
        public readonly string PlatformName;

        public HarkenEditorPublicationBinding(string sourceGuid, string directory, string companionGuid,
            string language, string platformId, string platformName)
        {
            SourceGuid = sourceGuid ?? string.Empty;
            Directory = directory ?? string.Empty;
            CompanionGuid = companionGuid ?? string.Empty;
            Language = language ?? string.Empty;
            PlatformId = platformId ?? string.Empty;
            PlatformName = platformName ?? string.Empty;
        }

        public static bool TryLoad(string projectId, out HarkenEditorPublicationBinding binding)
        {
            binding = default;
            if (string.IsNullOrEmpty(projectId) || !PackManifest.IsPlatformId(projectId)) return false;
            string json = EditorPrefs.GetString(Key(projectId), string.Empty);
            if (string.IsNullOrEmpty(json)) return false;
            var parsed = JsonUtility.FromJson<Serialized>(json);
            if (parsed == null) return false;
            binding = new HarkenEditorPublicationBinding(parsed.sourceGuid, parsed.directory, parsed.companionGuid,
                parsed.language, parsed.platformId, parsed.platformName);
            return true;
        }

        public static bool Save(string projectId, HarkenEditorPublicationBinding binding)
        {
            if (string.IsNullOrEmpty(projectId) || !PackManifest.IsPlatformId(projectId)) return false;
            var parsed = new Serialized
            {
                sourceGuid = binding.SourceGuid,
                directory = binding.Directory,
                companionGuid = binding.CompanionGuid,
                language = binding.Language,
                platformId = binding.PlatformId,
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

        public TextAsset LoadCompanion()
        {
            if (string.IsNullOrEmpty(CompanionGuid)) return null;
            string path = AssetDatabase.GUIDToAssetPath(CompanionGuid);
            return string.IsNullOrEmpty(path) ? null : AssetDatabase.LoadAssetAtPath<TextAsset>(path);
        }

        internal static string Key(string projectId)
            => "Harken.Editor.Publication." + Application.dataPath + "." + projectId;

        [Serializable]
        private sealed class Serialized
        {
            public string sourceGuid;
            public string directory;
            public string companionGuid;
            public string language;
            public string platformId;
            public string platformName;
        }
    }
}
