#if UNITY_EDITOR
using UnityEditor;

namespace Harken.Unity.Editor
{
    internal static class HarkenEditorLifecycle
    {
        private const uint HostOutputMuteFadeMs = 10;
        private static bool hostOutputMuted;

        [InitializeOnLoadMethod]
        private static void Register()
        {
            EditorApplication.playModeStateChanged -= HandlePlayModeState;
            EditorApplication.playModeStateChanged += HandlePlayModeState;
            AssemblyReloadEvents.beforeAssemblyReload -= Shutdown;
            AssemblyReloadEvents.beforeAssemblyReload += Shutdown;
            EditorApplication.quitting -= Shutdown;
            EditorApplication.quitting += Shutdown;
            hostOutputMuted = EditorUtility.audioMasterMute;
            HarkenEngine.Initialized -= ApplyHostOutputMute;
            HarkenEngine.Initialized += ApplyHostOutputMute;
            EditorApplication.update -= SyncHostOutputMute;
            EditorApplication.update += SyncHostOutputMute;
        }

        private static void SyncHostOutputMute()
        {
            bool muted = EditorUtility.audioMasterMute;
            if (muted == hostOutputMuted) return;
            hostOutputMuted = muted;
            ApplyHostOutputMute();
        }

        private static void ApplyHostOutputMute()
            => HarkenEngine.SetHostOutputMute(hostOutputMuted ? 1u : 0u, HostOutputMuteFadeMs);

        private static void HandlePlayModeState(PlayModeStateChange state)
        {
            HarkenEditorPreview.ShutdownOwnedSession();
            if (state == PlayModeStateChange.ExitingPlayMode &&
                HarkenAudioSystem.Instance != null)
            {
                HarkenAudioSystem.Instance.ShutdownRuntime();
            }
        }

        private static void Shutdown()
        {
            HarkenEditorPreview.ShutdownOwnedSession();
            if (HarkenAudioSystem.Instance != null) HarkenAudioSystem.Instance.ShutdownRuntime();
        }
    }
}
#endif
