using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Harken
{
    public static class Values
    {
        public static unsafe VnResult SetValue(string variableName, string projectId,
            float value, Emitter? emitter = null)
        {
            if (!VariableGameplay.IsGameThread || !AcceptsName(variableName) ||
                !TryProject(projectId, out Guid project) ||
                (emitter.HasValue && !emitter.Value.IsValid))
                return VnResult.VN_INVALID_ARG;
            VnResult resolved = Resolve(variableName, project, out VnObjectId id);
            return resolved == VnResult.VN_OK ? SetValue(id, value, emitter) : resolved;
        }

        public static unsafe VnResult SetValue(string variableName, string projectId,
            string labelName, Emitter? emitter = null)
        {
            if (!VariableGameplay.IsGameThread || !AcceptsName(variableName) ||
                !AcceptsName(labelName) ||
                !TryProject(projectId, out Guid project) ||
                (emitter.HasValue && !emitter.Value.IsValid))
                return VnResult.VN_INVALID_ARG;
            VnResult resolved = Resolve(variableName, project, out VnObjectId id);
            return resolved == VnResult.VN_OK ? SetValue(id, labelName, emitter) : resolved;
        }

        public static unsafe VnResult SetValue(string variableName, string projectId,
            VnObjectId labelId, Emitter? emitter = null)
        {
            if (!VariableGameplay.IsGameThread || !AcceptsName(variableName) ||
                !TryProject(projectId, out Guid project) ||
                (emitter.HasValue && !emitter.Value.IsValid))
                return VnResult.VN_INVALID_ARG;
            VnResult resolved = Resolve(variableName, project, out VnObjectId id);
            return resolved == VnResult.VN_OK ? SetValue(id, labelId, emitter) : resolved;
        }

        public static VnResult SetValue(VnObjectId variableId, VnObjectId labelId,
            Emitter? emitter = null)
        {
            if (!VariableGameplay.IsGameThread || (emitter.HasValue && !emitter.Value.IsValid))
                return VnResult.VN_INVALID_ARG;
            return HarkenNative.Vn_SetValueByLabelId(variableId, labelId, emitter?.Handle ?? 0);
        }

        public static VnResult SetValue(HarkenVariableId variable, VnObjectId labelId,
            Emitter? emitter = null)
        {
            if (!variable.IsAssigned || (emitter.HasValue && !emitter.Value.IsValid))
                return VnResult.VN_INVALID_ARG;
            VnResult guard = VariableCatalogGuard.QueryAndEvaluate();
            return guard == VnResult.VN_OK
                ? SetValue(variable.stableId, labelId, emitter) : guard;
        }

        public static VnResult SetValue(HarkenVariableId variable, string name,
            Emitter? emitter = null)
        {
            if (!variable.IsAssigned || string.IsNullOrEmpty(name) || name.IndexOf('\0') >= 0 ||
                (emitter.HasValue && !emitter.Value.IsValid)) return VnResult.VN_INVALID_ARG;
            VnResult guard = VariableCatalogGuard.QueryAndEvaluate();
            return guard == VnResult.VN_OK
                ? SetValue(variable.stableId, name, emitter) : guard;
        }

        public static unsafe VnResult SetValue(VnObjectId variableId, string name,
            Emitter? emitter = null)
        {
            if (!VariableGameplay.IsGameThread || string.IsNullOrEmpty(name) || name.IndexOf('\0') >= 0 ||
                (emitter.HasValue && !emitter.Value.IsValid)) return VnResult.VN_INVALID_ARG;
            Span<byte> buffer = stackalloc byte[256];
            byte[] heap = Utf8.TryEncode(name, buffer, out _) ? null : Encoding.UTF8.GetBytes(name + '\0');
            fixed (byte* stackPointer = buffer)
            fixed (byte* heapPointer = heap)
                return HarkenNative.Vn_SetValueByLabelName(variableId,
                    heap != null ? heapPointer : stackPointer, emitter?.Handle ?? 0);
        }

        public static VnResult Resolve(string variableName, out VnObjectId variableId)
        {
            variableId = default;
            if (!VariableGameplay.IsGameThread || !AcceptsName(variableName))
                return VnResult.VN_INVALID_ARG;
            return Resolve(variableName, Guid.Empty, out variableId);
        }

        internal static VnResult Resolve(string variableName, string projectId, out VnObjectId variableId)
        {
            variableId = default;
            if (!VariableGameplay.IsGameThread || !AcceptsName(variableName) ||
                !TryProject(projectId, out Guid project))
                return VnResult.VN_INVALID_ARG;
            return Resolve(variableName, project, out variableId);
        }

        public static VnResult SetValue(string variableName, float value,
            Emitter? emitter = null)
        {
            if (emitter.HasValue && !emitter.Value.IsValid)
                return VnResult.VN_INVALID_ARG;
            VnResult result = Resolve(variableName, out VnObjectId variableId);
            return result == VnResult.VN_OK ? SetValue(variableId, value, emitter) : result;
        }

        public static VnResult SetValue(string variableName, string labelName,
            Emitter? emitter = null)
        {
            if (!AcceptsName(labelName) || (emitter.HasValue && !emitter.Value.IsValid))
                return VnResult.VN_INVALID_ARG;
            VnResult result = Resolve(variableName, out VnObjectId variableId);
            return result == VnResult.VN_OK ? SetValue(variableId, labelName, emitter) : result;
        }

        public static VnResult SetValue(string variableName, VnObjectId labelId,
            Emitter? emitter = null)
        {
            if (emitter.HasValue && !emitter.Value.IsValid)
                return VnResult.VN_INVALID_ARG;
            VnResult result = Resolve(variableName, out VnObjectId variableId);
            return result == VnResult.VN_OK ? SetValue(variableId, labelId, emitter) : result;
        }

        public static VnResult SetValue(VnObjectId variableId, float value,
            Emitter? emitter = null)
        {
            if (!VariableGameplay.IsGameThread) return VnResult.VN_INVALID_ARG;
            if (emitter.HasValue)
                return emitter.Value.IsValid
                    ? HarkenNative.Vn_SetEmitterValue(emitter.Value.Handle, variableId, value)
                    : VnResult.VN_INVALID_ARG;
            return HarkenNative.Vn_SetValue(variableId, value);
        }

        public static VnResult SetValue(HarkenVariableId variable, float value,
            Emitter? emitter = null)
        {
            if (!variable.IsAssigned || (emitter.HasValue && !emitter.Value.IsValid))
                return VnResult.VN_INVALID_ARG;
            VnResult guard = VariableCatalogGuard.QueryAndEvaluate();
            return guard == VnResult.VN_OK
                ? SetValue(variable.stableId, value, emitter) : guard;
        }

        [Obsolete("Use SetValue(variable, value, optional emitter).")]
        internal static VnResult SetValues(VnVariableWriteV1[] writes, int count,
            out VnVariableTicketV1 ticket)
            => DispatchRaw(0, false, writes, count, out ticket);

        [Obsolete("Use SetValue(variable, value).")]
        internal static VnResult SetValues(HarkenVariableId variable, float value)
            => SetValue(variable, value);

        [Obsolete("Use SetValue with the Variable default value.")]
        internal static VnResult ResetValues(VnVariableWriteV1[] writes, int count,
            out VnVariableTicketV1 ticket)
            => DispatchRaw(0, true, writes, count, out ticket);

        [Obsolete("Use SetValue with the Variable default value.")]
        internal static unsafe VnResult ResetValues(HarkenVariableId variable)
        {
            if (!variable.IsAssigned) return VnResult.VN_INVALID_ARG;
            VnResult guard = VariableCatalogGuard.QueryAndEvaluate();
            if (guard != VnResult.VN_OK) return guard;
            VnVariableWriteV1 write = default;
            write.variableId = variable.stableId;
            VnVariableBatchDescV1 desc = default;
            desc.structSize = (uint)sizeof(VnVariableBatchDescV1);
            desc.writeCount = 1;
            desc.writes = (IntPtr)(&write);
            VnVariableTicketV1 ticket = default;
            return HarkenNative.Vn_ResetValues(in desc, ref ticket);
        }

        public static VnResult QueryVariable(in VnVariableQueryDescV1 desc,
            ref VnVariableQueryResultV1 result)
            => HarkenNative.Vn_QueryVariable(in desc, ref result);

        public static VnResult QueryVariable(HarkenVariableId variable,
            out VnVariableQueryResultV1 result)
        {
            result = default;
            result.structSize = (uint)Marshal.SizeOf<VnVariableQueryResultV1>();
            VnResult guard = VariableCatalogGuard.QueryAndEvaluate();
            if (guard != VnResult.VN_OK) return guard;
            VnVariableQueryDescV1 desc = default;
            desc.structSize = (uint)Marshal.SizeOf<VnVariableQueryDescV1>();
            desc.variableId = variable.stableId;
            return HarkenNative.Vn_QueryVariable(in desc, ref result);
        }

        public static VnResult QueryVariableTicket(in VnVariableTicketQueryDescV1 desc,
            ref VnVariableTicketQueryResultV1 result)
            => HarkenNative.Vn_QueryVariableTicket(in desc, ref result);

        internal static unsafe VnResult DispatchRaw(ulong emitter, bool reset,
            VnVariableWriteV1[] writes, int count, out VnVariableTicketV1 ticket)
        {
            ticket = default;
            if (writes == null || count <= 0 || count > 8 || count > writes.Length ||
                (reset && emitter != 0))
                return VnResult.VN_INVALID_ARG;
            VnVariableBatchDescV1 desc = default;
            desc.structSize = (uint)sizeof(VnVariableBatchDescV1);
            desc.writeCount = (uint)count;
            fixed (VnVariableWriteV1* pointer = writes)
            {
                desc.writes = (IntPtr)pointer;
                if (reset) return HarkenNative.Vn_ResetValues(in desc, ref ticket);
                return emitter == 0
                    ? HarkenNative.Vn_SetValues(in desc, ref ticket)
                    : HarkenNative.Vn_SetEmitterValues(emitter, in desc, ref ticket);
            }
        }

        static bool AcceptsName(string name)
            => !string.IsNullOrEmpty(name) && name.IndexOf((char)0) < 0;

        static bool TryProject(string projectId, out Guid project)
        {
            project = Guid.Empty;
            return Guid.TryParseExact(projectId, "D", out project) && project != Guid.Empty;
        }

        static unsafe VnResult Resolve(string variableName, Guid project, out VnObjectId id)
        {
            id = default;
            if (variableName.Length > 128) return VnResult.VN_INVALID_ARG;
            Span<byte> buffer = stackalloc byte[129];
            for (int i = 0; i < variableName.Length; ++i)
            {
                char c = variableName[i];
                if (c == 0 || c > 127) return VnResult.VN_INVALID_ARG;
                buffer[i] = (byte)c;
            }
            buffer[variableName.Length] = 0;
            fixed (byte* stackPointer = buffer)
            {
                VnPostActionByNameDesc desc = default;
                desc.structSize = (uint)Marshal.SizeOf<VnPostActionByNameDesc>();
                Span<byte> projectBytes = stackalloc byte[16];
                project.TryWriteBytes(projectBytes);
                for (int i = 0; i < 16; ++i)
                {
                    int source = i < 4 ? 3 - i : i < 6 ? 9 - i : i < 8 ? 13 - i : i;
                    desc.projectGuid[i] = projectBytes[source];
                }
                desc.utf8Name = (IntPtr)stackPointer;
                VnVariableResolved native = default;
                native.structSize = (uint)Marshal.SizeOf<VnVariableResolved>();
                VnResult result = HarkenNative.Vn_ResolveVariableByName(in desc, ref native);
                if (result == VnResult.VN_OK)
                    id = native.variableId;
                return result;
            }
        }

    }

    internal static class VariableGameplay
    {
        public static Func<bool> GameThreadProbe;
        public static ulong SessionVersion { get; private set; }
        public static bool IsGameThread => GameThreadProbe == null || GameThreadProbe();
        public static void NotifySessionChanged()
        {
            unchecked { ++SessionVersion; }
        }
    }
}
