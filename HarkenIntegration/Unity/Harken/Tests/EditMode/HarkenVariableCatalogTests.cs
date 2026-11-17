using System;
using System.IO;
using System.Reflection;
using NUnit.Framework;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;
using UnityEngine.TestTools;
using Harken;
using Harken.Unity.Editor;
using Object = UnityEngine.Object;

namespace Harken.Unity.Tests
{
    public sealed class HarkenVariableCatalogTests
    {
        private static class Symbols
        {
            public const ulong ExecutionCatalogFingerprint = 0x1111UL;
            public const ulong CatalogFingerprint = 0x2222UL;
            public static readonly VnObjectId VehicleRPM = VnObjectId.Parse("67e0c65b-f36e-40d9-80ee-75af00ecf712");
        }

        private static void RegisterSymbols() => Assert.That(
            Values.RegisterGeneratedCatalog(Symbols.ExecutionCatalogFingerprint, Symbols.CatalogFingerprint,
                typeof(Symbols)), Is.EqualTo(VnResult.VN_OK));

        [TearDown]
        public void TearDown()
        {
            VariableCatalogGuard.ResetTestState();
            VariableGameplay.GameThreadProbe = null;
        }

        [Test]
        public void GuardComparesOnlyAgainstARegisteredCatalog()
        {
            var live = new VnVariableCatalogInfoV1 { structSize = 24, executionCatalogFingerprint = 0x9999UL };
            Assert.That(VariableCatalogGuard.Evaluate(in live), Is.EqualTo(VnResult.VN_OK));
            Assert.That(Values.RegisterGeneratedCatalog(1UL, 2UL, null), Is.EqualTo(VnResult.VN_INVALID_ARG));
            RegisterSymbols();
            Assert.That(VariableCatalogGuard.Evaluate(in live), Is.EqualTo(VnResult.VN_VARIABLE_CATALOG_MISMATCH));
            live.executionCatalogFingerprint = Symbols.ExecutionCatalogFingerprint;
            Assert.That(VariableCatalogGuard.Evaluate(in live), Is.EqualTo(VnResult.VN_OK));
        }

        [Test]
        public void DrawerResolvesOnlyRegisteredSymbols()
        {
            Assert.That(HarkenVariableIdDrawer.TryResolve("VehicleRPM", out _), Is.False);
            RegisterSymbols();
            Assert.That(HarkenVariableIdDrawer.TryResolve("VehicleRPM", out VnObjectId id), Is.True);
            Assert.That(id, Is.EqualTo(Symbols.VehicleRPM));
            Assert.That(HarkenVariableIdDrawer.TryResolve("ExecutionCatalogFingerprint", out _), Is.False);
        }

        [Test]
        public void EmitterRequiresVariableRowsAndRejectsInvalidFields()
        {
            Assert.That(VariableCatalogEmitter.EmitCSharpFromJson("{\"variables\":[]}", "empty", out string empty, out _),
                Is.EqualTo(VnResult.VN_OK));
            Assert.That(empty, Is.Not.Null);
            foreach (string invalid in new[] { "{}", "{\"variableCatalog\":{\"variables\":[]}}",
                "{\"variables\":null}", "{\"variables\":{}}", "{\"variables\":true}" })
            {
                Assert.That(VariableCatalogEmitter.EmitCSharpFromJson(invalid, "invalid", out string refused, out string diagnostic),
                    Is.EqualTo(VnResult.VN_BAD_PACK));
                Assert.That(refused, Is.Null);
                Assert.That(diagnostic, Is.Not.Empty);
            }
            const string valid = "{\"variables\":[{\"variableId\":\"3da91a64-b947-4560-0000-000000000003\",\"kind\":0,\"scopePolicy\":1," +
                "\"writers\":1,\"rangeMinBits\":0,\"rangeMaxBits\":1065353216,\"defaultBits\":0," +
                "\"codeName\":\"Demo_Choice\"}]}";
            Assert.That(VariableCatalogEmitter.EmitCSharpFromJson(valid, "valid", out string csharp, out string error),
                Is.EqualTo(VnResult.VN_OK), error);
            Assert.That(csharp, Does.Contain("Demo_Choice = new VnObjectId(0x3da91a64b9474560UL, 0x0000000000000003UL)"));

            string badPrecision = valid.Replace("\"codeName\":\"Demo_Choice\"",
                "\"codeName\":\"Demo_Choice\",\"precision\":\"nope\"");
            Assert.That(VariableCatalogEmitter.EmitCSharpFromJson(badPrecision, "bad-precision", out csharp, out error),
                Is.EqualTo(VnResult.VN_BAD_PACK));
            Assert.That(csharp, Is.Null);

            string badLabels = valid.Replace("\"codeName\":\"Demo_Choice\"",
                "\"codeName\":\"Demo_Choice\",\"labels\":\"none\"");
            Assert.That(VariableCatalogEmitter.EmitCSharpFromJson(badLabels, "bad-labels", out csharp, out error),
                Is.EqualTo(VnResult.VN_BAD_PACK));
            Assert.That(csharp, Is.Null);
        }

        [Test]
        public void EmitterCommentDoesNotBreakOutOfGeneratedHeader()
        {
            const string valid = "{\"variables\":[{\"variableId\":\"3da91a64-b947-4560-0000-000000000003\",\"kind\":0,\"scopePolicy\":1," +
                "\"writers\":1,\"rangeMinBits\":0,\"rangeMaxBits\":1065353216,\"defaultBits\":0," +
                "\"codeName\":\"Demo_Choice\"}]}";
            string injected = "C:/packs/HarkenPackManifest.json\r\npublic class Injected {}";
            Assert.That(VariableCatalogEmitter.EmitCSharpFromJson(valid, injected, out string csharp, out string error),
                Is.EqualTo(VnResult.VN_OK), error);
            Assert.That(csharp, Does.Not.Contain("\npublic class Injected"));
            Assert.That(csharp, Does.Contain("// C:/packs/HarkenPackManifest.json  public class Injected {}."));
        }

        [Test]
        public void HarkenVariableIdSurvivesPrefabAndInstantiate()
        {
            string folder = "Assets/HarkenVariablePrefab_" + Guid.NewGuid().ToString("N");
            AssetDatabase.CreateFolder("Assets", folder.Substring("Assets/".Length));
            Scene scene = default;
            try
            {
                scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene,
                    NewSceneMode.Single);
                var owner = new GameObject("Variable");
                owner.SetActive(false);
                SceneManager.MoveGameObjectToScene(owner, scene);
                HarkenVariable component = owner.AddComponent<HarkenVariable>();
                SerializedObject serialized = new SerializedObject(component);
                SerializedProperty id = serialized.FindProperty("variable");
                VnObjectId expected = Typed("VehicleRPM").stableId;
                id.FindPropertyRelative("stableId").FindPropertyRelative("high").ulongValue = expected.high;
                id.FindPropertyRelative("stableId").FindPropertyRelative("low").ulongValue = expected.low;
                id.FindPropertyRelative("codeName").stringValue = "VehicleRPM";
                serialized.ApplyModifiedPropertiesWithoutUndo();
                PrefabUtility.SaveAsPrefabAsset(owner, folder + "/Variable.prefab");
                Object.DestroyImmediate(owner);
                var instance = (GameObject)PrefabUtility.InstantiatePrefab(
                    AssetDatabase.LoadAssetAtPath<GameObject>(folder + "/Variable.prefab"),
                    scene);
                serialized = new SerializedObject(instance.GetComponent<HarkenVariable>());
                id = serialized.FindProperty("variable");
                Assert.That(id.FindPropertyRelative("stableId").FindPropertyRelative("high").ulongValue,
                    Is.EqualTo(expected.high));
                Assert.That(id.FindPropertyRelative("stableId").FindPropertyRelative("low").ulongValue,
                    Is.EqualTo(expected.low));
                Assert.That(id.FindPropertyRelative("codeName").stringValue,
                    Is.EqualTo("VehicleRPM"));
                Assert.That(instance.GetComponent<HarkenVariable>().VariableStableId,
                    Is.EqualTo(expected));
            }
            finally
            {
                if (scene.IsValid() && scene.isLoaded)
                {
                    EditorSceneManager.CloseScene(scene, true);
                }
                AssetDatabase.DeleteAsset(folder);
            }
        }

        [Test]
        public void TypedWriteRejectsOffThreadBeforeNativeQuery()
        {
            VariableGameplay.GameThreadProbe = () => false;
            VariableCatalogGuard.Query = (ref VnVariableCatalogInfoV1 info) =>
                throw new InvalidOperationException("Native query must not run off-thread.");
            Assert.That(Values.SetValue(Typed("VehicleRPM"), 1), Is.EqualTo(VnResult.VN_INVALID_ARG));
        }

        [Test]
        public void TypedGuardCallbackCannotWriteAcrossSessionReplacement()
        {
            RegisterSymbols();
            VariableCatalogGuard.Query = (ref VnVariableCatalogInfoV1 info) =>
            {
                info.executionCatalogFingerprint = Symbols.ExecutionCatalogFingerprint;
                info.sdkCatalogFingerprint = Symbols.CatalogFingerprint ^ 1UL;
                return VnResult.VN_OK;
            };
            VariableCatalogGuard.DisplayMismatchHandler = (live, generated) =>
                VariableGameplay.NotifySessionChanged();
            LogAssert.Expect(LogType.Warning, "Harken Variable API catalog is stale; write still proceeds.");
            Assert.That(Values.SetValue(Typed("VehicleRPM"), 1), Is.EqualTo(VnResult.VN_NOT_INITIALIZED));
            Assert.That(VariableCatalogGuard.DisplayMismatchCount, Is.EqualTo(1));
        }

        [Test]
        public void DestroyedEmitterComponentNeverBecomesGlobalWrite()
        {
            var owner = new GameObject("VariableEmitterScope");
            owner.SetActive(false);
            try
            {
                var component = owner.AddComponent<HarkenVariable>();
                var emitter = owner.AddComponent<HarkenEmitter>();
                component.Variable = Typed("VehicleRPM");
                typeof(HarkenVariable).GetField("emitter", BindingFlags.Instance | BindingFlags.NonPublic)
                    .SetValue(component, emitter);
                Object.DestroyImmediate(emitter);
                Assert.That(ReferenceEquals(emitter, null), Is.False);
                Assert.That(emitter == null, Is.True);
                Assert.That(component.Apply(), Is.EqualTo(VnResult.VN_STALE_HANDLE));
            }
            finally
            {
                Object.DestroyImmediate(owner);
            }
        }

        [Test]
        public void LocalClearAndPluralSettersAreAbsentAndScalarSettersRemainPublic()
        {
            Assert.That(typeof(Emitter).GetMethod("ClearValue"), Is.Null);
            Assert.That(typeof(Emitter).GetMethod("ClearValues"), Is.Null);
            Assert.That(typeof(HarkenEmitter).GetMethod("ResetVariables"), Is.Null);
            Assert.That(typeof(Emitter).GetMethod("SetValue"), Is.Null);
            Assert.That(typeof(Emitter).GetMethod("SetValues"), Is.Null);
            Assert.That(typeof(Values).GetMethod("SetValues"), Is.Null);
            Assert.That(typeof(Values).GetMethod("ResetValues"), Is.Null);
            MethodInfo scalar = typeof(Values).GetMethod("SetValue",
                new[] { typeof(VnObjectId), typeof(float), typeof(Emitter?) });
            Assert.That(scalar, Is.Not.Null);
            Assert.That(scalar.GetParameters()[2].ParameterType, Is.EqualTo(typeof(Emitter?)));
            Assert.That(scalar.GetParameters()[2].IsOptional, Is.True);
            MethodInfo named = typeof(Values).GetMethod("SetValue",
                new[] { typeof(string), typeof(string), typeof(float), typeof(Emitter?) });
            Assert.That(named, Is.Not.Null);
            Assert.That(named.GetParameters()[3].IsOptional, Is.True);
            Assert.That(typeof(Actions).GetMethod("Post",
                new[] { typeof(string), typeof(Emitter), typeof(ActionFiring).MakeByRefType() }), Is.Not.Null);
        }

        private static HarkenVariableId Typed(string name)
        {
            return HarkenVariableId.FromGenerated(name, Symbols.VehicleRPM);
        }
    }
}
