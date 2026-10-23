using System;
using System.IO;
using System.Linq;
using System.Text;
using UnityEditor;
using UnityEngine;

namespace Harken.Unity.Editor
{
    internal static class HarkenEditorPublicationSupport
    {
        // Temporarily suspend manifest-backed tooling. Restore these entry points only after
        // catalog, preview, waveform and symbol generation consume the current Pack companions.
        // Keep this capability independent of schema numbers so a schema release needs no UI edits.
        internal static bool IsAvailable => false;
        internal const string Diagnostic =
            "Editor publication tools are temporarily unavailable. Configure Action names and project ids manually for runtime playback.";
    }

    public sealed class HarkenEditorCatalog
    {
        public GameplayPublication Publication { get; private set; }
        public HarkenPreparedPacks PreparedPacks { get; private set; }
        public string DirectoryPath { get; private set; }
        public VnResult LastResult { get; private set; } = VnResult.VN_CAPABILITY_UNAVAILABLE;
        public string Diagnostic { get; private set; } = "No paired gameplay publication is selected.";
        public uint Revision { get; private set; }
        public VnResult WaveformResult { get; private set; } = VnResult.VN_CAPABILITY_UNAVAILABLE;
        public string WaveformDiagnostic { get; private set; } = "No Editor waveform companion is selected.";
        private HarkenWaveformPublication waveforms;

        public VnResult RefreshPlatform(string root, string platformId, string platformName,
            string basePlatform, string language = null)
        {
            if (!HarkenEditorPublicationSupport.IsAvailable)
                return Adopt(VnResult.VN_CAPABILITY_UNAVAILABLE, null, null, null, HarkenEditorPublicationSupport.Diagnostic);
            if (!PackManifest.TryReadPlatformDirectory(root, platformId, platformName, basePlatform,
                language, out string directory, out string error))
                return Adopt(VnResult.VN_INVALID_ARG, null, null, null, error);
            return Refresh(directory, language);
        }

        public VnResult Refresh(string directory, string language = null)
        {
            if (!HarkenEditorPublicationSupport.IsAvailable)
                return Adopt(VnResult.VN_CAPABILITY_UNAVAILABLE, null, null, null, HarkenEditorPublicationSupport.Diagnostic);
            if (string.IsNullOrEmpty(directory))
                return Adopt(VnResult.VN_INVALID_ARG, null, null, null, "No publication directory is selected.");
            try
            {
                string marker = Path.Combine(directory, ".harken-pack-publishing");
                if (File.Exists(marker)) return Adopt(VnResult.VN_PACK_STALE_REVISION, null, null, null, "Pack publication is in progress.");
                string inventoryPath = Path.Combine(directory, PackManifest.ManifestFileName);
                byte[] inventoryBytes = File.ReadAllBytes(inventoryPath);
                var utf8 = new UTF8Encoding(false, true);
                string inventory = utf8.GetString(inventoryBytes);
                if (!PackManifest.TryGetSelectedManifestPath(inventory, language, out string relative))
                    return Adopt(VnResult.VN_BAD_PACK, null, null, null, "Invalid selected manifest.");
                string selectedPath = inventoryPath;
                if (relative != null && !PackManifest.TryJoinContainedPath(directory, relative, out selectedPath))
                    return Adopt(VnResult.VN_PACK_PATH_ESCAPE, null, null, null, "Invalid selected manifest path.");
                byte[] manifest = relative == null ? inventoryBytes : File.ReadAllBytes(selectedPath);
                VnResult result = GameplayPublication.Parse(inventory, relative == null ? null : utf8.GetString(manifest),
                    language, out GameplayPublication candidate, out string error);
                if (result != VnResult.VN_OK) return Adopt(result, null, null, null, error);
                if (HarkenUnityPublicationReader.HasAmbiguousPackFiles(directory, candidate.InitPack.Path, out string ambiguous))
                    return Adopt(VnResult.VN_INVALID_ARG, null, null, null, ambiguous);
                if (HarkenUnityPublicationReader.ShouldCaptureUnityAssets(directory, candidate.InitPack.Path) &&
                    HarkenUnityPublicationReader.TryManifestPath(directory, out string manifestPath))
                {
                    VnResult capturedResult = Refresh(manifestPath, HarkenUnityPublicationReader.Read, language);
                    if (capturedResult == VnResult.VN_OK) DirectoryPath = directory;
                    return capturedResult;
                }
                if (candidate.Packs.Any(entry => HarkenPackFiles.HasWrapper(Path.Combine(directory, entry.Path))))
                {
                    VnResult capturedResult = Refresh(PackManifest.ManifestFileName, logical =>
                    {
                        if (!PackManifest.TryJoinContainedPath(directory, logical, out string full))
                            throw new IOException("Pack path leaves the selected directory.");
                        return HarkenPackFiles.Read(full);
                    }, language);
                    if (capturedResult == VnResult.VN_OK) DirectoryPath = directory;
                    return capturedResult;
                }
                result = candidate.ValidateInit(HarkenPackFiles.Read(Path.Combine(directory, candidate.InitPack.Path)), out error);
                if (result != VnResult.VN_OK) return Adopt(result, null, null, null, error);
                byte[] companion = null;
                VnResult waveformResult;
                string waveformError;
                HarkenWaveformPublication captured = null;
                try
                {
                    string companionPath = Path.Combine(Path.GetDirectoryName(selectedPath), HarkenWaveformPublication.FileName);
                    if (File.Exists(companionPath))
                    {
                        if (new FileInfo(companionPath).Length > HarkenWaveformPublication.MaxBytes)
                            throw new IOException("Editor waveform companion exceeds its byte budget.");
                        companion = File.ReadAllBytes(companionPath);
                    }
                    waveformResult = HarkenWaveformPublication.Parse(companion, manifest, candidate, out captured, out waveformError);
                }
                catch (IOException exception) { waveformResult = VnResult.VN_BAD_PACK; waveformError = exception.Message; }
                catch (UnauthorizedAccessException exception) { waveformResult = VnResult.VN_BAD_PACK; waveformError = exception.Message; }
                if (File.Exists(marker) || !inventoryBytes.SequenceEqual(File.ReadAllBytes(inventoryPath)) ||
                    !manifest.SequenceEqual(File.ReadAllBytes(selectedPath)))
                    return Adopt(VnResult.VN_PACK_STALE_REVISION, null, null, null, "Pack publication changed during capture.");
                Adopt(VnResult.VN_OK, candidate, null, directory, null);
                waveforms = captured;
                WaveformResult = waveformResult;
                WaveformDiagnostic = waveformError;
                return VnResult.VN_OK;
            }
            catch (IOException exception) { return Adopt(VnResult.VN_BAD_PACK, null, null, null, exception.Message); }
            catch (UnauthorizedAccessException exception) { return Adopt(VnResult.VN_BAD_PACK, null, null, null, exception.Message); }
            catch (ArgumentException exception) { return Adopt(VnResult.VN_BAD_PACK, null, null, null, exception.Message); }
        }

        public VnResult Refresh(HarkenPackSource source, string language = null)
            => Refresh(source, language, null);

        public VnResult Refresh(HarkenPackSource source, string language, TextAsset companion)
        {
            if (!HarkenEditorPublicationSupport.IsAvailable)
                return Adopt(VnResult.VN_CAPABILITY_UNAVAILABLE, null, null, null, HarkenEditorPublicationSupport.Diagnostic);
            if (source == null) return Adopt(VnResult.VN_CONTENT_NOT_LOADED, null, null, null,
                "The prepared Pack source is missing.");
            VnResult result = source.Capture(out HarkenPreparedPacks prepared, out byte[] manifest, out string error, language);
            Adopt(result, prepared?.Publication, prepared, null, error);
            if (result == VnResult.VN_OK)
            {
                byte[] bytes = companion != null ? (byte[])companion.bytes.Clone() : null;
                WaveformResult = HarkenWaveformPublication.Parse(bytes, manifest, Publication, out waveforms, out string waveformError);
                WaveformDiagnostic = waveformError;
            }
            return result;
        }

        public VnResult Refresh(string manifestPath, Func<string, byte[]> readBytes, string language = null)
        {
            if (!HarkenEditorPublicationSupport.IsAvailable)
                return Adopt(VnResult.VN_CAPABILITY_UNAVAILABLE, null, null, null, HarkenEditorPublicationSupport.Diagnostic);
            VnResult result = HarkenPreparedPacks.Capture(manifestPath, readBytes,
                out HarkenPreparedPacks prepared, out byte[] manifest, out string error, language);
            if (result != VnResult.VN_OK) return Adopt(result, null, null, null, error);
            if (!PackManifest.TrySelectPlatform(new[] { prepared.Publication.Platform }, null, null,
                    HarkenAudioSystem.CurrentPackBasePlatform(), out _, out string platformError))
                return Adopt(VnResult.VN_INVALID_ARG, null, null, null, platformError);
            Adopt(VnResult.VN_OK, prepared.Publication, prepared, null, null);
            ReadCompanionWaveform(prepared.SelectedManifestPath, readBytes, manifest);
            return VnResult.VN_OK;
        }

        private void ReadCompanionWaveform(string selectedManifestPath, Func<string, byte[]> readBytes, byte[] manifest)
        {
            string companionPath = CompanionPath(selectedManifestPath);
            byte[] companion;
            try { companion = readBytes(companionPath); }
            catch (Exception exception)
            {
                waveforms = null;
                WaveformResult = VnResult.VN_PACK_PROVIDER_FAILED;
                WaveformDiagnostic = "waveform '" + companionPath + "': " +
                    exception.GetType().Name + ": " + exception.Message;
                return;
            }
            if (companion != null && companion.Length > HarkenWaveformPublication.MaxBytes)
            {
                waveforms = null;
                WaveformResult = VnResult.VN_BAD_PACK;
                WaveformDiagnostic = "Editor waveform companion exceeds its byte budget.";
                return;
            }
            WaveformResult = HarkenWaveformPublication.Parse(companion, manifest, Publication, out waveforms,
                out string waveformError);
            WaveformDiagnostic = waveformError;
        }

        private static string CompanionPath(string selectedManifestPath)
        {
            if (string.IsNullOrEmpty(selectedManifestPath)) return HarkenWaveformPublication.FileName;
            int slash = selectedManifestPath.LastIndexOf('/');
            return slash < 0 ? HarkenWaveformPublication.FileName
                : selectedManifestPath.Substring(0, slash) + "/" + HarkenWaveformPublication.FileName;
        }

        private VnResult Adopt(VnResult result, GameplayPublication candidate, HarkenPreparedPacks prepared,
            string directory, string error)
        {
            waveforms = null;
            WaveformResult = VnResult.VN_CAPABILITY_UNAVAILABLE;
            WaveformDiagnostic = "No Editor waveform companion is selected.";
            if (result != VnResult.VN_OK)
            {
                ++Revision;
                Publication = null;
                PreparedPacks = null;
                DirectoryPath = null;
            }
            else
            {
                if (!SamePublication(Publication, candidate)) ++Revision;
                Publication = candidate;
                PreparedPacks = prepared;
                DirectoryPath = directory;
            }
            LastResult = result;
            Diagnostic = error ?? string.Empty;
            return result;
        }

        private static bool SamePublication(GameplayPublication left, GameplayPublication right)
        {
            if (left == null || right == null || left.ProjectId != right.ProjectId || left.Language != right.Language ||
                left.Digest != right.Digest || left.InitPackStableId != right.InitPackStableId ||
                left.InitContentRevision != right.InitContentRevision || left.Packs.Count != right.Packs.Count) return false;
            foreach (PackManifestEntry pack in left.Packs)
                if (!right.TryFindPack(pack.PackStableId, out PackManifestEntry match) ||
                    pack.ContentHash != match.ContentHash || pack.Path != match.Path) return false;
            return true;
        }

        public VnResult CheckReference(string name, string projectId, out string diagnostic)
        {
            if (Publication == null)
            {
                VnResult unbound = LastResult == VnResult.VN_OK ? VnResult.VN_CAPABILITY_UNAVAILABLE : LastResult;
                string reason = string.IsNullOrEmpty(Diagnostic)
                    ? "No paired gameplay publication is selected." : Diagnostic;
                diagnostic = "Action name was not validated. " + reason;
                return unbound;
            }
            VnResult result;
            if (string.IsNullOrEmpty(name)) result = VnResult.VN_INVALID_ARG;
            else if (!PackManifest.IsPlatformId(projectId))
            {
                diagnostic = "Action '" + name + "' is not bound to a project id.";
                return VnResult.VN_INVALID_ARG;
            }
            else if (Publication.ProjectId != projectId) result = VnResult.VN_GAMEPLAY_WRONG_PROJECT;
            else if (Publication.TryFindAction(name, out _)) result = VnResult.VN_OK;
            else result = Publication.RetiredActions.Any(row => row.Name == name)
                ? VnResult.VN_GAMEPLAY_NAME_RETIRED : VnResult.VN_UNKNOWN_ID;
            diagnostic = result == VnResult.VN_OK ? string.Empty : Actions.Describe(result, name) +
                (result == VnResult.VN_GAMEPLAY_WRONG_PROJECT
                    ? " An explicit project rebind is required." : string.Empty);
            return result;
        }

        public VnResult SelectAction(HarkenAction action, string name, bool rebindProject, out string diagnostic)
        {
            diagnostic = "Action '" + name + "' cannot be selected without a paired publication.";
            if (action == null || Publication == null) return VnResult.VN_CAPABILITY_UNAVAILABLE;
            string project = string.IsNullOrEmpty(action.ProjectId) || rebindProject ? Publication.ProjectId : action.ProjectId;
            VnResult result = CheckReference(name, project, out diagnostic);
            if (result != VnResult.VN_OK) return result;
            var serialized = new SerializedObject(action);
            serialized.FindProperty("actionName").stringValue = name;
            serialized.FindProperty("projectId").stringValue = Publication.ProjectId;
            serialized.ApplyModifiedProperties();
            PrefabUtility.RecordPrefabInstancePropertyModifications(action);
            return VnResult.VN_OK;
        }

        public VnResult GetPreparationOrder(string name, out ulong[] ids, out string diagnostic)
        {
            ids = Array.Empty<ulong>();
            GameplayPublication publication = Publication;
            if (publication == null) return CheckReference(name, null, out diagnostic);
            VnResult result = CheckReference(name, publication.ProjectId, out diagnostic);
            if (result != VnResult.VN_OK) return result;
            result = publication.GetActionPreparationOrder(new[] { name }, publication.ProjectId, out ids);
            if (result != VnResult.VN_OK)
                diagnostic = "Action '" + name + "' has inconsistent explicit Pack dependencies.";
            return result;
        }

        public VnResult GetWaveform(string name, out float[] samples, out double? durationSeconds, out string diagnostic)
        {
            samples = Array.Empty<float>();
            durationSeconds = null;
            VnResult result = GetActionWaveform(name, out HarkenActionWaveform waveform, out diagnostic);
            if (result != VnResult.VN_OK) return result;
            samples = waveform.Source.GetMergedMinMax();
            durationSeconds = waveform.OneShotDurationSeconds;
            return VnResult.VN_OK;
        }

        public VnResult GetActionPreview(string name, out HarkenActionPreview preview, out string diagnostic)
        {
            preview = null;
            VnResult result = CheckReference(name, Publication?.ProjectId, out diagnostic);
            if (result != VnResult.VN_OK) return result;
            diagnostic = "Action '" + name + "': " + WaveformDiagnostic;
            if (WaveformResult != VnResult.VN_OK) return WaveformResult;
            if (waveforms == null || !waveforms.TryFindPreview(name, out preview)) return VnResult.VN_CAPABILITY_UNAVAILABLE;
            diagnostic = preview.Kind == HarkenActionPreviewKind.Unavailable ? preview.SourceUnavailableReason : preview.DurationUnavailableReason;
            return preview.Kind == HarkenActionPreviewKind.Unavailable ? VnResult.VN_CAPABILITY_UNAVAILABLE : VnResult.VN_OK;
        }

        public VnResult GetActionWaveform(string name, out HarkenActionWaveform waveform, out string diagnostic)
        {
            waveform = null;
            VnResult result = CheckReference(name, Publication?.ProjectId, out diagnostic);
            if (result != VnResult.VN_OK) return result;
            diagnostic = "Action '" + name + "': " + WaveformDiagnostic;
            if (WaveformResult != VnResult.VN_OK) return WaveformResult;
            if (waveforms == null || !waveforms.TryFindAction(name, out waveform)) return VnResult.VN_CAPABILITY_UNAVAILABLE;
            if (waveform.Source == null)
            {
                diagnostic = waveform.SourceUnavailableReason;
                return VnResult.VN_CAPABILITY_UNAVAILABLE;
            }
            diagnostic = waveform.DurationUnavailableReason;
            return VnResult.VN_OK;
        }
    }
}
