using System;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace Harken
{
    internal static class PackPaths
    {
        internal static bool IsPlatformId(string id) => Guid.TryParseExact(id, "D", out Guid value) &&
            value != Guid.Empty && value.ToString("D") == id;

        internal static bool IsPlatformName(string name)
        {
            if (string.IsNullOrEmpty(name) || name != name.Trim() || name.EndsWith(".", StringComparison.Ordinal) ||
                Encoding.UTF8.GetByteCount(name) > 120 ||
                Regex.IsMatch(name, @"[\x00-\x1f\x7f-\x9f<>:""/\\|?*]") ||
                Regex.IsMatch(name, @"^(CON|PRN|AUX|NUL|CONIN\$|CONOUT\$|COM[1-9\u00b9\u00b2\u00b3]|LPT[1-9\u00b9\u00b2\u00b3])(?:\.|$)", RegexOptions.IgnoreCase))
                return false;
            for (int i = 0; i < name.Length; ++i)
                if (char.IsSurrogate(name[i]) &&
                    (!char.IsHighSurrogate(name[i]) || ++i >= name.Length || !char.IsLowSurrogate(name[i]))) return false;
            return true;
        }

        internal static bool TryJoinContainedPath(string root, string relativePath, out string resolved)
        {
            resolved = null;
            if (string.IsNullOrEmpty(root) || !IsSafeRelativePath(relativePath)) return false;
            string fullRoot = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) + Path.DirectorySeparatorChar;
            string destination = Path.GetFullPath(Path.Combine(fullRoot, relativePath));
            StringComparison comparison = Path.DirectorySeparatorChar == '\\' ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            if (!destination.StartsWith(fullRoot, comparison)) return false;
            resolved = destination;
            return true;
        }

        internal static bool IsSafeRelativePath(string value)
        {
            if (string.IsNullOrWhiteSpace(value) || Path.IsPathRooted(value)) return false;
            int start = 0;
            for (int i = 0; i <= value.Length; ++i)
            {
                if (i < value.Length && value[i] != '/')
                {
                    char c = value[i];
                    if (c < 32 || c == '\\' || c == ':' || c == '<' || c == '>' || c == '"' ||
                        c == '|' || c == '?' || c == '*') return false;
                    continue;
                }
                int length = i - start;
                if (length == 0 || (value[start] == '.' &&
                    (length == 1 || (length == 2 && value[start + 1] == '.')))) return false;
                start = i + 1;
            }
            return true;
        }

    }
}
