using System.IO;
using System.Collections;
using NUnit.Framework;
using UnityEditor;
using PackageInfo = UnityEditor.PackageManager.PackageInfo;
using UnityEngine.TestTools;
using Harken.Unity.Editor;

namespace Harken.Unity.Tests
{
    public sealed class HarkenPluginImporterTests
    {
        [Test]
        public void ResolvesTheInstalledSdkInsteadOfAPresetDirectory()
        {
            string root = HarkenPluginImporterSetup.FindPluginRoot();
            Assert.That(root, Is.Not.Null);
            Assert.That(AssetDatabase.LoadAssetAtPath<MonoScript>(
                root.Replace("Runtime/Plugins/", "Editor/HarkenPluginImporterSetup.cs")), Is.Not.Null);
        }

        [TestCase("Assets/Audio/SDK/Runtime/Plugins/x86_64/HarkenCore.dll", true)]
        [TestCase("Assets/Audio/SDK/Runtime/Plugins", true)]
        [TestCase("Assets/Audio/SDK", true)]
        [TestCase("Assets/Audio", true)]
        [TestCase("Assets/Audio/SDK-other/Runtime/Plugins/x86_64/HarkenCore.dll", false)]
        [TestCase("Assets/Audio/Plugins/x86_64/HarkenCore.dll", false)]
        [TestCase("Assets/Other/HarkenCore.dll", false)]
        public void RecognizesMovesWithoutClaimingNeighbouringPlugins(string path, bool expected)
        {
            Assert.That(HarkenPluginImporterSetup.IsRelevantAsset(path,
                "Assets/Audio/SDK/Runtime/Plugins/"), Is.EqualTo(expected));
        }

        [TestCase("Assets/NewSdk/Editor/HarkenPluginImporterSetup.cs")]
        [TestCase("Assets/NewSdk/package.json")]
        public void ScriptAndManifestImportCanRecoverAnUnresolvedRoot(string path)
        {
            Assert.That(HarkenPluginImporterSetup.IsRelevantAsset(path, null), Is.True);
            Assert.That(HarkenPluginImporterSetup.IsRelevantAsset("Assets/Other/HarkenCore.dll", null), Is.False);
        }

        [UnityTest]
        public IEnumerator MovingNativeBackIntoSdkConfiguresItWithoutTouchingItOutside()
        {
            string root = HarkenPluginImporterSetup.FindPluginRoot();
            Assert.That(root, Is.Not.Null);
            string path = root + "Android/arm64-v8a/libHarkenCore.so";
            if (!path.StartsWith("Assets/", System.StringComparison.Ordinal))
                Assert.Ignore("Package assets cannot be moved using the Assets workflow.");
            if (!(AssetImporter.GetAtPath(path) is PluginImporter))
                Assert.Ignore("Android ARM64 library is not installed.");
            string folder = "Assets/HarkenPluginImporterMoveTest";
            Assert.That(AssetDatabase.IsValidFolder(folder), Is.False);
            AssetDatabase.CreateFolder("Assets", "HarkenPluginImporterMoveTest");
            string outside = folder + "/libHarkenCore.so";
            string guid = AssetDatabase.AssetPathToGUID(path);
            try
            {
                Assert.That(AssetDatabase.MoveAsset(path, outside), Is.Empty);
                var importer = (PluginImporter)AssetImporter.GetAtPath(outside);
                importer.SetCompatibleWithPlatform(BuildTarget.Android, false);
                importer.SaveAndReimport();
                byte[] outsideMeta = File.ReadAllBytes(outside + ".meta");
                HarkenPluginImporterSetup.ConfigureImporters();
                Assert.That(File.ReadAllBytes(outside + ".meta"), Is.EqualTo(outsideMeta));
                Assert.That(AssetDatabase.MoveAsset(outside, path), Is.Empty);
                AssetDatabase.Refresh(ImportAssetOptions.ForceSynchronousImport);
                for (int frame = 0; frame < 30; ++frame)
                {
                    if (((PluginImporter)AssetImporter.GetAtPath(path)).GetCompatibleWithPlatform(BuildTarget.Android))
                        break;
                    yield return null;
                }
                Assert.That(((PluginImporter)AssetImporter.GetAtPath(path))
                    .GetCompatibleWithPlatform(BuildTarget.Android), Is.True);
                Assert.That(AssetDatabase.AssetPathToGUID(path), Is.EqualTo(guid));
            }
            finally
            {
                if (AssetImporter.GetAtPath(outside) != null)
                    Assert.That(AssetDatabase.MoveAsset(outside, path), Is.Empty,
                        "Native library recovery failed; the temporary folder is retained.");
                Assert.That(File.Exists(outside), Is.False,
                    "Native library remains outside the SDK; the temporary folder is retained.");
                HarkenPluginImporterSetup.ConfigureImporters();
                AssetDatabase.DeleteAsset(folder);
            }
        }

        [TestCase("x86_64/HarkenCore.dll", BuildTarget.StandaloneWindows64, "x86_64", "Windows")]
        [TestCase("x86_64/libHarkenCore.so", BuildTarget.StandaloneLinux64, "x86_64", "Linux")]
        [TestCase("macOS/libHarkenCore.dylib", BuildTarget.StandaloneOSX, "AnyCPU", "OSX")]
        [TestCase("Android/arm64-v8a/libHarkenCore.so", BuildTarget.Android, "ARM64", null)]
        [TestCase("iOS/libHarkenCore.a", BuildTarget.iOS, "AnyCPU", null)]
        public void ImportedNativeLibraryHasOnlyItsIntendedPlatformsAndIsStable(
            string relativePath, BuildTarget target, string cpu, string editorOs)
        {
            string root = HarkenPluginImporterSetup.FindPluginRoot();
            Assert.That(root, Is.Not.Null);
            string path = root + relativePath;
            PackageInfo package = PackageInfo.FindForAssetPath(path);
            string physical = package == null ? path :
                Path.Combine(package.resolvedPath, path.Substring(package.assetPath.Length + 1));
            var importer = AssetImporter.GetAtPath(path) as PluginImporter;
            if (importer == null)
            {
                Assert.That(File.Exists(physical), Is.False, "Installed native library has no PluginImporter: " + path);
                Assert.Ignore("Native target is not installed: " + path);
            }

            Assert.That(importer.GetCompatibleWithAnyPlatform(), Is.False, path);
            Assert.That(importer.GetCompatibleWithEditor(), Is.EqualTo(editorOs != null), path);
            foreach (BuildTarget platform in new[] { BuildTarget.StandaloneWindows64,
                BuildTarget.StandaloneLinux64, BuildTarget.StandaloneOSX, BuildTarget.Android, BuildTarget.iOS })
                Assert.That(importer.GetCompatibleWithPlatform(platform), Is.EqualTo(platform == target), path);
            Assert.That(importer.GetPlatformData(target, "CPU"), Is.EqualTo(cpu), path);
            if (editorOs != null)
            {
                Assert.That(importer.GetEditorData("OS"), Is.EqualTo(editorOs), path);
                Assert.That(importer.GetEditorData("CPU"), Is.EqualTo(cpu), path);
            }
            if (target == BuildTarget.StandaloneWindows64)
                Assert.That(importer.GetPlatformData(target, "OS"), Is.EqualTo("Windows"));
            if (target == BuildTarget.iOS)
                Assert.That(importer.GetPlatformData(target, "AddToEmbeddedBinaries"), Is.EqualTo("false"));

            byte[] before = File.ReadAllBytes(physical + ".meta");
            var modified = File.GetLastWriteTimeUtc(physical + ".meta");
            HarkenPluginImporterSetup.ConfigureImporters();
            HarkenPluginImporterSetup.ConfigureImporters();
            Assert.That(File.ReadAllBytes(physical + ".meta"), Is.EqualTo(before));
            Assert.That(File.GetLastWriteTimeUtc(physical + ".meta"), Is.EqualTo(modified));
        }
    }
}
