#if !UNITY_5_3_OR_NEWER
using System;

namespace AOT
{
    // Compile-time shim so the SDK callback thunks build outside Unity (the
    // dotnet ABI / zero-GC harness). In a Unity build UnityEngine provides the
    // real AOT.MonoPInvokeCallbackAttribute and this shim is compiled out. The
    // attribute is advisory to the IL2CPP AOT compiler and a no-op at runtime,
    // so the shim being empty is behaviorally correct off-Unity.
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = false)]
    internal sealed class MonoPInvokeCallbackAttribute : Attribute
    {
        public MonoPInvokeCallbackAttribute(Type delegateType)
        {
            DelegateType = delegateType;
        }

        public Type DelegateType { get; }
    }
}
#endif
