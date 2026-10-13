using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json.Nodes;
using Harken;
using Harken.Unity.Editor;

namespace Harken.Test
{
    internal static class EditorWaveformContract
    {
        internal static bool Run()
        {
            try
            {
                string root = Path.Combine(AppContext.BaseDirectory, "EditorMetadata");
                byte[] manifest = File.ReadAllBytes(Path.Combine(root, "golden", "HarkenPackManifest.json"));
                byte[] companion = File.ReadAllBytes(Path.Combine(root, "golden", HarkenWaveformPublication.FileName));
                Check(GameplayPublication.Parse(Encoding.UTF8.GetString(manifest), null, null,
                    out GameplayPublication publication, out string error) == VnResult.VN_OK, error);
                Check(HarkenWaveformPublication.Parse(companion, manifest, publication, out var parsed, out error) == VnResult.VN_OK, error);
                Check(parsed.TryFindAction("Play_Golden", out var waveform), "Missing golden Action");
                Check(waveform.OneShotDurationKnown && waveform.Source.FrameCount == 3 && waveform.Source.SampleRate == 48000,
                    "Exact frame and duration identity");
                Check(waveform.OneShotDurationSeconds == 3.0 / 48000, "Nominal duration");
                Check(waveform.Source.GetMergedMinMax().SequenceEqual(new float[] { 0, 1, -0.25f, 0.5f, -1, 0 }),
                    "Channel extrema projection must not average");
                float[] borrowed = waveform.Source.GetChannelMinima(0);
                borrowed[0] = 99;
                Check(waveform.Source.GetChannelMinima(0)[0] == 0, "Source arrays must be immutable");
                var valid = JsonNode.Parse(companion).AsObject();
                int count = 0;
                foreach (var test in JsonNode.Parse(File.ReadAllBytes(Path.Combine(root, "negative-cases.json"))).AsArray())
                {
                    var candidate = valid.DeepClone();
                    Set(candidate, test["path"].GetValue<string>(), test["value"].DeepClone());
                    var expected = Enum.Parse<VnResult>(test["result"].GetValue<string>());
                    Check(HarkenWaveformPublication.Parse(Encoding.UTF8.GetBytes(candidate.ToJsonString()), manifest, publication,
                        out parsed, out error) == expected && parsed == null, test["name"].GetValue<string>() + ": " + error);
                    ++count;
                }
                var delayed = valid.DeepClone();
                delayed["actions"][0]["oneShotDurationKnown"] = false;
                delayed["actions"][0]["durationReason"] = "authored-timing";
                Check(HarkenWaveformPublication.Parse(Encoding.UTF8.GetBytes(delayed.ToJsonString()), manifest, publication,
                    out parsed, out error) == VnResult.VN_OK, error);
                Check(parsed.TryFindAction("Play_Golden", out waveform) && waveform.Source != null &&
                    waveform.OneShotDurationSeconds == null && waveform.Source.SourceDurationSeconds == 3.0 / 48000,
                    "Unknown Action duration preserves source envelope");
                var duplicate = valid.DeepClone();
                duplicate["actions"].AsArray().Add(duplicate["actions"][0].DeepClone());
                Check(HarkenWaveformPublication.Parse(Encoding.UTF8.GetBytes(duplicate.ToJsonString()), manifest, publication,
                    out _, out _) != VnResult.VN_OK, "Duplicate Action row");
                foreach (string literal in new[] { "NaN", "Infinity", "1e400" })
                {
                    string invalid = valid.ToJsonString().Replace("\"min\":[0,", "\"min\":[" + literal + ",");
                    Check(HarkenWaveformPublication.Parse(Encoding.UTF8.GetBytes(invalid), manifest, publication,
                        out _, out _) == VnResult.VN_BAD_PACK, "Nonfinite " + literal);
                }
                Check(HarkenWaveformPublication.Parse(null, manifest, publication, out parsed, out _) == VnResult.VN_CAPABILITY_UNAVAILABLE && parsed == null,
                    "Missing companion");
                Check(HarkenWaveformPublication.Parse(new byte[HarkenWaveformPublication.MaxBytes + 1], manifest, publication,
                    out parsed, out _) == VnResult.VN_BAD_PACK && parsed == null, "Companion byte budget");
                Check(HarkenWaveformPublication.Parse(companion, manifest.Concat(new byte[] { 10 }).ToArray(), publication,
                    out parsed, out _) == VnResult.VN_PACK_STALE_REVISION && parsed == null, "Raw manifest bytes bind whitespace");
                Console.WriteLine("[Editor waveform] native golden, " + count + " shared negatives, channel projection and duration separation PASS");
                RunVersionTwo();
                RunGeneratedBudgetFixture();
                return true;
            }
            catch (Exception error) { Console.WriteLine("[Editor waveform] FAIL: " + error); return false; }
        }

        private static void RunGeneratedBudgetFixture()
        {
            string root = Environment.GetEnvironmentVariable("HARKEN_EDITOR_WAVEFORM_BUDGET_FIXTURE");
            if (string.IsNullOrEmpty(root)) return;
            byte[] manifest = File.ReadAllBytes(Path.Combine(root, "HarkenPackManifest.json"));
            byte[] bytes = File.ReadAllBytes(Path.Combine(root, HarkenWaveformPublication.FileName));
            Check(GameplayPublication.Parse(Encoding.UTF8.GetString(manifest), null, null,
                out var publication, out string error) == VnResult.VN_OK, error);
            Check(HarkenWaveformPublication.Parse(bytes, manifest, publication, out var parsed,
                out error) == VnResult.VN_OK, "Native decoded-budget publication: " + error);
            Check(parsed.TryFindPreview("Play_Budget_254", out var refused) &&
                refused.Kind == HarkenActionPreviewKind.Unavailable && refused.Candidates.Count == 0 &&
                refused.SourceUnavailableReason == "metadata-budget", "Atomic whole-Action budget refusal");
            Check(parsed.TryFindPreview("Play_Budget_255", out var after) &&
                after.Kind == HarkenActionPreviewKind.DirectSound, "Failed Action must release pending decoded budget");
            Check(parsed.TryFindPreview("Play_Golden", out var first) &&
                parsed.TryFindPreview("Play_Budget_256", out var shared) &&
                object.ReferenceEquals(first.Candidates[0].Source, shared.Candidates[0].Source),
                "Existing source must not consume decoded budget twice");
            Console.WriteLine("[Editor waveform] native-produced decoded-budget boundary fixture PASS");
        }

        private static void RunVersionTwo()
        {
            string root = Path.Combine(AppContext.BaseDirectory, "EditorMetadataV2");
            foreach (string name in new[] { "golden", "random" })
            {
                byte[] manifest = File.ReadAllBytes(Path.Combine(root, name, "HarkenPackManifest.json"));
                byte[] bytes = File.ReadAllBytes(Path.Combine(root, name, HarkenWaveformPublication.FileName));
                Check(GameplayPublication.Parse(Encoding.UTF8.GetString(manifest), null, null, out var publication, out string error) == VnResult.VN_OK, error);
                Check(HarkenWaveformPublication.Parse(bytes, manifest, publication, out var parsed, out error) == VnResult.VN_OK, error);
                Check(parsed.TryFindPreview("Play_Golden", out var preview), "Missing v2 preview");
                Check(parsed.TryFindAction("Play_Golden", out var legacy), "Missing v2 legacy projection");
                if (name == "golden")
                {
                    Check(preview.Kind == HarkenActionPreviewKind.DirectSound && legacy.Source != null && legacy.OneShotDurationKnown, "Direct v2 projection");
                    Check(preview.Overview.MinMax.SequenceEqual(legacy.Source.GetMergedMinMax()), "Direct overview source identity");
                    continue;
                }
                Check(preview.Kind == HarkenActionPreviewKind.RandomCandidates && preview.Candidates.Count == 3, "Complete Random candidates");
                Check(object.ReferenceEquals(preview.Candidates[0].Source, preview.Candidates[2].Source), "Repeated occurrence must share source");
                Check(legacy.Source == null && legacy.OneShotDurationSeconds == null, "Random must not masquerade as legacy Sound");
                Check(preview.DurationKind == HarkenActionDurationKind.Bounded && preview.MinimumDurationSeconds == 4.0 / 48000 &&
                    preview.MaximumDurationSeconds == 8.0 / 24000, "Independent duration bounds");
                Check(object.ReferenceEquals(preview.Overview, preview.Overview), "Overview must be cached");
                var overview = preview.Overview;
                Check(overview.DurationSeconds == 8.0 / 24000 && overview.BucketCount == 8, "Absolute source-time alignment");
                Check(overview.MinMax.SequenceEqual(new float[] { -0.5f, 0.5f, -1, 1, -0.25f, 0, -0.25f, 0, 0, 0, 0, 0, 0, 0.75f, -0.75f, 0 }),
                    "Mixed rates, zero-padded short candidates and tail extrema: " + string.Join(",", overview.MinMax));
                var document = JsonNode.Parse(bytes);
                void Reject(Action<JsonNode> mutate, string label)
                {
                    var candidate = document.DeepClone(); mutate(candidate);
                    Check(HarkenWaveformPublication.Parse(Encoding.UTF8.GetBytes(candidate.ToJsonString()), manifest, publication,
                        out var rejected, out _) != VnResult.VN_OK && rejected == null, label);
                }
                Reject(d => d["sources"].AsArray().Add(d["sources"][0].DeepClone()), "Duplicate source identity");
                Reject(d => d["actions"][0]["candidates"][1]["occurrence"] = d["actions"][0]["candidates"][0]["occurrence"].GetValue<string>(), "Duplicate occurrence");
                Reject(d => d["actions"][0]["candidates"][0]["sourceKey"] = "missing", "Dangling source reference");
                Reject(d => d["sources"][0]["contentHash"] = "1", "Wrong shared-media Pack hash");
                Reject(d => d["actions"][0]["duration"]["minSeconds"] = -1, "Negative duration");
                Reject(d => d["actions"][0]["duration"]["kind"] = "exact", "Inexact bounds labelled exact");
                Reject(d => d["sources"][0]["channels"][0]["min"][0] = 100, "Inverted envelope");
                Reject(d => d["sources"][0]["channels"][0]["min"].AsArray().RemoveAt(0), "Wrong envelope length");
                Reject(d => { for (int i = 0; i < 254; ++i) d["actions"][0]["candidates"].AsArray().Add(
                    new JsonObject { ["occurrence"] = "extra:" + i, ["sourceKey"] = d["sources"][0]["key"].GetValue<string>() }); }, "Occurrence budget");
                var unknown = document.DeepClone();
                unknown["actions"][0]["duration"] = new JsonObject { ["kind"] = "unknown", ["reason"] = "authored-pitch-or-loop" };
                Check(HarkenWaveformPublication.Parse(Encoding.UTF8.GetBytes(unknown.ToJsonString()), manifest, publication, out var next, out error) == VnResult.VN_OK, error);
                Check(next.TryFindPreview("Play_Golden", out var uncertain) && uncertain.DurationKind == HarkenActionDurationKind.Unknown &&
                    uncertain.MaximumDurationSeconds == null && uncertain.Overview != null, "Unknown duration retains source overview");
                Check(!object.ReferenceEquals(uncertain.Overview, overview), "Publication replacement cannot share overview cache");
                var future = document.DeepClone(); future["schemaVersion"] = 99;
                Check(HarkenWaveformPublication.Parse(Encoding.UTF8.GetBytes(future.ToJsonString()), manifest, publication, out next, out error) ==
                    VnResult.VN_CAPABILITY_UNAVAILABLE && next == null && error.Contains("99"), "Future metadata version diagnosis");
            }
            Console.WriteLine("[Editor preview v2] native direct/shared Random fixtures, occurrences, timing, extrema, compatibility and refusal PASS");
        }

        private static void Set(JsonNode root, string path, JsonNode value)
        {
            string[] segments = path.Split('/');
            for (int i = 0; i < segments.Length - 1; ++i)
                root = root is JsonArray ? root[int.Parse(segments[i])] : root[segments[i]];
            if (root is JsonArray array) array[int.Parse(segments[segments.Length - 1])] = value;
            else root[segments[segments.Length - 1]] = value;
        }
        private static void Check(bool condition, string error) { if (!condition) throw new InvalidOperationException(error); }
    }
}
