using System;
using System.Collections.Generic;
using System.Text;
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

        [SerializeField] private TextAsset inventory;
        [SerializeField] private TextAsset selectedManifest;
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
                if (!PackManifest.IsSafeRelativePath(item.relativePath) || assets.ContainsKey(item.relativePath))
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
            error = "Capture Pack TextAssets on their Unity main thread.";
            if (Thread.CurrentThread.ManagedThreadId != mainThread) return VnResult.VN_INVALID_ARG;
            if (inventory == null || initPack == null)
            {
                error = "The Pack inventory and paired Init TextAsset are required.";
                return VnResult.VN_CONTENT_NOT_LOADED;
            }
            string effectiveLanguage = string.IsNullOrEmpty(selectedLanguage) ? language : selectedLanguage;
            string inventoryText = inventory.text;
            VnResult result = GameplayPublication.Parse(inventoryText,
                selectedManifest != null ? selectedManifest.text : null, effectiveLanguage,
                out GameplayPublication publication, out error);
            if (result != VnResult.VN_OK) return result;
            result = HarkenPreparedPacks.Create(publication, initPack.bytes, out prepared, out error);
            if (result != VnResult.VN_OK) return result;
            var paths = new HashSet<string>(StringComparer.Ordinal);
            foreach (ContentAsset source in content ?? Array.Empty<ContentAsset>())
            {
                if (!PackManifest.IsSafeRelativePath(source.relativePath) || !paths.Add(source.relativePath))
                {
                    error = "Invalid or duplicate prepared Content path: " + source.relativePath;
                    prepared = null;
                    return VnResult.VN_INVALID_ARG;
                }
                PackManifestEntry entry = default;
                foreach (PackManifestEntry candidate in publication.Packs)
                    if (candidate.Path == source.relativePath) { entry = candidate; break; }
                if (!entry.IsValid || entry.PackStableId == publication.InitPackStableId)
                {
                    error = "Content is not in the selected publication: " + source.relativePath;
                    prepared = null;
                    return VnResult.VN_UNKNOWN_ID;
                }
                if (source.asset == null) continue;
                result = prepared.SupplyPack(entry.PackStableId, source.asset.bytes);
                if (result != VnResult.VN_OK)
                {
                    error = "Prepared Content '" + source.relativePath + "': " + result;
                    prepared = null;
                    return result;
                }
            }
            if (!PackManifest.TryGetSelectedManifestPath(inventoryText, effectiveLanguage, out string selectedPath))
            {
                prepared = null;
                return VnResult.VN_BAD_PACK;
            }
            manifestBytes = (byte[])(selectedPath == null ? inventory.bytes : selectedManifest.bytes).Clone();
            error = null;
            return VnResult.VN_OK;
        }
    }

    public sealed class HarkenPreparedPacks
    {
        private readonly byte[] initBytes;
        private readonly Dictionary<ulong, byte[]> content = new Dictionary<ulong, byte[]>();
        private readonly int ownerThread = Thread.CurrentThread.ManagedThreadId;
        private PackByteProvider.Handler demandProvider;
        private object demandState;

        private HarkenPreparedPacks(GameplayPublication publication, byte[] init)
        {
            Publication = publication;
            initBytes = init;
        }

        public GameplayPublication Publication { get; }
        public string SelectedManifestPath { get; private set; }
        private static readonly UTF8Encoding Utf8 = new UTF8Encoding(false, true);

        /// <summary>Reads one publication through a caller-owned synchronous byte reader.</summary>
        public static VnResult Capture(string manifestPath, Func<string, byte[]> readBytes,
            out HarkenPreparedPacks prepared, out byte[] manifestBytes, out string error,
            string language = null)
        {
            prepared = null;
            manifestBytes = null;
            error = "A safe manifest path and a byte reader are required.";
            if (!PackManifest.IsSafeRelativePath(manifestPath) || readBytes == null)
                return VnResult.VN_INVALID_ARG;
            VnResult result = ReadRequired(readBytes, manifestPath, "inventory", out byte[] inventory, out error);
            if (result != VnResult.VN_OK) return result;
            result = DecodeUtf8(inventory, "inventory", manifestPath, out string inventoryJson, out error);
            if (result != VnResult.VN_OK) return result;
            if (!PackManifest.TryGetSelectedManifestPath(inventoryJson, language, out string selectedRelative))
            {
                error = "Invalid selected manifest.";
                return VnResult.VN_BAD_PACK;
            }
            string root = LogicalRoot(manifestPath);
            string selectedPath = manifestPath;
            byte[] selected = inventory;
            if (selectedRelative != null)
            {
                if (!TryJoinLogical(root, selectedRelative, out selectedPath))
                {
                    error = "Invalid selected manifest path: " + selectedRelative;
                    return VnResult.VN_INVALID_ARG;
                }
                result = ReadRequired(readBytes, selectedPath, "selected manifest", out selected, out error);
                if (result != VnResult.VN_OK) return result;
            }
            string selectedJson = null;
            if (selectedRelative != null)
            {
                result = DecodeUtf8(selected, "selected manifest", selectedPath, out selectedJson, out error);
                if (result != VnResult.VN_OK) return result;
            }
            result = GameplayPublication.Parse(inventoryJson, selectedJson, language,
                out GameplayPublication publication, out error);
            if (result != VnResult.VN_OK) return result;
            if (!TryJoinLogical(root, publication.InitPack.Path, out string initPath))
            {
                error = "Invalid Init Pack path: " + publication.InitPack.Path;
                return VnResult.VN_INVALID_ARG;
            }
            result = ReadRequired(readBytes, initPath, "Init Pack", out byte[] initBytes, out error);
            if (result != VnResult.VN_OK) return result;
            result = Create(publication, initBytes, out HarkenPreparedPacks captured, out error);
            if (result != VnResult.VN_OK) return result;
            result = captured.SetDemandProvider(ProvideDemand, new DemandState(root, readBytes));
            if (result != VnResult.VN_OK)
            {
                error = "SetDemandProvider: " + result;
                return result;
            }
            result = ConfirmUnchanged(readBytes, manifestPath, inventory, selectedPath, selected, out error);
            if (result != VnResult.VN_OK) return result;
            captured.SelectedManifestPath = selectedPath;
            prepared = captured;
            manifestBytes = (byte[])selected.Clone();
            error = null;
            return VnResult.VN_OK;
        }

        private sealed class DemandState
        {
            public DemandState(string root, Func<string, byte[]> readBytes)
            {
                Root = root;
                ReadBytes = readBytes;
            }

            public string Root { get; }
            public Func<string, byte[]> ReadBytes { get; }
        }

        private static VnResult ProvideDemand(ulong id, string path, uint variant, ulong hash,
            out byte[] bytes, object state)
        {
            bytes = null;
            var demand = (DemandState)state;
            if (!TryJoinLogical(demand.Root, path, out string logical))
                return VnResult.VN_INVALID_ARG;
            try
            {
                bytes = demand.ReadBytes(logical);
                return bytes == null || bytes.Length == 0 ? VnResult.VN_CONTENT_NOT_LOADED : VnResult.VN_OK;
            }
            catch (Exception)
            {
                bytes = null;
                return VnResult.VN_PACK_PROVIDER_FAILED;
            }
        }

        private static VnResult ReadRequired(Func<string, byte[]> readBytes, string path, string operation,
            out byte[] bytes, out string error)
        {
            bytes = null;
            try
            {
                byte[] raw = readBytes(path);
                if (raw == null || raw.Length == 0)
                {
                    error = "Missing " + operation + ": " + path;
                    return VnResult.VN_CONTENT_NOT_LOADED;
                }
                bytes = (byte[])raw.Clone();
                error = null;
                return VnResult.VN_OK;
            }
            catch (Exception exception)
            {
                error = operation + " '" + path + "': " + exception.GetType().Name + ": " + exception.Message;
                return VnResult.VN_PACK_PROVIDER_FAILED;
            }
        }

        private static VnResult DecodeUtf8(byte[] bytes, string operation, string path,
            out string text, out string error)
        {
            text = null;
            try
            {
                text = Utf8.GetString(bytes);
                error = null;
                return VnResult.VN_OK;
            }
            catch (DecoderFallbackException exception)
            {
                error = "Invalid UTF-8 " + operation + " '" + path + "': " + exception.Message;
                return VnResult.VN_BAD_PACK;
            }
        }

        private static VnResult ConfirmUnchanged(Func<string, byte[]> readBytes, string inventoryPath,
            byte[] inventory, string selectedPath, byte[] selected, out string error)
        {
            try
            {
                if (!SameBytes(inventory, readBytes(inventoryPath)))
                {
                    error = "Pack inventory changed during capture: " + inventoryPath;
                    return VnResult.VN_PACK_STALE_REVISION;
                }
                if (selectedPath != inventoryPath && !SameBytes(selected, readBytes(selectedPath)))
                {
                    error = "Selected Pack manifest changed during capture: " + selectedPath;
                    return VnResult.VN_PACK_STALE_REVISION;
                }
                error = null;
                return VnResult.VN_OK;
            }
            catch (Exception exception)
            {
                error = "re-read '" + inventoryPath + "': " + exception.GetType().Name + ": " + exception.Message;
                return VnResult.VN_PACK_PROVIDER_FAILED;
            }
        }

        private static bool SameBytes(byte[] captured, byte[] current)
        {
            if (captured == null || current == null || captured.Length != current.Length) return false;
            for (int i = 0; i < captured.Length; ++i)
                if (captured[i] != current[i]) return false;
            return true;
        }

        private static string LogicalRoot(string manifestPath)
        {
            int slash = manifestPath.LastIndexOf('/');
            return slash < 0 ? string.Empty : manifestPath.Substring(0, slash);
        }

        private static bool TryJoinLogical(string root, string relativePath, out string joined)
        {
            joined = null;
            if (!PackManifest.IsSafeRelativePath(relativePath)) return false;
            joined = string.IsNullOrEmpty(root) ? relativePath : root + "/" + relativePath;
            return true;
        }

        public static VnResult Create(GameplayPublication publication, byte[] init,
            out HarkenPreparedPacks prepared, out string error)
        {
            prepared = null;
            error = "A paired gameplay publication is required.";
            if (publication == null) return VnResult.VN_INVALID_ARG;
            byte[] snapshot = init != null ? (byte[])init.Clone() : null;
            VnResult result = publication.ValidateInit(snapshot, out error);
            if (result == VnResult.VN_OK) prepared = new HarkenPreparedPacks(publication, snapshot);
            return result;
        }

        public VnResult SupplyPack(ulong id, byte[] bytes)
        {
            if (Thread.CurrentThread.ManagedThreadId != ownerThread) return VnResult.VN_INVALID_ARG;
            if (id == Publication.InitPackStableId) return VnResult.VN_INVALID_ARG;
            if (!Publication.TryFindPack(id, out PackManifestEntry entry)) return VnResult.VN_UNKNOWN_ID;
            if (bytes == null || bytes.Length == 0) return VnResult.VN_CONTENT_NOT_LOADED;
            byte[] snapshot = (byte[])bytes.Clone();
            if (!PackManifest.VerifyPackBytes(snapshot, entry)) return VnResult.VN_PACK_HASH_MISMATCH;
            content[id] = snapshot;
            return VnResult.VN_OK;
        }

        public VnResult Start(ulong[] startupIds, ref VnConfig config, out PackBootstrapSession session)
        {
            session = null;
            if (Thread.CurrentThread.ManagedThreadId != ownerThread) return VnResult.VN_INVALID_ARG;
            return PackBootstrap.Start(Publication, initBytes, startupIds, Provide, this, ref config, out session);
        }

        public VnResult SetDemandProvider(PackByteProvider.Handler provider, object state = null)
        {
            if (Thread.CurrentThread.ManagedThreadId != ownerThread) return VnResult.VN_INVALID_ARG;
            demandProvider = provider;
            demandState = state;
            return VnResult.VN_OK;
        }

        // Explicit snapshots win. Demand bytes are borrowed for this synchronous load only.
        private static VnResult Provide(ulong id, string path, uint variant, ulong hash, out byte[] bytes, object state)
        {
            bytes = null;
            var source = (HarkenPreparedPacks)state;
            if (Thread.CurrentThread.ManagedThreadId != source.ownerThread) return VnResult.VN_INVALID_ARG;
            if (!source.Publication.TryFindPack(id, out PackManifestEntry entry)) return VnResult.VN_UNKNOWN_ID;
            if (entry.Path != path || entry.VariantKey != variant || entry.ContentHash != hash)
                return VnResult.VN_PACK_STALE_REVISION;
            if (source.content.TryGetValue(id, out bytes)) return VnResult.VN_OK;
            return source.demandProvider == null ? VnResult.VN_CONTENT_NOT_LOADED :
                source.demandProvider(id, path, variant, hash, out bytes, source.demandState);
        }
    }
}
