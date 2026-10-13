using UnityEngine;
using Harken.Unity;

namespace Harken.Samples.PlayableDemo
{
    public sealed class HarkenPlayableDemoController : MonoBehaviour
    {
        [SerializeField] private HarkenAudioSystem runtime;
        [SerializeField] private HarkenAction[] actions;
        [SerializeField] private string[] labels;
        [SerializeField] private HarkenVariable variable;

        private float variableValue;
        private GUIStyle titleStyle;
        private GUIStyle statusStyle;
        private GUIStyle buttonStyle;

        private void OnGUI()
        {
            EnsureStyles();
            float width = Mathf.Min(760.0f, Screen.width - 32.0f);
            float height = Mathf.Min(600.0f, Screen.height - 32.0f);
            GUILayout.BeginArea(
                new Rect((Screen.width - width) * 0.5f, 16.0f, width, height),
                GUI.skin.box);
            GUILayout.Space(12.0f);
            GUILayout.Label("Harken Playable Audio Demo", titleStyle);
            GUILayout.Label(RuntimeStatus(), statusStyle);
            GUILayout.Space(12.0f);

            GUI.enabled = runtime != null && runtime.IsPreparationComplete;
            for (int i = 0; actions != null && i < actions.Length; i += 2)
            {
                GUILayout.BeginHorizontal();
                DrawActionButton(i);
                DrawActionButton(i + 1);
                GUILayout.EndHorizontal();
                GUILayout.Space(6.0f);
            }

            GUILayout.Space(10.0f);
            GUILayout.BeginHorizontal();
            GUILayout.Label("Variable Control: " + variableValue.ToString("0"));
            if (GUILayout.Button("Set 0", buttonStyle, GUILayout.Height(42.0f)))
            {
                variableValue = 0.0f;
                variable?.SetValue(variableValue);
            }
            if (GUILayout.Button("Set 1", buttonStyle, GUILayout.Height(42.0f)))
            {
                variableValue = 1.0f;
                variable?.SetValue(variableValue);
            }
            GUILayout.EndHorizontal();
            GUI.enabled = true;
            GUILayout.Space(12.0f);
            GUILayout.Label(runtime == null
                ? "No Runtime owner"
                : "Last result: " + runtime.LastOperation + " = " +
                    runtime.LastResult);
            GUILayout.EndArea();
        }

        private void DrawActionButton(int index)
        {
            if (actions == null || index >= actions.Length || actions[index] == null)
            {
                GUILayout.FlexibleSpace();
                return;
            }
            string label = labels != null && index < labels.Length && !string.IsNullOrEmpty(labels[index])
                ? labels[index]
                : actions[index].ActionName;
            if (GUILayout.Button("Play " + label, buttonStyle,
                GUILayout.Height(54.0f)))
            {
                actions[index].Play();
            }
        }

        private string RuntimeStatus()
        {
            if (runtime == null)
            {
                return "Runtime missing";
            }
            if (!runtime.IsReady) return runtime.State + " - " + runtime.LastResult;
            if (runtime.Session != null && !runtime.Session.IsStartupComplete)
                return "PREPARATION FAILED - Pack " + runtime.Session.FailedStartupPackStableId +
                    " - " + runtime.Session.StartupResult;
            return runtime.IsPreparationComplete ? "READY" : "PREPARING";
        }

        private void EnsureStyles()
        {
            if (titleStyle != null)
            {
                return;
            }
            titleStyle = new GUIStyle(GUI.skin.label)
            {
                alignment = TextAnchor.MiddleCenter,
                fontSize = 26,
                fontStyle = FontStyle.Bold
            };
            statusStyle = new GUIStyle(GUI.skin.label)
            {
                alignment = TextAnchor.MiddleCenter,
                fontSize = 16,
                fontStyle = FontStyle.Bold
            };
            buttonStyle = new GUIStyle(GUI.skin.button)
            {
                fontSize = 15,
                fontStyle = FontStyle.Bold
            };
        }
    }
}
