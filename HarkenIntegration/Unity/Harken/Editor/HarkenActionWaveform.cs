using System;
using System.Collections.Generic;
using System.Globalization;
using System.Numerics;

namespace Harken.Unity.Editor
{
    public sealed class HarkenSourceEnvelope
    {
        private readonly float[][] minima;
        private readonly float[][] maxima;
        private readonly ulong[] boundaries;

        internal HarkenSourceEnvelope(VnObjectId soundId, ulong fingerprint, ulong revision,
            ulong frames, uint rate, float[][] minima, float[][] maxima, ulong[] boundaries = null)
        {
            SoundStableId = soundId;
            PropertyFingerprint = fingerprint;
            ContentRevision = revision;
            FrameCount = frames;
            SampleRate = rate;
            this.minima = minima;
            this.maxima = maxima;
            this.boundaries = boundaries;
        }

        public VnObjectId SoundStableId { get; }
        public ulong PropertyFingerprint { get; }
        public ulong ContentRevision { get; }
        public ulong FrameCount { get; }
        public uint SampleRate { get; }
        public int ChannelCount => minima.Length;
        public int BucketCount => minima[0].Length;
        public double SourceDurationSeconds => (double)FrameCount / SampleRate;
        public ulong GetBucketFrameBoundary(int boundary)
        {
            if (boundary < 0 || boundary > BucketCount) throw new ArgumentOutOfRangeException(nameof(boundary));
            return boundaries != null ? boundaries[boundary] : FrameCount / (ulong)BucketCount * (ulong)boundary +
                FrameCount % (ulong)BucketCount * (ulong)boundary / (ulong)BucketCount;
        }
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
        internal VnPackOccurrenceFactV1 NativeOccurrence { get; set; }
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
        private static ulong Boundary(HarkenSourceEnvelope source, int bucket) => source.GetBucketFrameBoundary(bucket);
        private static void Merge(float[] values, int b, float lo, float hi)
        { values[b * 2] = Math.Min(values[b * 2], lo); values[b * 2 + 1] = Math.Max(values[b * 2 + 1], hi); }
    }

    public sealed class HarkenActionWaveform
    {
        internal HarkenActionWaveform(GameplayAction action, HarkenSourceEnvelope source,
            bool oneShotKnown, string sourceReason, string durationReason, double? executionDuration = null)
        {
            Action = action;
            Source = source;
            OneShotDurationKnown = oneShotKnown;
            SourceUnavailableReason = sourceReason;
            DurationUnavailableReason = durationReason;
            OneShotDurationSeconds = executionDuration;
        }

        public GameplayAction Action { get; }
        public HarkenSourceEnvelope Source { get; }
        public bool OneShotDurationKnown { get; }
        public double? OneShotDurationSeconds { get; }
        public string SourceUnavailableReason { get; }
        public string DurationUnavailableReason { get; }
    }

    public sealed class HarkenWaveformPublication
    {
        private HarkenWaveformPublication() { }

        public bool TryFindPreview(string name, out HarkenActionPreview preview)
        {
            preview = null;
            return false;
        }

        public bool TryFindAction(string name, out HarkenActionWaveform waveform)
        {
            waveform = null;
            return false;
        }

        public static VnResult Parse(byte[] companionBytes, byte[] manifestBytes, GameplayPublication publication,
            out HarkenWaveformPublication waveforms, out string diagnostic)
        {
            waveforms = null;
            diagnostic = "Legacy Editor waveform publications are unsupported. Editor publication tools remain unavailable.";
            return VnResult.VN_CAPABILITY_UNAVAILABLE;
        }
    }
}
