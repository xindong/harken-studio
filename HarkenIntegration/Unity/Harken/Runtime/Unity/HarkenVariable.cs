using UnityEngine;
using Harken;

namespace Harken.Unity
{
    public sealed class HarkenVariable : MonoBehaviour
    {
        [SerializeField] private HarkenAudioSystem runtime;
        [SerializeField] private HarkenEmitter emitter;
        [SerializeField] private HarkenVariableId variable;
        [SerializeField] private string variableName = string.Empty;
        [SerializeField] private string projectId = string.Empty;
        [SerializeField] private float value;
        [SerializeField] private bool setOnEnable = true;

        private bool pendingApply;
        private int applyAttempts;
        private bool runtimeBound;
        private const int MaxStartupFrames = 120;

        public VnObjectId VariableStableId => variable.stableId;
        public string VariableName => variableName ?? string.Empty;
        public string ProjectId => projectId ?? string.Empty;
        public HarkenVariableId Variable
        {
            get => variable;
            set
            {
                variable = value;
                variableName = string.Empty;
                projectId = string.Empty;
            }
        }
        public float Value => value;
        public VnResult LastResult { get; private set; } = VnResult.VN_OK;

        private void OnEnable()
        {
            pendingApply = setOnEnable;
            applyAttempts = 0;
            TryBindRuntime();
        }

        private void Update()
        {
            if (!TryBindRuntime())
            {
                AgePending();
                return;
            }
            if (!pendingApply)
            {
                return;
            }
            VnResult result = Apply();
            if (result != VnResult.VN_NOT_INITIALIZED ||
                ++applyAttempts >= MaxStartupFrames)
            {
                pendingApply = false;
            }
        }

        private bool TryBindRuntime()
        {
            if (runtimeBound && runtime != null)
            {
                return true;
            }
            runtimeBound = false;
            runtime = runtime != null ? runtime : HarkenAudioSystem.Instance;
            if (runtime == null)
            {
                LastResult = VnResult.VN_NOT_INITIALIZED;
                return false;
            }
            runtime.BecameReady += HandleRuntimeReady;
            runtimeBound = true;
            if (runtime.IsReady && setOnEnable)
            {
                pendingApply = true;
            }
            return true;
        }

        public VnResult SetValue(float nextValue)
        {
            value = nextValue;
            pendingApply = false;
            applyAttempts = 0;
            return Apply();
        }

        public VnResult Apply()
        {
            if (!ReferenceEquals(emitter, null) && emitter == null)
            {
                return Observe(VnResult.VN_STALE_HANDLE);
            }
            bool typedOnly = string.IsNullOrEmpty(variableName) && string.IsNullOrEmpty(projectId);
            if (typedOnly ? !variable.IsAssigned :
                (string.IsNullOrEmpty(variableName) || string.IsNullOrEmpty(projectId)))
            {
                return Observe(VnResult.VN_INVALID_ARG);
            }
            if (runtime == null)
            {
                return Observe(VnResult.VN_NOT_INITIALIZED);
            }
            if (!typedOnly && variable.IsAssigned)
            {
                VnResult resolved = Values.Resolve(variableName, projectId, out VnObjectId namedId);
                if (resolved != VnResult.VN_OK) return Observe(resolved);
                if (namedId != variable.stableId) return Observe(VnResult.VN_INVALID_ARG);
            }
            VnResult result;
            if (emitter != null)
            {
                result = emitter.IsCreated
                    ? (typedOnly ? Values.SetValue(variable, value, emitter.Handle)
                        : Values.SetValue(variableName, projectId, value, emitter.Handle))
                    : VnResult.VN_NOT_INITIALIZED;
            }
            else
            {
                result = typedOnly ? Values.SetValue(variable, value)
                    : Values.SetValue(variableName, projectId, value);
            }
            return Observe(result);
        }

        private void AgePending()
        {
            if (!pendingApply)
            {
                return;
            }
            if (++applyAttempts >= MaxStartupFrames)
            {
                pendingApply = false;
                LastResult = VnResult.VN_NOT_INITIALIZED;
            }
        }

        private void HandleRuntimeReady(HarkenAudioSystem owner)
        {
            if (setOnEnable)
            {
                pendingApply = true;
                applyAttempts = 0;
            }
        }

        private VnResult Observe(VnResult result)
        {
            LastResult = result;
            return runtime != null
                ? runtime.ObserveResult(
                    "HarkenVariable.SetValue(" + VariableName + ")", result)
                : result;
        }

        private void OnDisable()
        {
            if (runtimeBound && runtime != null)
            {
                runtime.BecameReady -= HandleRuntimeReady;
            }
            runtimeBound = false;
            pendingApply = false;
            applyAttempts = 0;
        }
    }
}
