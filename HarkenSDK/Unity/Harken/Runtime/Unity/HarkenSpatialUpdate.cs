using System;
using System.Collections.Generic;
using UnityEngine;

namespace Harken.Unity
{
    [DisallowMultipleComponent]
    [AddComponentMenu("")]
    [DefaultExecutionOrder(10000)]
    internal sealed class HarkenSpatialUpdate : MonoBehaviour
    {
        internal HarkenAudioSystem Owner;

        private void LateUpdate()
        {
            if (Owner != null && Owner.isActiveAndEnabled) Owner.AdvanceFrame();
        }
    }

    public partial class HarkenAudioSystem
    {
        private readonly List<HarkenEmitter> spatialEmitters = new List<HarkenEmitter>();
        private readonly List<HarkenListener> listenerCandidates = new List<HarkenListener>();
        private SpatialRecord[] spatialRecords = Array.Empty<SpatialRecord>();
        private HarkenListener selectedListener;
        private ulong listenerRevision;
        private bool publishingSpatial;
        private bool advancingSpatialFrame;

        internal bool IsPublishingSpatial => publishingSpatial;
        public HarkenListener SelectedListener => selectedListener;

        internal struct SpatialRecord
        {
            internal HarkenEmitter Component;
            internal HarkenSpatialSource Source;
            internal HarkenListener Listener;
            internal Emitter Handle;
            internal UnitySpatialPose Pose;
            internal VnResult Result;
            internal bool Notify;

            internal bool IsCurrent(HarkenAudioSystem host, PackBootstrapSession owner)
            {
                if (Listener != null) return Listener.Matches(host, owner);
                if (Component != null) return Component.Matches(host, owner, Handle);
                return Source != null && Source.Matches(host, owner, Handle);
            }

            internal void RecordResult()
            {
                if (Listener != null) Listener.RecordSpatialResult(Result);
                else if (Component != null) Component.RecordSpatialResult(Result);
                else if (Source != null) Source.RecordSpatialResult(Result);
            }

            internal void Publish()
            {
                if (Result != VnResult.VN_OK) { Notify = true; return; }
                if (Listener != null) Result = Listener.PublishPose(in Pose, false, out Notify);
                else if (Component != null) Result = Component.PublishPose(in Pose, out Notify);
                else if (Source != null) Result = Source.PublishPose(in Pose, out Notify);
            }
        }

        internal struct SpatialPost
        {
            internal PackBootstrapSession Session;
            internal ulong ListenerRevision;
            internal SpatialRecord Listener;
            internal SpatialRecord Source;
            internal bool Active;
        }

        private bool SpatialSessionIsCurrent(PackBootstrapSession owner)
            => this && owner != null && owner.IsActive && ReferenceEquals(owner, session) && !shuttingDown;

        private void ReserveSpatialRecords()
        {
            int required = 1 + spatialEmitters.Count + spatialSources.Count;
            if (spatialRecords.Length < required)
                Array.Resize(ref spatialRecords, Math.Max(required, Math.Max(8, spatialRecords.Length * 2)));
        }

        internal void RegisterSpatialEmitter(HarkenEmitter emitter)
        {
            if (!spatialEmitters.Contains(emitter)) spatialEmitters.Add(emitter);
            ReserveSpatialRecords();
        }

        internal void UnregisterSpatialEmitter(HarkenEmitter emitter) => spatialEmitters.Remove(emitter);

        internal void RegisterListener(HarkenListener listener)
        {
            if (!listener.Matches(this, session)) return;
            if (!listenerCandidates.Contains(listener)) listenerCandidates.Add(listener);
            ReserveSpatialRecords();
            if (selectedListener != null && selectedListener != listener)
            {
                listener.RecordSpatialResult(VnResult.VN_IN_USE);
                return;
            }
            SelectListener(listener);
        }

        internal void UnregisterListener(HarkenListener listener)
        {
            listenerCandidates.Remove(listener);
            if (!ReferenceEquals(selectedListener, listener)) return;
            selectedListener = null;
            ++listenerRevision;
        }

        public VnResult SelectListener(HarkenListener listener)
        {
            if (publishingSpatial) return VnResult.VN_IN_USE;
            PackBootstrapSession owner = session;
            if (!SpatialSessionIsCurrent(owner) || !IsReady)
                return ObserveResult("HarkenAudioSystem.SelectListener", VnResult.VN_NOT_INITIALIZED);
            if (listener == null || !listenerCandidates.Contains(listener) || !listener.Matches(this, owner))
                return ObserveResult("HarkenAudioSystem.SelectListener", VnResult.VN_INVALID_ARG);
            VnResult result;
            publishingSpatial = true;
            try
            {
                result = UnitySpatialPose.Capture(listener.transform, out UnitySpatialPose pose);
                if (result == VnResult.VN_OK)
                    result = listener.PublishPose(in pose, true, out _);
                listener.RecordSpatialResult(result);
                if (result == VnResult.VN_OK && !ReferenceEquals(selectedListener, listener))
                {
                    selectedListener = listener;
                    ++listenerRevision;
                }
            }
            finally { publishingSpatial = false; }
            return ObserveResult("HarkenAudioSystem.SelectListener", result);
        }

        private SpatialRecord CaptureListener()
        {
            var record = new SpatialRecord { Listener = selectedListener };
            if (selectedListener != null)
                record.Result = selectedListener.Matches(this, session)
                    ? UnitySpatialPose.Capture(selectedListener.transform, out record.Pose)
                    : VnResult.VN_STALE_HANDLE;
            record.Notify = record.Result != VnResult.VN_OK;
            if (record.Notify) record.RecordResult();
            return record;
        }

        private SpatialRecord CaptureEmitter(HarkenEmitter component)
        {
            var record = new SpatialRecord { Component = component, Handle = component.Handle };
            record.Result = component.Matches(this, session, record.Handle)
                ? UnitySpatialPose.Capture(component.transform, out record.Pose)
                : VnResult.VN_STALE_HANDLE;
            record.Notify = record.Result != VnResult.VN_OK;
            if (record.Notify) record.RecordResult();
            return record;
        }

        private SpatialRecord CaptureSource(HarkenSpatialSource source)
        {
            var record = new SpatialRecord { Source = source };
            record.Result = source.Validate(this, session, out record.Handle);
            if (record.Result == VnResult.VN_OK) record.Result = source.CapturePose(out record.Pose);
            record.Notify = record.Result != VnResult.VN_OK;
            if (record.Notify) record.RecordResult();
            return record;
        }

        internal VnResult BeginSpatialPost(PackBootstrapSession owner, HarkenEmitter component,
            HarkenSpatialSource source, out SpatialPost publication)
        {
            publication = default;
            if (publishingSpatial) return VnResult.VN_IN_USE;
            if (!SpatialSessionIsCurrent(owner)) return VnResult.VN_NOT_INITIALIZED;
            publishingSpatial = true;
            publication.Active = true;
            publication.Session = owner;
            publication.ListenerRevision = listenerRevision;
            publication.Listener = CaptureListener();
            if (component != null) publication.Source = CaptureEmitter(component);
            else if (source != null) publication.Source = CaptureSource(source);
            if (publication.Listener.Result != VnResult.VN_OK) return publication.Listener.Result;
            if (publication.Source.Result != VnResult.VN_OK) return publication.Source.Result;
            publication.Listener.Publish();
            if (publication.Listener.Result != VnResult.VN_OK) return publication.Listener.Result;
            publication.Source.Publish();
            return publication.Source.Result;
        }

        internal void EndSpatialPost(in SpatialPost publication)
        {
            if (publication.Active) publishingSpatial = false;
        }

        internal bool SpatialPostIsCurrent(in SpatialPost publication)
            => !publication.Active || (SpatialSessionIsCurrent(publication.Session) &&
                publication.ListenerRevision == listenerRevision &&
                (ReferenceEquals(publication.Source.Component, null) && publication.Source.Source == null ||
                    publication.Source.IsCurrent(this, publication.Session)));

        internal void NotifySpatialPost(in SpatialPost publication)
        {
            if (!publication.Active) return;
            NotifySpatialRecord(in publication.Listener, publication.Session, publication.ListenerRevision);
            NotifySpatialRecord(in publication.Source, publication.Session, publication.ListenerRevision);
        }

        private void NotifySpatialRecord(in SpatialRecord record, PackBootstrapSession owner, ulong revision)
        {
            if (!record.Notify || !SpatialSessionIsCurrent(owner) || revision != listenerRevision ||
                !record.IsCurrent(this, owner)) return;
            ObserveResult(record.Listener != null ? "UnitySpatial.SetListener" :
                record.Component != null ? "UnitySpatial.SetEmitterTransform" : "SpatialSource.Update", record.Result);
        }

        private int PublishSpatialFrame()
        {
            int count = 0;
            publishingSpatial = true;
            try
            {
                spatialRecords[count++] = CaptureListener();
                for (int i = 0; i < spatialEmitters.Count; ++i)
                {
                    HarkenEmitter component = spatialEmitters[i];
                    if (component != null && component.IsCreated && component.isActiveAndEnabled)
                        spatialRecords[count++] = CaptureEmitter(component);
                }
                for (int i = 0; i < spatialSources.Count; ++i)
                {
                    HarkenSpatialSource source = spatialSources[i];
                    if (source.IsFollowing) spatialRecords[count++] = CaptureSource(source);
                }
                if (spatialRecords[0].Result == VnResult.VN_OK)
                {
                    spatialRecords[0].Publish();
                    if (spatialRecords[0].Result == VnResult.VN_OK)
                        for (int i = 1; i < count; ++i) spatialRecords[i].Publish();
                }
                return count;
            }
            finally { publishingSpatial = false; }
        }

        private void ResetSpatialSession()
        {
            selectedListener = null;
            ++listenerRevision;
            spatialEmitters.Clear();
            listenerCandidates.Clear();
            ReserveSpatialRecords();
        }
    }
}
