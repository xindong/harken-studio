using System;
using System.Runtime.InteropServices;

namespace Harken.Unity.Editor
{
    public sealed partial class HarkenEditorPreview
    {
        private sealed class SelectedSound
        {
            internal VnMonitorRecordHeader Header;
            internal VnMonitorExecutionFactV3 Fact;
        }

        private ulong observationSession, observationEpoch, observationEngine, observationDelivery;
        private readonly byte[] observationBuffer = new byte[64 * 1024];
        private bool observationFailed;

        private void OpenObservation()
        {
            if (observationSession != 0 || observationFailed) return;
            var desc = new VnMonitorSessionDescV2
            {
                structSize = (uint)Marshal.SizeOf<VnMonitorSessionDescV2>(),
                requestedStreamKindMask = (uint)VnMonitorStreamKind.VN_MONITOR_STREAM_EVENT,
                requestedExecutionFactVersion = 3,
                requestedPermissionMask = (uint)VnMonitorPermissionMask.VN_MONITOR_PERM_OBSERVE_EVENTS,
                consumerQueueBytes = 256 * 1024
            };
            if (HarkenNative.Vn_MonitorOpenEventSessionV2(in desc, out observationSession) != VnResult.VN_OK)
            { observationFailed = true; return; }
            CheckObservation();
        }

        private bool CheckObservation()
        {
            if (observationFailed || observationSession == 0) return false;
            var status = new VnMonitorEventSessionStatusV2 { structSize = (uint)Marshal.SizeOf<VnMonitorEventSessionStatusV2>() };
            if (HarkenNative.Vn_MonitorGetEventSessionStatusV2(observationSession, ref status) != VnResult.VN_OK ||
                status.acceptedExecutionFactVersion != 3 || status.sourceDroppedRecords != 0 ||
                status.deliveryDroppedRecords != 0 || status.identityUnavailableCount != 0 ||
                (observationEpoch != 0 && observationEpoch != status.monitorSessionEpoch))
            { InvalidateObservation(); return false; }
            observationEpoch = status.monitorSessionEpoch;
            return true;
        }

        private void InvalidateObservation()
        {
            observationFailed = true;
            foreach (var owner in firings.Values) { owner.Sounds.Clear(); owner.SelectionInvalid = true; }
        }

        private void PollObservation()
        {
            if (!CheckObservation()) return;
            var pinned = GCHandle.Alloc(observationBuffer, GCHandleType.Pinned);
            try
            {
                // Bound Editor work per update. Remaining records stay in the native queue.
                for (int batch = 0; batch != 4; ++batch)
                {
                    var result = new VnMonitorPollResult { structSize = (uint)Marshal.SizeOf<VnMonitorPollResult>() };
                    {
                        var desc = new VnMonitorPollDesc { structSize = (uint)Marshal.SizeOf<VnMonitorPollDesc>(),
                            buffer = pinned.AddrOfPinnedObject(), bufferBytes = (ulong)observationBuffer.Length };
                        if (HarkenNative.Vn_MonitorPoll(observationSession, in desc, ref result) != VnResult.VN_OK)
                        { InvalidateObservation(); return; }
                    }
                    if (result.bytesWritten > (ulong)observationBuffer.Length) throw new FormatException();
                    int limit = (int)result.bytesWritten, at = 0;
                    uint records = 0;
                    while (at < limit)
                    {
                        var header = HarkenMonitorWire.Header(observationBuffer, at, limit);
                        if (header.recordBytes < VnConstants.VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES ||
                            header.recordBytes > (ulong)(limit - at) || (header.recordBytes & 7) != 0 ||
                            header.monitorSessionEpoch != observationEpoch ||
                            (observationDelivery != 0 && header.deliverySequence != observationDelivery + 1))
                            throw new FormatException();
                        observationDelivery = header.deliverySequence;
                        if (header.recordKind == (uint)VnMonitorRecordKind.VN_MONITOR_RECORD_DROP_SUMMARY)
                        { InvalidateObservation(); return; }
                        if (header.recordKind == (uint)VnMonitorRecordKind.VN_MONITOR_RECORD_EXECUTION_FACT)
                        {
                            if (header.recordVersion != 3 || header.payloadBytes != 544 || header.recordBytes != 864 ||
                                header.engineMonitorInstanceId == 0 ||
                                (observationEngine != 0 && observationEngine != header.engineMonitorInstanceId)) throw new FormatException();
                            observationEngine = header.engineMonitorInstanceId;
                            var fact = HarkenMonitorWire.Fact(observationBuffer,
                                at + (int)VnConstants.VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES, at + (int)header.recordBytes);
                            ObserveSelection(header, fact);
                        }
                        at += (int)header.recordBytes;
                        ++records;
                    }
                    if (records != result.recordsWritten) throw new FormatException();
                    if (result.recordsRemaining == 0) break;
                }
                CheckObservation();
            }
            catch (Exception) { InvalidateObservation(); }
            finally { pinned.Free(); }
        }

        private static bool Has(VnMonitorExecutionFactV1 fact, VnMonitorExecutionField field)
        {
            int index = (int)field;
            return ((index < 64 ? fact.validityLo : fact.validityHi) & (1ul << (index % 64))) != 0;
        }

        private void ObserveSelection(VnMonitorRecordHeader header, VnMonitorExecutionFactV3 value)
        {
            var fact = value.fact;
            if (fact.subjectNamespace != (byte)VnMonitorExecutionNamespace.VN_MONITOR_SUBJECT_PLAYABLE ||
                fact.contentKind != (byte)VnMonitorExecutionContentKind.VN_MONITOR_CONTENT_SOUND ||
                !firings.TryGetValue(header.actionInstanceHandle, out FiringOwner owner) || owner.StopRequested) return;
            var kind = (VnMonitorExecutionFactKind)fact.factKind;
            if (kind == VnMonitorExecutionFactKind.VN_MONITOR_FACT_PLAYABLE_RETIRED ||
                kind == VnMonitorExecutionFactKind.VN_MONITOR_FACT_PLAYABLE_FINISHED ||
                kind == VnMonitorExecutionFactKind.VN_MONITOR_FACT_PLAYABLE_CANCELED)
            { owner.Sounds.Remove(header.instanceHandle); return; }
            if (kind != VnMonitorExecutionFactKind.VN_MONITOR_FACT_PLAYABLE_STARTED || owner.SelectionInvalid) return;
            if (fact.factFlags != 0 || value.playable.knownFlags != VnConstants.VN_MONITOR_PLAYABLE_PROVENANCE_KNOWN ||
                !Has(fact, VnMonitorExecutionField.VN_MONITOR_FACT_FIELD_SUBJECT) ||
                !Has(fact, VnMonitorExecutionField.VN_MONITOR_FACT_FIELD_PROJECT) ||
                !Has(fact, VnMonitorExecutionField.VN_MONITOR_FACT_FIELD_CONTENT_OBJECT) ||
                !Has(fact, VnMonitorExecutionField.VN_MONITOR_FACT_FIELD_CONTENT_SEMANTIC) ||
                !Has(fact, VnMonitorExecutionField.VN_MONITOR_FACT_FIELD_RELATED_ACTION) ||
                !Has(fact, VnMonitorExecutionField.VN_MONITOR_FACT_FIELD_ROOT) ||
                header.instanceHandle == 0 || fact.root == 0 || owner.Sounds.Count != 0)
            { owner.SelectionInvalid = true; owner.Sounds.Clear(); return; }
            owner.Sounds.Add(header.instanceHandle, new SelectedSound { Header = header, Fact = value });
        }

        private void CloseObservation()
        {
            if (observationSession != 0) HarkenNative.Vn_MonitorCloseSession(observationSession);
            observationSession = observationEpoch = observationEngine = observationDelivery = 0;
            observationFailed = false;
            foreach (var owner in firings.Values) owner.Sounds.Clear();
        }

        public sealed partial class Scope
        {
            /// <summary>The one source actually started for this firing. Unavailable after loss, stop, refresh or ambiguity.</summary>
            public VnResult GetPlayingWaveform(ActionFiring firing, HarkenEditorCatalog catalog,
                out HarkenActionWaveform waveform, out string diagnostic)
            {
                waveform = null;
                diagnostic = "No unambiguous started source is available for this firing.";
                if (disposed || !IsPrepared || catalog == null || !ReferenceEquals(catalog.PreparedPacks, source) ||
                    !ReferenceEquals(preview.session, capturedSession) || preview.observationFailed ||
                    !preview.firings.TryGetValue(firing.Handle, out FiringOwner owner) || !ReferenceEquals(owner.Owner, this) ||
                    owner.StopRequested || owner.SelectionInvalid || owner.Sounds.Count != 1 ||
                    owner.PresentationRevision != source.PresentationRevision) return VnResult.VN_CAPABILITY_UNAVAILABLE;
                VnResult result = catalog.GetActionPreview(owner.Name, out HarkenActionPreview candidates, out diagnostic);
                if (result != VnResult.VN_OK) return result;
                SelectedSound selected = null;
                foreach (var sound in owner.Sounds.Values) selected = sound;
                if (selected.Fact.fact.projectId != VnObjectId.Parse(catalog.Publication.ProjectId)) return VnResult.VN_GAMEPLAY_WRONG_PROJECT;
                var actual = selected.Fact.playable;
                HarkenActionPreviewCandidate match = null;
                foreach (var candidate in candidates.Candidates)
                {
                    var expected = candidate.NativeOccurrence;
                    if (expected.soundId != selected.Header.stableId || expected.soundProgramFingerprint != selected.Header.programSemanticFingerprint ||
                        expected.rootPlayableId != actual.rootPlayableId || expected.rootProgramFingerprint != actual.rootProgramFingerprint ||
                        expected.childOrdinal != actual.childOrdinal || expected.occurrenceFingerprint != actual.occurrenceFingerprint ||
                        expected.mediaChecksum != actual.mediaChecksum) continue;
                    if (match != null) return VnResult.VN_CAPABILITY_UNAVAILABLE;
                    match = candidate;
                }
                if (match == null) { diagnostic = "Playback identity differs from the catalog snapshot."; return VnResult.VN_PACK_STALE_REVISION; }
                bool durationKnown = (match.NativeOccurrence.flags & VnConstants.VN_PACK_OCCURRENCE_DURATION) != 0;
                waveform = new HarkenActionWaveform(candidates.Action, match.Source, durationKnown, string.Empty,
                    durationKnown ? string.Empty : "Native duration is unknown.", durationKnown ? match.NativeOccurrence.durationSeconds : (double?)null);
                diagnostic = string.Empty;
                return VnResult.VN_OK;
            }
        }
    }
}
