using UnityEngine;
using Harken;

namespace Harken.Unity
{
    [CreateAssetMenu(menuName = "Harken/Variable Reference", fileName = "HarkenVariable")]
    public sealed class HarkenVariableReference : ScriptableObject
    {
        [SerializeField] private HarkenVariableId variable;

        public HarkenVariableId Variable => variable;
    }
}
