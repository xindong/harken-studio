using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEngine;

namespace Harken.Unity.Editor
{
    internal static class HarkenEditorPublicationSupport
    {
        internal static bool IsAvailable => false;
        internal static bool CompanionAvailable => true;
        internal const string Diagnostic = "Legacy publication and symbol tools are unavailable. Use current Pack companions.";
    }

    public sealed class HarkenEditorPack
    {
        internal HarkenEditorPack(PackCatalogSnapshot.Entry row)
        {
            Name = row.Input.Names.Name; PackStableId = row.Input.Facts.packStableId;
            BytesPath = row.Path + ".bytes"; JsonPath = row.Path + ".json";
            ByteCount = row.ByteCount; ContentHash = row.Input.Facts.contentHash;
            ContentRevision = row.Input.Facts.contentRevision;
        }
        public string Name { get; }
        public ulong PackStableId { get; }
        public string BytesPath { get; }
        public string JsonPath { get; }
        public long ByteCount { get; }
        public ulong ContentHash { get; }
        public ulong ContentRevision { get; }
    }

    public sealed class HarkenActionDuration
    {
        internal HarkenActionDuration(PackCatalogSnapshot.Entry row, uint revision)
        {
            ActionId = row.Input.Names.RootId; ContentHash = row.Input.Facts.contentHash;
            CatalogRevision = revision;
            Kind = HarkenActionDurationKind.Unknown;
            Reason = "Native inspection cannot prove this Action's execution duration.";
            if (row.Occurrences.Length == 0) return;
            double minimum = double.PositiveInfinity, maximum = 0;
            foreach (var occurrence in row.Occurrences)
            {
                if ((occurrence.flags & VnConstants.VN_PACK_OCCURRENCE_DURATION) == 0 ||
                    double.IsNaN(occurrence.durationSeconds) || double.IsInfinity(occurrence.durationSeconds) ||
                    occurrence.durationSeconds < 0) return;
                minimum = Math.Min(minimum, occurrence.durationSeconds);
                maximum = Math.Max(maximum, occurrence.durationSeconds);
            }
            Kind = minimum == maximum ? HarkenActionDurationKind.Exact : HarkenActionDurationKind.Bounded;
            MinimumSeconds = minimum; MaximumSeconds = maximum; Reason = string.Empty;
        }
        public HarkenActionDurationKind Kind { get; }
        public double? MinimumSeconds { get; }
        public double? MaximumSeconds { get; }
        public double? ExactSeconds => Kind == HarkenActionDurationKind.Exact ? MinimumSeconds : null;
        public string Reason { get; }
        public VnObjectId ActionId { get; }
        public ulong ContentHash { get; }
        public uint CatalogRevision { get; }
    }

    public sealed partial class HarkenEditorCatalog
    {
        private PackCatalogSnapshot snapshot;
        private object sourceIdentity;
        public GameplayPublication Publication => snapshot?.Publication;
        public HarkenPreparedPacks PreparedPacks { get; private set; }
        public string DirectoryPath { get; private set; }
        public VnResult LastResult { get; private set; } = VnResult.VN_CAPABILITY_UNAVAILABLE;
        public string Diagnostic { get; private set; } = "No Pack source is selected.";
        public uint Revision { get; private set; }
        public VnResult WaveformResult { get; private set; } = VnResult.VN_CAPABILITY_UNAVAILABLE;
        public string WaveformDiagnostic { get; private set; } = "No waveform is selected.";

        public VnResult RefreshPlatform(string root, string platformId, string platformName,
            string basePlatform, string language = null)
            => Refresh(Path.Combine(root ?? "", string.IsNullOrEmpty(platformName) ? basePlatform ?? "" : platformName), language);

        public VnResult Refresh(string directory, string language = null)
        {
            try
            {
                if (string.IsNullOrWhiteSpace(directory)) return Fail(VnResult.VN_CAPABILITY_UNAVAILABLE, "No Pack directory is selected.");
                string full = Path.GetFullPath(directory);
                var names = CandidateFiles(full).ToList();
                if (!string.IsNullOrEmpty(language) && PackDirectorySource.ValidLanguage(language) && Directory.Exists(Path.Combine(full, language)))
                    names.AddRange(CandidateFiles(Path.Combine(full, language)));
                var resources = HarkenEditorResources.FromDirectory(Path.GetDirectoryName(full) ?? full, full);
                return RefreshCore(PackSource.FromDirectory(full, language), names, full + "|" + (language ?? ""), full, resources);
            }
            catch (Exception ex) { return Fail(VnResult.VN_PACK_PROVIDER_FAILED, ex.Message); }
        }

        private static IEnumerable<string> CandidateFiles(string directory)
        {
            foreach (string path in Directory.EnumerateFiles(directory, "*", SearchOption.TopDirectoryOnly))
            {
                string name = Path.GetFileNameWithoutExtension(path);
                if (name == "Init" || !PackCompanion.IsName(name)) continue;
                if (path.EndsWith(".bytes", StringComparison.Ordinal)) yield return name;
                else if (path.EndsWith(".json", StringComparison.Ordinal))
                {
                    bool action = false;
                    try
                    {
                        var parser = new JsonParser(File.ReadAllText(path));
                        action = parser.Read(0) is Dictionary<string, object> root &&
                            root.TryGetValue("name", out object value) && value is string;
                    }
                    catch (FormatException) { }
                    if (action) yield return name;
                }
            }
        }

        public VnResult Refresh(PackSource source, IEnumerable<string> actionNames)
            => RefreshCore(source, actionNames, source, null);
        public VnResult Refresh(PackSource source, IEnumerable<string> actionNames, HarkenEditorResources resources)
            => RefreshCore(source, actionNames, source, null, resources);
        public VnResult Refresh(HarkenPackSource source, string language = null) => Refresh(source, language, null);
        public VnResult Refresh(HarkenPackSource source, string language, TextAsset companion)
            => Refresh(source, language, companion, null);
        public VnResult Refresh(HarkenPackSource source, string language, TextAsset companion, HarkenEditorResources resources)
        {
            if (source == null) return Fail(VnResult.VN_CONTENT_NOT_LOADED, "A Pack source is required.");
            VnResult result = source.CaptureCompanion(companion, out PackSource captured, out string error, language);
            return result == VnResult.VN_OK ? RefreshCore(captured, source.CandidateNames(language), source, null, resources) : Fail(result, error);
        }
        public VnResult Refresh(string manifestPath, Func<string, byte[]> readBytes, string language = null)
            => Fail(VnResult.VN_PACK_INCOMPATIBLE, "Use Refresh(PackSource, actionNames) for current companion readers.");

        private VnResult RefreshCore(PackSource source, IEnumerable<string> names, object identity, string directory,
            HarkenEditorResources resources = null)
        {
            VnResult result = PackCatalogSnapshot.Capture(source, names, out PackCatalogSnapshot next, out string error);
            if (result != VnResult.VN_OK) return Fail(result, error);
            bool keepExecution = Equals(sourceIdentity, identity) && next.SameExecution(snapshot);
            snapshot = next;
            sourceIdentity = identity;
            if (!keepExecution) PreparedPacks = new HarkenPreparedPacks(next);
            DirectoryPath = directory;
            ResetWaveforms(resources);
            ++Revision;
            ++PreparedPacks.PresentationRevision;
            LastResult = VnResult.VN_OK; Diagnostic = string.Empty;
            WaveformResult = VnResult.VN_CAPABILITY_UNAVAILABLE;
            WaveformDiagnostic = "No waveform is selected.";
            return LastResult;
        }

        private VnResult Fail(VnResult result, string error)
        {
            ++Revision;
            if (PreparedPacks != null) ++PreparedPacks.PresentationRevision;
            snapshot = null; PreparedPacks = null; sourceIdentity = null; DirectoryPath = null;
            ResetWaveforms(null);
            LastResult = result; Diagnostic = error;
            WaveformResult = result; WaveformDiagnostic = error;
            return result;
        }

        public VnResult CheckReference(string name, string projectId, out string diagnostic)
        {
            diagnostic = Diagnostic;
            if (snapshot == null) return LastResult;
            VnResult result = !string.IsNullOrEmpty(projectId) && projectId != Publication.ProjectId
                ? VnResult.VN_GAMEPLAY_WRONG_PROJECT
                : Publication.TryFindAction(name, out _) ? VnResult.VN_OK : VnResult.VN_UNKNOWN_ID;
            diagnostic = result == VnResult.VN_OK ? string.Empty : name + ": " + result;
            return result;
        }
        public VnResult SelectAction(HarkenAction action, string name, bool rebindProject, out string diagnostic)
        {
            if (action == null) { diagnostic = "An Action component is required."; return VnResult.VN_INVALID_ARG; }
            VnResult result = CheckReference(name, rebindProject ? null : action.ProjectId, out diagnostic);
            if (result != VnResult.VN_OK) return result;
            Undo.RecordObject(action, "Select Harken Action");
            var properties = new SerializedObject(action);
            properties.FindProperty("actionName").stringValue = name;
            properties.FindProperty("projectId").stringValue = Publication.ProjectId;
            properties.ApplyModifiedProperties();
            EditorUtility.SetDirty(action);
            return VnResult.VN_OK;
        }
        public VnResult GetPreparationOrder(string name, out ulong[] ids, out string diagnostic)
        {
            ids = Array.Empty<ulong>();
            VnResult result = CheckReference(name, null, out diagnostic);
            return result == VnResult.VN_OK ? Publication.GetActionPreparationOrder(new[] { name }, Publication.ProjectId, out ids) : result;
        }
        public VnResult GetPack(string name, out HarkenEditorPack pack, out string diagnostic)
        {
            pack = null;
            VnResult result = CheckReference(name, null, out diagnostic);
            if (result == VnResult.VN_OK) pack = new HarkenEditorPack(snapshot.Actions[name]);
            return result;
        }
        public VnResult GetDuration(string name, out HarkenActionDuration duration, out string diagnostic)
        {
            duration = null;
            VnResult result = CheckReference(name, null, out diagnostic);
            if (result == VnResult.VN_OK) { duration = new HarkenActionDuration(snapshot.Actions[name], Revision); diagnostic = duration.Reason; }
            return result;
        }
        public VnResult GetWaveform(string name, out float[] samples, out double? durationSeconds, out string diagnostic)
        {
            samples = Array.Empty<float>(); durationSeconds = null;
            VnResult result = GetDuration(name, out HarkenActionDuration duration, out diagnostic);
            if (result != VnResult.VN_OK) return result;
            durationSeconds = duration.ExactSeconds;
            result = GetActionWaveform(name, out HarkenActionWaveform waveform, out diagnostic);
            if (result == VnResult.VN_OK) samples = waveform.Source.GetMergedMinMax();
            return result;
        }
    }
}
