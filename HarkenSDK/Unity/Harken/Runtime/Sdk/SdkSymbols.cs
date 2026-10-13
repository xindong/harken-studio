using System;
using System.Collections.Generic;

namespace Harken
{
    internal static class SdkSymbols
    {
        private static readonly HashSet<string> Reserved = new HashSet<string>((
            "abstract as base bool break byte case catch char checked class const continue " +
            "decimal default delegate do double else enum event explicit extern false finally " +
            "fixed float for foreach goto if implicit in int interface internal is lock long " +
            "namespace new null object operator out override params private protected public " +
            "readonly ref return sbyte sealed short sizeof stackalloc static string struct " +
            "switch this throw true try typeof uint ulong unchecked unsafe ushort using " +
            "virtual void volatile while alignas alignof and and_eq asm auto bitand bitor " +
            "char8_t char16_t char32_t compl concept consteval constexpr constinit const_cast " +
            "co_await co_return co_yield decltype delete dynamic_cast export friend inline " +
            "mutable noexcept not not_eq nullptr or or_eq register reinterpret_cast requires " +
            "signed static_assert static_cast template thread_local typedef typeid typename " +
            "union unsigned wchar_t xor xor_eq HarkenActions HarkenVariables " +
            "ExecutionCatalogFingerprint CatalogFingerprint").Split(' '), StringComparer.Ordinal);

        internal static string Name(string name, string prefix)
            => Reserved.Contains(name) || name.StartsWith(prefix, StringComparison.Ordinal) ||
                (prefix == "V_" && name.StartsWith("Label_", StringComparison.Ordinal)) ? prefix + name : name;
    }
}
