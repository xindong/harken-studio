using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Harken.Unity.Editor
{
    public sealed class HarkenSourceWaveform
    {
        internal HarkenSourceWaveform(VnObjectId id, string file, ulong start, ulong end,
            HarkenSourceEnvelope source, string diagnostic)
        { SoundId = id; File = file; TrimStartFrame = start; TrimEndFrame = end; Source = source; Diagnostic = diagnostic; }
        public VnObjectId SoundId { get; }
        public string File { get; }
        public ulong TrimStartFrame { get; }
        public ulong TrimEndFrame { get; }
        public HarkenSourceEnvelope Source { get; }
        public string Diagnostic { get; }
    }

    public sealed partial class HarkenEditorCatalog
    {
        private const long CacheBudget = 64 * 1024 * 1024;
        private HarkenEditorResources resources;
        private readonly Dictionary<string, HarkenVkd> vkdCache = new Dictionary<string, HarkenVkd>(StringComparer.OrdinalIgnoreCase);
        private readonly HashSet<string> verifiedPaths = new HashSet<string>(StringComparer.Ordinal);
        private readonly Dictionary<string, HarkenActionPreview> previewCache = new Dictionary<string, HarkenActionPreview>(StringComparer.Ordinal);
        private readonly Dictionary<string, IReadOnlyList<HarkenSourceWaveform>> collectionCache = new Dictionary<string, IReadOnlyList<HarkenSourceWaveform>>(StringComparer.Ordinal);
        private long cacheBytes;
        private sealed class WaveReference
        {
            internal VnObjectId Sound;
            internal string File;
            internal ulong Start, End;
        }

        private void ResetWaveforms(HarkenEditorResources next)
        { resources = next; vkdCache.Clear(); verifiedPaths.Clear(); previewCache.Clear(); collectionCache.Clear(); cacheBytes = 0; }

        private Dictionary<VnObjectId, WaveReference> References(PackCatalogSnapshot.Entry entry)
        {
            var result = new Dictionary<VnObjectId, WaveReference>();
            var parser = new JsonParser(new UTF8Encoding(false, true).GetString(entry.Input.Json));
            if (!(parser.Read(0) is Dictionary<string, object> root) || !parser.End) throw new InvalidDataException("Invalid companion JSON.");
            if (!root.TryGetValue("waveforms", out object value)) return result;
            if (!(value is List<object> rows)) throw new InvalidDataException("Invalid optional waveforms array.");
            foreach (object item in rows)
            {
                if (!(item is Dictionary<string, object> row) || !row.TryGetValue("soundGuid", out object guid) ||
                    !(guid is string text) || !VnObjectId.TryParse(text, out VnObjectId id) ||
                    !row.TryGetValue("file", out object file) || !(file is string path) || string.IsNullOrWhiteSpace(path) ||
                    !row.TryGetValue("trimStartFrame", out object start) || !Frame(start, out ulong from) ||
                    !row.TryGetValue("trimEndFrame", out object end) || !Frame(end, out ulong to) || from >= to ||
                    result.ContainsKey(id)) throw new InvalidDataException("Invalid or duplicate optional waveform reference.");
                result.Add(id, new WaveReference { Sound = id, File = path, Start = from, End = to });
            }
            return result;
        }
        private static bool Frame(object value, out ulong frame)
        {
            frame = value is ulong unsigned ? unsigned : value is long signed && signed >= 0 ? (ulong)signed : ulong.MaxValue;
            return frame <= 9007199254740991ul;
        }
        private HarkenSourceEnvelope Envelope(PackCatalogSnapshot.Entry entry, WaveReference reference,
            float gain, ulong occurrence)
        {
            if (resources == null) throw new FileNotFoundException("No presentation resource reader was supplied.");
            string path = resources.Resolve(entry.Path + ".json", reference.File);
            // Read every uncached reference to prove availability at this location, even when content is shared.
            string digest = Path.GetFileName(path);
            if (!vkdCache.TryGetValue(digest, out HarkenVkd vkd) || !verifiedPaths.Contains(path))
            {
                byte[] bytes = resources.Read(path, HarkenVkd.ByteLimit);
                vkd = HarkenVkd.Read(bytes, path);
                Reserve(bytes.Length);
                vkdCache[digest] = vkd;
                verifiedPaths.Add(path);
            }
            HarkenSourceEnvelope envelope = vkd.Trim(reference.Sound, reference.Start, reference.End, gain,
                occurrence, entry.Input.Facts.contentRevision);
            return envelope;
        }
        private static long EnvelopeBytes(HarkenSourceEnvelope envelope)
            => (long)envelope.BucketCount * envelope.ChannelCount * 8 + ((long)envelope.BucketCount + 1) * 8;
        private void Reserve(long bytes)
        {
            if (bytes > CacheBudget) throw new InvalidDataException("Waveform cache budget exceeded.");
            if (cacheBytes + bytes > CacheBudget || vkdCache.Count >= 128 || previewCache.Count + collectionCache.Count >= 256)
            { vkdCache.Clear(); verifiedPaths.Clear(); previewCache.Clear(); collectionCache.Clear(); cacheBytes = 0; }
            cacheBytes += bytes;
        }
        private VnResult WaveFailure(string error, out string diagnostic)
        {
            WaveformResult = VnResult.VN_CAPABILITY_UNAVAILABLE;
            WaveformDiagnostic = diagnostic = error;
            return WaveformResult;
        }

        /// <summary>All source media in the parent Pack, including embedded Action media. Not a mixed output.</summary>
        public VnResult GetSourceWaveforms(string name, out IReadOnlyList<HarkenSourceWaveform> sources, out string diagnostic)
        {
            sources = Array.Empty<HarkenSourceWaveform>();
            VnResult valid = CheckReference(name, null, out diagnostic);
            if (valid != VnResult.VN_OK) return valid;
            if (collectionCache.TryGetValue(name, out sources)) { diagnostic = string.Empty; return VnResult.VN_OK; }
            try
            {
                var entry = snapshot.Actions[name];
                var references = References(entry);
                var rows = new List<HarkenSourceWaveform>();
                long resultBytes = 0;
                foreach (var media in entry.Media)
                {
                    if (!references.TryGetValue(media.soundId, out WaveReference reference))
                    {
                        rows.Add(new HarkenSourceWaveform(media.soundId, null, 0, 0, null, "No waveform reference for this media."));
                        continue;
                    }
                    try
                    {
                        var envelope = Envelope(entry, reference, 1, 0);
                        if (resultBytes + EnvelopeBytes(envelope) > CacheBudget / 2) throw new IOException("Source collection exceeds its display budget.");
                        resultBytes += EnvelopeBytes(envelope);
                        rows.Add(new HarkenSourceWaveform(media.soundId, reference.File, reference.Start, reference.End, envelope, string.Empty));
                    }
                    catch (Exception ex) { rows.Add(new HarkenSourceWaveform(media.soundId, reference.File, reference.Start, reference.End, null, ex.Message)); }
                }
                sources = rows.AsReadOnly();
                bool complete = rows.Count != 0 && rows.TrueForAll(row => row.Source != null);
                if (complete) { Reserve(resultBytes); collectionCache[name] = sources; }
                diagnostic = complete ? string.Empty : "Some source waveforms are unavailable; inspect each source diagnostic.";
                WaveformDiagnostic = diagnostic;
                WaveformResult = complete ? VnResult.VN_OK : VnResult.VN_CAPABILITY_UNAVAILABLE;
                return WaveformResult;
            }
            catch (Exception ex) { sources = Array.Empty<HarkenSourceWaveform>(); return WaveFailure(ex.Message, out diagnostic); }
        }

        public VnResult GetActionPreview(string name, out HarkenActionPreview preview, out string diagnostic)
        {
            preview = null;
            VnResult valid = CheckReference(name, null, out diagnostic);
            if (valid != VnResult.VN_OK) return valid;
            if (previewCache.TryGetValue(name, out preview)) { diagnostic = string.Empty; return VnResult.VN_OK; }
            try
            {
                var entry = snapshot.Actions[name];
                if (entry.Occurrences.Length == 0) return WaveFailure("This Action has no supported single-source preview.", out diagnostic);
                var references = References(entry);
                var candidates = new List<HarkenActionPreviewCandidate>();
                long resultBytes = 0;
                foreach (var occurrence in entry.Occurrences)
                {
                    if ((occurrence.flags & VnConstants.VN_PACK_OCCURRENCE_FIXED_GAIN) == 0)
                        return WaveFailure("Native fixed gain is unavailable for this occurrence.", out diagnostic);
                    if (!references.TryGetValue(occurrence.soundId, out WaveReference reference))
                        return WaveFailure("No waveform reference for the inspected Sound.", out diagnostic);
                    var source = Envelope(entry, reference, occurrence.fixedGainLinear, occurrence.occurrenceFingerprint);
                    resultBytes += EnvelopeBytes(source);
                    if (resultBytes > CacheBudget / 2) throw new IOException("Candidate waveforms exceed their display budget.");
                    candidates.Add(new HarkenActionPreviewCandidate(occurrence.rootPlayableId + ":" + occurrence.childOrdinal + ":" + occurrence.occurrenceFingerprint, source)
                        { NativeOccurrence = occurrence });
                }
                GetDuration(name, out HarkenActionDuration duration, out _);
                Publication.TryFindAction(name, out GameplayAction action);
                var kind = entry.Input.Facts.previewKind == VnConstants.VN_PACK_PREVIEW_SOUND
                    ? HarkenActionPreviewKind.DirectSound : HarkenActionPreviewKind.RandomCandidates;
                preview = new HarkenActionPreview(action, kind, candidates.ToArray(), duration.Kind,
                    duration.MinimumSeconds, duration.MaximumSeconds, string.Empty, duration.Reason);
                Reserve(resultBytes);
                previewCache[name] = preview;
                diagnostic = WaveformDiagnostic = string.Empty;
                WaveformResult = VnResult.VN_OK;
                return VnResult.VN_OK;
            }
            catch (Exception ex) { preview = null; return WaveFailure(ex.Message, out diagnostic); }
        }

        public VnResult GetActionWaveform(string name, out HarkenActionWaveform waveform, out string diagnostic)
        {
            waveform = null;
            VnResult result = GetActionPreview(name, out HarkenActionPreview preview, out diagnostic);
            if (result != VnResult.VN_OK) return result;
            if (preview.Kind != HarkenActionPreviewKind.DirectSound || preview.Candidates.Count != 1)
                return WaveFailure("Random preview requires the actual started playback selection.", out diagnostic);
            GetDuration(name, out HarkenActionDuration duration, out _);
            waveform = new HarkenActionWaveform(preview.Action, preview.Candidates[0].Source,
                duration.Kind == HarkenActionDurationKind.Exact, string.Empty, duration.Reason, duration.ExactSeconds);
            return VnResult.VN_OK;
        }
    }
}
