using System;
using System.Reflection;
using System.IO;
using NUnit.Framework;
using UnityEditor;
using UnityEditor.Build;
using UnityEngine;
using Harken;
using Harken.Unity.Editor;

namespace Harken.Unity.Tests
{
    public sealed class HarkenProjectSettingsTests
    {
        private HarkenProjectSettings settings;
        private string original;
        private bool existed;
        private string duplicateFolder;

        [SetUp]
        public void SetUp()
        {
            existed = File.Exists(HarkenProjectSettingsProvider.AssetPath);
            settings = HarkenProjectSettingsProvider.GetOrCreateSettings();
            original = JsonUtility.ToJson(settings);
        }

        [TearDown]
        public void TearDown()
        {
            if (duplicateFolder != null) AssetDatabase.DeleteAsset(duplicateFolder);
            if (!existed) AssetDatabase.DeleteAsset(HarkenProjectSettingsProvider.AssetPath);
            else
            {
                settings = AssetDatabase.LoadAssetAtPath<HarkenProjectSettings>(
                    HarkenProjectSettingsProvider.AssetPath);
                if (settings == null)
                {
                    settings = ScriptableObject.CreateInstance<HarkenProjectSettings>();
                    JsonUtility.FromJsonOverwrite(original, settings);
                    EnsureFolder("Assets/Harken");
                    EnsureFolder("Assets/Harken/Resources");
                    AssetDatabase.CreateAsset(settings, HarkenProjectSettingsProvider.AssetPath);
                }
                else
                {
                    JsonUtility.FromJsonOverwrite(original, settings);
                    EditorUtility.SetDirty(settings);
                    AssetDatabase.SaveAssetIfDirty(settings);
                }
            }
            AssetDatabase.Refresh();
        }

        [Test]
        public void PageIsProjectScopedAndFreshSettingsEnableCommunication()
        {
            SettingsProvider provider = HarkenProjectSettingsProvider.CreateSettingsProvider();
            Assert.That(provider.settingsPath, Is.EqualTo("Project/Harken"));
            Assert.That(provider.scope, Is.EqualTo(SettingsScope.Project));
            var fresh = ScriptableObject.CreateInstance<HarkenProjectSettings>();
            try
            {
                Assert.That(fresh.EnableCommunicationInBuilds, Is.True);
                Assert.That(fresh.MaxVoices, Is.EqualTo(HarkenProjectSettings.DefaultMaxVoices));
                Assert.That(fresh.MaxPacks, Is.EqualTo(256u));
                Assert.That(fresh.MaxActions, Is.EqualTo(256u));
                Assert.That(fresh.MaxActionFirings, Is.EqualTo(256u));
                Assert.That(fresh.SuppressUnityAudio, Is.False);
            }
            finally { UnityEngine.Object.DestroyImmediate(fresh); }
        }

        [Test]
        public void FreshAssetPersistsFourStartupCapacitiesAt256()
        {
            if (!existed)
            {
                Assert.That(File.Exists(HarkenProjectSettingsProvider.AssetPath), Is.True);
                var generated = new SerializedObject(settings);
                Assert.That(generated.FindProperty("maxVoices").longValue, Is.EqualTo(256L));
                Assert.That(generated.FindProperty("maxPacks").longValue, Is.EqualTo(256L));
                Assert.That(generated.FindProperty("maxActions").longValue, Is.EqualTo(256L));
                Assert.That(generated.FindProperty("maxActionFirings").longValue, Is.EqualTo(256L));
                var config = new VnConfig();
                HarkenProjectSettings.ApplyStartupCapacity(ref config);
                Assert.That(config.maxVoices, Is.EqualTo(256u));
                Assert.That(config.maxPacks, Is.EqualTo(256u));
                Assert.That(config.maxActions, Is.EqualTo(256u));
                Assert.That(config.maxActionFirings, Is.EqualTo(256u));
            }
            string path = "Assets/Harken/FreshCapacity_" + Guid.NewGuid().ToString("N") + ".asset";
            var fresh = ScriptableObject.CreateInstance<HarkenProjectSettings>();
            try
            {
                AssetDatabase.CreateAsset(fresh, path);
                AssetDatabase.SaveAssetIfDirty(fresh);
                AssetDatabase.ImportAsset(path, ImportAssetOptions.ForceSynchronousImport);
                var asset = AssetDatabase.LoadAssetAtPath<HarkenProjectSettings>(path);
                Assert.That(asset, Is.Not.Null);
                var serialized = new SerializedObject(asset);
                Assert.That(serialized.FindProperty("maxVoices").longValue, Is.EqualTo(256L));
                Assert.That(serialized.FindProperty("maxPacks").longValue, Is.EqualTo(256L));
                Assert.That(serialized.FindProperty("maxActions").longValue, Is.EqualTo(256L));
                Assert.That(serialized.FindProperty("maxActionFirings").longValue, Is.EqualTo(256L));
            }
            finally { AssetDatabase.DeleteAsset(path); }
        }

        [Test]
        public void PersistedSettingsKeepExplicitStartupCapacities()
        {
            var serialized = new SerializedObject(settings);
            serialized.FindProperty("maxVoices").longValue = 111;
            serialized.FindProperty("maxPacks").longValue = 112;
            serialized.FindProperty("maxActions").longValue = 113;
            serialized.FindProperty("maxActionFirings").longValue = 114;
            serialized.ApplyModifiedPropertiesWithoutUndo();
            AssetDatabase.SaveAssetIfDirty(settings);
            AssetDatabase.ImportAsset(HarkenProjectSettingsProvider.AssetPath,
                ImportAssetOptions.ForceSynchronousImport);

            var config = new VnConfig();
            HarkenProjectSettings.ApplyStartupCapacity(ref config);
            Assert.That(config.maxVoices, Is.EqualTo(111u));
            Assert.That(config.maxPacks, Is.EqualTo(112u));
            Assert.That(config.maxActions, Is.EqualTo(113u));
            Assert.That(config.maxActionFirings, Is.EqualTo(114u));
        }

        [Test]
        public void DisabledBuildSettingPersistsAsPlayerResourceWithoutDisablingEditor()
        {
            var serialized = new SerializedObject(settings);
            serialized.FindProperty("enableCommunicationInBuilds").boolValue = false;
            serialized.FindProperty("remoteBindAddress").stringValue = "127.0.0.1";
            serialized.FindProperty("enableRemoteDiscovery").boolValue = false;
            serialized.FindProperty("remoteApplicationName").stringValue = "Settings fixture";
            serialized.ApplyModifiedPropertiesWithoutUndo();
            AssetDatabase.SaveAssetIfDirty(settings);
            AssetDatabase.ImportAsset(HarkenProjectSettingsProvider.AssetPath, ImportAssetOptions.ForceSynchronousImport);
            settings = AssetDatabase.LoadAssetAtPath<HarkenProjectSettings>(HarkenProjectSettingsProvider.AssetPath);
            Assert.That(settings.EnableCommunicationInBuilds, Is.False);
            var reloaded = new SerializedObject(settings);
            Assert.That(reloaded.FindProperty("remoteBindAddress").stringValue, Is.EqualTo("127.0.0.1"));
            Assert.That(reloaded.FindProperty("enableRemoteDiscovery").boolValue, Is.False);
            Assert.That(reloaded.FindProperty("remoteApplicationName").stringValue, Is.EqualTo("Settings fixture"));
            Assert.That(HarkenProjectSettings.Load(), Is.SameAs(settings));
            Assert.That(HarkenProjectSettings.CommunicationEnabled, Is.True);
            Assert.DoesNotThrow(HarkenProjectSettingsProvider.ValidateSettingsAssets);
        }

        [Test]
        public void DisabledBuildPolicyKeepsPreviewCommunicationUntilAudioShutdown()
        {
            var serialized = new SerializedObject(settings);
            serialized.FindProperty("enableCommunicationInBuilds").boolValue = false;
            serialized.ApplyModifiedPropertiesWithoutUndo();
            using (var preview = new HarkenEditorPreview())
            {
                string root = Path.Combine(Application.streamingAssetsPath, "Audio", "U4Original");
                Assert.That(preview.Prepare(root, null, Array.Empty<ulong>(), true), Is.EqualTo(VnResult.VN_OK), preview.Diagnostic);
                Assert.That(preview.LastRemoteResult, Is.EqualTo(VnResult.VN_OK));
                Assert.That(RemoteCommunication.GetStatus(out VnRemoteStatus status), Is.EqualTo(VnResult.VN_OK));
                Assert.That(status.state, Is.EqualTo((uint)VnRemoteState.VN_REMOTE_LISTENING));
                Assert.That(status.tcpPort, Is.GreaterThan(0));
                Assert.That(preview.Shutdown(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(RemoteCommunication.GetStatus(out status), Is.EqualTo(VnResult.VN_OK));
                Assert.That(status.state, Is.EqualTo((uint)VnRemoteState.VN_REMOTE_STOPPED));
                Assert.That(status.tcpPort, Is.Zero);
            }
        }

        [Test]
        public void ConflictingPlayerResourceRefusesBuildInsteadOfChoosingArbitraryPolicy()
        {
            duplicateFolder = "Assets/HarkenSettingsFixture_" + Guid.NewGuid().ToString("N");
            AssetDatabase.CreateFolder("Assets", Path.GetFileName(duplicateFolder));
            AssetDatabase.CreateFolder(duplicateFolder, "Resources");
            AssetDatabase.CreateAsset(ScriptableObject.CreateInstance<HarkenProjectSettings>(),
                duplicateFolder + "/Resources/" + HarkenProjectSettings.ResourceName + ".asset");
            Assert.Throws<BuildFailedException>(HarkenProjectSettingsProvider.ValidateSettingsAssets);
        }

        [Test]
        public void MissingSettingsCreatesDocumentedDefaults()
        {
            Assert.That(AssetDatabase.DeleteAsset(HarkenProjectSettingsProvider.AssetPath), Is.True);
            HarkenProjectSettings created = HarkenProjectSettingsProvider.GetOrCreateSettings();
            Assert.That(created, Is.Not.Null);
            Assert.That(created.EnableCommunicationInBuilds, Is.True);
            Assert.That(created.MaxVoices, Is.EqualTo(HarkenProjectSettings.DefaultMaxVoices));
            Assert.That(AssetDatabase.GetAssetPath(created),
                Is.EqualTo(HarkenProjectSettingsProvider.AssetPath));
        }

        [Test]
        public void ProjectPageSplitsStartupFromCommunication()
        {
            SettingsProvider project = HarkenProjectSettingsProvider.CreateProjectSettingsProvider();
            SettingsProvider communication = HarkenProjectSettingsProvider.CreateCommunicationSettingsProvider();
            Assert.That(project.settingsPath, Is.EqualTo("Project/Harken/Project"));
            Assert.That(project.scope, Is.EqualTo(SettingsScope.Project));
            Assert.That(communication.settingsPath, Is.EqualTo("Project/Harken/Communication"));
            Assert.That(communication.scope, Is.EqualTo(SettingsScope.Project));
        }

        [Test]
        public void StartupCapacityIgnoresSceneLimits()
        {
            settings.SetMaxVoices(HarkenProjectSettings.DefaultMaxVoices);
            var owner = new GameObject("Capacity");
            owner.SetActive(false);
            try
            {
                var system = owner.AddComponent<HarkenAudioSystem>();
                system.GetType().GetField("maxVoices", BindingFlags.Instance | BindingFlags.NonPublic)
                    .SetValue(system, 32u);
                VnConfig config = HarkenEngine.DefaultConfig();
                HarkenProjectSettings.ApplyStartupCapacity(ref config);
                Assert.That(config.maxVoices, Is.EqualTo(256u));
                settings.SetMaxVoices(20000);
                HarkenProjectSettings.ApplyStartupCapacity(ref config);
                Assert.That(config.maxVoices, Is.EqualTo(20000u));
                settings.SetMaxActionFirings(0);
                HarkenProjectSettings.ApplyStartupCapacity(ref config);
                Assert.That(config.maxActionFirings, Is.EqualTo(HarkenProjectSettings.UnlimitedActionFirings));
            }
            finally { UnityEngine.Object.DestroyImmediate(owner); }
        }

        [Test]
        public void PackDirectoryRejectsTheOldStreamingRelativePath()
        {
            settings.SetPackDirectory(string.Empty);
            Assert.That(HarkenProjectSettings.TryResolvePackDirectory(out _, out string error), Is.False);
            Assert.That(error, Does.Not.Contain("StreamingAssets"));
            settings.SetPackDirectory("Audio/GeneratedPacks");
            Assert.That(HarkenProjectSettings.TryResolvePackDirectory(out string obsolete, out error), Is.False);
            Assert.That(obsolete, Is.Null);
            Assert.That(error, Does.Not.Contain("StreamingAssets"));
            settings.SetPackDirectory("Assets/Audio/GeneratedPacks");
            HarkenProjectSettings.TryResolvePackDirectory(out string selected, out error);
            string normalized = (selected ?? string.Empty).Replace('\\', '/');
            Assert.That(normalized, Does.EndWith("Assets/Audio/GeneratedPacks"));
            Assert.That(normalized, Does.Not.Contain("/StreamingAssets/"));
        }

        [Test]
        public void PackDirectoryMustStayOutOfTheAudioProject()
        {
            string folder = "Assets/HarkenU2Audio_" + Guid.NewGuid().ToString("N");
            string audio = folder + "/AudioProject";
            string packs = audio + "/Packs";
            string sibling = folder + "/GeneratedPacks";
            Directory.CreateDirectory(Path.Combine(HarkenProjectSettings.ProjectRoot(), packs));
            Directory.CreateDirectory(Path.Combine(HarkenProjectSettings.ProjectRoot(), sibling));
            var previous = HarkenProjectSettings.ReadAudioProjectDirectory;
            try
            {
                HarkenProjectSettings.ReadAudioProjectDirectory = () => audio;
                settings.SetPackDirectory(packs);
                Assert.That(HarkenProjectSettings.TryResolvePackDirectory(out _, out string error), Is.False);
                Assert.That(error, Does.Contain("audio project"));
                settings.SetPackDirectory(sibling);
                Assert.That(HarkenProjectSettings.TryResolvePackDirectory(out string full, out error), Is.True, error);
                Assert.That(full.Replace('\\', '/'), Does.Not.Contain("/StreamingAssets/"));
            }
            finally
            {
                HarkenProjectSettings.ReadAudioProjectDirectory = previous;
                AssetDatabase.DeleteAsset(folder);
                string leftover = Path.Combine(HarkenProjectSettings.ProjectRoot(), folder);
                if (Directory.Exists(leftover)) Directory.Delete(leftover, true);
            }
        }

        [Test]
        public void TextAssetBytesDoNotFailUnlessBothPackCopiesExist()
        {
            string folder = Path.Combine(Path.GetTempPath(), "HarkenU2Packs_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(folder);
            try
            {
                string pack = Path.Combine(folder, "Init.pack");
                File.WriteAllBytes(pack + ".bytes", new byte[] { 1, 2, 3 });
                Assert.That(HarkenPackFiles.Read(pack), Is.EqualTo(new byte[] { 1, 2, 3 }));
                File.WriteAllBytes(pack, new byte[] { 4 });
                Assert.Throws<IOException>(() => HarkenPackFiles.Read(pack));
            }
            finally { Directory.Delete(folder, true); }
        }


        [TestCase("Init.bytes")]
        [TestCase("HarkenPackManifest.json")]
        [TestCase("HarkenEditorWaveforms.json")]
        public void CompleteAssetPathNeverResolvesOrConflictsWithAnExtraSuffix(string name)
        {
            string folder = Path.Combine(Path.GetTempPath(), "HarkenBytes_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(folder);
            try
            {
                string path = Path.Combine(folder, name);
                File.WriteAllBytes(path + ".bytes", new byte[] { 9 });
                Assert.That(HarkenPackFiles.Read(path), Is.Null);
                File.WriteAllBytes(path, new byte[] { 1, 2, 3 });
                Assert.That(HarkenPackFiles.Read(path), Is.EqualTo(new byte[] { 1, 2, 3 }));
            }
            finally { Directory.Delete(folder, true); }
        }

        [TestCase(false)]
        [TestCase(true)]
        public void DesktopPlayerResolvesPacksInsideRelocatedStreamingAssets(bool absolute)
        {
            string root = Path.Combine(Path.GetTempPath(), "Harken Player \u6d4b\u8bd5 " + Guid.NewGuid().ToString("N"));
            string streaming = Path.Combine(root, "Game_Data", "StreamingAssets");
            string packs = Path.Combine(streaming, "Harken", "GeneratedPacks");
            Directory.CreateDirectory(packs);
            try
            {
                string configured = absolute ? packs : "Assets/StreamingAssets/Harken/GeneratedPacks";
                Assert.That(HarkenProjectSettings.TryResolvePlayerPackDirectory(configured, streaming, root,
                    out string full, out string error), Is.True, error);
                Assert.That(full, Is.EqualTo(Path.GetFullPath(packs)));
                Assert.That(Directory.Exists(Path.Combine(root, "Assets")), Is.False);
            }
            finally { Directory.Delete(root, true); }
        }

        [TestCase("Assets/StreamingAssets/MissingPacks")]
        [TestCase("Assets/StreamingAssets/../OutsidePacks")]
        [TestCase("Assets/Audio/GeneratedPacks")]
        public void DesktopPlayerRejectsMissingOrEscapingPackDirectory(string configured)
        {
            string root = Path.Combine(Path.GetTempPath(), "HarkenPlayerMissing_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(root);
            try
            {
                Assert.That(HarkenProjectSettings.TryResolvePlayerPackDirectory(configured, root, root,
                    out _, out string error), Is.False);
                Assert.That(error, Is.Not.Empty);
            }
            finally { Directory.Delete(root, true); }
        }

        [TestCase("Assets/Audio/GeneratedPacks")]
        [TestCase("GameContent/Audio/GeneratedPacks")]
        [TestCase("Game Content/\u6d4b\u8bd5/Packs")]
        public void DesktopPlayerUsesConfiguredRelativeDirectoryAfterRelocation(string configured)
        {
            string root = Path.Combine(Path.GetTempPath(), "HarkenRelocated_" + Guid.NewGuid().ToString("N"));
            string installed = Path.Combine(root, "Installed Game");
            string streaming = Path.Combine(installed, "Game_Data", "StreamingAssets");
            string packs = Path.Combine(installed, configured);
            Directory.CreateDirectory(packs);
            try
            {
                Assert.That(HarkenProjectSettings.TryResolvePlayerPackDirectory(configured, streaming, installed,
                    out string full, out string error), Is.True, error);
                Assert.That(full, Is.EqualTo(Path.GetFullPath(packs)));
                Assert.That(Directory.Exists(streaming), Is.False);
            }
            finally { Directory.Delete(root, true); }
        }

        [Test]
        public void DesktopPlayerHonorsExplicitAbsolutePackDirectory()
        {
            string root = Path.Combine(Path.GetTempPath(), "HarkenExternal_" + Guid.NewGuid().ToString("N"));
            string installed = Path.Combine(root, "Game");
            string external = Path.Combine(root, "External Packs");
            Directory.CreateDirectory(external);
            try
            {
                Assert.That(HarkenProjectSettings.TryResolvePlayerPackDirectory(external,
                    Path.Combine(installed, "Game_Data", "StreamingAssets"), installed,
                    out string full, out string error), Is.True, error);
                Assert.That(full, Is.EqualTo(Path.GetFullPath(external)));
            }
            finally { Directory.Delete(root, true); }
        }

        [Test]
        public void DesktopPlayerRejectsRelativeEscapeEvenWhenDirectoryExists()
        {
            string root = Path.Combine(Path.GetTempPath(), "HarkenEscape_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(Path.Combine(root, "Outside"));
            try
            {
                Assert.That(HarkenProjectSettings.TryResolvePlayerPackDirectory("../Outside",
                    Path.Combine(root, "Game", "StreamingAssets"), Path.Combine(root, "Game"),
                    out string full, out string error), Is.False);
                Assert.That(full, Is.Null);
                Assert.That(error, Is.Not.Empty);
            }
            finally { Directory.Delete(root, true); }
        }

        [Test]
        public void DesktopPlayerDoesNotSubstituteAnUnselectedStreamingDirectory()
        {
            string root = Path.Combine(Path.GetTempPath(), "HarkenNoFallback_" + Guid.NewGuid().ToString("N"));
            string streaming = Path.Combine(root, "Game_Data", "StreamingAssets");
            Directory.CreateDirectory(Path.Combine(streaming, "Audio", "GeneratedPacks"));
            try
            {
                Assert.That(HarkenProjectSettings.TryResolvePlayerPackDirectory("Assets/Audio/GeneratedPacks",
                    streaming, root, out _, out string error), Is.False);
                Assert.That(error, Is.Not.Empty);
            }
            finally { Directory.Delete(root, true); }
        }

        [TestCase("Assets/StreamingAssets/Audio/GeneratedPacks", "Audio/GeneratedPacks")]
        [TestCase("Assets\\StreamingAssets\\Audio\\GeneratedPacks", "Audio/GeneratedPacks")]
        [TestCase("jar:file:///data/app/game/base.apk!/assets/Audio/GeneratedPacks", "Audio/GeneratedPacks")]
        public void AndroidPackDirectoryResolvesWithoutFilesystemAccess(string configured, string expected)
        {
            Assert.That(HarkenProjectSettings.TryResolveStreamingPackDirectory(configured,
                "jar:file:///data/app/game/base.apk!/assets", out string relative, out string error), Is.True, error);
            Assert.That(relative, Is.EqualTo(expected));
        }

        [TestCase("")]
        [TestCase("Audio/GeneratedPacks")]
        [TestCase("Assets/Audio/GeneratedPacks")]
        [TestCase("Assets/StreamingAssetsOther/GeneratedPacks")]
        [TestCase("Assets/StreamingAssets/../GeneratedPacks")]
        [TestCase("Assets/StreamingAssets/Audio/../../GeneratedPacks")]
        [TestCase("jar:file:///other/base.apk!/assets/Audio/GeneratedPacks")]
        [TestCase("jar:file:///data/app/game/base.apk!/assets-other/GeneratedPacks")]
        [TestCase("jar:file:///data/app/game/base.apk!/assets/../GeneratedPacks")]
        public void AndroidPackDirectoryRejectsUnselectedOutsideAndEscapingPaths(string configured)
        {
            Assert.That(HarkenProjectSettings.TryResolveStreamingPackDirectory(configured,
                "jar:file:///data/app/game/base.apk!/assets", out string relative, out string error), Is.False);
            Assert.That(relative, Is.Null);
            Assert.That(error, Is.Not.Empty);
        }

        private static void EnsureFolder(string path)
        {
            if (AssetDatabase.IsValidFolder(path)) return;
            string parent = Path.GetDirectoryName(path)?.Replace('\\', '/');
            string name = Path.GetFileName(path);
            if (!string.IsNullOrEmpty(parent)) EnsureFolder(parent);
            AssetDatabase.CreateFolder(parent, name);
        }
    }
}
