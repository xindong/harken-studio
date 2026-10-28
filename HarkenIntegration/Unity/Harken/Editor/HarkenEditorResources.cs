using System;
using System.Collections.Generic;
using System.IO;
using UnityEngine;

namespace Harken.Unity.Editor
{
    /// <summary>Optional presentation resources, bounded separately from executable Pack paths.</summary>
    public sealed class HarkenEditorResources
    {
        private readonly Func<string, byte[]> reader;
        private readonly string root;
        internal string PlatformPath { get; }
        private HarkenEditorResources(string platformPath, Func<string, byte[]> reader, string root = null)
        { PlatformPath = Normalize(platformPath); this.reader = reader; this.root = root; }

        public static HarkenEditorResources FromDirectory(string resourceRoot, string platformDirectory)
        {
            string root = Path.GetFullPath(resourceRoot);
            string platform = Path.GetFullPath(platformDirectory);
            string relative = Path.GetRelativePath(root, platform).Replace('\\', '/');
            return new HarkenEditorResources(relative, null, root);
        }
        /// <summary>Paths delivered to the reader are normalized relative to its declared resource root.</summary>
        public static HarkenEditorResources FromReader(string platformPath, Func<string, byte[]> reader)
            => new HarkenEditorResources(platformPath, reader ?? throw new ArgumentNullException(nameof(reader)));

        public static HarkenEditorResources FromAssets(string platformPath, IEnumerable<HarkenPackSource.ContentAsset> assets)
        {
            if (assets == null) throw new ArgumentNullException(nameof(assets));
            var captured = new Dictionary<string, TextAsset>(StringComparer.Ordinal);
            foreach (var row in assets) captured.Add(Normalize(row.relativePath), row.asset);
            int thread = Environment.CurrentManagedThreadId;
            return FromReader(platformPath, path =>
            {
                if (thread != Environment.CurrentManagedThreadId) throw new InvalidOperationException("TextAssets require their Unity thread.");
                if (!captured.TryGetValue(path, out TextAsset asset)) return null;
                if (asset == null) throw new IOException("A referenced TextAsset is no longer available.");
                return asset.bytes;
            });
        }

        internal string Resolve(string jsonPath, string reference)
        {
            if (string.IsNullOrWhiteSpace(reference) || reference.StartsWith("/", StringComparison.Ordinal) ||
                reference.IndexOf('\\') >= 0 || reference.IndexOf(':') >= 0) throw new IOException("Invalid waveform reference.");
            string directory = Path.GetDirectoryName(jsonPath)?.Replace('\\', '/') ?? "";
            return Normalize(PlatformPath + "/" + directory + "/" + reference);
        }
        internal byte[] Read(string path, int limit)
        {
            path = Normalize(path);
            if (root == null)
            {
                byte[] bytes = reader(path);
                if (bytes != null && bytes.Length > limit) throw new IOException("Waveform exceeds its byte budget.");
                return bytes;
            }
            string full = FilePath(root, path);
            try
            {
                RejectRedirects(root, path);
                using (var stream = new FileStream(full, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                {
                    if (stream.Length > limit) throw new IOException("Waveform exceeds its byte budget.");
                    var bytes = new byte[checked((int)stream.Length)];
                    int offset = 0;
                    while (offset < bytes.Length)
                    {
                        int count = stream.Read(bytes, offset, bytes.Length - offset);
                        if (count == 0) throw new EndOfStreamException();
                        offset += count;
                    }
                    if (stream.Length != bytes.Length) throw new IOException("Waveform changed during capture.");
                    return bytes;
                }
            }
            catch (FileNotFoundException) { return null; }
            catch (DirectoryNotFoundException) { return null; }
        }
        internal static string Normalize(string path)
        {
            if (path == null || path.StartsWith("/", StringComparison.Ordinal) || path.IndexOf('\\') >= 0 ||
                path.IndexOf(':') >= 0 || path.IndexOf('\0') >= 0) throw new IOException("Invalid resource path.");
            var parts = new List<string>();
            foreach (string part in path.Split('/'))
            {
                if (part.Length == 0 || part == ".") continue;
                if (part == "..")
                {
                    if (parts.Count == 0) throw new IOException("Resource escapes its declared root.");
                    parts.RemoveAt(parts.Count - 1);
                }
                else
                {
                    if (part.TrimEnd(' ', '.') != part || part.IndexOfAny(new[] { '*', '?', '"', '<', '>', '|' }) >= 0)
                        throw new IOException("Invalid resource path segment.");
                    parts.Add(part);
                }
            }
            return string.Join("/", parts);
        }
        internal static string FilePath(string root, string relative) => Path.Combine(root, Normalize(relative).Replace('/', Path.DirectorySeparatorChar));
        internal static void RejectExistingRedirects(string fullPath)
        {
            string full = Path.GetFullPath(fullPath);
            string cursor = Path.GetPathRoot(full);
            foreach (string part in full.Substring(cursor.Length).Split(Path.DirectorySeparatorChar))
            {
                cursor = Path.Combine(cursor, part);
                if (!Directory.Exists(cursor) && !File.Exists(cursor)) break;
                if ((File.GetAttributes(cursor) & FileAttributes.ReparsePoint) != 0)
                    throw new IOException("Resource path redirects.");
            }
        }
        internal static void RejectRedirects(string root, string relative)
        {
            RejectExistingRedirects(root);
            string path = root;
            foreach (string part in Normalize(relative).Split('/'))
            {
                path = Path.Combine(path, part);
                if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) throw new IOException("Resource path redirects.");
            }
        }
    }
}
