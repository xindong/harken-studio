using System;
using System.Collections.Generic;
using System.Globalization;
using System.Numerics;
using System.Text;

namespace Harken.Unity.Editor
{
    public sealed class HarkenSourceEnvelope
    {
        private readonly float[][] minima;
        private readonly float[][] maxima;

        internal HarkenSourceEnvelope(VnObjectId soundId, ulong fingerprint, ulong revision,
            ulong frames, uint rate, float[][] minima, float[][] maxima)
        {
            SoundStableId = soundId;
            PropertyFingerprint = fingerprint;
            ContentRevision = revision;
            FrameCount = frames;
            SampleRate = rate;
            this.minima = minima;
            this.maxima = maxima;
        }

        public VnObjectId SoundStableId { get; }
        public ulong PropertyFingerprint { get; }
        public ulong ContentRevision { get; }
        public ulong FrameCount { get; }
        public uint SampleRate { get; }
        public int ChannelCount => minima.Length;
        public int BucketCount => minima[0].Length;
        public double SourceDurationSeconds => (double)FrameCount / SampleRate;
        public float[] GetChannelMinima(int channel) => (float[])minima[channel].Clone();
        public float[] GetChannelMaxima(int channel) => (float[])maxima[channel].Clone();

        /// <summary>Alternating min/max extrema per bucket, merged across channels without averaging.</summary>
        public float[] GetMergedMinMax()
        {
            var result = new float[BucketCount * 2];
            for (int b = 0; b < BucketCount; ++b)
            {
                float lo = minima[0][b], hi = maxima[0][b];
                for (int c = 1; c < ChannelCount; ++c)
                {
                    lo = Math.Min(lo, minima[c][b]);
                    hi = Math.Max(hi, maxima[c][b]);
                }
                result[b * 2] = lo;
                result[b * 2 + 1] = hi;
            }
            return result;
        }
    }

    public enum HarkenActionPreviewKind { Unavailable, DirectSound, RandomCandidates }
    public enum HarkenActionDurationKind { Unknown, Exact, Bounded }

    public sealed class HarkenActionPreviewCandidate
    {
        internal HarkenActionPreviewCandidate(string occurrence, HarkenSourceEnvelope source)
        { Occurrence = occurrence; Source = source; }
        public string Occurrence { get; }
        public HarkenSourceEnvelope Source { get; }
    }

    public sealed class HarkenCandidateOverview
    {
        private readonly float[] extrema;
        internal HarkenCandidateOverview(double seconds, float[] extrema)
        { DurationSeconds = seconds; this.extrema = extrema; MinMax = System.Array.AsReadOnly(extrema); }
        public double DurationSeconds { get; }
        public int BucketCount => extrema.Length / 2;
        public IReadOnlyList<float> MinMax { get; }
        public float[] GetMergedMinMax() => (float[])extrema.Clone();
    }

    public sealed class HarkenActionPreview
    {
        private HarkenCandidateOverview overview;
        internal HarkenActionPreview(GameplayAction action, HarkenActionPreviewKind kind,
            HarkenActionPreviewCandidate[] candidates, HarkenActionDurationKind duration,
            double? minimum, double? maximum, string sourceReason, string durationReason)
        {
            Action = action; Kind = kind; Candidates = System.Array.AsReadOnly(candidates);
            DurationKind = duration; MinimumDurationSeconds = minimum; MaximumDurationSeconds = maximum;
            SourceUnavailableReason = sourceReason; DurationUnavailableReason = durationReason;
        }
        public GameplayAction Action { get; }
        public HarkenActionPreviewKind Kind { get; }
        public IReadOnlyList<HarkenActionPreviewCandidate> Candidates { get; }
        public HarkenActionDurationKind DurationKind { get; }
        public double? MinimumDurationSeconds { get; }
        public double? MaximumDurationSeconds { get; }
        public string SourceUnavailableReason { get; }
        public string DurationUnavailableReason { get; }

        /// <summary>Cached source-time extrema of every candidate; not the selected playback or its processed output.</summary>
        public HarkenCandidateOverview Overview
        {
            get
            {
                if (Kind == HarkenActionPreviewKind.Unavailable) return null;
                if (overview != null) return overview;
                double seconds = 0;
                int buckets = 0;
                HarkenSourceEnvelope longest = null;
                var sources = new HashSet<HarkenSourceEnvelope>();
                foreach (var candidate in Candidates)
                {
                    sources.Add(candidate.Source);
                    if (longest == null || candidate.Source.SourceDurationSeconds > seconds) longest = candidate.Source;
                    seconds = Math.Max(seconds, candidate.Source.SourceDurationSeconds);
                    buckets = Math.Max(buckets, candidate.Source.BucketCount);
                }
                var merged = new float[buckets * 2];
                for (int b = 0; b < buckets; ++b) { merged[b * 2] = float.PositiveInfinity; merged[b * 2 + 1] = float.NegativeInfinity; }
                foreach (HarkenSourceEnvelope source in sources)
                {
                    float[] extrema = source.GetMergedMinMax();
                    for (int b = 0; b < source.BucketCount; ++b)
                    {
                        // Use the producer's integer frame boundaries, including nonuniform tail buckets.
                        int first = Math.Min(buckets - 1, ProjectBoundary(source, Boundary(source, b), longest, buckets, false));
                        int last = Math.Max(first, Math.Min(buckets - 1, ProjectBoundary(source, Boundary(source, b + 1), longest, buckets, true) - 1));
                        for (int at = first; at <= last; ++at) Merge(merged, at, extrema[b * 2], extrema[b * 2 + 1]);
                    }
                    if (source.SourceDurationSeconds < seconds)
                    {
                        int firstZero = Math.Min(buckets - 1, ProjectBoundary(source, source.FrameCount, longest, buckets, false));
                        for (int b = firstZero; b < buckets; ++b) Merge(merged, b, 0, 0);
                    }
                }
                overview = new HarkenCandidateOverview(seconds, merged);
                return overview;
            }
        }
        private static int ProjectBoundary(HarkenSourceEnvelope source, ulong frame, HarkenSourceEnvelope longest, int buckets, bool ceiling)
        {
            // Exact rational projection avoids rounding an aligned boundary into the next bucket.
            BigInteger numerator = (BigInteger)frame * longest.SampleRate * buckets;
            BigInteger denominator = (BigInteger)source.SampleRate * longest.FrameCount;
            BigInteger index = BigInteger.DivRem(numerator, denominator, out BigInteger remainder);
            return (int)index + (ceiling && !remainder.IsZero ? 1 : 0);
        }
        private static ulong Boundary(HarkenSourceEnvelope source, int bucket)
            => source.FrameCount / (ulong)source.BucketCount * (ulong)bucket +
                source.FrameCount % (ulong)source.BucketCount * (ulong)bucket / (ulong)source.BucketCount;
        private static void Merge(float[] values, int b, float lo, float hi)
        { values[b * 2] = Math.Min(values[b * 2], lo); values[b * 2 + 1] = Math.Max(values[b * 2 + 1], hi); }
    }

    public sealed class HarkenActionWaveform
    {
        internal HarkenActionWaveform(GameplayAction action, HarkenSourceEnvelope source,
            bool oneShotKnown, string sourceReason, string durationReason)
        {
            Action = action;
            Source = source;
            OneShotDurationKnown = oneShotKnown;
            SourceUnavailableReason = sourceReason;
            DurationUnavailableReason = durationReason;
        }

        public GameplayAction Action { get; }
        public HarkenSourceEnvelope Source { get; }
        public bool OneShotDurationKnown { get; }
        public double? OneShotDurationSeconds => OneShotDurationKnown ? Source.SourceDurationSeconds : (double?)null;
        public string SourceUnavailableReason { get; }
        public string DurationUnavailableReason { get; }
    }

    public sealed class HarkenWaveformPublication
    {
        public const string FileName = "HarkenEditorWaveforms.json";
        public const int MaxBytes = 16 * 1024 * 1024;
        private readonly Dictionary<string, HarkenActionWaveform> actions;

        private readonly Dictionary<string, HarkenActionPreview> previews;
        private HarkenWaveformPublication(Dictionary<string, HarkenActionWaveform> actions,
            Dictionary<string, HarkenActionPreview> previews = null)
        {
            this.actions = actions;
            this.previews = previews ?? new Dictionary<string, HarkenActionPreview>(StringComparer.Ordinal);
            if (previews != null) return;
            foreach (var pair in actions)
            {
                var wave = pair.Value;
                var candidates = wave.Source == null ? System.Array.Empty<HarkenActionPreviewCandidate>() :
                    new[] { new HarkenActionPreviewCandidate(wave.Action.ActionStableId + ":0", wave.Source) };
                this.previews.Add(pair.Key, new HarkenActionPreview(wave.Action,
                    wave.Source == null ? HarkenActionPreviewKind.Unavailable : HarkenActionPreviewKind.DirectSound,
                    candidates, wave.OneShotDurationKnown ? HarkenActionDurationKind.Exact : HarkenActionDurationKind.Unknown,
                    wave.OneShotDurationSeconds, wave.OneShotDurationSeconds, wave.SourceUnavailableReason, wave.DurationUnavailableReason));
            }
        }

        public bool TryFindPreview(string name, out HarkenActionPreview preview)
        { preview = null; return name != null && previews.TryGetValue(name, out preview); }

        public bool TryFindAction(string name, out HarkenActionWaveform waveform)
        {
            waveform = null;
            return name != null && actions.TryGetValue(name, out waveform);
        }

        public static VnResult Parse(byte[] companionBytes, byte[] manifestBytes, GameplayPublication publication,
            out HarkenWaveformPublication waveforms, out string diagnostic)
        {
            waveforms = null;
            diagnostic = "The selected publication has no Editor waveform companion.";
            if (companionBytes == null) return VnResult.VN_CAPABILITY_UNAVAILABLE;
            diagnostic = "Invalid Editor waveform companion.";
            if (publication == null || manifestBytes == null || companionBytes.Length == 0 ||
                companionBytes.Length > MaxBytes || manifestBytes.Length > MaxBytes) return VnResult.VN_BAD_PACK;
            try
            {
                var parser = new JsonParser(new UTF8Encoding(false, true).GetString(companionBytes));
                Dictionary<string, object> root = Object(parser.Read(0));
                if (!parser.End) throw new FormatException();
                ulong version = Number(root, "schemaVersion");
                if (version != 1 && version != 2)
                {
                    diagnostic = "Unsupported Editor waveform companion version: " + version;
                    return VnResult.VN_CAPABILITY_UNAVAILABLE;
                }
                if (version == 1 && root.ContainsKey("sources")) throw new FormatException();
                Keys(root, "schemaVersion", "envelopeKind", "manifestByteLength", "manifestFnv1a64", "projectId",
                    "platformId", "basePlatform", "language", "initPackStableId", "initContentHash", "catalogDigest", "actions", "sources");
                Require(Text(root, "envelopeKind") == "exported-source-pcm-minmax");
                Identity(Number(root, "manifestByteLength") == (ulong)manifestBytes.Length &&
                    Id(root, "manifestFnv1a64") == Hash(manifestBytes) && Text(root, "projectId") == publication.ProjectId &&
                    Text(root, "platformId") == publication.Platform.Id && Text(root, "basePlatform") == publication.Platform.BasePlatform &&
                    Text(root, "language") == publication.Language && Id(root, "initPackStableId") == publication.InitPackStableId &&
                    Id(root, "initContentHash") == publication.InitPack.ContentHash && Id(root, "catalogDigest") == publication.Digest);
                if (version == 2)
                {
                    waveforms = ParseVersionTwo(root, publication);
                    diagnostic = string.Empty;
                    return VnResult.VN_OK;
                }
                List<object> rows = Array(root, "actions");
                Require(rows.Count <= 4096);
                Identity(rows.Count == publication.Actions.Count);
                var result = new Dictionary<string, HarkenActionWaveform>(StringComparer.Ordinal);
                var ids = new HashSet<VnObjectId>();
                foreach (object item in rows)
                {
                    var row = Object(item);
                    Keys(row, "name", "actionStableId", "nameBindingRevision", "callableFingerprint",
                        "ownerPackStableId", "providerClosureFingerprint", "sourceAvailable", "sourceReason",
                        "oneShotDurationKnown", "durationReason", "source");
                    string name = Text(row, "name");
                    VnObjectId id = VnObjectId.Parse(Text(row, "actionStableId"));
                    Require(ids.Add(id) && !result.ContainsKey(name));
                    Identity(publication.TryFindAction(name, out GameplayAction action));
                    Identity(id == action.ActionStableId &&
                        Id(row, "nameBindingRevision") == action.NameBindingRevision &&
                        Id(row, "callableFingerprint") == action.CallableFingerprint &&
                        Id(row, "ownerPackStableId") == action.OwnerPackStableId &&
                        Id(row, "providerClosureFingerprint") == action.ProviderClosureFingerprint);
                    bool available = Boolean(row, "sourceAvailable"), known = Boolean(row, "oneShotDurationKnown");
                    HarkenSourceEnvelope source = null;
                    string sourceReason = string.Empty, durationReason = string.Empty;
                    if (!available)
                    {
                        Require(!known && !row.ContainsKey("source"));
                        sourceReason = Text(row, "sourceReason");
                        durationReason = Text(row, "durationReason");
                        Require(sourceReason.Length != 0 && durationReason.Length != 0);
                    }
                    else
                    {
                        Require(!row.ContainsKey("sourceReason"));
                        if (known) Require(!row.ContainsKey("durationReason"));
                        else { durationReason = Text(row, "durationReason"); Require(durationReason.Length != 0); }
                        var data = Object(Value(row, "source"));
                        Keys(data, "soundStableId", "propertyFingerprint", "contentRevision", "ownerPackStableId", "contentHash",
                            "frameCount", "sampleRate", "channelCount", "bucketCount", "channels");
                        Identity(Id(data, "ownerPackStableId") == action.OwnerPackStableId &&
                            publication.TryFindPack(action.OwnerPackStableId, out PackManifestEntry owner) &&
                            Id(data, "contentHash") == owner.ContentHash);
                        source = ParseSource(data);
                    }
                    result.Add(name, new HarkenActionWaveform(action, source, known, sourceReason, durationReason));
                }
                waveforms = new HarkenWaveformPublication(result);
                diagnostic = string.Empty;
                return VnResult.VN_OK;
            }
            catch (IdentityException) { diagnostic = "Editor waveform identity does not match the selected publication."; return VnResult.VN_PACK_STALE_REVISION; }
            catch (FormatException) { return VnResult.VN_BAD_PACK; }
            catch (ArgumentException) { return VnResult.VN_BAD_PACK; }
        }

        private sealed class SourceRow
        {
            public HarkenSourceEnvelope Source;
            public ulong Owner;
        }

        private static HarkenWaveformPublication ParseVersionTwo(Dictionary<string, object> root, GameplayPublication publication)
        {
            var sources = new Dictionary<string, SourceRow>(StringComparer.Ordinal);
            var identities = new HashSet<string>(StringComparer.Ordinal);
            long decodedBytes = 0;
            foreach (object item in Array(root, "sources"))
            {
                var data = Object(item);
                Keys(data, "key", "soundStableId", "propertyFingerprint", "contentRevision", "ownerPackStableId", "contentHash",
                    "frameCount", "sampleRate", "channelCount", "bucketCount", "channels");
                string key = Text(data, "key");
                ulong owner = Id(data, "ownerPackStableId");
                Identity(publication.TryFindPack(owner, out PackManifestEntry pack) && Id(data, "contentHash") == pack.ContentHash);
                ulong channels = Number(data, "channelCount"), buckets = Number(data, "bucketCount");
                Require(channels <= 32 && buckets <= 2048);
                decodedBytes += (long)(channels * buckets * 8) + 1024;
                Require(decodedBytes <= MaxBytes && key.Length > 0 && key.Length <= 256 && !sources.ContainsKey(key));
                HarkenSourceEnvelope source = ParseSource(data);
                string identity = source.SoundStableId + ":" + source.PropertyFingerprint.ToString(CultureInfo.InvariantCulture) + ":" +
                    source.ContentRevision.ToString(CultureInfo.InvariantCulture) + ":" + owner.ToString(CultureInfo.InvariantCulture) + ":" +
                    pack.ContentHash.ToString(CultureInfo.InvariantCulture);
                Require(identities.Add(identity) && key == identity);
                sources.Add(key, new SourceRow { Source = source, Owner = owner });
            }
            List<object> rows = Array(root, "actions");
            Require(rows.Count <= 4096);
            Identity(rows.Count == publication.Actions.Count);
            var waves = new Dictionary<string, HarkenActionWaveform>(StringComparer.Ordinal);
            var previews = new Dictionary<string, HarkenActionPreview>(StringComparer.Ordinal);
            var ids = new HashSet<VnObjectId>();
            var referenced = new HashSet<string>(StringComparer.Ordinal);
            foreach (object item in rows)
            {
                var row = Object(item);
                Keys(row, "name", "actionStableId", "nameBindingRevision", "callableFingerprint", "ownerPackStableId",
                    "providerClosureFingerprint", "kind", "sourceReason", "duration", "candidates");
                string name = Text(row, "name");
                VnObjectId id = VnObjectId.Parse(Text(row, "actionStableId"));
                Require(ids.Add(id) && !previews.ContainsKey(name));
                Identity(publication.TryFindAction(name, out GameplayAction action));
                Identity(id == action.ActionStableId && Id(row, "nameBindingRevision") == action.NameBindingRevision &&
                    Id(row, "callableFingerprint") == action.CallableFingerprint && Id(row, "ownerPackStableId") == action.OwnerPackStableId &&
                    Id(row, "providerClosureFingerprint") == action.ProviderClosureFingerprint);
                string type = Text(row, "kind"), sourceReason = string.Empty;
                HarkenActionPreviewKind kind = type == "directSound" ? HarkenActionPreviewKind.DirectSound :
                    type == "randomCandidates" ? HarkenActionPreviewKind.RandomCandidates : HarkenActionPreviewKind.Unavailable;
                Require(type == "directSound" || type == "randomCandidates" || type == "unavailable");
                List<object> candidateRows = Array(row, "candidates");
                Require(candidateRows.Count <= 256);
                if (kind == HarkenActionPreviewKind.Unavailable)
                {
                    sourceReason = Text(row, "sourceReason");
                    Require(candidateRows.Count == 0 && sourceReason.Length != 0);
                }
                else Require(!row.ContainsKey("sourceReason") && candidateRows.Count > 0 &&
                    (kind != HarkenActionPreviewKind.DirectSound || candidateRows.Count == 1));
                Identity(publication.GetActionPreparationOrder(new[] { name }, publication.ProjectId, out ulong[] order) == VnResult.VN_OK);
                var closure = new HashSet<ulong>(order) { publication.InitPackStableId };
                var occurrences = new HashSet<string>(StringComparer.Ordinal);
                var candidates = new HarkenActionPreviewCandidate[candidateRows.Count];
                double lower = double.MaxValue, upper = 0;
                for (int i = 0; i < candidates.Length; ++i)
                {
                    var candidate = Object(candidateRows[i]);
                    Keys(candidate, "occurrence", "sourceKey");
                    string occurrence = Text(candidate, "occurrence"), key = Text(candidate, "sourceKey");
                    Require(occurrence.Length > 0 && occurrence.Length <= 128 && occurrences.Add(occurrence));
                    Require(sources.TryGetValue(key, out SourceRow source));
                    Identity(closure.Contains(source.Owner));
                    referenced.Add(key);
                    candidates[i] = new HarkenActionPreviewCandidate(occurrence, source.Source);
                    lower = Math.Min(lower, source.Source.SourceDurationSeconds);
                    upper = Math.Max(upper, source.Source.SourceDurationSeconds);
                }
                var duration = Object(Value(row, "duration"));
                Keys(duration, "kind", "minSeconds", "maxSeconds", "reason");
                string durationType = Text(duration, "kind"), durationReason = string.Empty;
                HarkenActionDurationKind durationKind;
                double? minimum = null, maximum = null;
                if (durationType == "unknown")
                {
                    durationKind = HarkenActionDurationKind.Unknown;
                    durationReason = Text(duration, "reason");
                    Require(durationReason.Length != 0 && !duration.ContainsKey("minSeconds") && !duration.ContainsKey("maxSeconds"));
                }
                else
                {
                    Require(kind != HarkenActionPreviewKind.Unavailable && !duration.ContainsKey("reason") &&
                        (durationType == "exact" || durationType == "bounded"));
                    double lo = Sample(Value(duration, "minSeconds")), hi = Sample(Value(duration, "maxSeconds"));
                    Require(lo >= 0 && lo <= hi && (durationType == "exact" ? lo == hi : lo < hi));
                    // v2 only proves unchanged one-shot source timing. Values must agree with all candidates.
                    Require(Math.Abs(lo - lower) <= Math.Max(1e-12, lower * 1e-12) && Math.Abs(hi - upper) <= Math.Max(1e-12, upper * 1e-12));
                    durationKind = durationType == "exact" ? HarkenActionDurationKind.Exact : HarkenActionDurationKind.Bounded;
                    minimum = lo; maximum = hi;
                }
                var preview = new HarkenActionPreview(action, kind, candidates, durationKind, minimum, maximum, sourceReason, durationReason);
                previews.Add(name, preview);
                bool direct = kind == HarkenActionPreviewKind.DirectSound;
                waves.Add(name, new HarkenActionWaveform(action, direct ? candidates[0].Source : null,
                    direct && durationKind == HarkenActionDurationKind.Exact,
                    direct ? string.Empty : kind == HarkenActionPreviewKind.RandomCandidates ? "random-requires-action-preview" : sourceReason,
                    direct ? durationReason : "source-unavailable"));
            }
            Require(referenced.Count == sources.Count);
            return new HarkenWaveformPublication(waves, previews);
        }

        private static HarkenSourceEnvelope ParseSource(Dictionary<string, object> data)
        {
            ulong frames = Id(data, "frameCount"), rate = Number(data, "sampleRate");
            ulong channelCount = Number(data, "channelCount"), buckets = Number(data, "bucketCount");
            VnObjectId sound = VnObjectId.Parse(Text(data, "soundStableId"));
            ulong fingerprint = Id(data, "propertyFingerprint"), revision = Id(data, "contentRevision");
            Require(frames > 0 && rate > 0 && rate <= uint.MaxValue && !sound.IsNull && fingerprint != 0 && revision != 0 &&
                channelCount > 0 && channelCount <= 32 && buckets == Math.Min(frames, 2048UL));
            List<object> channels = Array(data, "channels");
            Require((ulong)channels.Count == channelCount);
            var minima = new float[channels.Count][];
            var maxima = new float[channels.Count][];
            for (int c = 0; c < channels.Count; ++c)
            {
                var channel = Object(channels[c]);
                Keys(channel, "min", "max");
                List<object> lo = Array(channel, "min"), hi = Array(channel, "max");
                Require((ulong)lo.Count == buckets && hi.Count == lo.Count);
                minima[c] = new float[lo.Count]; maxima[c] = new float[lo.Count];
                for (int b = 0; b < lo.Count; ++b)
                {
                    double minimum = Sample(lo[b]), maximum = Sample(hi[b]);
                    Require(minimum <= maximum);
                    minima[c][b] = (float)minimum; maxima[c][b] = (float)maximum;
                }
            }
            return new HarkenSourceEnvelope(sound, fingerprint, revision, frames, (uint)rate, minima, maxima);
        }

        private sealed class IdentityException : Exception { }
        private static void Identity(bool condition) { if (!condition) throw new IdentityException(); }
        private static void Require(bool condition) { if (!condition) throw new FormatException(); }
        private static object Value(Dictionary<string, object> row, string key)
            => row.TryGetValue(key, out object value) ? value : throw new FormatException();
        private static Dictionary<string, object> Object(object value)
            => value as Dictionary<string, object> ?? throw new FormatException();
        private static List<object> Array(Dictionary<string, object> row, string key)
            => Value(row, key) as List<object> ?? throw new FormatException();
        private static string Text(Dictionary<string, object> row, string key)
            => Value(row, key) as string ?? throw new FormatException();
        private static ulong Number(Dictionary<string, object> row, string key)
            => Value(row, key) is ulong number ? number : throw new FormatException();
        private static bool Boolean(Dictionary<string, object> row, string key)
            => Value(row, key) is bool boolean ? boolean : throw new FormatException();
        private static ulong Id(Dictionary<string, object> row, string key)
            => ulong.TryParse(Text(row, key), NumberStyles.None, CultureInfo.InvariantCulture, out ulong value)
                ? value : throw new FormatException();
        private static void Keys(Dictionary<string, object> row, params string[] keys)
        {
            foreach (string key in row.Keys) Require(System.Array.IndexOf(keys, key) >= 0);
        }
        private static double Sample(object value)
        {
            double number = value is double d ? d : value is long s ? s : value is ulong u ? u : throw new FormatException();
            Require(!double.IsNaN(number) && !double.IsInfinity(number) && number >= -float.MaxValue && number <= float.MaxValue);
            return number;
        }
        private static ulong Hash(byte[] bytes)
        {
            ulong hash = 14695981039346656037UL;
            foreach (byte value in bytes) hash = unchecked((hash ^ value) * 1099511628211UL);
            return hash;
        }
    }
}
