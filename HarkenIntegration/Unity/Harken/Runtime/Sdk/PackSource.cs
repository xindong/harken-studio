using System;
using System.Threading;
using System.Threading.Tasks;

namespace Harken
{
    /// <summary>
    /// One explicitly selected platform source. Paths use '/' and are relative to that source.
    /// A reader returns null only for a missing file and throws for a read failure. Readers without
    /// a revision callback must expose an immutable publication for the lifetime of the source.
    /// Mutable sources must advance their revision before making changed bytes visible, and never
    /// reuse a revision. A publication in progress must throw from the revision callback.
    /// </summary>
    public sealed class PackSource
    {
        private readonly Func<string, byte[]> reader;
        private readonly Func<string, CancellationToken, Task<byte[]>> asyncReader;
        private readonly Func<ulong> revision;
        private readonly PackDirectorySource directory;
        private readonly Func<string, VnObjectId, byte[], CompanionCapture> snapshot;
        private readonly Func<CompanionInput, VnResult> snapshotValidation;
        public string Language { get; }
        public bool IsAsync => asyncReader != null;
        internal string Root => directory?.Root ?? string.Empty;

        private PackSource(string language, Func<string, byte[]> reader = null,
            Func<string, CancellationToken, Task<byte[]>> asyncReader = null,
            Func<ulong> revision = null, PackDirectorySource directory = null,
            Func<string, VnObjectId, byte[], CompanionCapture> snapshot = null,
            Func<CompanionInput, VnResult> snapshotValidation = null)
        {
            if (!PackDirectorySource.ValidLanguage(language)) throw new ArgumentException("Invalid Pack language.", nameof(language));
            Language = language ?? string.Empty;
            this.reader = reader;
            this.asyncReader = asyncReader;
            this.revision = revision;
            this.directory = directory;
            this.snapshot = snapshot;
            this.snapshotValidation = snapshotValidation;
        }

        internal static PackSource FromSnapshot(string language,
            Func<string, VnObjectId, byte[], CompanionCapture> capture, Func<CompanionInput, VnResult> validate)
            => new PackSource(language, snapshot: capture, snapshotValidation: validate);

        public static PackSource FromDirectory(string platformDirectory, string language = null)
            => new PackSource(language, directory: new PackDirectorySource(platformDirectory, language));

        public static PackSource FromReader(Func<string, byte[]> reader, string language = null,
            Func<ulong> revision = null)
            => new PackSource(language, reader: reader ?? throw new ArgumentNullException(nameof(reader)), revision: revision);

        /// <summary>The reader is invoked on the caller's control thread, including after awaits.</summary>
        public static PackSource FromAsyncReader(Func<string, CancellationToken, Task<byte[]>> reader,
            string language = null, Func<ulong> revision = null)
            => new PackSource(language, asyncReader: reader ?? throw new ArgumentNullException(nameof(reader)), revision: revision);

        internal static bool HasControlContext => SynchronizationContext.Current != null &&
            SynchronizationContext.Current.GetType() != typeof(SynchronizationContext);

        internal VnResult Capture(string name, VnObjectId project, byte[] init, out CompanionInput input)
        {
            input = null;
            if (IsAsync) return VnResult.VN_CAPABILITY_UNAVAILABLE;
            if (snapshot != null)
            {
                CompanionCapture frozen = snapshot(name, project, init);
                input = frozen.Input;
                return frozen.Result;
            }
            if (directory != null) return directory.Capture(name, project, init, out input);
            // The synchronous reader completes without scheduling a continuation.
            var captured = CaptureAsync(name, project, init, CancellationToken.None).GetAwaiter().GetResult();
            input = captured.Input;
            return captured.Result;
        }

        internal VnResult Validate(CompanionInput input)
        {
            if (snapshotValidation != null) return snapshotValidation(input);
            try
            {
                return revision == null || revision() == input.SourceRevision
                    ? VnResult.VN_OK : VnResult.VN_PACK_STALE_REVISION;
            }
            catch { return VnResult.VN_PACK_PROVIDER_FAILED; }
        }

        private ulong ReadRevision(Action<bool> readPhase)
        {
            readPhase?.Invoke(true);
            try { return revision?.Invoke() ?? 0; }
            finally { readPhase?.Invoke(false); }
        }

        private async Task<byte[]> Read(string path, CancellationToken cancellation, Action<bool> readPhase)
        {
            cancellation.ThrowIfCancellationRequested();
            Task<byte[]> pending = null;
            byte[] bytes = null;
            readPhase?.Invoke(true);
            try
            {
                if (asyncReader == null) bytes = reader(path);
                else pending = asyncReader(path, cancellation);
            }
            finally { readPhase?.Invoke(false); }
            if (asyncReader != null) bytes = await AwaitRead(pending, cancellation);
            cancellation.ThrowIfCancellationRequested();
            return bytes == null ? null : (byte[])bytes.Clone();
        }

        private static async Task<byte[]> AwaitRead(Task<byte[]> pending, CancellationToken cancellation)
        {
            if (pending.IsCompleted || !cancellation.CanBeCanceled) return await pending;
            var cancelled = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
            using (cancellation.Register(() => cancelled.TrySetResult(true)))
            {
                await Task.WhenAny(pending, cancelled.Task);
                if (cancellation.IsCancellationRequested)
                {
                    _ = ObserveLateRead(pending);
                    cancellation.ThrowIfCancellationRequested();
                }
                return await pending;
            }
        }

        private static async Task ObserveLateRead(Task<byte[]> pending)
        {
            // A provider may ignore cancellation or fail after the SDK request has ended.
            try { await pending.ConfigureAwait(false); } catch { }
        }

        internal async Task<CompanionCapture> CaptureAsync(string name, VnObjectId project,
            byte[] init, CancellationToken cancellation, Action<bool> readPhase = null)
        {
            if (name != null && (!PackCompanion.IsName(name) || name == "Init"))
                return new CompanionCapture(VnResult.VN_INVALID_ARG);
            try
            {
                cancellation.ThrowIfCancellationRequested();
                if (snapshot != null) return snapshot(name, project, init);
                if (directory != null)
                {
                    VnResult result = directory.Capture(name, project, init, out CompanionInput file);
                    return new CompanionCapture(result, file);
                }
                ulong version = ReadRevision(readPhase);
                string scope = name == null ? string.Empty : Language;
                string basename = name ?? "Init";
                string prefix = scope.Length == 0 ? string.Empty : scope + "/";
                byte[] bytes = await Read(prefix + basename + ".bytes", cancellation, readPhase);
                byte[] json = await Read(prefix + basename + ".json", cancellation, readPhase);
                if (bytes == null && json == null && scope.Length != 0)
                {
                    scope = string.Empty;
                    bytes = await Read(basename + ".bytes", cancellation, readPhase);
                    json = await Read(basename + ".json", cancellation, readPhase);
                }
                if (revision != null && ReadRevision(readPhase) != version)
                    return new CompanionCapture(VnResult.VN_PACK_STALE_REVISION);
                if (bytes == null && json == null) return new CompanionCapture(VnResult.VN_CONTENT_NOT_LOADED);
                if (bytes == null || json == null) return new CompanionCapture(VnResult.VN_BAD_PACK);
                VnResult parsed = PackCompanion.Parse(json, bytes, name, scope, project, out PackCompanion names);
                if (parsed != VnResult.VN_OK) return new CompanionCapture(parsed);
                parsed = CompanionNative.Inspect(bytes, init, names, out VnPackInspectReportV1 facts);
                if (parsed != VnResult.VN_OK) return new CompanionCapture(parsed);
                names.ProjectId = facts.projectId;
                return new CompanionCapture(VnResult.VN_OK, new CompanionInput
                {
                    Bytes = bytes, Json = json, Names = names, Facts = facts, SourceRevision = version
                });
            }
            catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { throw; }
            catch (OutOfMemoryException) { return new CompanionCapture(VnResult.VN_DEVICE_ERROR); }
            catch { return new CompanionCapture(VnResult.VN_PACK_PROVIDER_FAILED); }
        }
    }

    internal readonly struct CompanionCapture
    {
        internal readonly VnResult Result;
        internal readonly CompanionInput Input;
        internal CompanionCapture(VnResult result, CompanionInput input = null) { Result = result; Input = input; }
    }
}
