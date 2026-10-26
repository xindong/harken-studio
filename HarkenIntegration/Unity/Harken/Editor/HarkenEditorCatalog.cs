using System;
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

        public VnResult RefreshPlatform(string root, string platformId, string platformName,
            string basePlatform, string language = null)
        {
            return Unavailable();
        }

        public VnResult Refresh(string directory, string language = null)
        {
            return Unavailable();
        }

        public VnResult Refresh(HarkenPackSource source, string language = null)
            => Refresh(source, language, null);

        public VnResult Refresh(HarkenPackSource source, string language, TextAsset companion)
        {
            return Unavailable();
        }

        public VnResult Refresh(string manifestPath, Func<string, byte[]> readBytes, string language = null)
        {
            return Unavailable();
        }

        private VnResult Unavailable()
        {
            ++Revision;
            Publication = null;
            PreparedPacks = null;
            DirectoryPath = null;
            LastResult = VnResult.VN_CAPABILITY_UNAVAILABLE;
            Diagnostic = HarkenEditorPublicationSupport.Diagnostic;
            WaveformResult = VnResult.VN_CAPABILITY_UNAVAILABLE;
            WaveformDiagnostic = HarkenEditorPublicationSupport.Diagnostic;
            return LastResult;
        }

        public VnResult CheckReference(string name, string projectId, out string diagnostic)
        {
            diagnostic = "Action name was not validated. " + Diagnostic;
            return VnResult.VN_CAPABILITY_UNAVAILABLE;
        }

        public VnResult SelectAction(HarkenAction action, string name, bool rebindProject, out string diagnostic)
        {
            return CheckReference(name, null, out diagnostic);
        }

        public VnResult GetPreparationOrder(string name, out ulong[] ids, out string diagnostic)
        {
            ids = Array.Empty<ulong>();
            return CheckReference(name, null, out diagnostic);
        }

        public VnResult GetWaveform(string name, out float[] samples, out double? durationSeconds, out string diagnostic)
        {
            samples = Array.Empty<float>();
            durationSeconds = null;
            return CheckReference(name, null, out diagnostic);
        }

        public VnResult GetActionPreview(string name, out HarkenActionPreview preview, out string diagnostic)
        {
            preview = null;
            return CheckReference(name, null, out diagnostic);
        }

        public VnResult GetActionWaveform(string name, out HarkenActionWaveform waveform, out string diagnostic)
        {
            waveform = null;
            return CheckReference(name, null, out diagnostic);
        }
    }
}
