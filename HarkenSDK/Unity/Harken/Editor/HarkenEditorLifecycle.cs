#if UNITY_EDITOR
using UnityEditor;

namespace Harken.Unity.Editor
{
    internal static class HarkenEditorLifecycle
    {
        [InitializeOnLoadMethod]
        private static void Register()
        {
            EditorApplication.playModeStateChanged -= HandlePlayModeState;
            EditorApplication.playModeStateChanged += HandlePlayModeState;
            AssemblyReloadEvents.beforeAssemblyReload -= Shutdown;
            AssemblyReloadEvents.beforeAssemblyReload += Shutdown;
            EditorApplication.quitting -= Shutdown;
            EditorApplication.quitting += Shutdown;
        }

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
