using System.Reflection;
using NUnit.Framework;
using UnityEngine;
using Harken.Unity.Editor;
using Object = UnityEngine.Object;

namespace Harken.Unity.Tests
{
    public sealed class HarkenEditorServiceTests
    {
        [Test]
        public void InspectorStaysUnboundUntilAPublicationIsSelected()
        {
            var owner = new GameObject("Unbound Inspector");
            owner.SetActive(false);
            UnityEditor.Editor inspector = null;
            try
            {
                var action = owner.AddComponent<HarkenAction>();
                inspector = UnityEditor.Editor.CreateEditor(action, typeof(HarkenActionInspector));
                var catalog = Get<HarkenEditorCatalog>(inspector, "catalog");
                Assert.That(catalog.Catalog, Is.Null);
                Assert.That(Get<string>(inspector, "directory"), Is.Null.Or.Empty);
                Assert.That(catalog.CheckReference("Play_U4_A", catalog.Catalog?.ProjectId, out string diagnostic),
                    Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
            }
            finally
            {
                if (inspector != null) Object.DestroyImmediate(inspector);
                Object.DestroyImmediate(owner);
            }
        }

        private static T Get<T>(object target, string name)
            => (T)target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).GetValue(target);
    }
}
