using System;
using System.Collections.Generic;
using System.Threading;
using UnityEngine;

namespace Harken.Unity
{
    [CreateAssetMenu(fileName = "HarkenPackSource", menuName = "Harken/Pack Source")]
    public sealed class HarkenPackSource : ScriptableObject
    {
        [Serializable]
        public struct ContentAsset
        {
            public string relativePath;
            public TextAsset asset;
        }

        [SerializeField] private string language = string.Empty;
        [SerializeField] private TextAsset initPack;
        [SerializeField] private ContentAsset[] content = Array.Empty<ContentAsset>();
        private int mainThread;

        private void OnEnable() => mainThread = Thread.CurrentThread.ManagedThreadId;

        /// <summary>Captures immutable TextAsset references for a companion source; content bytes remain on demand.</summary>
        public VnResult CaptureCompanion(TextAsset initJson, out PackSource source, out string error,
            string selectedLanguage = null)
        {
            source = null;
            error = "Capture Pack TextAssets on their Unity main thread.";
            if (Thread.CurrentThread.ManagedThreadId != mainThread) return VnResult.VN_INVALID_ARG;
            if (initPack == null || initJson == null)
            { error = "The paired Init bytes and JSON TextAssets are required."; return VnResult.VN_CONTENT_NOT_LOADED; }
            string selected = selectedLanguage ?? language;
            if (!PackDirectorySource.ValidLanguage(selected)) return VnResult.VN_INVALID_ARG;
            var assets = new Dictionary<string, TextAsset>(StringComparer.Ordinal)
            { ["Init.bytes"] = initPack, ["Init.json"] = initJson };
            foreach (ContentAsset item in content ?? Array.Empty<ContentAsset>())
            {
                if (!PackPaths.IsSafeRelativePath(item.relativePath) || assets.ContainsKey(item.relativePath))
                { error = "Invalid or duplicate companion asset path."; return VnResult.VN_INVALID_ARG; }
                assets.Add(item.relativePath, item.asset);
            }
            int thread = mainThread;
            source = PackSource.FromReader(path =>
            {
                if (Thread.CurrentThread.ManagedThreadId != thread) throw new InvalidOperationException("TextAsset reads require their Unity thread.");
                if (!assets.TryGetValue(path, out TextAsset asset)) return null;
                if (asset == null) throw new InvalidOperationException("A captured TextAsset is no longer available.");
                return asset.bytes;
            }, selected);
            error = null;
            return VnResult.VN_OK;
        }

        public VnResult Capture(out HarkenPreparedPacks prepared, out string error, string selectedLanguage = null)
            => Capture(out prepared, out _, out error, selectedLanguage);

        public VnResult Capture(out HarkenPreparedPacks prepared, out byte[] manifestBytes,
            out string error, string selectedLanguage = null)
        {
            prepared = null;
            manifestBytes = null;
            error = HarkenPreparedPacks.Diagnostic;
            return VnResult.VN_PACK_INCOMPATIBLE;
        }
    }

    public sealed class HarkenPreparedPacks
    {
        internal const string Diagnostic = "Legacy Pack manifests are unsupported. Re-export with the current Studio and use CaptureCompanion or PackSource.";
        private HarkenPreparedPacks() { }
        public GameplayPublication Publication => null;
        public string SelectedManifestPath => null;

        public static VnResult Capture(string manifestPath, Func<string, byte[]> readBytes,
            out HarkenPreparedPacks prepared, out byte[] manifestBytes, out string error,
            string language = null)
        {
            prepared = null;
            manifestBytes = null;
            error = Diagnostic;
            return VnResult.VN_PACK_INCOMPATIBLE;
        }

        public static VnResult Create(GameplayPublication publication, byte[] init,
            out HarkenPreparedPacks prepared, out string error)
        {
            prepared = null;
            error = Diagnostic;
            return VnResult.VN_PACK_INCOMPATIBLE;
        }

        public VnResult SupplyPack(ulong id, byte[] bytes) => VnResult.VN_PACK_INCOMPATIBLE;
        public VnResult SetDemandProvider(PackByteProvider.Handler provider, object state = null)
            => VnResult.VN_PACK_INCOMPATIBLE;
        public VnResult Start(ulong[] startupIds, ref VnConfig config, out PackBootstrapSession session)
        {
            session = null;
            return VnResult.VN_PACK_INCOMPATIBLE;
        }
    }
}
