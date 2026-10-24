using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using UnityEngine.Networking;

namespace Harken.Unity
{
    /// <summary>Reads an explicitly selected, immutable packaged platform directory, including Android jar URIs.</summary>
    public static class HarkenStreamingPackSource
    {
        public static PackSource Create(string platformRoot, string language = null)
        {
            if (string.IsNullOrWhiteSpace(platformRoot)) throw new ArgumentException("A selected platform URI is required.", nameof(platformRoot));
            if (!platformRoot.Contains("://")) return PackSource.FromDirectory(platformRoot, language);
            if (Uri.TryCreate(platformRoot, UriKind.Absolute, out Uri uri) && uri.IsFile)
                return PackSource.FromDirectory(uri.LocalPath, language);
            string root = platformRoot.TrimEnd('/', '\\');
            int thread = Environment.CurrentManagedThreadId;
            return PackSource.FromAsyncReader((path, token) => Read(root, path, thread, token), language);
        }

        private static async Task<byte[]> Read(string root, string path, int thread, CancellationToken cancellation)
        {
            if (Environment.CurrentManagedThreadId != thread) throw new InvalidOperationException("Packaged reads require their Unity thread.");
            if (!PackPaths.IsSafeRelativePath(path)) throw new ArgumentException("Invalid Pack path.");
            string url = root + "/" + string.Join("/", Array.ConvertAll(path.Split('/'), Uri.EscapeDataString));
            using (UnityWebRequest request = UnityWebRequest.Get(url))
            {
                var operation = request.SendWebRequest();
                while (!operation.isDone)
                {
                    if (cancellation.IsCancellationRequested) { request.Abort(); cancellation.ThrowIfCancellationRequested(); }
                    await Task.Yield();
                }
                cancellation.ThrowIfCancellationRequested();
                if (request.responseCode == 404) return null;
                if (request.result != UnityWebRequest.Result.Success) throw new IOException("Pack read failed: " + request.error);
                return request.downloadHandler.data;
            }
        }
    }
}
