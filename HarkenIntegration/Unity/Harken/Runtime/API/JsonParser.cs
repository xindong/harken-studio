using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

namespace Harken
{
    internal sealed class JsonParser
    {
        private readonly string text;
        private readonly string rawRootNumberKey;
        private int cursor;
        internal sealed class NumberToken
        {
            internal readonly string Text;
            internal NumberToken(string text) { Text = text; }
        }
        public JsonParser(string text, string rawRootNumberKey = null)
        { this.text = text ?? string.Empty; this.rawRootNumberKey = rawRootNumberKey; }
        private void Space() { while (cursor < text.Length && " \r\n\t".IndexOf(text[cursor]) >= 0) ++cursor; }
        public bool End { get { Space(); return cursor == text.Length; } }
        private bool Take(char value)
        {
            Space();
            if (cursor >= text.Length || text[cursor] != value) return false;
            ++cursor;
            return true;
        }
        public object Read(int depth, bool rawNumber = false)
        {
            Space();
            if (depth > 64 || cursor >= text.Length) throw new FormatException();
            if (Take('{'))
            {
                var result = new Dictionary<string, object>(StringComparer.Ordinal);
                if (Take('}')) return result;
                do
                {
                    Space();
                    string key = String();
                    if (!Take(':')) throw new FormatException();
                    result.Add(key, Read(depth + 1, depth == 0 && key == rawRootNumberKey));
                } while (Take(','));
                if (!Take('}')) throw new FormatException();
                return result;
            }
            if (Take('['))
            {
                var result = new List<object>();
                if (Take(']')) return result;
                do { result.Add(Read(depth + 1)); } while (Take(','));
                if (!Take(']')) throw new FormatException();
                return result;
            }
            if (text[cursor] == '"') return String();
            int start = cursor;
            while (cursor < text.Length && ",]} \r\n\t".IndexOf(text[cursor]) < 0) ++cursor;
            string token = text.Substring(start, cursor - start);
            if (token == "null") return null;
            if (token == "true") return true;
            if (token == "false") return false;
            if (!Regex.IsMatch(token, @"\A-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?\z")) throw new FormatException();
            if (rawNumber) return new NumberToken(token);
            if (ulong.TryParse(token, NumberStyles.None, CultureInfo.InvariantCulture, out ulong unsigned)) return unsigned;
            if (long.TryParse(token, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out long signed)) return signed;
            if (!double.TryParse(token, NumberStyles.Float, CultureInfo.InvariantCulture, out double number)) throw new FormatException();
            return number;
        }
        private string String()
        {
            if (cursor >= text.Length || text[cursor++] != '"') throw new FormatException();
            var value = new StringBuilder();
            while (cursor < text.Length)
            {
                char c = text[cursor++];
                if (c == '"') return value.ToString();
                if (c < 32) throw new FormatException();
                if (c != '\\') { value.Append(c); continue; }
                if (cursor >= text.Length) throw new FormatException();
                c = text[cursor++];
                int escape = "\"\\/bfnrt".IndexOf(c);
                if (escape >= 0) { value.Append("\"\\/\b\f\n\r\t"[escape]); continue; }
                if (c != 'u' || cursor + 4 > text.Length ||
                    !ushort.TryParse(text.Substring(cursor, 4), NumberStyles.AllowHexSpecifier,
                        CultureInfo.InvariantCulture, out ushort code)) throw new FormatException();
                cursor += 4;
                value.Append((char)code);
            }
            throw new FormatException();
        }
    }
}
