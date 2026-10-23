using System;
using System.Collections;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using NUnit.Framework;
using UnityEditor;
using UnityEngine;
using UnityEngine.TestTools;
using Harken.Unity.Editor;
using Object = UnityEngine.Object;

namespace Harken.Unity.Tests
{
    public sealed class HarkenEditorServiceTests
    {
        internal static void RequirePublicationTooling()
        {
            if (!HarkenEditorPublicationSupport.IsAvailable)
                Assert.Ignore("Temporarily suspended until Editor publication tools consume current Pack companions. Restore this coverage with the capability.");
        }

        [Test]
        public void UnavailableCatalogRejectsAllSourcesWithoutReadingOrPublishingCandidates()
        {
            var catalog = new HarkenEditorCatalog();
            var source = ScriptableObject.CreateInstance<HarkenPackSource>();
            var companion = new TextAsset("{}");
            int reads = 0;
            try
            {
                Assert.That(catalog.Refresh("missing-publication"), Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
                Assert.That(catalog.RefreshPlatform("missing-publication", null, null, "Windows"),
                    Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
                Assert.That(catalog.Refresh(source), Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
                Assert.That(catalog.Refresh(source, null, companion), Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
                Assert.That(catalog.Refresh("publication.json", path => { ++reads; throw new IOException(path); }),
                    Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
                Assert.That(reads, Is.Zero);
                Assert.That(catalog.Publication, Is.Null);
                Assert.That(catalog.PreparedPacks, Is.Null);
                Assert.That(catalog.DirectoryPath, Is.Null);
                Assert.That(catalog.GetPreparationOrder("Play", out ulong[] ids, out _), Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
                Assert.That(ids, Is.Empty);
                Assert.That(catalog.GetActionPreview("Play", out var preview, out _), Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
                Assert.That(preview, Is.Null);
                Assert.That(catalog.GetWaveform("Play", out var samples, out var duration, out _),
                    Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
                Assert.That(samples, Is.Empty);
                Assert.That(duration, Is.Null);
            }
            finally { Object.DestroyImmediate(source); Object.DestroyImmediate(companion); }
        }

        [TestCase(false)]
        [TestCase(true)]
        public void UnavailablePreviewCannotAcquireOrTerminateNativeOwnership(bool externalEngine)
        {
            VnConfig config = HarkenEngine.DefaultConfig();
            config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
            if (externalEngine) Assert.That(HarkenEngine.Init(in config), Is.EqualTo(VnResult.VN_OK));
            var source = ScriptableObject.CreateInstance<HarkenPackSource>();
            try
            {
                using (var preview = new HarkenEditorPreview())
                using (var scope = preview.CreateScope())
                {
                    Assert.That(preview.Prepare("missing-publication", null, Array.Empty<ulong>(), true),
                        Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
                    Assert.That(preview.Prepare(source, Array.Empty<ulong>(), true), Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
                    Assert.That(preview.Prepare((HarkenPreparedPacks)null, Array.Empty<ulong>(), true),
                        Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
                    Assert.That(scope.Prepare(null, new[] { "Play" }), Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
                    Assert.That(preview.IsPrepared, Is.False);
                    Assert.That(preview.Session, Is.Null);
                    Assert.That(scope.IsPrepared, Is.False);
                }
                if (!externalEngine) Assert.That(HarkenEngine.Init(in config), Is.EqualTo(VnResult.VN_OK));
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
            }
            finally { Object.DestroyImmediate(source); HarkenEngine.Term(); }
        }

        [Test]
        public void UnavailableSymbolGenerationDoesNotReadOrWriteFiles()
        {
            string root = Path.Combine(Application.temporaryCachePath, "HarkenSymbols_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(root);
            string input = Path.Combine(root, "publication.json");
            string output = Path.Combine(root, "Generated", "HarkenVariables.g.cs");
            File.WriteAllText(input, "{}");
            try
            {
                using (File.Open(input, FileMode.Open, FileAccess.ReadWrite, FileShare.None))
                {
                    Assert.That(HarkenVariableCatalogGenerator.Generate(input, output, out _),
                        Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
                    Assert.That(HarkenVariableCatalogGenerator.GeneratePublication(input, output, out _),
                        Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
                }
                Assert.That(HarkenVariableCatalogGenerator.ValidatePublication("{}", input, out _),
                    Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
                Assert.That(Directory.Exists(Path.GetDirectoryName(output)), Is.False);
                Assert.That(File.ReadAllText(input), Is.EqualTo("{}"));
            }
            finally { Directory.Delete(root, true); }
        }

        private static string Root(string name = "U4Original")
            => Path.Combine(Application.streamingAssetsPath, "Audio", name);

        private static void CopyCompanionAssets(string sourceDir, string destDir)
        {
            Directory.CreateDirectory(destDir);
            foreach (string file in Directory.GetFiles(sourceDir, "*", SearchOption.AllDirectories))
            {
                if (file.EndsWith(".meta", StringComparison.OrdinalIgnoreCase)) continue;
                string relative = file.Substring(sourceDir.Length).TrimStart(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
                string dest = Path.Combine(destDir, relative);
                Directory.CreateDirectory(Path.GetDirectoryName(dest));
                File.Copy(file, dest, true);
            }
            AssetDatabase.Refresh(ImportAssetOptions.ForceSynchronousImport);
        }

        private static HarkenEditorCatalog Catalog(string name = "U4Original")
        {
            var catalog = new HarkenEditorCatalog();
            Assert.That(catalog.Refresh(Root(name)), Is.EqualTo(VnResult.VN_OK), catalog.Diagnostic);
            return catalog;
        }

        [Test]
        public void RefreshInvalidatesOldExactNameAndInvalidPairClearsPreviousCandidate()
        {
            RequirePublicationTooling();
            var empty = new HarkenEditorCatalog();
            Assert.That(empty.GetPreparationOrder("Play_U4_A", out ulong[] pending, out string missing),
                Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
            Assert.That(pending, Is.Empty);
            HarkenEditorCatalog catalog = Catalog();
            string project = catalog.Publication.ProjectId;
            uint revision = catalog.Revision;
            Assert.That(catalog.Refresh(Root()), Is.EqualTo(VnResult.VN_OK));
            Assert.That(catalog.Revision, Is.EqualTo(revision));
            Assert.That(catalog.GetWaveform("Play_U4_A", out float[] samples, out double? duration,
                out string diagnostic), Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
            Assert.That(samples, Is.Empty);
            Assert.That(duration, Is.Null);
            Assert.That(diagnostic, Does.Contain("Play_U4_A"));
            Assert.That(catalog.Refresh(Root("U4Renamed")), Is.EqualTo(VnResult.VN_OK));
            Assert.That(catalog.Revision, Is.GreaterThan(revision));
            Assert.That(catalog.CheckReference("Play_U4_A", project, out diagnostic),
                Is.EqualTo(VnResult.VN_UNKNOWN_ID));
            Assert.That(diagnostic, Does.Contain("Play_U4_A"));
            Assert.That(catalog.Publication.RetiredActions, Is.Empty);
            Assert.That(catalog.CheckReference("Play_U4_Renamed", project, out diagnostic),
                Is.EqualTo(VnResult.VN_OK));
            Assert.That(catalog.CheckReference("play_u4_renamed", project, out diagnostic),
                Is.EqualTo(VnResult.VN_UNKNOWN_ID));
            Assert.That(catalog.Refresh(Root("Missing_" + Guid.NewGuid().ToString("N"))),
                Is.Not.EqualTo(VnResult.VN_OK));
            Assert.That(catalog.Publication, Is.Null);
            Assert.That(catalog.PreparedPacks, Is.Null);
            Assert.That(catalog.DirectoryPath, Is.Null);
            Assert.That(catalog.CheckReference("Play_U4_A", project, out diagnostic), Is.Not.EqualTo(VnResult.VN_OK));
            Assert.That(catalog.GetPreparationOrder("Play_U4_A", out pending, out diagnostic), Is.EqualTo(catalog.LastResult));
            Assert.That(pending, Is.Empty);
        }

        [Test]
        public void SessionlessReplacePackRefusesCanonicalLifecycleWithoutNativeCall()
        {
            string root = Root();
            string initPath = Path.Combine(root, "Init.bytes");
            Assert.That(File.Exists(initPath), Is.True);
            Assert.That(Packs.ReplacePack(default, initPath, root),
                Is.EqualTo(VnResult.VN_INVALID_ARG));
            Assert.That(Packs.ReplacePack(new Pack(1), null, root),
                Is.EqualTo(VnResult.VN_INVALID_ARG));
            Assert.That(Packs.ReplacePack(new Pack(1), initPath, null),
                Is.EqualTo(VnResult.VN_INVALID_ARG));
            ulong before = Packs.ChangeVersion;
            Assert.That(Packs.ReplacePack(new Pack(1), initPath, root),
                Is.EqualTo(VnResult.VN_PACK_INIT_LIFECYCLE));
            Assert.That(Packs.ChangeVersion, Is.EqualTo(before));
            VnConfig config = HarkenEngine.DefaultConfig();
            config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
            Assert.That(HarkenEngine.Init(in config), Is.EqualTo(VnResult.VN_OK));
            try
            {
                Assert.That(Packs.ReplacePack(new Pack(1), initPath, root),
                    Is.EqualTo(VnResult.VN_PACK_INIT_LIFECYCLE));
                Assert.That(Packs.ChangeVersion, Is.EqualTo(before));
            }
            finally
            {
                Assert.That(HarkenEngine.Term(), Is.EqualTo(VnResult.VN_OK));
            }
        }

        [Test]
        public void PublicationBindingSavesUnderCurrentPublicationIdOnly()
        {
            string publicationId = "a046b230-c437-4cea-8533-546be84a2e50";
            string staleId = "11111111-1111-1111-1111-111111111111";
            var binding = new HarkenEditorPublicationBinding(string.Empty, Root(), string.Empty, string.Empty,
                string.Empty, string.Empty);
            try
            {
                Assert.That(HarkenEditorPublicationBinding.Save(string.Empty, binding), Is.False);
                Assert.That(HarkenEditorPublicationBinding.Save("not-a-project-id", binding), Is.False);
                Assert.That(HarkenEditorPublicationBinding.Save(publicationId, binding), Is.True);
                Assert.That(HarkenEditorPublicationBinding.TryLoad(publicationId, out HarkenEditorPublicationBinding loaded),
                    Is.True);
                Assert.That(loaded.Directory, Is.EqualTo(Root()));
                Assert.That(HarkenEditorPublicationBinding.TryLoad(staleId, out _), Is.False);
            }
            finally
            {
                EditorPrefs.DeleteKey(HarkenEditorPublicationBinding.Key(publicationId));
            }
        }

        [Test]
        public void UnboundCatalogDoesNotTreatActionNameAsInvalid()
        {
            var catalog = new HarkenEditorCatalog();
            Assert.That(catalog.CheckReference("Play_UI_FryFire",
                "797e39f2-6f9c-48ac-8a06-6eafc359fda9", out string diagnostic),
                Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
        }

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
                Assert.That(catalog.Publication, Is.Null);
                Assert.That(Get<string>(inspector, "directory"), Is.Null.Or.Empty);
                Assert.That(catalog.CheckReference("Play_U4_A", catalog.Publication?.ProjectId, out string diagnostic),
                    Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
            }
            finally
            {
                if (inspector != null) Object.DestroyImmediate(inspector);
                Object.DestroyImmediate(owner);
            }
        }

        [Test]
        public void UnityAssetBytesDirectoryRefreshCapturesPreparedPacks()
        {
            RequirePublicationTooling();
            using (var fixture = new AssetFixture())
            {
                string dest = fixture.Folder + "/U4Bytes";
                CopyCompanionAssets(Root(), dest);
                var catalog = new HarkenEditorCatalog();
                Assert.That(catalog.Refresh(dest), Is.EqualTo(VnResult.VN_OK), catalog.Diagnostic);
                Assert.That(catalog.PreparedPacks, Is.Not.Null);
                Assert.That(catalog.CheckReference("Play_U4_A", catalog.Publication.ProjectId, out string diagnostic),
                    Is.EqualTo(VnResult.VN_OK), diagnostic);
                using (var preview = new HarkenEditorPreview())
                {
                    Assert.That(catalog.GetPreparationOrder("Play_U4_A", out ulong[] ids, out diagnostic),
                        Is.EqualTo(VnResult.VN_OK), diagnostic);
                    Assert.That(preview.Prepare(dest, null, ids, true), Is.EqualTo(VnResult.VN_OK), preview.Diagnostic);
                    Assert.That(preview.Play("Play_U4_A", catalog.Publication.ProjectId, out ActionFiring firing),
                        Is.EqualTo(VnResult.VN_OK));
                    Assert.That(firing.IsValid, Is.True);
                    Assert.That(preview.Shutdown(), Is.EqualTo(VnResult.VN_OK));
                }
            }
        }



        [Test]
        public void TypedNameCanBindAndProjectChangesNeedExplicitRebindWithUndo()
        {
            RequirePublicationTooling();
            var owner = new GameObject("Editor selected Action");
            owner.SetActive(false);
            try
            {
                var action = owner.AddComponent<HarkenAction>();
                var properties = new SerializedObject(action);
                properties.FindProperty("actionName").stringValue = "Play_U4_A";
                properties.ApplyModifiedPropertiesWithoutUndo();
                HarkenEditorCatalog catalog = Catalog();
                Assert.That(action.ProjectId, Is.Empty);
                Assert.That(catalog.SelectAction(action, action.ActionName, false, out string error),
                    Is.EqualTo(VnResult.VN_OK), error);
                string project = action.ProjectId;
                Assert.That(project, Is.EqualTo(catalog.Publication.ProjectId));
                Assert.That(catalog.Refresh(Root("U4OtherProject")), Is.EqualTo(VnResult.VN_OK));
                Assert.That(catalog.SelectAction(action, action.ActionName, false, out error),
                    Is.EqualTo(VnResult.VN_GAMEPLAY_WRONG_PROJECT));
                Assert.That(action.ProjectId, Is.EqualTo(project));
                Assert.That(error, Does.Contain("Play_U4_A"));
                Undo.IncrementCurrentGroup();
                Assert.That(catalog.SelectAction(action, action.ActionName, true, out error), Is.EqualTo(VnResult.VN_OK));
                Assert.That(action.ProjectId, Is.EqualTo(catalog.Publication.ProjectId));
                Undo.FlushUndoRecordObjects();
                Undo.PerformUndo();
                Assert.That(action.ProjectId, Is.EqualTo(project));
                Assert.That(action.ActionName, Is.EqualTo("Play_U4_A"));
            }
            finally { Object.DestroyImmediate(owner); }
        }

        [Test]
        public void CompanionTextAssetCaptureKeepsOriginalAssetsAndRejectsInvalidCandidates()
        {
            using (var fixture = new AssetFixture())
            {
                HarkenPackSource source = fixture.CreateSource();
                PackBootstrapSession session = null;
                try
                {
                    Assert.That(source.CaptureCompanion(fixture.InitJson, out PackSource captured, out string error),
                        Is.EqualTo(VnResult.VN_OK), error);
                    VnResult background = Task.Run(() => source.CaptureCompanion(fixture.InitJson, out _, out _)).GetAwaiter().GetResult();
                    Assert.That(background, Is.EqualTo(VnResult.VN_INVALID_ARG));
                    Set(source, "initPack", null);
                    Assert.That(source.CaptureCompanion(fixture.InitJson, out PackSource failed, out _),
                        Is.EqualTo(VnResult.VN_CONTENT_NOT_LOADED));
                    Assert.That(failed, Is.Null);
                    Object.DestroyImmediate(source);
                    VnConfig config = HarkenEngine.DefaultConfig();
                    config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
                    Assert.That(PackBootstrap.Start(captured, ref config, out session), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(session.PrepareActions(new[] { "Play_U4_A" }, session.ProjectId, out var preparation),
                        Is.EqualTo(VnResult.VN_OK));
                    fixture.DeleteAssets();
                    Assert.That(Actions.Resolve("Play_U4_A", session.ProjectId, out _), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(preparation.Release(), Is.EqualTo(VnResult.VN_OK));
                }
                finally
                {
                    if (source != null) Object.DestroyImmediate(source);
                    session?.Dispose();
                }
            }
        }

        [Test]
        public void MissingPreparedContentNeedsExplicitSupplyAndPrepare()
        {
            RequirePublicationTooling();
            using (var fixture = new AssetFixture())
            using (var preview = new HarkenEditorPreview())
            {
                HarkenPackSource source = fixture.CreateSource(false);
                try
                {
                    Assert.That(source.Capture(out HarkenPreparedPacks captured, out string error), Is.EqualTo(VnResult.VN_OK), error);
                    Assert.That(captured.Publication.TryFindAction("Play_U4_A", out GameplayAction action), Is.True);
                    Assert.That(preview.Prepare(captured, new[] { action.OwnerPackStableId }, true),
                        Is.EqualTo(VnResult.VN_CONTENT_NOT_LOADED));
                    Assert.That(preview.IsPrepared, Is.True);
                    Assert.That(preview.Play(action.Name, captured.Publication.ProjectId, out ActionFiring refused),
                        Is.EqualTo(VnResult.VN_CONTENT_NOT_LOADED));
                    Assert.That(refused.IsValid, Is.False);
                    Assert.That(captured.Publication.TryFindPack(action.OwnerPackStableId, out PackManifestEntry entry), Is.True);
                    Assert.That(captured.SupplyPack(entry.PackStableId, File.ReadAllBytes(Path.Combine(Root(), entry.Path))),
                        Is.EqualTo(VnResult.VN_OK));
                    Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(preview.FiringCount, Is.Zero);
                    Assert.That(preview.Session.IsStartupComplete, Is.False);
                    Assert.That(preview.PrepareStartupPacks(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(preview.Play(action.Name, captured.Publication.ProjectId, out _), Is.EqualTo(VnResult.VN_OK));
                }
                finally { Object.DestroyImmediate(source); }
            }
        }

        [Test]
        public void RefusedPreviewPrepareNeverTerminatesAnExternalEngine()
        {
            VnConfig config = HarkenEngine.DefaultConfig();
            config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
            PackBootstrapSession external = null;
            try
            {
                Assert.That(PackBootstrap.StartDirectory(Root(), null, ref config, out external), Is.EqualTo(VnResult.VN_OK));
                Assert.That(external.PrepareActions(new[] { "Play_U4_A" }, external.ProjectId, out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Actions.Resolve("Play_U4_A", external.ProjectId, out ActionResolved resolved), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Actions.Post(in resolved, Emitter.None, 0, out ActionFiring retained), Is.EqualTo(VnResult.VN_OK));
                using (var refused = new HarkenEditorPreview())
                {
                    Assert.That(refused.Prepare(Root(), null, Array.Empty<ulong>(), true), Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
                    Assert.That(refused.Shutdown(), Is.EqualTo(VnResult.VN_OK));
                }
                Assert.That(external.IsActive, Is.True);
                Assert.That(retained.Query(out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
            }
            finally { external?.Dispose(); }
        }

        [TestCase("U4Renamed")]
        [TestCase("Missing_Refresh_Target")]
        public void InspectorRefreshStopsOnlyItsOwnPreparedPreview(string nextDirectory)
        {
            RequirePublicationTooling();
            var owner = new GameObject("Inspector Refresh");
            owner.SetActive(false);
            UnityEditor.Editor inspector = null;
            try
            {
                var action = owner.AddComponent<HarkenAction>();
                inspector = UnityEditor.Editor.CreateEditor(action, typeof(HarkenActionInspector));
                var preview = Get<HarkenEditorPreview>(inspector, "preview");
                Assert.That(preview.Prepare(Root(), null, Array.Empty<ulong>(), true), Is.EqualTo(VnResult.VN_OK));
                PackBootstrapSession previous = preview.Session;
                Set(inspector, "directory", Root(nextDirectory));
                inspector.GetType().GetMethod("Refresh", BindingFlags.Instance | BindingFlags.NonPublic, null, Type.EmptyTypes, null).Invoke(inspector, null);
                Assert.That(previous.IsActive, Is.False);
                Assert.That(preview.IsPrepared, Is.False);
                Assert.That(preview.Play("Play_U4_A", Catalog().Publication.ProjectId, out _), Is.EqualTo(VnResult.VN_NOT_INITIALIZED));
                using (var external = new HarkenEditorPreview())
                {
                    Assert.That(external.Prepare(Root(), null, Array.Empty<ulong>(), true), Is.EqualTo(VnResult.VN_OK));
                    inspector.GetType().GetMethod("Refresh", BindingFlags.Instance | BindingFlags.NonPublic, null, Type.EmptyTypes, null).Invoke(inspector, null);
                    Assert.That(external.IsPrepared, Is.True);
                    Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                }
            }
            finally
            {
                if (inspector != null) Object.DestroyImmediate(inspector);
                Object.DestroyImmediate(owner);
            }
        }

        [UnityTest]
        public IEnumerator PreviewStopsIndependentFiringsAndClosesAfterNewAudioBlocks()
        {
            RequirePublicationTooling();
            HarkenEditorCatalog catalog = Catalog();
            Assert.That(catalog.GetPreparationOrder("Play_U4_A", out ulong[] ids, out _), Is.EqualTo(VnResult.VN_OK));
            using (var preview = new HarkenEditorPreview())
            {
                Assert.That(preview.Prepare(Root(), null, ids), Is.EqualTo(VnResult.VN_OK), preview.Diagnostic);
                PackBootstrapSession session = preview.Session;
                Assert.That(HarkenNative.Vn_SetBusMute(VnConstants.VN_BUS_MASTER, 1, 0), Is.EqualTo(VnResult.VN_IN_USE));
                yield return NewBlock(session, CaptureBlock());
                Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(preview.Play("Play_U4_A", catalog.Publication.ProjectId, out ActionFiring first), Is.EqualTo(VnResult.VN_OK));
                Assert.That(preview.Play("Play_U4_A", catalog.Publication.ProjectId, out ActionFiring second), Is.EqualTo(VnResult.VN_OK));
                Assert.That(first.Handle, Is.Not.EqualTo(second.Handle));
                yield return NewBlock(session, CaptureBlock());
                Assert.That(first.Query(out VnActionFiringInfo active), Is.EqualTo(VnResult.VN_OK));
                Assert.That(active.livePlayableCount, Is.GreaterThan(0));
                ulong beforeStop = CaptureBlock();
                Assert.That(preview.Stop(first, 50), Is.EqualTo(VnResult.VN_OK));
                yield return NewBlock(session, beforeStop);
                float deadline = Time.realtimeSinceStartup + 3;
                while (preview.FiringCount == 2 && Time.realtimeSinceStartup < deadline) yield return null;
                Assert.That(preview.FiringCount, Is.EqualTo(1));
                Assert.That(first.Query(out _), Is.EqualTo(VnResult.VN_STALE_HANDLE));
                Assert.That(second.Query(out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(preview.Session, Is.SameAs(session));
                var update = (EditorApplication.CallbackFunction)Delegate.CreateDelegate(
                    typeof(EditorApplication.CallbackFunction), preview, "OnEditorUpdate");
                EditorApplication.update -= update;
                beforeStop = CaptureBlock();
                Assert.That(preview.Stop(second, 50), Is.EqualTo(VnResult.VN_OK));
                yield return NewBlock(session, beforeStop);
                deadline = Time.realtimeSinceStartup + 3;
                while (preview.IsPrepared && Time.realtimeSinceStartup < deadline)
                {
                    Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
                    yield return null;
                }
                Assert.That(preview.IsPrepared, Is.False);
                Assert.That(session.IsActive, Is.False);
            }
        }

#if HARKEN_SDK_TEST_NULL_PUMP
        // These exports exist only in the isolated SDK fixture DLL.
        [DllImport(HarkenNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        private static extern VnResult HarkenSdkTest_PumpNull(uint blocks);

        [DllImport(HarkenNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        private static extern VnResult HarkenSdkTest_QueueNotification();

        [Test]
        public void FirstExplicitPreviewPlayAfterEditorGapKeepsThePriorFiringPlaying()
        {
            RequirePublicationTooling();
            HarkenEditorCatalog catalog = Catalog();
            Assert.That(catalog.GetPreparationOrder("Play_U4_A", out ulong[] ids, out _), Is.EqualTo(VnResult.VN_OK));
            using (var preview = new HarkenEditorPreview())
            {
                Assert.That(preview.Prepare(Root(), null, ids, true), Is.EqualTo(VnResult.VN_OK));
                PackBootstrapSession session = preview.Session;
                Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(preview.Play("Play_U4_A", catalog.Publication.ProjectId, out ActionFiring prior), Is.EqualTo(VnResult.VN_OK));
                Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(HarkenSdkTest_PumpNull(1), Is.EqualTo(VnResult.VN_OK));
                Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
                ulong baseline = CaptureBlock();
                // Audio advances while the Editor does not service another logical update.
                Assert.That(HarkenSdkTest_PumpNull(32), Is.EqualTo(VnResult.VN_OK));
                VnSnapshot afterGap = HarkenEngine.DefaultSnapshot();
                Assert.That(HarkenEngine.Capture(ref afterGap), Is.EqualTo(VnResult.VN_OK));
                Assert.That(afterGap.blockIndex, Is.GreaterThan(baseline));
                Assert.That(afterGap.activeVoiceCount, Is.EqualTo(1));
                Assert.That(preview.Session, Is.SameAs(session));
                Assert.That(session.IsActive, Is.True);

                Assert.That(preview.Play("Play_U4_A", catalog.Publication.ProjectId, out ActionFiring next),
                    Is.EqualTo(VnResult.VN_OK), "The first explicit request must not depend on another attempt.");
                Assert.That(next.IsValid, Is.True);
                Assert.That(next.Handle, Is.Not.EqualTo(prior.Handle));
                Assert.That(preview.FiringCount, Is.EqualTo(2));
                Assert.That(prior.Query(out VnActionFiringInfo info), Is.EqualTo(VnResult.VN_OK));
                Assert.That(info.livePlayableCount, Is.GreaterThan(0));
                // Observe the retained native result before preview Tick can release it.
                // Four-second fixture media needs 750 default render blocks plus retirement.
                for (int block = 0; block < 800 && info.state !=
                    (uint)VnActionFiringState.VN_ACTION_FIRING_TERMINAL_RETAINED; ++block)
                {
                    Assert.That(preview.Session, Is.SameAs(session));
                    Assert.That(session.IsActive, Is.True);
                    Assert.That(HarkenSdkTest_PumpNull(1), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(prior.Query(out info), Is.EqualTo(VnResult.VN_OK));
                }
                Assert.That(CaptureBlock(), Is.GreaterThan(afterGap.blockIndex));
                Assert.That(info.state, Is.EqualTo((uint)VnActionFiringState.VN_ACTION_FIRING_TERMINAL_RETAINED));
                Assert.That(info.terminalReason, Is.EqualTo((uint)VnActionTerminalReason.VN_ACTION_TERMINAL_NATURAL),
                    "Admitting the next request must not stop the prior firing.");
            }
        }

        [Test]
        public void NullPreviewStopKeepsOtherFiringsAndClosesOnlyAfterTheLastStop()
        {
            RequirePublicationTooling();
            HarkenEditorCatalog catalog = Catalog();
            Assert.That(catalog.GetPreparationOrder("Play_U4_A", out ulong[] ids, out _), Is.EqualTo(VnResult.VN_OK));
            using (var preview = new HarkenEditorPreview())
            {
                Assert.That(preview.Prepare(Root(), null, ids, true), Is.EqualTo(VnResult.VN_OK));
                PackBootstrapSession session = preview.Session;
                Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(preview.Play("Play_U4_A", catalog.Publication.ProjectId, out ActionFiring first), Is.EqualTo(VnResult.VN_OK));
                Assert.That(preview.Play("Play_U4_A", catalog.Publication.ProjectId, out ActionFiring second), Is.EqualTo(VnResult.VN_OK));
                Assert.That(HarkenSdkTest_PumpNull(1), Is.EqualTo(VnResult.VN_OK));
                Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
                ulong baseline = CaptureBlock();
                Assert.That(preview.Stop(first, 0), Is.EqualTo(VnResult.VN_OK));
                for (int block = 0; block < 64 && preview.FiringCount == 2; ++block)
                {
                    Assert.That(preview.Session, Is.SameAs(session));
                    Assert.That(HarkenSdkTest_PumpNull(1), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
                }
                Assert.That(preview.Session, Is.SameAs(session));
                Assert.That(session.IsActive, Is.True);
                Assert.That(CaptureBlock(), Is.GreaterThan(baseline));
                Assert.That(preview.FiringCount, Is.EqualTo(1));
                Assert.That(first.Query(out _), Is.EqualTo(VnResult.VN_STALE_HANDLE));
                Assert.That(second.Query(out VnActionFiringInfo active), Is.EqualTo(VnResult.VN_OK));
                Assert.That(active.livePlayableCount, Is.GreaterThan(0));
                Assert.That(preview.Stop(second, 0), Is.EqualTo(VnResult.VN_OK));
                for (int block = 0; block < 64 && preview.IsPrepared; ++block)
                {
                    Assert.That(preview.Session, Is.SameAs(session));
                    Assert.That(HarkenSdkTest_PumpNull(1), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
                }
                Assert.That(session.IsActive, Is.False);
                Assert.That(preview.IsPrepared, Is.False);
                Assert.That(preview.FiringCount, Is.Zero);
                Assert.That(preview.Play("Play_U4_A", catalog.Publication.ProjectId, out ActionFiring refused), Is.EqualTo(VnResult.VN_NOT_INITIALIZED));
                Assert.That(refused.IsValid, Is.False);
            }
        }

        [Test]
        public void PreviewPlayAfterTheLastIndividualStopKeepsTheSessionAndReleasesTheOldFiring()
        {
            RequirePublicationTooling();
            HarkenEditorCatalog catalog = Catalog();
            using (var preview = new HarkenEditorPreview())
            {
                ActionFiring prior = PreparePlayingNullPreview(preview, catalog);
                PackBootstrapSession session = preview.Session;
                ulong baseline = CaptureBlock();
                Assert.That(preview.Stop(prior, 0), Is.EqualTo(VnResult.VN_OK));
                PumpUntilPreviewFiringTerminal(preview, session, prior, baseline);

                Assert.That(preview.Play("Play_U4_A", catalog.Publication.ProjectId, out ActionFiring next),
                    Is.EqualTo(VnResult.VN_OK), "Maintenance must keep the session open for this explicit request.");
                Assert.That(preview.Session, Is.SameAs(session));
                Assert.That(session.IsActive, Is.True);
                Assert.That(next.IsValid, Is.True);
                Assert.That(next.Handle, Is.Not.EqualTo(prior.Handle));
                Assert.That(next.Query(out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(prior.Query(out _), Is.EqualTo(VnResult.VN_STALE_HANDLE));
                Assert.That(preview.FiringCount, Is.EqualTo(1));
            }
        }

        [Test]
        public void FailedPreviewPlayAfterTheLastIndividualStopStillClosesOnTheNextTick()
        {
            RequirePublicationTooling();
            HarkenEditorCatalog catalog = Catalog();
            using (var preview = new HarkenEditorPreview())
            {
                ActionFiring prior = PreparePlayingNullPreview(preview, catalog);
                PackBootstrapSession session = preview.Session;
                ulong baseline = CaptureBlock();
                Assert.That(preview.Stop(prior, 0), Is.EqualTo(VnResult.VN_OK));
                PumpUntilPreviewFiringTerminal(preview, session, prior, baseline);

                Assert.That(preview.Play("Missing_Preview_Action", catalog.Publication.ProjectId, out ActionFiring refused),
                    Is.EqualTo(VnResult.VN_UNKNOWN_ID));
                Assert.That(refused.IsValid, Is.False);
                Assert.That(preview.Session, Is.SameAs(session));
                Assert.That(session.IsActive, Is.True);
                Assert.That(prior.Query(out _), Is.EqualTo(VnResult.VN_STALE_HANDLE));
                Assert.That(preview.FiringCount, Is.Zero);
                Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(session.IsActive, Is.False);
                Assert.That(preview.IsPrepared, Is.False);
            }
        }

        [Test]
        public void PreviewGlobalStopStillRefusesPlayBeforeIdleShutdown()
        {
            RequirePublicationTooling();
            HarkenEditorCatalog catalog = Catalog();
            using (var preview = new HarkenEditorPreview())
            {
                ActionFiring prior = PreparePlayingNullPreview(preview, catalog);
                PackBootstrapSession session = preview.Session;
                ulong baseline = CaptureBlock();
                Assert.That(preview.Stop(0), Is.EqualTo(VnResult.VN_OK));
                PumpUntilPreviewFiringTerminal(preview, session, prior, baseline);

                Assert.That(preview.Play("Play_U4_A", catalog.Publication.ProjectId, out ActionFiring refused),
                    Is.EqualTo(VnResult.VN_IN_USE));
                Assert.That(refused.IsValid, Is.False);
                Assert.That(preview.Session, Is.SameAs(session));
                Assert.That(session.IsActive, Is.True);
                Assert.That(preview.FiringCount, Is.EqualTo(1));
                Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(session.IsActive, Is.False);
                Assert.That(preview.IsPrepared, Is.False);
                Assert.That(preview.FiringCount, Is.Zero);
            }
        }

        [Test]
        public void SecondScopePrepareReusesTheSessionWithoutPublicCanPrepare()
        {
            RequirePublicationTooling();
            using (var fixture = new AssetFixture())
            using (var preview = new HarkenEditorPreview())
            {
                HarkenPreparedPacks prepared = Captured(fixture, out HarkenPackSource source);
                try
                {
                    EnableNullDevice(preview);
                    HarkenEditorPreview.Scope first = preview.CreateScope();
                    HarkenEditorPreview.Scope second = preview.CreateScope();
                    Assert.That(first.Prepare(prepared, new[] { "Play_U4_A" }), Is.EqualTo(VnResult.VN_OK), first.Diagnostic);
                    PackBootstrapSession session = preview.Session;
                    Assert.That(session, Is.Not.Null);
                    Assert.That(session.IsActive, Is.True);
                    Assert.That(second.Prepare(prepared, new[] { "Play_U4_B" }), Is.EqualTo(VnResult.VN_OK), second.Diagnostic);
                    Assert.That(preview.Session, Is.SameAs(session));
                    Assert.That(first.IsPrepared, Is.True);
                    Assert.That(second.IsPrepared, Is.True);
                    Assert.That(preview.Prepare(prepared, Array.Empty<ulong>(), true), Is.EqualTo(VnResult.VN_ALREADY_INITIALIZED));
                }
                finally { UnityEngine.Object.DestroyImmediate(source); }
            }
        }

        [Test]
        public void PreviewScopesKeepIndependentStateOnlyIdentityUntilDispose()
        {
            RequirePublicationTooling();
            using (var fixture = new AssetFixture())
            using (var preview = new HarkenEditorPreview())
            {
                HarkenPreparedPacks prepared = Captured(fixture, out HarkenPackSource source);
                try
                {
                    EnableNullDevice(preview);
                    var first = preview.CreateScope();
                    var second = preview.CreateScope();
                    var names = new[] { "Play_U4_A" };
                    Assert.That(first.Prepare(prepared, names), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(second.Prepare(prepared, names), Is.EqualTo(VnResult.VN_OK));
                    Emitter firstIdentity = first.stateEmitter;
                    Emitter secondIdentity = second.stateEmitter;
                    Assert.That(firstIdentity.IsValid, Is.True);
                    Assert.That(secondIdentity.IsValid, Is.True);
                    Assert.That(firstIdentity, Is.Not.EqualTo(secondIdentity));
                    Assert.That(firstIdentity.SetPosition(default), Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
                    Assert.That(first.Play("Play_U4_A", out _), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(first.StopAll(0), Is.EqualTo(VnResult.VN_OK));
                    PumpUntilPreviewQuiet(preview);
                    Assert.That(first.Prepare(prepared, names), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(first.stateEmitter, Is.EqualTo(firstIdentity));
                    first.Dispose();
                    Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(firstIdentity.Stop(0), Is.EqualTo(VnResult.VN_STALE_HANDLE));
                    Assert.That(second.stateEmitter, Is.EqualTo(secondIdentity));
                    Assert.That(second.Play("Play_U4_A", out _), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(second.StopAll(0), Is.EqualTo(VnResult.VN_OK));
                    PumpUntilPreviewQuiet(preview);
                    second.Dispose();
                    preview.Tick();
                    Assert.That(preview.IsPrepared, Is.False);
                }
                finally { UnityEngine.Object.DestroyImmediate(source); }
            }
        }

        [Test]
        public void ScopeIdleTickDoesNotReleasePreparationBeforePlay()
        {
            RequirePublicationTooling();
            using (var fixture = new AssetFixture())
            using (var preview = new HarkenEditorPreview())
            {
                HarkenPreparedPacks prepared = Captured(fixture, out HarkenPackSource source);
                try
                {
                    EnableNullDevice(preview);
                    HarkenEditorPreview.Scope scope = preview.CreateScope();
                    Assert.That(scope.Prepare(prepared, new[] { "Play_U4_A" }), Is.EqualTo(VnResult.VN_OK), scope.Diagnostic);
                    for (int i = 0; i < 3; ++i) Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(scope.IsPrepared, Is.True);
                    Assert.That(scope.Play("Play_U4_A", out ActionFiring firing), Is.EqualTo(VnResult.VN_OK), scope.Diagnostic);
                    Assert.That(firing.IsValid, Is.True);
                    Assert.That(scope.IsPrepared, Is.True);
                }
                finally { UnityEngine.Object.DestroyImmediate(source); }
            }
        }

        [Test]
        public void ScopeLocalStopLeavesPreparationPlayableAfterIdleTick()
        {
            RequirePublicationTooling();
            using (var fixture = new AssetFixture())
            using (var preview = new HarkenEditorPreview())
            {
                HarkenPreparedPacks prepared = Captured(fixture, out HarkenPackSource source);
                try
                {
                    EnableNullDevice(preview);
                    HarkenEditorPreview.Scope scope = preview.CreateScope();
                    Assert.That(scope.Prepare(prepared, new[] { "Play_U4_A" }), Is.EqualTo(VnResult.VN_OK), scope.Diagnostic);
                    Assert.That(scope.Play("Play_U4_A", out ActionFiring firing), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(scope.Stop(firing, 0), Is.EqualTo(VnResult.VN_OK));
                    PumpUntilPreviewQuiet(preview);
                    Assert.That(preview.IsPrepared, Is.True);
                    Assert.That(scope.IsPrepared, Is.True);
                    Assert.That(scope.Play("Play_U4_A", out ActionFiring again), Is.EqualTo(VnResult.VN_OK), scope.Diagnostic);
                    Assert.That(again.IsValid, Is.True);
                    Assert.That(again.Handle, Is.Not.EqualTo(firing.Handle));
                }
                finally { UnityEngine.Object.DestroyImmediate(source); }
            }
        }

        [Test]
        public void ScopeStopOnALeavesIdleBPlayable()
        {
            RequirePublicationTooling();
            using (var fixture = new AssetFixture())
            using (var preview = new HarkenEditorPreview())
            {
                HarkenPreparedPacks prepared = Captured(fixture, out HarkenPackSource source);
                try
                {
                    EnableNullDevice(preview);
                    HarkenEditorPreview.Scope a = preview.CreateScope();
                    HarkenEditorPreview.Scope b = preview.CreateScope();
                    Assert.That(a.Prepare(prepared, new[] { "Play_U4_A" }), Is.EqualTo(VnResult.VN_OK), a.Diagnostic);
                    Assert.That(b.Prepare(prepared, new[] { "Play_U4_B" }), Is.EqualTo(VnResult.VN_OK), b.Diagnostic);
                    PackBootstrapSession session = preview.Session;
                    Assert.That(a.Play("Play_U4_A", out ActionFiring firing), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(a.Stop(firing, 0), Is.EqualTo(VnResult.VN_OK));
                    PumpUntilPreviewQuiet(preview);
                    Assert.That(preview.Session, Is.SameAs(session));
                    Assert.That(session.IsActive, Is.True);
                    Assert.That(b.IsPrepared, Is.True);
                    Assert.That(b.Play("Play_U4_B", out ActionFiring other), Is.EqualTo(VnResult.VN_OK), b.Diagnostic);
                    Assert.That(other.IsValid, Is.True);
                }
                finally { UnityEngine.Object.DestroyImmediate(source); }
            }
        }

        [Test]
        public void ScopeStopAllStopsDiscardedMarkerHandles()
        {
            RequirePublicationTooling();
            using (var fixture = new AssetFixture())
            using (var preview = new HarkenEditorPreview())
            {
                HarkenPreparedPacks prepared = Captured(fixture, out HarkenPackSource source);
                try
                {
                    EnableNullDevice(preview);
                    HarkenEditorPreview.Scope scope = preview.CreateScope();
                    Assert.That(scope.Prepare(prepared, new[] { "Play_U4_A" }), Is.EqualTo(VnResult.VN_OK), scope.Diagnostic);
                    Assert.That(scope.Play("Play_U4_A", out _), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(scope.Play("Play_U4_A", out ActionFiring kept), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(preview.FiringCount, Is.EqualTo(2));
                    Assert.That(scope.StopAll(0), Is.EqualTo(VnResult.VN_OK));
                    PumpUntilPreviewQuiet(preview);
                    Assert.That(preview.FiringCount, Is.Zero);
                    Assert.That(kept.Query(out _), Is.EqualTo(VnResult.VN_STALE_HANDLE));
                    Assert.That(preview.IsPrepared, Is.True);
                    Assert.That(scope.IsPrepared, Is.True);
                }
                finally { UnityEngine.Object.DestroyImmediate(source); }
            }
        }

        [Test]
        public void ScopeRetargetBThenCPreparesOnlyTheLastRequestOnTick()
        {
            RequirePublicationTooling();
            using (var fixture = new AssetFixture())
            using (var preview = new HarkenEditorPreview())
            {
                HarkenPreparedPacks prepared = Captured(fixture, out HarkenPackSource source);
                try
                {
                    EnableNullDevice(preview);
                    HarkenEditorPreview.Scope scope = preview.CreateScope();
                    Assert.That(scope.Prepare(prepared, new[] { "Play_U4_A" }), Is.EqualTo(VnResult.VN_OK), scope.Diagnostic);
                    Assert.That(scope.Play("Play_U4_A", out ActionFiring playing), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(scope.Prepare(prepared, new[] { "Play_U4_B" }), Is.EqualTo(VnResult.VN_IN_USE));
                    Assert.That(scope.Prepare(prepared, new[] { "Play_U4_Caller" }), Is.EqualTo(VnResult.VN_IN_USE));
                    PumpUntilScopePrepared(preview, scope);
                    Assert.That(scope.IsPrepared, Is.True);
                    Assert.That(scope.Play("Play_U4_B", out _), Is.EqualTo(VnResult.VN_UNKNOWN_ID));
                    Assert.That(scope.Play("Play_U4_Caller", out ActionFiring next), Is.EqualTo(VnResult.VN_OK), scope.Diagnostic);
                    Assert.That(next.IsValid, Is.True);
                    Assert.That(playing.Query(out _), Is.EqualTo(VnResult.VN_STALE_HANDLE));
                }
                finally { UnityEngine.Object.DestroyImmediate(source); }
            }
        }

        [Test]
        public void ScopeDisposeDuringProviderCallbackCancelsAndDefersNativeRelease()
        {
            RequirePublicationTooling();
            using (var fixture = new AssetFixture())
            using (var preview = new HarkenEditorPreview())
            {
                HarkenPackSource asset = fixture.CreateSource(false);
                try
                {
                    Assert.That(asset.Capture(out HarkenPreparedPacks prepared, out string error), Is.EqualTo(VnResult.VN_OK), error);
                    EnableNullDevice(preview);
                    HarkenEditorPreview.Scope scope = preview.CreateScope();
                    bool disposedInCallback = false;
                    Assert.That(prepared.SetDemandProvider((ulong id, string path, uint variant, ulong hash, out byte[] bytes, object state) =>
                    {
                        bytes = File.ReadAllBytes(Path.Combine(Root(), path));
                        if (!disposedInCallback)
                        {
                            disposedInCallback = true;
                            scope.Dispose();
                        }
                        return VnResult.VN_OK;
                    }), Is.EqualTo(VnResult.VN_OK));
                    VnResult preparedResult = scope.Prepare(prepared, new[] { "Play_U4_A" });
                    Assert.That(disposedInCallback, Is.True);
                    Assert.That(preparedResult, Is.EqualTo(VnResult.VN_NOT_INITIALIZED));
                    Assert.That(scope.IsPrepared, Is.False);
                    Assert.That(scope.Prepare(prepared, new[] { "Play_U4_A" }), Is.EqualTo(VnResult.VN_NOT_INITIALIZED));
                    for (int i = 0; i < 8; ++i) preview.Tick();
                    Assert.That(scope.Play("Play_U4_A", out ActionFiring firing), Is.EqualTo(VnResult.VN_NOT_INITIALIZED));
                    Assert.That(firing.IsValid, Is.False);
                }
                finally { UnityEngine.Object.DestroyImmediate(asset); }
            }
        }

        [Test]
        public void ScopeRecoversAfterPrepareStopAndReleaseRefusals()
        {
            RequirePublicationTooling();
            using (var fixture = new AssetFixture())
            using (var preview = new HarkenEditorPreview())
            {
                HarkenPackSource asset = fixture.CreateSource(false);
                try
                {
                    Assert.That(asset.Capture(out HarkenPreparedPacks prepared, out string error), Is.EqualTo(VnResult.VN_OK), error);
                    EnableNullDevice(preview);
                    HarkenEditorPreview.Scope scope = preview.CreateScope();
                    Assert.That(scope.Prepare(prepared, new[] { "Play_U4_A" }), Is.EqualTo(VnResult.VN_CONTENT_NOT_LOADED));
                    Assert.That(scope.IsPrepared, Is.False);
                    for (int i = 0; i < 8; ++i) Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK).Or.EqualTo(VnResult.VN_NOT_INITIALIZED));
                    Assert.That(prepared.Publication.TryFindAction("Play_U4_A", out GameplayAction action), Is.True);
                    Assert.That(prepared.Publication.TryFindPack(action.OwnerPackStableId, out PackManifestEntry pack), Is.True);
                    Assert.That(prepared.SupplyPack(pack.PackStableId, File.ReadAllBytes(Path.Combine(Root(), pack.Path))), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(scope.Prepare(prepared, new[] { "Play_U4_A" }), Is.EqualTo(VnResult.VN_OK), scope.Diagnostic);
                    Assert.That(scope.Play("Play_U4_A", out ActionFiring firing), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(scope.Stop(firing, 0), Is.EqualTo(VnResult.VN_OK));
                    PumpUntilPreviewQuiet(preview);
                    Assert.That(scope.IsPrepared, Is.True);
                    Assert.That(scope.Play("Play_U4_A", out firing), Is.EqualTo(VnResult.VN_OK), scope.Diagnostic);
                    Assert.That(firing.IsValid, Is.True);
                }
                finally { UnityEngine.Object.DestroyImmediate(asset); }
            }
        }

        [Test]
        public void ScopePublicPreparePlayStopReentryReturnsInUse()
        {
            RequirePublicationTooling();
            using (var fixture = new AssetFixture())
            using (var preview = new HarkenEditorPreview())
            {
                HarkenPackSource asset = fixture.CreateSource(false);
                try
                {
                    Assert.That(asset.Capture(out HarkenPreparedPacks prepared, out string error), Is.EqualTo(VnResult.VN_OK), error);
                    EnableNullDevice(preview);
                    HarkenEditorPreview.Scope scope = preview.CreateScope();
                    VnResult nested = VnResult.VN_OK;
                    Assert.That(prepared.SetDemandProvider((ulong id, string path, uint variant, ulong hash, out byte[] bytes, object state) =>
                    {
                        bytes = File.ReadAllBytes(Path.Combine(Root(), path));
                        nested = scope.Prepare(prepared, new[] { "Play_U4_A" });
                        return VnResult.VN_OK;
                    }), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(scope.Prepare(prepared, new[] { "Play_U4_A" }), Is.EqualTo(VnResult.VN_OK), scope.Diagnostic);
                    Assert.That(nested, Is.EqualTo(VnResult.VN_IN_USE));
                    Assert.That(scope.Play("Play_U4_A", out ActionFiring firing), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(scope.Stop(firing, 0), Is.EqualTo(VnResult.VN_OK));
                }
                finally { UnityEngine.Object.DestroyImmediate(asset); }
            }
        }

        [Test]
        public void ScopeDoesNotBindAnOldSessionOntoANewOne()
        {
            RequirePublicationTooling();
            using (var fixture = new AssetFixture())
            using (var preview = new HarkenEditorPreview())
            {
                HarkenPreparedPacks prepared = Captured(fixture, out HarkenPackSource source);
                try
                {
                    EnableNullDevice(preview);
                    HarkenEditorPreview.Scope live = preview.CreateScope();
                    HarkenEditorPreview.Scope dead = preview.CreateScope();
                    Assert.That(live.Prepare(prepared, new[] { "Play_U4_A" }), Is.EqualTo(VnResult.VN_OK), live.Diagnostic);
                    PackBootstrapSession first = preview.Session;
                    dead.Dispose();
                    Assert.That(preview.Shutdown(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(first.IsActive, Is.False);
                    Assert.That(live.IsPrepared, Is.False);
                    Assert.That(dead.Prepare(prepared, new[] { "Play_U4_A" }), Is.EqualTo(VnResult.VN_NOT_INITIALIZED));
                    Assert.That(live.Prepare(prepared, new[] { "Play_U4_A" }), Is.EqualTo(VnResult.VN_OK), live.Diagnostic);
                    Assert.That(preview.Session, Is.Not.SameAs(first));
                    Assert.That(preview.Session.IsActive, Is.True);
                    Assert.That(live.Play("Play_U4_A", out ActionFiring firing), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(firing.IsValid, Is.True);
                }
                finally { UnityEngine.Object.DestroyImmediate(source); }
            }
        }

        [Test]
        public void ShutdownKeepsClosingWhenFirstAttemptRefusesWithLiveScopesAndNoFirings()
        {
            RequirePublicationTooling();
            using (var fixture = new AssetFixture())
            using (var preview = new HarkenEditorPreview())
            {
                HarkenPreparedPacks prepared = Captured(fixture, out HarkenPackSource source);
                try
                {
                    EnableNullDevice(preview);
                    HarkenEditorPreview.Scope scope = preview.CreateScope();
                    Assert.That(scope.Prepare(prepared, new[] { "Play_U4_A" }), Is.EqualTo(VnResult.VN_OK), scope.Diagnostic);
                    PackBootstrapSession session = preview.Session;
                    Set(preview, "shutdownRequested", true);
                    Assert.That(preview.FiringCount, Is.Zero);
                    VnResult first = preview.Tick();
                    if (session.IsActive)
                    {
                        Assert.That(first, Is.Not.EqualTo(VnResult.VN_OK));
                        for (int i = 0; i < 8 && session.IsActive; ++i) preview.Tick();
                    }
                    Assert.That(session.IsActive, Is.False);
                    Assert.That(preview.IsPrepared, Is.False);
                    Assert.That(scope.Play("Play_U4_A", out _), Is.EqualTo(VnResult.VN_NOT_INITIALIZED));
                }
                finally { UnityEngine.Object.DestroyImmediate(source); }
            }
        }

        private static HarkenPreparedPacks Captured(AssetFixture fixture, out HarkenPackSource source)
        {
            source = fixture.CreateSource();
            Assert.That(source.Capture(out HarkenPreparedPacks prepared, out string error), Is.EqualTo(VnResult.VN_OK), error);
            return prepared;
        }

        private static void EnableNullDevice(HarkenEditorPreview preview) => Set(preview, "forceNullDevice", true);

        private static void PumpUntilPreviewQuiet(HarkenEditorPreview preview)
        {
            for (int block = 0; block < 64 && preview.FiringCount > 0; ++block)
            {
                Assert.That(HarkenSdkTest_PumpNull(1), Is.EqualTo(VnResult.VN_OK));
                Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK).Or.EqualTo(VnResult.VN_NOT_INITIALIZED));
            }
            Assert.That(preview.FiringCount, Is.Zero);
        }

        private static void PumpUntilScopePrepared(HarkenEditorPreview preview, HarkenEditorPreview.Scope scope)
        {
            for (int block = 0; block < 64 && !scope.IsPrepared; ++block)
            {
                Assert.That(HarkenSdkTest_PumpNull(1), Is.EqualTo(VnResult.VN_OK));
                preview.Tick();
            }
            Assert.That(scope.IsPrepared, Is.True, scope.Diagnostic);
        }

        private static ActionFiring PreparePlayingNullPreview(HarkenEditorPreview preview, HarkenEditorCatalog catalog)
        {
            Assert.That(catalog.GetPreparationOrder("Play_U4_A", out ulong[] ids, out _), Is.EqualTo(VnResult.VN_OK));
            Assert.That(preview.Prepare(Root(), null, ids, true), Is.EqualTo(VnResult.VN_OK));
            Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
            Assert.That(preview.Play("Play_U4_A", catalog.Publication.ProjectId, out ActionFiring firing), Is.EqualTo(VnResult.VN_OK));
            Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
            Assert.That(HarkenSdkTest_PumpNull(1), Is.EqualTo(VnResult.VN_OK));
            Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
            Assert.That(firing.Query(out VnActionFiringInfo info), Is.EqualTo(VnResult.VN_OK));
            Assert.That(info.livePlayableCount, Is.GreaterThan(0));
            return firing;
        }

        private static void PumpUntilPreviewFiringTerminal(HarkenEditorPreview preview, PackBootstrapSession session,
            ActionFiring firing, ulong baseline)
        {
            VnActionFiringInfo info = default;
            // Drain native retirements without Preview Tick releasing the firing or closing the session.
            for (int block = 0; block < 64; ++block)
            {
                Assert.That(preview.Session, Is.SameAs(session));
                Assert.That(session.IsActive, Is.True);
                Assert.That(HarkenSdkTest_PumpNull(1), Is.EqualTo(VnResult.VN_OK));
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(firing.Query(out info), Is.EqualTo(VnResult.VN_OK));
                if (info.state == (uint)VnActionFiringState.VN_ACTION_FIRING_TERMINAL_RETAINED) break;
            }
            VnSnapshot afterStop = HarkenEngine.DefaultSnapshot();
            Assert.That(HarkenEngine.Capture(ref afterStop), Is.EqualTo(VnResult.VN_OK));
            Assert.That(afterStop.blockIndex, Is.GreaterThan(baseline));
            Assert.That(afterStop.activeVoiceCount, Is.Zero);
            Assert.That(preview.Session, Is.SameAs(session));
            Assert.That(session.IsActive, Is.True);
            Assert.That(info.state, Is.EqualTo((uint)VnActionFiringState.VN_ACTION_FIRING_TERMINAL_RETAINED));
            Assert.That(info.terminalReason, Is.EqualTo((uint)VnActionTerminalReason.VN_ACTION_TERMINAL_STOP_REQUESTED));
            Assert.That(preview.FiringCount, Is.EqualTo(1));
        }

        [Test]
        public void PreviewPlayMaintenanceCannotPostIntoAReplacementSession()
        {
            RequirePublicationTooling();
            HarkenEditorCatalog catalog = Catalog();
            Assert.That(catalog.GetPreparationOrder("Play_U4_A", out ulong[] ids, out _), Is.EqualTo(VnResult.VN_OK));
            using (var preview = new HarkenEditorPreview())
            {
                bool notified = false, restarted = false, inNotification = false, restartedInNotification = false;
                VnResult shutdown = VnResult.VN_NOT_INITIALIZED, restart = VnResult.VN_NOT_INITIALIZED;
                var registration = PrepareNotificationPreview(preview, catalog, ids,
                    (in VnNotification notification, object state) =>
                    {
                        if (notified || notification.type != (uint)VnNotificationType.VN_NOTIFY_DEGRADATION_OCCURRED) return;
                        notified = true;
                        inNotification = true;
                        try { shutdown = preview.Shutdown(); }
                        finally { inNotification = false; }
                    });
                EventInfo returned = typeof(HarkenEngine).GetEvent("OutermostUpdateReturned", BindingFlags.Static | BindingFlags.NonPublic);
                Action restartAfterUpdate = () =>
                {
                    if (!notified || restarted) return;
                    restarted = true;
                    restartedInNotification = inNotification;
                    restart = preview.Prepare(Root(), null, ids, true);
                };
                try
                {
                    PackBootstrapSession previous = preview.Session;
                    Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
                    returned.GetAddMethod(true).Invoke(null, new object[] { restartAfterUpdate });
                    Assert.That(HarkenSdkTest_QueueNotification(), Is.EqualTo(VnResult.VN_OK));
                    VnResult result = preview.Play("Play_U4_A", catalog.Publication.ProjectId, out ActionFiring refused);
                    Assert.That(notified, Is.True, "Play maintenance must drain the queued native notification.");
                    Assert.That(shutdown, Is.EqualTo(VnResult.VN_OK));
                    Assert.That(restarted, Is.True);
                    Assert.That(restartedInNotification, Is.False);
                    Assert.That(restart, Is.EqualTo(VnResult.VN_OK));
                    Assert.That(result, Is.EqualTo(VnResult.VN_NOT_INITIALIZED));
                    Assert.That(refused.IsValid, Is.False);
                    Assert.That(previous.IsActive, Is.False);
                    Assert.That(preview.Session, Is.Not.SameAs(previous));
                    Assert.That(preview.IsPrepared, Is.True);
                    Assert.That(preview.FiringCount, Is.Zero);
                    Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(preview.FiringCount, Is.Zero, "The old request must not be replayed in the new session.");
                }
                finally
                {
                    returned.GetRemoveMethod(true).Invoke(null, new object[] { restartAfterUpdate });
                    preview.Shutdown();
                    registration.Dispose();
                }
            }
        }

        [Test]
        public void PreviewPlayMaintenanceRejectsReentrantPlayWithoutRecursiveNotificationDispatch()
        {
            RequirePublicationTooling();
            HarkenEditorCatalog catalog = Catalog();
            Assert.That(catalog.GetPreparationOrder("Play_U4_A", out ulong[] ids, out _), Is.EqualTo(VnResult.VN_OK));
            using (var preview = new HarkenEditorPreview())
            {
                int notifications = 0, depth = 0, maxDepth = 0;
                VnResult nested = VnResult.VN_OK, queued = VnResult.VN_NOT_INITIALIZED;
                ActionFiring nestedFiring = default;
                var registration = PrepareNotificationPreview(preview, catalog, ids,
                    (in VnNotification notification, object state) =>
                    {
                        if (notification.type != (uint)VnNotificationType.VN_NOTIFY_DEGRADATION_OCCURRED) return;
                        ++depth;
                        maxDepth = Math.Max(maxDepth, depth);
                        try
                        {
                            if (++notifications != 1) return;
                            queued = HarkenSdkTest_QueueNotification();
                            nested = preview.Play("Play_U4_A", catalog.Publication.ProjectId, out nestedFiring);
                        }
                        finally { --depth; }
                    });
                try
                {
                    PackBootstrapSession session = preview.Session;
                    Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(HarkenSdkTest_QueueNotification(), Is.EqualTo(VnResult.VN_OK));
                    VnResult result = preview.Play("Play_U4_A", catalog.Publication.ProjectId, out ActionFiring firing);
                    Assert.That(notifications, Is.GreaterThanOrEqualTo(1));
                    Assert.That(queued, Is.EqualTo(VnResult.VN_OK));
                    Assert.That(maxDepth, Is.EqualTo(1));
                    Assert.That(nested, Is.EqualTo(VnResult.VN_IN_USE));
                    Assert.That(nestedFiring.IsValid, Is.False);
                    Assert.That(result, Is.EqualTo(VnResult.VN_OK));
                    Assert.That(firing.IsValid, Is.True);
                    Assert.That(preview.Session, Is.SameAs(session));
                    Assert.That(session.IsActive, Is.True);
                    Assert.That(preview.FiringCount, Is.EqualTo(1));
                }
                finally
                {
                    preview.Shutdown();
                    registration.Dispose();
                }
            }
        }

        private static HarkenCallbacks.Registration PrepareNotificationPreview(HarkenEditorPreview preview,
            HarkenEditorCatalog catalog, ulong[] ids, HarkenCallbacks.NotificationHandler handler)
        {
            VnConfig config = HarkenEngine.DefaultConfig();
            config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
            var registration = HarkenCallbacks.InstallNotification(ref config, handler);
            PackBootstrapSession session = null;
            try
            {
                Assert.That(PackBootstrap.Start(Path.Combine(Root(), catalog.Publication.InitPack.Path), Root(), ids,
                    ref config, out session), Is.EqualTo(VnResult.VN_OK));
                // Public Preview preparation has no callback parameter; only fixture setup adopts this real session.
                Set(preview, "session", session);
                var adopt = typeof(HarkenEditorPreview).GetMethod("Adopt", BindingFlags.Instance | BindingFlags.NonPublic);
                Assert.That((VnResult)adopt.Invoke(preview, new object[] { VnResult.VN_OK }), Is.EqualTo(VnResult.VN_OK));
                return registration;
            }
            catch
            {
                preview.Shutdown();
                session?.Dispose();
                registration.Dispose();
                throw;
            }
        }
#endif

        private static ulong CaptureBlock()
        {
            VnSnapshot snapshot = HarkenEngine.DefaultSnapshot();
            Assert.That(HarkenEngine.Capture(ref snapshot), Is.EqualTo(VnResult.VN_OK));
            return snapshot.blockIndex;
        }

        private static IEnumerator NewBlock(PackBootstrapSession session, ulong baseline)
        {
            float deadline = Time.realtimeSinceStartup + 3;
            while (Time.realtimeSinceStartup < deadline)
            {
                Assert.That(session.IsActive, Is.True);
                if (CaptureBlock() > baseline) yield break;
                yield return null;
            }
            Assert.Fail("No fresh audio block was published by the same preview session.");
        }

        private static T Get<T>(object target, string name)
            => (T)target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).GetValue(target);

        private static void Set(object target, string name, object value)
            => target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).SetValue(target, value);

        private sealed class AssetFixture : IDisposable
        {
            public readonly string Folder = "Assets/HarkenEditorServices_" + Guid.NewGuid().ToString("N");
            public AssetFixture() => AssetDatabase.CreateFolder("Assets", Folder.Substring("Assets/".Length));

            public TextAsset InitJson { get; private set; }

            public HarkenPackSource CreateSource(bool includeContent = true)
            {
                var source = ScriptableObject.CreateInstance<HarkenPackSource>();
                InitJson = Import(Path.Combine(Root(), "Init.json"), "Init.json");
                Set(source, "initPack", Import(Path.Combine(Root(), "Init.bytes"), "Init.bytes"));
                if (includeContent)
                    Set(source, "content", Directory.GetFiles(Root())
                        .Where(path => (path.EndsWith(".bytes", StringComparison.Ordinal) || path.EndsWith(".json", StringComparison.Ordinal)) &&
                            Path.GetFileName(path) != "Init.bytes" && Path.GetFileName(path) != "Init.json")
                        .Select(path => new HarkenPackSource.ContentAsset
                        {
                            relativePath = Path.GetFileName(path),
                            asset = Import(path, Path.GetFileName(path))
                        }).ToArray());
                return source;
            }

            private TextAsset Import(string source, string name)
            {
                string path = Folder + "/" + name;
                File.Copy(source, path);
                AssetDatabase.ImportAsset(path, ImportAssetOptions.ForceSynchronousImport);
                TextAsset asset = AssetDatabase.LoadAssetAtPath<TextAsset>(path);
                Assert.That(asset, Is.Not.Null, path);
                Assert.That(asset.bytes, Is.EqualTo(File.ReadAllBytes(source)));
                return asset;
            }

            public void DeleteAssets() => AssetDatabase.DeleteAsset(Folder);
            public void Dispose() => DeleteAssets();
        }
    }
}
