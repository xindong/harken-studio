using System;
using System.IO;
using System.Runtime.CompilerServices;
using UnityEngine;
using Harken;

[assembly: InternalsVisibleTo("Harken.Unity.Editor")]
[assembly: InternalsVisibleTo("Harken.Unity.EditModeTests")]
[assembly: InternalsVisibleTo("Harken.Unity.PlayModeTests")]

namespace Harken.Unity
{
    public sealed class HarkenProjectSettings : ScriptableObject
    {
        public const string ResourceName = "HarkenProjectSettings";
        public const uint DefaultMaxVoices = 256;
        public const uint DefaultMaxPacks = 256;
        public const uint DefaultMaxActions = 256;
        public const uint DefaultMaxActionFirings = 256;
        public const uint UnlimitedActionFirings = 256;
        internal const string ObsoleteRelativePackRoot = "Audio/GeneratedPacks";

        [SerializeField] private bool enableCommunicationInBuilds = true;
        [SerializeField] private string remoteBindAddress = "0.0.0.0";
        [SerializeField, Min(0)] private int remotePort;
        [SerializeField] private bool enableRemoteDiscovery = true;
        [SerializeField, Min(0)] private int remoteDiscoveryPort;
        [SerializeField] private string remoteApplicationName = string.Empty;
        [SerializeField] private uint maxVoices = DefaultMaxVoices;
        [SerializeField] private uint maxPacks = DefaultMaxPacks;
        [SerializeField] private uint maxActions = DefaultMaxActions;
        [SerializeField] private uint maxActionFirings = DefaultMaxActionFirings;
        [SerializeField] private uint maxEmitters;
        [SerializeField] private uint emitterVariableSlots;
        [SerializeField] private string language = string.Empty;
        [SerializeField] private string packPlatformId = string.Empty;
        [SerializeField] private string packPlatform = string.Empty;
        [SerializeField] private string initPack = "Init.pack";
        [SerializeField] private bool suppressUnityAudio;
        [SerializeField] private string packDirectory = string.Empty;
        [SerializeField] private string[] playerExports = Array.Empty<string>();

        private static HarkenProjectSettings transient;

        public bool EnableCommunicationInBuilds => enableCommunicationInBuilds;
        public uint MaxVoices => NonZero(maxVoices, DefaultMaxVoices);
        public uint MaxPacks => NonZero(maxPacks, DefaultMaxPacks);
        public uint MaxActions => NonZero(maxActions, DefaultMaxActions);
        public uint MaxActionFirings => maxActionFirings != 0 ? maxActionFirings : UnlimitedActionFirings;
        public uint MaxEmitters => maxEmitters;
        public uint EmitterVariableSlots => emitterVariableSlots;
        public string Language => language ?? string.Empty;
        public string PlatformId => packPlatformId ?? string.Empty;
        public string PlatformName => packPlatform ?? string.Empty;
        public string InitPack => string.IsNullOrEmpty(initPack) ? "Init.pack" : initPack;
        public string PackDirectory => packDirectory ?? string.Empty;
        public bool SuppressUnityAudio => suppressUnityAudio;
        public string[] PlayerExports => playerExports ?? Array.Empty<string>();

        public static HarkenProjectSettings Load() => Resources.Load<HarkenProjectSettings>(ResourceName);
        public static bool CommunicationEnabled => Application.isEditor || (Load()?.enableCommunicationInBuilds ?? true);
        internal static Func<string> ReadAudioProjectDirectory { get; set; }

        internal static HarkenProjectSettings ActiveForStartup()
        {
            HarkenProjectSettings loaded = Load();
            if (loaded != null) return loaded;
            if (transient == null)
            {
                transient = CreateInstance<HarkenProjectSettings>();
                transient.hideFlags = HideFlags.HideAndDontSave;
            }
            return transient;
        }

        internal static string CaptureStartupState() => JsonUtility.ToJson(ActiveForStartup());

        internal static void RestoreStartupState(string snapshot)
        {
            if (!string.IsNullOrEmpty(snapshot))
                JsonUtility.FromJsonOverwrite(snapshot, ActiveForStartup());
        }

        internal void SetPackDirectory(string value) => packDirectory = value ?? string.Empty;
        internal void SetMaxVoices(uint value) => maxVoices = value;
        internal void SetMaxPacks(uint value) => maxPacks = value;
        internal void SetMaxActionFirings(uint value) => maxActionFirings = value;
        internal void SetSuppressUnityAudio(bool value) => suppressUnityAudio = value;
        internal void SetLanguage(string value) => language = value ?? string.Empty;
        internal void SetInitPack(string value) => initPack = value ?? string.Empty;
        internal void SetPlayerExports(string[] value) => playerExports = value ?? Array.Empty<string>();

        internal void SetPlatform(string id, string name)
        {
            packPlatformId = id ?? string.Empty;
            packPlatform = name ?? string.Empty;
        }

        internal static void ApplyStartupCapacity(ref VnConfig config)
        {
            HarkenProjectSettings settings = ActiveForStartup();
            config.maxVoices = settings.MaxVoices;
            // A single emitter stop can address every render voice atomically.
            if (config.maxVoices > DefaultMaxVoices)
            {
                config.commandQueueDepth = Math.Max(config.commandQueueDepth, config.maxVoices);
                config.maxControlCommandsPerQuantum = Math.Max(config.maxControlCommandsPerQuantum, config.maxVoices);
            }
            config.maxPacks = settings.MaxPacks;
            config.maxActions = settings.MaxActions;
            config.maxActionFirings = settings.MaxActionFirings;
            config.maxEmitters = settings.maxEmitters;
            config.emitterVariableSlots = settings.emitterVariableSlots;
        }

        internal static bool TryResolvePackDirectory(out string fullPath, out string error)
        {
            fullPath = null;
#if !UNITY_EDITOR
            return TryResolvePlayerPackDirectory(ActiveForStartup().PackDirectory,
                Application.streamingAssetsPath, ProjectRoot(), out fullPath, out error);
#else
            string raw = ActiveForStartup().PackDirectory.Trim();
            if (string.IsNullOrEmpty(raw) || IsObsoletePackRoot(raw))
            {
                error = "Select a GeneratedPacks directory in Harken Project Settings.";
                return false;
            }
            string projectRoot = ProjectRoot();
            if (!TryResolveProjectPath(raw, projectRoot, out fullPath) || !IsInside(projectRoot, fullPath))
            {
                error = "The GeneratedPacks directory must stay inside this Unity project.";
                return false;
            }
            if (Directory.Exists(fullPath) && IsReparsePoint(fullPath))
            {
                error = "The GeneratedPacks directory cannot be a symbolic link.";
                return false;
            }
            string audioProject = ReadAudioProjectDirectory != null ? ReadAudioProjectDirectory() : null;
            if (!string.IsNullOrWhiteSpace(audioProject) &&
                TryResolveProjectPath(audioProject.Trim(), projectRoot, out string audioFull) &&
                (PathsEqual(audioFull, fullPath) || IsInside(audioFull, fullPath)))
            {
                error = "The GeneratedPacks directory cannot be inside the selected audio project.";
                return false;
            }
            if (!Directory.Exists(fullPath))
            {
                error = "The selected GeneratedPacks directory does not exist.";
                return false;
            }
            error = null;
            return true;
#endif
        }

        internal static bool TryResolvePlayerPackDirectory(string configuredDirectory,
            string streamingAssetsPath, string playerRoot, out string fullPath, out string error)
        {
            fullPath = null;
            error = "Set the deployed Pack directory in Harken Project Settings.";
            string raw = (configuredDirectory ?? string.Empty).Trim().Replace('\\', '/');
            if (string.IsNullOrEmpty(raw) || raw.Contains("://")) return false;
            bool streaming = raw.StartsWith("Assets/StreamingAssets/", StringComparison.Ordinal);
            if (streaming)
            {
                if (!TryResolveStreamingPackDirectory(raw, streamingAssetsPath, out string relative, out error) ||
                    !PackManifest.TryJoinContainedPath(streamingAssetsPath, relative, out fullPath)) return false;
            }
            else if (!TryResolveProjectPath(raw, playerRoot, out fullPath) ||
                     (!Path.IsPathRooted(raw) && !IsInside(playerRoot, fullPath)))
            {
                fullPath = null;
                error = "A relative Player Pack directory must stay inside the player root. Use an absolute path for an external directory.";
                return false;
            }
            if (!Directory.Exists(fullPath))
            {
                error = "The selected GeneratedPacks directory does not exist in the player.";
                return false;
            }
            error = null;
            return true;
        }

        internal static bool TryResolveStreamingPackDirectory(string configuredDirectory,
            string streamingAssetsPath, out string relative, out string error)
        {
            relative = null;
            error = "Select a GeneratedPacks directory inside the player's StreamingAssets.";
            string raw = (configuredDirectory ?? string.Empty).Trim().Replace('\\', '/').TrimEnd('/');
            const string projectPrefix = "Assets/StreamingAssets/";
            string playerPrefix = (streamingAssetsPath ?? string.Empty).Replace('\\', '/').TrimEnd('/') + "/";
            string candidate;
            if (raw.StartsWith(projectPrefix, StringComparison.Ordinal))
                candidate = raw.Substring(projectPrefix.Length);
            else if (!string.IsNullOrEmpty(streamingAssetsPath) && raw.StartsWith(playerPrefix, StringComparison.Ordinal))
                candidate = raw.Substring(playerPrefix.Length);
            else
                return false;
            if (!PackManifest.IsSafeRelativePath(candidate)) return false;
            relative = candidate;
            error = null;
            return true;
        }

        internal static bool PlayerIncludesExport(string platformDirectory, out string error)
        {
            error = null;
            string name = string.IsNullOrEmpty(platformDirectory)
                ? string.Empty
                : Path.GetFileName(platformDirectory.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
            string[] exports = ActiveForStartup().PlayerExports;
            if (exports == null || exports.Length == 0)
            {
                error = "Select which exports are included in the player.";
                return false;
            }
            foreach (string export in exports)
            {
                if (string.Equals(export, name, StringComparison.OrdinalIgnoreCase)) return true;
            }
            error = "Select which exports are included in the player.";
            return false;
        }

        internal static bool IsObsoletePackRoot(string value)
        {
            if (string.IsNullOrWhiteSpace(value)) return false;
            string normalized = value.Trim().Replace('\\', '/').Trim('/');
            return string.Equals(normalized, ObsoleteRelativePackRoot, StringComparison.OrdinalIgnoreCase);
        }

        internal static string ProjectRoot() =>
            Path.GetFullPath(Path.Combine(Application.dataPath, ".."));

        internal static bool TryResolveProjectPath(string raw, string projectRoot, out string fullPath)
        {
            fullPath = null;
            if (string.IsNullOrWhiteSpace(raw)) return false;
            try
            {
                fullPath = Path.IsPathRooted(raw)
                    ? Path.GetFullPath(raw)
                    : Path.GetFullPath(Path.Combine(projectRoot, raw));
                return true;
            }
            catch (ArgumentException) { return false; }
            catch (NotSupportedException) { return false; }
            catch (PathTooLongException) { return false; }
            catch (IOException) { return false; }
        }

        internal static bool IsInside(string parent, string child)
        {
            if (string.IsNullOrEmpty(parent) || string.IsNullOrEmpty(child)) return false;
            try
            {
                string root = WithSeparator(Path.GetFullPath(parent));
                string full = WithSeparator(Path.GetFullPath(child));
                StringComparison comparison = Path.DirectorySeparatorChar == '\\'
                    ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
                return full.StartsWith(root, comparison);
            }
            catch (ArgumentException) { return false; }
            catch (NotSupportedException) { return false; }
            catch (PathTooLongException) { return false; }
            catch (IOException) { return false; }
        }

        internal static VnResult StartCommunication(string projectName)
        {
            if (!CommunicationEnabled) return VnResult.VN_CAPABILITY_UNAVAILABLE;
            HarkenProjectSettings settings = Load();
            string name = settings == null || string.IsNullOrEmpty(settings.remoteApplicationName)
                ? Application.productName : settings.remoteApplicationName;
            return RemoteCommunication.Start(name,
                settings != null ? settings.remoteBindAddress : "0.0.0.0",
                settings != null ? settings.remotePort : 0,
                settings == null || settings.enableRemoteDiscovery,
                settings != null ? settings.remoteDiscoveryPort : 0,
                projectName);
        }

        private static uint NonZero(uint value, uint fallback) => value != 0 ? value : fallback;

        private static bool PathsEqual(string left, string right)
        {
            try
            {
                StringComparison comparison = Path.DirectorySeparatorChar == '\\'
                    ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
                return string.Equals(TrimSeparator(Path.GetFullPath(left)), TrimSeparator(Path.GetFullPath(right)), comparison);
            }
            catch (ArgumentException) { return false; }
            catch (NotSupportedException) { return false; }
            catch (PathTooLongException) { return false; }
            catch (IOException) { return false; }
        }

        private static string WithSeparator(string path) => TrimSeparator(path) + Path.DirectorySeparatorChar;

        private static string TrimSeparator(string path) =>
            path.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);

        private static bool IsReparsePoint(string path)
        {
            try { return (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0; }
            catch (IOException) { return false; }
            catch (UnauthorizedAccessException) { return false; }
            catch (ArgumentException) { return false; }
        }
    }

    internal struct HarkenProjectSettingsLease : IDisposable
    {
        private readonly string snapshot;

        private HarkenProjectSettingsLease(string snapshot) => this.snapshot = snapshot;

        public HarkenProjectSettings Settings => HarkenProjectSettings.ActiveForStartup();

        public static HarkenProjectSettingsLease Acquire() =>
            new HarkenProjectSettingsLease(HarkenProjectSettings.CaptureStartupState());

        public void Dispose() => HarkenProjectSettings.RestoreStartupState(snapshot);
    }
}
