using System;
using System.Collections.Generic;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;

namespace Harken
{
    internal sealed class PackCompanion
    {
        internal const uint SupportedPackMetadataVersion = 4;

        internal readonly struct Variable
        {
            internal readonly VnObjectId Id;
            internal readonly string Name;
            internal Variable(VnObjectId id, string name) { Id = id; Name = name; }
        }

        internal readonly struct Label
        {
            internal readonly VnObjectId VariableId, Id;
            internal readonly string Name;
            internal Label(VnObjectId variable, VnObjectId id, string name)
            { VariableId = variable; Id = id; Name = name; }
        }

        internal VnObjectId ProjectId;
        internal VnObjectId RootId;
        internal string Name = string.Empty;
        internal string Language = string.Empty;
        internal Variable[] Variables = Array.Empty<Variable>();
        internal Label[] Labels = Array.Empty<Label>();

        internal static bool IsName(string value)
        {
            if (string.IsNullOrEmpty(value) || value.Length > 128 || !Letter(value[0])) return false;
            foreach (char c in value)
                if (!Letter(c) && (c < '0' || c > '9') && c != '_') return false;
            return true;
        }

        private static bool Letter(char c) => c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z';

        internal static VnResult Parse(byte[] json, byte[] pack, string name, string language,
            VnObjectId project, out PackCompanion companion)
        {
            companion = null;
            if (json == null || json.Length == 0 || pack == null || pack.Length < 8 ||
                pack[0] != 'V' || pack[1] != 'S' || pack[2] != 'P' || pack[3] != 'K')
                return VnResult.VN_BAD_PACK;
            try
            {
                var parser = new JsonParser(new UTF8Encoding(false, true).GetString(json), "packMetadataVersion");
                if (!(parser.Read(0) is Dictionary<string, object> root) || !parser.End ||
                    !root.TryGetValue("packMetadataVersion", out object version) ||
                    !(version is JsonParser.NumberToken token) || !MatchesVersion(token.Text, SupportedPackMetadataVersion))
                    return VnResult.VN_BAD_PACK;
                var candidate = new PackCompanion { ProjectId = project, Language = language ?? string.Empty };
                if (name != null)
                {
                    if (!IsName(name) || Text(root, "name") != name || !Id(root, out candidate.RootId))
                        return VnResult.VN_BAD_PACK;
                    candidate.Name = name;
                }
                else
                {
                    if (!root.TryGetValue("variables", out object rows) || !(rows is List<object> variables))
                        return VnResult.VN_BAD_PACK;
                    candidate.Variables = new Variable[variables.Count];
                    var labels = new List<Label>();
                    for (int i = 0; i < variables.Count; ++i)
                    {
                        if (!(variables[i] is Dictionary<string, object> variable) ||
                            !IsName(Text(variable, "name")) || !Id(variable, out VnObjectId id) ||
                            !variable.TryGetValue("labels", out object entries) || !(entries is List<object> namedLabels))
                            return VnResult.VN_BAD_PACK;
                        candidate.Variables[i] = new Variable(id, Text(variable, "name"));
                        foreach (object entry in namedLabels)
                        {
                            if (!(entry is Dictionary<string, object> label) || !Id(label, out VnObjectId labelId) ||
                                string.IsNullOrEmpty(Text(label, "name")) || Text(label, "name").IndexOf('\0') >= 0)
                                return VnResult.VN_BAD_PACK;
                            labels.Add(new Label(id, labelId, Text(label, "name")));
                        }
                    }
                    candidate.Labels = labels.ToArray();
                }
                // Optional display metadata has no bearing on names or playback admission.
                companion = candidate;
                return VnResult.VN_OK;
            }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            catch (ArgumentException) { return VnResult.VN_BAD_PACK; }
            catch (FormatException) { return VnResult.VN_BAD_PACK; }
        }

        private static bool MatchesVersion(string token, uint expectedVersion)
        {
            // Compare decimal digits and scale without floating-point rounding.
            if (token[0] == '-') return false;
            int exponentAt = token.IndexOfAny(new[] { 'e', 'E' });
            string coefficient = exponentAt < 0 ? token : token.Substring(0, exponentAt);
            long exponent = 0;
            if (exponentAt >= 0 && !long.TryParse(token.Substring(exponentAt + 1),
                    NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out exponent)) return false;
            int dot = coefficient.IndexOf('.');
            int fraction = dot < 0 ? 0 : coefficient.Length - dot - 1;
            string digits = coefficient.Replace(".", "").TrimStart('0');
            string significant = digits.TrimEnd('0');
            string expected = expectedVersion.ToString(CultureInfo.InvariantCulture);
            string expectedSignificant = expected.TrimEnd('0');
            return significant == expectedSignificant && exponent ==
                (long)fraction - (digits.Length - significant.Length) + (expected.Length - expectedSignificant.Length);
        }

        private static string Text(Dictionary<string, object> row, string key)
            => row.TryGetValue(key, out object value) ? value as string : null;

        private static bool Id(Dictionary<string, object> row, out VnObjectId id)
        {
            id = default;
            string text = Text(row, "guid");
            if (!Guid.TryParseExact(text, "D", out Guid guid) || guid == Guid.Empty ||
                text != guid.ToString("D")) return false;
            id = VnObjectId.FromGuid(guid);
            return true;
        }
    }

    internal readonly struct CompanionMember
    {
        internal readonly ulong Id;
        internal readonly uint Kind, Operation;
        internal readonly Pack Expected;
        internal readonly byte[] Bytes;
        internal readonly PackCompanion Names;
        internal CompanionMember(ulong id, uint kind, Pack expected, uint operation = VnConstants.VN_PACK_MEMBER_KEEP,
            byte[] bytes = null, PackCompanion names = null)
        { Id = id; Kind = kind; Expected = expected; Operation = operation; Bytes = bytes; Names = names; }
    }

    internal static class CompanionNative
    {
        internal static unsafe VnResult Inspect(byte[] bytes, byte[] init, PackCompanion names,
            out VnPackInspectReportV1 report)
            => Inspect(bytes, init, names, out report, out _, out _, false);

        internal static unsafe VnResult Inspect(byte[] bytes, byte[] init, PackCompanion names,
            out VnPackInspectReportV1 report, out VnPackOccurrenceFactV1[] occurrences,
            out VnPackMediaFactV1[] media, bool details = true)
        {
            report = default;
            occurrences = Array.Empty<VnPackOccurrenceFactV1>();
            media = Array.Empty<VnPackMediaFactV1>();
            if (bytes == null || names == null) return VnResult.VN_INVALID_ARG;
            try
            {
                fixed (byte* packPtr = bytes)
                fixed (byte* initPtr = init)
                {
                    var desc = new VnPackInspectDescV1
                    {
                        structSize = (uint)sizeof(VnPackInspectDescV1), rootActionId = names.RootId,
                        pack = new VnPackByteSpan { structSize = (uint)sizeof(VnPackByteSpan),
                            bytes = (IntPtr)packPtr, byteCount = (ulong)bytes.LongLength },
                        init = new VnPackByteSpan { structSize = (uint)sizeof(VnPackByteSpan),
                            bytes = (IntPtr)initPtr, byteCount = (ulong)(init?.LongLength ?? 0) },
                        maxInputBytes = (ulong)bytes.LongLength + (ulong)(init?.LongLength ?? 0) + 1024u
                    };
                    report.structSize = (uint)sizeof(VnPackInspectReportV1);
                    VnResult result = HarkenNative.Vn_InspectCanonicalPackV1(desc, ref report);
                    if (result != VnResult.VN_OK || !details) return result;
                    var occurrenceRows = new VnPackOccurrenceFactV1[checked((int)report.occurrenceCount)];
                    var mediaRows = new VnPackMediaFactV1[checked((int)report.mediaCount)];
                    var variables = new VnPackVariableFactV1[checked((int)report.variableCount)];
                    var labels = new VnPackLabelFactV1[checked((int)report.labelCount)];
                    fixed (VnPackOccurrenceFactV1* occurrencePtr = occurrenceRows)
                    fixed (VnPackMediaFactV1* mediaPtr = mediaRows)
                    fixed (VnPackVariableFactV1* variablePtr = variables)
                    fixed (VnPackLabelFactV1* labelPtr = labels)
                    {
                        desc.occurrenceCapacity = report.occurrenceCount;
                        desc.mediaCapacity = report.mediaCount;
                        desc.variableCapacity = report.variableCount;
                        desc.labelCapacity = report.labelCount;
                        desc.occurrences = (IntPtr)occurrencePtr;
                        desc.media = (IntPtr)mediaPtr;
                        desc.variables = (IntPtr)variablePtr;
                        desc.labels = (IntPtr)labelPtr;
                        result = HarkenNative.Vn_InspectCanonicalPackV1(desc, ref report);
                    }
                    if (result == VnResult.VN_OK) { occurrences = occurrenceRows; media = mediaRows; }
                    return result;
                }
            }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
        }

        internal static unsafe VnResult Activate(CompanionMember[] members, Pack init, ulong revision,
            out VnCanonicalPackSetActivationReportV1 report, out VnCanonicalPackSetMemberResultV1[] results)
        {
            report = default;
            results = null;
            int[] providers = null;
            try
            {
                var native = new VnCanonicalPackSetMemberV2[members.Length];
                var rows = new VnCanonicalPackSetMemberResultV1[members.Length];
                providers = new int[members.Length];
                for (int i = 0; i < members.Length; ++i)
                {
                    CompanionMember source = members[i];
                    ref VnCanonicalPackSetMemberV2 member = ref native[i];
                    member.structSize = (uint)sizeof(VnCanonicalPackSetMemberV2);
                    member.operation = source.Operation;
                    member.source.structSize = (uint)sizeof(VnCanonicalPackSetMemberV1);
                    member.source.packStableId = source.Id;
                    member.source.packKind = source.Kind;
                    member.source.expectedHandle = source.Expected.Handle;
                    rows[i].structSize = (uint)sizeof(VnCanonicalPackSetMemberResultV1);
                    if (source.Operation == VnConstants.VN_PACK_MEMBER_KEEP) continue;
                    providers[i] = PackByteProvider.Register(PackByteProvider.PreparedBytes, source.Bytes);
                    if (providers[i] == 0) return VnResult.VN_BACKPRESSURE;
                    PackByteProvider.BindMember(ref member.source, providers[i]);
                }
                VnResult result = ActivatePrepared(native, rows, members.Length, init, revision, out report);
                if (result == VnResult.VN_OK) results = rows;
                return result;
            }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            finally
            {
                if (providers != null) foreach (int key in providers) if (key != 0) PackByteProvider.Unregister(key);
            }
        }

        internal static unsafe VnResult ActivatePrepared(VnCanonicalPackSetMemberV2[] members,
            VnCanonicalPackSetMemberResultV1[] rows, int count, Pack init, ulong revision,
            out VnCanonicalPackSetActivationReportV1 report)
        {
            report = default;
            fixed (VnCanonicalPackSetMemberV2* membersPtr = members)
            fixed (VnCanonicalPackSetMemberResultV1* rowsPtr = rows)
            {
                var desc = new VnCanonicalPackSetActivationDescV2
                {
                    structSize = (uint)sizeof(VnCanonicalPackSetActivationDescV2),
                    expectedCurrentInitHandle = init.Handle, baseSetRevision = revision, setRevision = revision + 1,
                    memberCount = (uint)count, memberStructSize = (uint)sizeof(VnCanonicalPackSetMemberV2),
                    members = (IntPtr)membersPtr, outputMemberCapacity = (uint)count,
                    outputMemberStructSize = (uint)sizeof(VnCanonicalPackSetMemberResultV1), outputMembers = (IntPtr)rowsPtr
                };
                report.structSize = (uint)sizeof(VnCanonicalPackSetActivationReportV1);
                return Packs.ObserveMutation(HarkenNative.Vn_ActivateCanonicalPackSetV2(desc, ref report));
            }
        }
    }
}
