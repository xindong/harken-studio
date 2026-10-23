using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using UnityEngine;

[assembly: InternalsVisibleTo("Harken.Unity.Editor")]

namespace Harken.Unity
{
    public sealed class HarkenSpatialSource : IDisposable
    {
        private readonly object owner;
        private readonly PackBootstrapSession session;
        private Transform target;
        private Emitter emitter;
        private int users;
        private bool ended;
        private Vector3 position;
        private Quaternion rotation;
        private bool published;

        internal HarkenSpatialSource(object owner, PackBootstrapSession session, Transform target, Emitter emitter)
        {
            this.owner = owner;
            this.session = session;
            this.target = target;
            this.emitter = emitter;
        }

        public bool IsFollowing => !ended && session.IsActive && target != null && target.gameObject.activeInHierarchy;
        public VnResult LastResult { get; private set; } = VnResult.VN_OK;
        internal bool IsReleased => !emitter.IsValid || !session.IsActive;
        internal bool Matches(Transform value, PackBootstrapSession current)
            => IsFollowing && target == value && ReferenceEquals(session, current);

        internal bool Matches(object expectedOwner, PackBootstrapSession expectedSession, Emitter handle)
            => IsFollowing && ReferenceEquals(owner, expectedOwner) && ReferenceEquals(session, expectedSession) &&
                emitter.IsValid && emitter.Handle == handle.Handle;

        public VnResult EndFollow()
        {
            if (ended) return LastResult;
            VnResult result = Publish();
            ended = true;
            target = null;
            return LastResult = result;
        }

        public void Dispose() => EndFollow();

        /// <summary>Changes the followed identity's range; an ended source cannot be changed.</summary>
        public VnResult SetDistanceScale(float scale)
        {
            if (owner is HarkenAudioSystem runtime)
            {
                if (runtime == null || !ReferenceEquals(runtime.Session, session))
                    return LastResult = VnResult.VN_STALE_HANDLE;
                if (runtime.IsPublishingSpatial) return LastResult = VnResult.VN_IN_USE;
            }
            VnResult result = Validate(owner, session, out Emitter handle);
            return LastResult = result == VnResult.VN_OK ? handle.SetDistanceScale(scale) : result;
        }

        internal VnResult Validate(object expectedOwner, PackBootstrapSession expectedSession, out Emitter handle)
        {
            handle = default;
            if (!ReferenceEquals(owner, expectedOwner) || !ReferenceEquals(session, expectedSession) ||
                !session.IsActive || !emitter.IsValid)
                return LastResult = VnResult.VN_STALE_HANDLE;
            if (!IsFollowing) return LastResult = VnResult.VN_INVALID_ARG;
            handle = emitter;
            return VnResult.VN_OK;
        }

        internal VnResult Retain(object expectedOwner, PackBootstrapSession expectedSession)
        {
            VnResult result = Validate(expectedOwner, expectedSession, out _);
            if (result == VnResult.VN_OK) ++users;
            return result;
        }

        internal void Release() { if (users > 0) --users; }

        internal VnResult Maintain()
        {
            if (!session.IsActive)
            {
                Invalidate();
                return VnResult.VN_OK;
            }
            if (target == null || !target.gameObject.activeInHierarchy)
            {
                ended = true;
                target = null;
            }
            VnResult result = VnResult.VN_OK;
            if (ended && users == 0 && emitter.IsValid)
            {
                VnResult released = emitter.Destroy();
                if (released == VnResult.VN_OK || released == VnResult.VN_STALE_HANDLE)
                    emitter = default;
                else result = released;
            }
            return LastResult = result;
        }

        internal void Invalidate()
        {
            ended = true;
            target = null;
            emitter = default;
            users = 0;
        }

        internal VnResult CapturePose(out UnitySpatialPose pose)
            => UnitySpatialPose.Capture(target, out pose);

        internal void RecordSpatialResult(VnResult result) => LastResult = result;

        internal VnResult PublishPose(in UnitySpatialPose pose, out bool changed)
        {
            changed = !published || pose.Position != position || pose.Rotation != rotation;
            if (!changed) return LastResult = VnResult.VN_OK;
            VnResult result = UnitySpatial.SetEmitterTransform(emitter, pose.Position, pose.Forward);
            if (result == VnResult.VN_OK)
            {
                position = pose.Position;
                rotation = pose.Rotation;
                published = true;
            }
            return LastResult = result;
        }

        internal VnResult Publish()
        {
            if (!session.IsActive) return LastResult = VnResult.VN_NOT_INITIALIZED;
            if (ended) return VnResult.VN_OK;
            if (target == null || !target.gameObject.activeInHierarchy)
            {
                ended = true;
                target = null;
                return VnResult.VN_OK;
            }
            VnResult result = CapturePose(out UnitySpatialPose pose);
            if (result == VnResult.VN_OK) result = PublishPose(in pose, out _);
            return LastResult = result;
        }
    }

    internal sealed class HarkenSpatialSources
    {
        private readonly List<HarkenSpatialSource> sources = new List<HarkenSpatialSource>();
        internal int Count => sources.Count;
        internal HarkenSpatialSource this[int index] => sources[index];

        internal VnResult Create(object owner, PackBootstrapSession session, Transform target, out HarkenSpatialSource source)
        {
            source = null;
            if (session == null || !session.IsActive) return VnResult.VN_NOT_INITIALIZED;
            if (target == null || !target.gameObject.activeInHierarchy) return VnResult.VN_INVALID_ARG;
            foreach (HarkenSpatialSource existing in sources)
                if (existing.Matches(target, session)) { source = existing; return VnResult.VN_OK; }
            VnResult result = Emitter.Create(out Emitter emitter);
            if (result != VnResult.VN_OK) return result;
            var created = new HarkenSpatialSource(owner, session, target, emitter);
            sources.Add(created);
            result = created.CapturePose(out _);
            if (result == VnResult.VN_OK) source = created;
            else
            {
                created.EndFollow();
                created.Maintain();
            }
            return result;
        }

        internal VnResult Maintain()
        {
            VnResult first = VnResult.VN_OK;
            for (int i = sources.Count - 1; i >= 0; --i)
            {
                VnResult result = sources[i].Maintain();
                if (first == VnResult.VN_OK) first = result;
                if (sources[i].IsReleased) sources.RemoveAt(i);
            }
            return first;
        }

        internal VnResult Publish()
        {
            VnResult first = VnResult.VN_OK;
            for (int i = 0; i < sources.Count; ++i)
            {
                VnResult result = sources[i].Publish();
                if (first == VnResult.VN_OK) first = result;
            }
            return first;
        }

        internal void EndAll()
        {
            foreach (HarkenSpatialSource source in sources) source.EndFollow();
        }

        internal void ClearEndedSession()
        {
            foreach (HarkenSpatialSource source in sources) source.Invalidate();
            sources.Clear();
        }
    }
}
