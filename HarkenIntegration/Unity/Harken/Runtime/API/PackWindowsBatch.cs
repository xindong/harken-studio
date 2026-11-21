using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;

namespace Harken
{
    public sealed unsafe partial class PackBootstrapSession
    {
        internal ulong inputBudget;

        private sealed class BatchCall
        {
            internal PackBootstrapSession Session;
            internal string[] Names;
            internal SharedPack[] Owners;
            internal CancellationToken Cancel;
            internal bool Submitted;
        }
        private static readonly VnResidentPackVisitorFn BatchVisitor = ConsumeBatch;
        private static readonly IntPtr BatchVisitorPtr = Marshal.GetFunctionPointerForDelegate(BatchVisitor);

        [AOT.MonoPInvokeCallback(typeof(VnResidentPackVisitorFn))]
        private static unsafe VnResult ConsumeBatch(IntPtr user, IntPtr batch, uint count)
        {
            var call=(BatchCall)GCHandle.FromIntPtr(user).Target;
            var session = call.Session;
            try
            {
                var rows = (VnResidentPackRowV1*)batch;
                var packs = new Dictionary<ulong, SharedPack>((int)count);
                var names = new Dictionary<string, SharedPack>((int)count, StringComparer.Ordinal);
                var roots = new HashSet<VnObjectId>();
                var changes = new Dictionary<SharedPack, CompanionInput>((int)count);
                var project = VnObjectId.Parse(session.ProjectId);
                for (int i = 0; i < count; ++i)
                {
                    ref var row = ref rows[i];
                    string name = call.Names[i];
                    if (row.facts.projectId != project) return VnResult.VN_BAD_PACK;
                    if (packs.ContainsKey(row.facts.packStableId) || !roots.Add(row.facts.rootActionId))
                        return VnResult.VN_CAPABILITY_UNAVAILABLE;
                    SharedPack owner;
                    if (session.packs.Count != 0)
                    {
                        if (!session.companionNames.TryGetValue(name, out owner) ||
                            owner.StableId != row.facts.packStableId || owner.Names.RootId != row.facts.rootActionId)
                            return VnResult.VN_CAPABILITY_UNAVAILABLE;
                    }
                    else owner = new SharedPack();
                    names.Add(name, owner);
                    packs.Add(row.facts.packStableId, owner);
                    changes.Add(owner, new CompanionInput { Facts = row.facts, BatchLength = checked((int)row.byteCount),
                        BatchProvider = row.provider, Names = new PackCompanion { RootId = row.facts.rootActionId,
                        Name = name, ProjectId = project } });
                    call.Owners[i] = owner;
                }
                call.Cancel.ThrowIfCancellationRequested();
                call.Submitted = true;
                VnResult result = session.CommitCompanions(packs, names, changes);
                if (result == VnResult.VN_OK)
                    foreach (var owner in call.Owners) ++owner.Requests;
                return result;
            }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            catch (OperationCanceledException) { return VnResult.VN_IN_USE; }
            catch { return VnResult.VN_PACK_PROVIDER_FAILED; }
        }

        private unsafe VnResult VisitBatch(string[] names, SharedPack[] owners, CancellationToken cancellation, out bool submitted)
        {
            var call=new BatchCall { Session=this, Names=names, Owners=owners, Cancel=cancellation };
            var token=GCHandle.Alloc(call);
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
                        visit = BatchVisitorPtr, userData = GCHandle.ToIntPtr(token)
                    };
                    result = HarkenNative.Vn_VisitResidentPackDirectoryV1(desc);
                }
                submitted = call.Submitted;
                if (result != VnResult.VN_OK) cancellation.ThrowIfCancellationRequested();
                return result;
            }
            catch(EntryPointNotFoundException) { submitted=false; return VnResult.VN_CAPABILITY_UNAVAILABLE; }
            finally { token.Free(); mutationPhase = MutationPhase.Idle; }
        }
    }
}
