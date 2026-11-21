using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;

namespace Harken
{
    public sealed partial class PackBootstrapSession
    {
        internal ulong inputBudget;

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
            bool initial = packs.Count == 0;
            var nextPacks = initial ? new Dictionary<ulong, SharedPack>(count) : packs;
            var nextNames = initial ? new Dictionary<string, SharedPack>(count, StringComparer.Ordinal) : companionNames;
            var names = GameplayNames.PrepareActions(initNames, count);
            var metadata = new PackCompanion[count];
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
                    metadata[i] = new PackCompanion { ProjectId = facts.projectId, RootId = facts.rootActionId, Name = name };
                }
                else
                {
                    if (!nextNames.TryGetValue(name, out owner) || owner.StableId != facts.packStableId ||
                        owner.Names.RootId != facts.rootActionId) return VnResult.VN_CAPABILITY_UNAVAILABLE;
                    metadata[i] = owner.Names.Language.Length == 0 ? owner.Names :
                        new PackCompanion { ProjectId = facts.projectId, RootId = facts.rootActionId, Name = name };
                }
                call.Owners[i] = owner;
                names.Actions.Add(name, new GameplayNames.Action(facts.rootActionId, ++revision, facts.executionFingerprint, owner));
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
                    ref var member = ref companionKeepMembers[i + 1];
                    member.structSize = (uint)sizeof(VnCanonicalPackSetMemberV2);
                    member.operation = VnConstants.VN_PACK_MEMBER_CONTENT;
                    member.source = rows[index].provider;
                    companionKeepResults[i + 1].structSize = (uint)sizeof(VnCanonicalPackSetMemberResultV1);
                }
                call.Cancel.ThrowIfCancellationRequested();
                call.Submitted = true;
                mutationPhase = MutationPhase.ApplyingNative;
                var result = CompanionNative.ActivatePrepared(companionKeepMembers, companionKeepResults,
                    count + 1, InitPack, SetRevision, out var report);
                if (result != VnResult.VN_OK) return result;
                for (int i = 0; i < count; ++i)
                {
                    int index = order[i];
                    var owner = sortedOwners[i];
                    ref var row = ref rows[index];
                    owner.Handle = new Pack(companionKeepResults[i + 1].handle);
                    owner.StableId = row.facts.packStableId;
                    owner.ContentHash = row.facts.contentHash;
                    owner.ByteCount = (int)row.byteCount;
                    owner.Names = metadata[index];
                    owner.NameRevision = nameRevision + (ulong)index + 1;
                    owner.CallableFingerprint = row.facts.executionFingerprint;
                    ++owner.Requests;
                }
                nameRevision = revision;
                companionOwners = sortedOwners;
                packs = nextPacks;
                companionNames = nextNames;
                InitPack = new Pack(report.initHandle);
                SetRevision = report.resultSetRevision;
                GameplayNames.Publish(names);
                return VnResult.VN_OK;
            }
            finally
            {
                // Provider pointers belong to the native visit and cannot outlive this callback.
                Array.Clear(companionKeepMembers, 1, count);
            }
        }

        private unsafe VnResult VisitBatch(string[] names, SharedPack[] owners, CancellationToken cancellation, out bool submitted)
        {
            var call = new BatchCall { Session = this, Names = names, Owners = owners, Cancel = cancellation };
            var token = GCHandle.Alloc(call);
            mutationPhase = MutationPhase.ReadingContent;
            try
            {
                var ids = initSelection.Identity.BatchIdentity(initSelection.ChangeTime);
                var path = System.Text.Encoding.UTF8.GetBytes(companionSource.Root + "\0");
                var list = System.Text.Encoding.UTF8.GetBytes(string.Join("\n", names) + "\0");
                VnResult result;
                fixed (byte* init = initBytes, root = path, actions = list)
                {
                    var desc = new VnResidentPackVisitDescV1
                    {
                        structSize = (uint)sizeof(VnResidentPackVisitDescV1), memberCount = (uint)names.Length,
                        utf8Directory = (IntPtr)root, utf8Names = (IntPtr)actions,
                        initVolumeId = ids[0], initFileId = ids[1], initFileSize = ids[2], initWriteTime = ids[3],
                        initChangeTime = ids[4], maxInputBytes = inputBudget != 0 ? inputBudget : 64ul * 1024 * 1024,
                        init = new VnPackByteSpan { structSize = (uint)sizeof(VnPackByteSpan),
                            bytes = (IntPtr)init, byteCount = (ulong)initBytes.Length },
                        visit = BatchCall.VisitorPtr, userData = GCHandle.ToIntPtr(token)
                    };
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
