using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

namespace Harken
{
    public sealed partial class PackBootstrapSession
    {
        internal ulong inputBudget;
        private bool directEntryMissing;

        private sealed class BatchCall
        {
            internal PackBootstrapSession Session;
            internal string[] Names;
            internal SharedPack[] Owners;
            internal CancellationToken Cancel;
            internal bool Submitted;
            private static readonly VnResidentPackVisitorFn Visitor = ConsumeBatch;
            internal static readonly IntPtr VisitorPtr = Marshal.GetFunctionPointerForDelegate(Visitor);
        }

        [AOT.MonoPInvokeCallback(typeof(VnResidentPackVisitorFn))]
        private static unsafe VnResult ConsumeBatch(IntPtr user, IntPtr batch, uint count)
        {
            var call = (BatchCall)GCHandle.FromIntPtr(user).Target;
            var session = call.Session;
            try
            {
                var rows = (VnResidentPackRowV1*)batch;
                if (count != call.Names.Length || batch == IntPtr.Zero) return VnResult.VN_PACK_PROVIDER_FAILED;
                return session.PublishResidentBatch(call, rows);
            }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            catch (OperationCanceledException) { return VnResult.VN_IN_USE; }
            catch { return VnResult.VN_PACK_PROVIDER_FAILED; }
        }

        private unsafe VnResult PublishResidentBatch(BatchCall call, VnResidentPackRowV1* rows)
        {
            int count = call.Names.Length;
            EnsureOwnerIndexes();
            bool initial = packs.Count == 0;
            var nextPacks = initial ? new Dictionary<ulong, SharedPack>(count) : packs;
            var nextNames = initial ? new Dictionary<string, SharedPack>(count, StringComparer.Ordinal) : metadataNames;
            var names = NameTable.PrepareActions(initNames, count);
            var metadata = new PackMetadata[count];
            var sortedOwners = new SharedPack[count];
            var order = new int[count];
            var roots = count > SmallBatchLimit ? new HashSet<VnObjectId>() : null;
            ulong revision = nameRevision;
            for (int i = 0; i < count; ++i)
            {
                ref var facts = ref rows[i].facts;
                string name = call.Names[i];
                if (facts.projectId != initNames.Project) return VnResult.VN_BAD_PACK;
                if (rows[i].byteCount > int.MaxValue) return VnResult.VN_PACK_PROVIDER_FAILED;
                if (roots != null)
                {
                    if (!roots.Add(facts.rootActionId)) return VnResult.VN_CAPABILITY_UNAVAILABLE;
                }
                else
                {
                    for (int j = 0; j < i; ++j)
                        if (rows[j].facts.rootActionId == facts.rootActionId) return VnResult.VN_CAPABILITY_UNAVAILABLE;
                }
                SharedPack owner;
                if (initial)
                {
                    if (nextPacks.ContainsKey(facts.packStableId)) return VnResult.VN_CAPABILITY_UNAVAILABLE;
                    owner = new SharedPack();
                    nextPacks.Add(facts.packStableId, owner);
                    nextNames.Add(name, owner);
                    metadata[i] = new PackMetadata { ProjectId = facts.projectId, RootId = facts.rootActionId, Name = name };
                }
                else
                {
                    if (!nextNames.TryGetValue(name, out owner) || owner.StableId != facts.packStableId ||
                        owner.Names.RootId != facts.rootActionId) return VnResult.VN_CAPABILITY_UNAVAILABLE;
                    metadata[i] = owner.Names.Language.Length == 0 ? owner.Names :
                        new PackMetadata { ProjectId = facts.projectId, RootId = facts.rootActionId, Name = name };
                }
                call.Owners[i] = owner;
                names.Actions.Add(name, new NameTable.Action(facts.rootActionId, ++revision, facts.executionFingerprint, owner));
                order[i] = i;
            }

            if (count > SmallBatchLimit)
            {
                var ids = new ulong[count];
                for (int i = 0; i < count; ++i) ids[i] = rows[i].facts.packStableId;
                Array.Sort(ids, order);
            }
            else
            {
                for (int i = 1; i < count; ++i)
                {
                    int j = i;
                    while (j > 0 && rows[order[j - 1]].facts.packStableId > rows[i].facts.packStableId)
                    {
                        order[j] = order[j - 1];
                        --j;
                    }
                    order[j] = i;
                }
            }
            ReserveKeepBuffers(count + 1);
            SetKeepMember(0, Inspection.PackStableId, VnConstants.VN_CANONICAL_PACK_SET_MEMBER_INIT, InitPack);
            try
            {
                for (int i = 0; i < count; ++i)
                {
                    int index = order[i];
                    sortedOwners[i] = call.Owners[index];
                    ref var member = ref packKeepMembers[i + 1];
                    member.structSize = (uint)sizeof(VnCanonicalPackSetMemberV2);
                    member.operation = VnConstants.VN_PACK_MEMBER_CONTENT;
                    member.source = rows[index].provider;
                    packKeepResults[i + 1].structSize = (uint)sizeof(VnCanonicalPackSetMemberResultV1);
                }
                call.Cancel.ThrowIfCancellationRequested();
                call.Submitted = true;
                mutationPhase = MutationPhase.ApplyingNative;
                var result = PackMetadataNative.ActivatePrepared(packKeepMembers, packKeepResults,
                    count + 1, InitPack, SetRevision, out var report);
                if (result != VnResult.VN_OK) return result;
                for (int i = 0; i < count; ++i)
                {
                    int index = order[i];
                    var owner = sortedOwners[i];
                    ref var row = ref rows[index];
                    owner.Handle = new Pack(packKeepResults[i + 1].handle);
                    owner.StableId = row.facts.packStableId;
                    owner.ContentHash = row.facts.contentHash;
                    owner.ByteCount = (int)row.byteCount;
                    owner.Names = metadata[index];
                    owner.NameRevision = nameRevision + (ulong)index + 1;
                    owner.CallableFingerprint = row.facts.executionFingerprint;
                    ++owner.Requests;
                }
                nameRevision = revision;
                packOwners = sortedOwners;
                packs = nextPacks;
                metadataNames = nextNames;
                InitPack = new Pack(report.initHandle);
                SetRevision = report.resultSetRevision;
                NameTable.Publish(names);
                return VnResult.VN_OK;
            }
            finally
            {
                // Provider pointers belong to the native visit and cannot outlive this callback.
                Array.Clear(packKeepMembers, 1, count);
            }
        }

        private bool HasBlockEnvelope(string[] names)
        {
            var header = new byte[4];
            foreach (string name in names)
            {
                try
                {
                    // This only selects a reader. Capture and activation must still validate the entire set.
                    using var file = File.Open(Path.Combine(packSource.Root, name + ".hrp"),
                        FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                    if (file.Read(header, 0, header.Length) == header.Length &&
                        header[0] == 'H' && header[1] == 'R' && header[2] == 'P' && header[3] == '2') return true;
                }
                catch (IOException) { }
                catch (UnauthorizedAccessException) { }
            }
            return false;
        }

        private static byte[] EncodeDirectory(string path)
        {
            for (int i = 0; i < path.Length; ++i)
                if (path[i] > 127) return System.Text.Encoding.UTF8.GetBytes(path + "\0");
            var bytes = new byte[path.Length + 1];
            for (int i = 0; i < path.Length; ++i) bytes[i] = (byte)path[i];
            return bytes;
        }

        private unsafe VnResidentPackVisitDescV1 DescribeDirectory(int count, byte* init, byte* path, byte* names)
        {
            var directory = new VnResidentPackVisitDescV1
            {
                structSize = (uint)sizeof(VnResidentPackVisitDescV1), memberCount = (uint)count,
                utf8Directory = (IntPtr)path, utf8Names = (IntPtr)names,
                maxInputBytes = inputBudget != 0 ? inputBudget : ulong.MaxValue,
                init = new VnPackByteSpan { structSize = (uint)sizeof(VnPackByteSpan),
                    bytes = (IntPtr)init, byteCount = (ulong)initBytes.Length }
            };
            initSelection.Identity.SetBatchIdentity(ref directory, initSelection.ChangeTime);
            return directory;
        }

        internal sealed class DirectoryLease
        {
            internal string[] Names;
            internal VnResidentDirectoryMemberV1[] Rows;
            internal byte[] Keys;
            internal ulong InitHandle, SetRevision;
            internal SharedPack[] Scope;
            internal ulong Revision;

            internal VnResult Find(string name, VnObjectId id, out NameTable.Action action)
            {
                if (Scope != null)
                {
                    for (int i = 0; i < Names.Length; ++i)
                    {
                        ref var row = ref Rows[i];
                        if (name != null ? Names[i] == name : row.rootActionId == id)
                        {
                            action = new NameTable.Action(row.rootActionId, Revision + (ulong)i + 1,
                                row.executionFingerprint, null);
                            return VnResult.VN_OK;
                        }
                    }
                }
                action = default;
                return VnResult.VN_UNKNOWN_ID;
            }
        }

        private DirectoryLease directoryLease;
        private bool namedEntryMissing;

        private bool CanAcquireNamedDirectory(int count)
        {
            return HarkenEngine.NativeAbiMinor >= 3 && !namedEntryMissing && !directEntryMissing &&
                initSelection != null && Language.Length == 0 && count > 0 && count <= SmallBatchLimit &&
                (directoryLease == null ? KnownOwnerCount == 0 :
                    directoryLease.Scope == null && directoryLease.Names.Length == count);
        }

        private void ExpandDirectoryLease()
        {
            var lease = directoryLease;
            if (lease == null) return;
            var owners = new SharedPack[lease.Names.Length];
            var ordered = new SharedPack[owners.Length];
            var table = new NameTable.Table { Project = initNames.Project, Variables = initNames.Variables,
                Labels = initNames.Labels, SmallActions = ordered };
            for (int i = 0; i < owners.Length; ++i)
            {
                ref var row = ref lease.Rows[i];
                var owner = new SharedPack { Binding = row, Requests = lease.Scope == null ? 0 : 1,
                    Names = new PackMetadata { Name = lease.Names[i], RootId = row.rootActionId, ProjectId = initNames.Project },
                    NameRevision = lease.Revision + (ulong)i + 1 };
                owners[i] = owner;
                int j = i;
                while (j > 0 && ordered[j - 1].StableId > owner.StableId)
                {
                    ordered[j] = ordered[j - 1];
                    --j;
                }
                ordered[j] = owner;
            }
            if (lease.Scope != null) Array.Copy(owners, lease.Scope, owners.Length);
            packOwners = ordered;
            packs = null;
            metadataNames = null;
            directoryLease = null;
            NameTable.Expand(lease, table);
        }

        internal VnResult ReleaseDirectoryLease(SharedPack[] scope, out bool released)
        {
            released = false;
            var lease = directoryLease;
            if (lease == null || !ReferenceEquals(scope, lease.Scope)) return VnResult.VN_OK;
            var allowed = PackMutationAllowed();
            if (allowed != VnResult.VN_OK) return allowed;
            try
            {
                SetKeepMember(0, Inspection.PackStableId, VnConstants.VN_CANONICAL_PACK_SET_MEMBER_INIT, InitPack);
                mutationPhase = MutationPhase.ApplyingNative;
                var result = PackMetadataNative.ActivatePrepared(packKeepMembers, packKeepResults, 1,
                    InitPack, SetRevision, out var report);
                if (result != VnResult.VN_OK)
                {
                    ExpandDirectoryLease();
                    return VnResult.VN_OK;
                }
                for (int i = 0; i < lease.Rows.Length; ++i) lease.Rows[i].handle = 0;
                lease.Scope = null;
                InitPack = new Pack(report.initHandle);
                SetRevision = report.resultSetRevision;
                released = true;
                return VnResult.VN_OK;
            }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            finally { mutationPhase = MutationPhase.Idle; }
        }

        internal unsafe VnResult ActivateSmallResidentSet(string[] names, SharedPack[] acquired, bool named = false)
        {
            try
            {
                var old = directoryLease;
                int count = names.Length;
                var rows = new VnResidentDirectoryMemberV1[count];
                var next = new DirectoryLease { Names = names, Rows = rows, Scope = acquired, Revision = nameRevision };
                var table = new NameTable.Table { Project = initNames.Project, Variables = initNames.Variables,
                    Labels = initNames.Labels, Directory = next };
                int length = 0;
                for (int i = 0; i < count; ++i)
                {
                    if (old != null)
                    {
                        if (old.Names[i] != names[i]) return VnResult.VN_CAPABILITY_UNAVAILABLE;
                        rows[i].packStableId = old.Rows[i].packStableId;
                        rows[i].rootActionId = old.Rows[i].rootActionId;
                    }
                    length += names[i].Length + 1;
                }
                byte[] list = null, path = null;
                string wideNames = null;
                if (named)
                {
                    wideNames = string.Join("\n", names);
                    next.Keys = new byte[count * 129];
                }
                else
                {
                    list = new byte[length];
                    int offset = 0;
                    for (int i = 0; i < count; ++i)
                    {
                        foreach (char ch in names[i]) list[offset++] = (byte)ch;
                        list[offset++] = i + 1 == count ? (byte)0 : (byte)10;
                    }
                    path = EncodeDirectory(packSource.Root);
                }
                ReserveKeepBuffers(count + 1);
                var report = new VnCanonicalPackSetActivationReportV1 { structSize = (uint)sizeof(VnCanonicalPackSetActivationReportV1) };
                mutationPhase = MutationPhase.ApplyingNative;
                fixed (char* wideDirectory = packSource.Root, wideActions = wideNames)
                fixed (VnResidentDirectoryMemberV1* output = rows)
                fixed (byte* init = initBytes, root = path, actions = list, keys = next.Keys)
                {
                    var desc = new VnResidentDirectoryActivationV1
                    {
                        structSize = (uint)sizeof(VnResidentDirectoryActivationV1), rowCapacity = (uint)count,
                        initStableId = Inspection.PackStableId, initHandle = InitPack.Handle, baseSetRevision = SetRevision,
                        projectId = table.Project, rows = (IntPtr)output,
                        directory = DescribeDirectory(count, init, root, actions)
                    };
                    var result = named
                        ? HarkenNative.Vn_ActivateNamedResidentDirectoryV1(desc, (ushort*)wideDirectory, (uint)packSource.Root.Length,
                            (ushort*)wideActions, (uint)wideNames.Length,
                            keys, (uint)next.Keys.Length, ref report)
                        : HarkenNative.Vn_ActivateResidentPackDirectoryV1(desc, ref report);
                    Packs.ObserveMutation(result);
                    if (result != VnResult.VN_OK) return result;
                }
                next.InitHandle = report.initHandle;
                next.SetRevision = report.resultSetRevision;
                InitPack = new Pack(report.initHandle);
                SetRevision = report.resultSetRevision;
                nameRevision += (ulong)count;
                directoryLease = next;
                NameTable.Publish(table);
                return VnResult.VN_OK;
            }
            catch (EntryPointNotFoundException)
            {
                if (named) namedEntryMissing = true; else directEntryMissing = true;
                return VnResult.VN_CAPABILITY_UNAVAILABLE;
            }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            finally { mutationPhase = MutationPhase.Idle; }
        }

        private unsafe VnResult VisitBatch(string[] names, SharedPack[] owners, CancellationToken cancellation, out bool submitted)
        {
            var call = new BatchCall { Session = this, Names = names, Owners = owners, Cancel = cancellation };
            var token = GCHandle.Alloc(call);
            mutationPhase = MutationPhase.ReadingContent;
            try
            {
                var path = EncodeDirectory(packSource.Root);
                var list = System.Text.Encoding.UTF8.GetBytes(string.Join("\n", names) + "\0");
                VnResult result;
                fixed (byte* init = initBytes, root = path, actions = list)
                {
                    var desc = DescribeDirectory(names.Length, init, root, actions);
                    desc.visit = BatchCall.VisitorPtr;
                    desc.userData = GCHandle.ToIntPtr(token);
                    result = HarkenNative.Vn_VisitResidentPackDirectoryV1(desc);
                }
                submitted = call.Submitted;
                if (result != VnResult.VN_OK) cancellation.ThrowIfCancellationRequested();
                return result;
            }
            catch (EntryPointNotFoundException) { submitted = false; return VnResult.VN_CAPABILITY_UNAVAILABLE; }
            finally { token.Free(); mutationPhase = MutationPhase.Idle; }
        }
    }
}
