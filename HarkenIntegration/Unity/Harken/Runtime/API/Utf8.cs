using System;
using System.Text;

namespace Harken
{
    /// <summary>
    /// UTF-8 marshaling helper for the NAME-BASED convenience paths
    /// (pack, stream, and Current World file paths).
    /// These marshal a managed string and are therefore explicitly NOT the
    /// zero-GC hot path: resolve a name to an id once at load time and fire by
    /// id on the hot path (the header's own guidance, projected). The encoder
    /// itself performs no heap allocation — the caller supplies a stack buffer.
    /// </summary>
    internal static class Utf8
    {
        /// <summary>
        /// Encodes <paramref name="s"/> as NUL-terminated UTF-8 into
        /// <paramref name="dst"/>. Returns true and the byte count written
        /// (including the terminator) when it fits; false when
        /// <paramref name="dst"/> is too small (the caller falls back to a heap
        /// buffer). No managed-heap allocation.
        /// </summary>
        public static bool TryEncode(string s, Span<byte> dst, out int written)
        {
            written = 0;
            if (s == null)
            {
                return false;
            }
            int need = Encoding.UTF8.GetByteCount(s) + 1;
            if (need > dst.Length)
            {
                return false;
            }
            int n = Encoding.UTF8.GetBytes(s.AsSpan(), dst);
            dst[n] = 0;
            written = n + 1;
            return true;
        }
    }
}
