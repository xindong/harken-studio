import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
import unittest.mock as mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from release import release_evidence as evidence
from release import deploy_macos_bundle


class NoticeComponentsTest(unittest.TestCase):
    def test_repository_notice_covers_packaging_inputs(self):
        entries = evidence.notice_components()
        self.assertEqual({entry["id"] for entry in entries}, {entry["id"] for entry in evidence.COMPONENT_INPUTS})
        for entry in entries:
            with self.subTest(component=entry["id"]):
                self.assertTrue(entry["licenseName"])
                self.assertTrue(entry["version"])
                for name in entry.get("evidence", []):
                    self.assertTrue((evidence.REPO_ROOT / name).is_file())
                for name in entry.get("legalFiles", []):
                    sources = [source for files in evidence.collect_legal_texts.THIRD_PARTY_FILES.values()
                               for source, filename in files if filename == name]
                    self.assertTrue(sources)
                    for source in sources:
                        self.assertTrue(source.is_file())


class QtComponentsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.kit = self.root / "qt"
        self.model = {"cache": {"Qt6_DIR": str(self.kit / "lib/cmake/Qt6")}}
        version = mock.patch.object(evidence.subprocess, "check_output", return_value="6.8.3\n")
        self.query_version = version.start()
        self.addCleanup(version.stop)

    def test_deployed_dbus_includes_its_registered_supplier_on_both_platforms(self):
        for platform, name in (("windows", "Qt6DBus.dll"),
                               ("macos", "Contents/Frameworks/QtDBus.framework/Versions/A/QtDBus")):
            with self.subTest(platform=platform):
                payload = self.root / platform
                binary = payload / name
                binary.parent.mkdir(parents=True)
                binary.write_bytes(b"fixture")
                components, qt_root = evidence.qt_components(self.model, payload, "studio")
                self.assertEqual(qt_root, self.kit)
                self.assertEqual({entry["bom-ref"] for entry in components}, {"qt", "qt:libdbus-1-headers"})
                dbus = next(entry for entry in components if entry["bom-ref"] == "qt:libdbus-1-headers")
                self.assertEqual(dbus["version"], "dbus-1.13.12")
                self.assertEqual(dbus["licenses"], [{"license": {"name": "AFL-2.1"}}])

    def test_modules_without_third_party_entries_are_accepted(self):
        for module in ("Widgets", "Concurrent", "OpenGL", "OpenGLWidgets"):
            (self.root / f"Qt6{module}.dll").write_bytes(b"fixture")
        components, _ = evidence.qt_components(self.model, self.root, "studio")
        self.assertEqual([entry["bom-ref"] for entry in components], ["qt"])

    def test_undeployed_dbus_supplier_is_not_included(self):
        (self.root / "Qt6Core.dll").write_bytes(b"fixture")
        components, _ = evidence.qt_components(self.model, self.root, "studio")
        self.assertIn("qt:blake2", {entry["bom-ref"] for entry in components})
        self.assertNotIn("qt:libdbus-1-headers", {entry["bom-ref"] for entry in components})

    def test_unreviewed_modules_still_require_an_inventory(self):
        (self.root / "Qt6WebEngineCore.dll").write_bytes(b"fixture")
        with self.assertRaisesRegex(ValueError, 'webenginecore'):
            evidence.qt_components(self.model, self.root, "studio")

    def test_each_product_uses_its_own_qt_version_and_inventory(self):
        (self.root / "Qt6Core.dll").write_bytes(b"fixture")
        directories = {product: self.root / product for product in ("studio", "hub")}
        for product, version in (("studio", "6.8.3"), ("hub", "6.9.0")):
            evidence.write_json(directories[product] / "Qt6-SOURCES.json", {
                "qt_version": version, "components": [{"id": product, "module": "qtcore", "name": product,
                    "version": "1.0", "license": "MIT", "attribution": "https://example.invalid/source"}]})
        with mock.patch.object(evidence.collect_legal_texts, "LEGAL_SOURCE_DIRS", directories):
            for product, version in (("studio", "6.8.3"), ("hub", "6.9.0")):
                with self.subTest(product=product):
                    self.query_version.return_value = version
                    components, _ = evidence.qt_components(self.model, self.root, product)
                    self.assertEqual({entry["bom-ref"] for entry in components}, {"qt", "qt:" + product})
            with self.assertRaisesRegex(ValueError, "no reviewed component inventory"):
                evidence.qt_components(self.model, self.root, "studio")


class ReleaseEvidenceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        (self.repo / ".gitignore").write_text(".temp/\n", encoding="utf-8")
        self.payload = self.root / "payload"
        self.payload.mkdir()
        self.output = self.root / "output"
        self.build = self.root / "build"
        self.dependency = self.repo / "ThirdParty" / "example"
        self.dependency.mkdir(parents=True)
        self.write_notice()
        (self.dependency / "LICENSE").write_text("Example license\n", encoding="utf-8")
        (self.dependency / "example.c").write_text("int example(void) { return 1; }\n", encoding="utf-8")
        self.entry = {"id": "example", "path": "ThirdParty/example", "notice": "Example",
                      "url": "https://example.invalid/source",
                      "evidence": ["ThirdParty/example/LICENSE"]}
        self.inputs = [self.entry]
        self.run_git("init", "--quiet")
        self.run_git("config", "user.email", "test@example.invalid")
        self.run_git("config", "user.name", "Release test")
        self.commit()
        self.head = self.run_git("rev-parse", "HEAD")
        (self.payload / "app.exe").write_bytes(b"MZfixture")
        self.identity = {"gitCommit": self.head, "gitDirty": False, "version": "1.0.0",
                         "hubVersion": "1.0.0", "buildId": "test-build", "platform": "test-x64", "channel": "dev"}
        self.model = {"identity": self.identity, "paths": {self.dependency / "example.c"},
                      "targets": ["harken_hub"], "artifacts": [{"target": "harken_hub", "type": "EXECUTABLE",
                       "name": "app.exe", "sha256": evidence.sha256(self.payload / "app.exe")}],
                      "cache": {}, "toolchains": [], "cmakeVersion": "3.31.6"}
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(evidence, "REPO_ROOT", self.repo).start()
        mock.patch.object(evidence, "COMPONENT_INPUTS", self.inputs).start()
        self.legal_directories = {"studio": self.repo / "HarkenStudio/Resources/Legal",
                                  "hub": self.repo / "HarkenHub/Resources/Legal"}
        mock.patch.object(evidence.collect_legal_texts, "LEGAL_SOURCE_DIRS", self.legal_directories).start()

    def write_notice(self, version="1.0", license_name="MIT", extra=""):
        (self.repo / "NOTICE").write_text(
            "| Name | Version | License | Use | How obtained |\n|---|---|---|---|---|\n"
            f"| Example | {version} | {license_name} | Test dependency | Vendored |\n{extra}",
            encoding="utf-8", newline="\n")

    def run_git(self, *args):
        return evidence.git(self.repo, *args)

    def commit(self):
        self.run_git("add", ".")
        self.run_git("commit", "--quiet", "-m", "test fixture")

    def prepare(self, **kwargs):
        return evidence.prepare("hub", self.payload, self.build, self.output, "1.0.0", "test-build",
                                model=self.model, **kwargs)

    def test_package_evidence_records_the_commit_without_source_archives(self):
        directory = self.prepare()
        package = self.root / "installer.exe"
        package.write_bytes(b"installer")
        evidence.finalize(directory, [package], self.payload)
        self.assertFalse((self.repo / ".temp").exists())
        self.assertEqual(list(self.output.rglob("*.tar.gz")), [])
        bom = evidence.read_json(directory / "sbom.cdx.json")
        self.assertEqual(bom["bomFormat"], "CycloneDX")
        self.assertEqual(bom["components"][0]["version"], "1.0")
        receipt = evidence.read_json(directory / "receipt.json")
        self.assertEqual(receipt["identity"]["gitCommit"], self.head)
        self.assertEqual(receipt["artifacts"][0]["sha256"], evidence.sha256(package))
        self.assertEqual(receipt["releaseApproval"], "pending")
        self.assertNotIn("sources", receipt)
        self.assertEqual(receipt["qtSourceReferences"], [])
        self.assertEqual({path.name for path in directory.iterdir()}, {"sbom.cdx.json", "receipt.json"})
        self.assertNotIn("_payloadState", receipt)
        for record in receipt["evidenceFiles"]:
            self.assertEqual(record["sha256"], evidence.sha256(directory / record["path"]))

    def test_qt_sources_are_recorded_without_downloading(self):
        sources = [{"module": "qtbase", "commit": "a" * 40,
                    "url": "https://example.invalid/qtbase.tar.gz", "sha256": "b" * 64}]
        evidence.write_json(self.legal_directories["hub"] / "Qt6-SOURCES.json", {"source_archives": sources})
        evidence.write_json(self.legal_directories["studio"] / "Qt6-SOURCES.json", {"source_archives": []})
        self.commit()
        self.identity["gitCommit"] = self.run_git("rev-parse", "HEAD")
        kit = self.root / "qt"
        kit.mkdir()
        (kit / "Qt6Config.cmake").write_text("set(QT_VERSION 6.8.3)\n", encoding="utf-8")
        self.model["cache"]["Qt6_DIR"] = str(kit)
        qt = evidence.component({"id": "qt", "name": "Qt", "version": "6.8.3",
                                 "license": "LGPL-3.0-only", "url": "https://code.qt.io/qt/"})
        with mock.patch.object(evidence, "qt_components", return_value=([qt], kit)) as qt_components, \
                mock.patch("urllib.request.urlopen", side_effect=AssertionError("unexpected download")) as download:
            directory = self.prepare()
        download.assert_not_called()
        qt_components.assert_called_once_with(self.model, self.payload, "hub")
        self.assertEqual(evidence.read_json(directory / "receipt.json")["qtSourceReferences"], sources)
        self.assertFalse((self.repo / ".temp").exists())
        with tarfile.open(directory / "qt-build-metadata.tar.gz") as archive:
            self.assertEqual(archive.getnames(), ["Qt6Config.cmake"])

    def test_each_product_resolves_same_named_license_from_its_own_packaging_inputs(self):
        self.entry.pop("evidence")
        self.entry["legalFiles"] = ["Example-LICENSE.txt"]
        files = {}
        for product, directory in self.legal_directories.items():
            directory.mkdir(parents=True)
            source = directory / "Example-LICENSE.txt"
            source.write_text(product + " license\n", encoding="utf-8")
            files[product] = ((source, source.name),)
        with mock.patch.object(evidence.collect_legal_texts, "THIRD_PARTY_FILES", files):
            for product in files:
                with self.subTest(product=product):
                    entries, _ = evidence.resolve_components(self.model, product)
                    source = files[product][0][0]
                    expected = [{"path": source.relative_to(self.repo).as_posix(), "sha256": evidence.sha256(source)}]
                    properties = evidence.component(entries[0])["properties"]
                    self.assertEqual(next(item["value"] for item in properties
                                          if item["name"] == "release:license-evidence"),
                                     evidence.json.dumps(expected))
            files["hub"][0][0].unlink()
            evidence.resolve_components(self.model, "studio")
            with self.assertRaisesRegex(ValueError, "license evidence is missing"):
                evidence.resolve_components(self.model, "hub")

    def test_runtime_and_sdk_evidence_does_not_require_desktop_legal_materials(self):
        with mock.patch.object(evidence.collect_legal_texts, "THIRD_PARTY_FILES", {}), \
                mock.patch.object(evidence.collect_legal_texts, "LEGAL_SOURCE_DIRS", {}):
            for product in ("runtime", "sdk"):
                with self.subTest(product=product):
                    entries, _ = evidence.resolve_components(self.model, product)
                    self.assertEqual(entries[0]["evidence"], ["ThirdParty/example/LICENSE"])
                    self.assertEqual(evidence.qt_components(self.model, self.payload, product), ([], None))

    def test_notice_is_the_source_of_sbom_version_and_license(self):
        self.write_notice("2.0", "BSD-3-Clause", "| Example | 2.0 | BSD-3-Clause | Another use | Vendored |\n")
        directory = self.prepare()
        component = evidence.read_json(directory / "sbom.cdx.json")["components"][0]
        self.assertEqual(component["version"], "2.0")
        self.assertEqual(component["licenses"], [{"license": {"name": "BSD-3-Clause"}}])

    def test_conflicting_notice_rows_block_packaging(self):
        for version, license_name in (("2.0", "MIT"), ("1.0", "BSD-3-Clause")):
            self.write_notice(extra=f"| Example | {version} | {license_name} | Another use | Vendored |\n")
            with self.subTest(version=version, license=license_name), self.assertRaises(ValueError):
                self.prepare()
        self.assertFalse(self.output.exists())

    def test_missing_notice_row_blocks_packaging(self):
        (self.repo / "NOTICE").write_text("Example appears in prose only\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            self.prepare()

    def test_notice_table_allows_escaped_pipes_and_rejects_shifted_columns(self):
        self.write_notice(license_name="MIT \\| BSD")
        self.assertEqual(evidence.notice_components()[0]["licenseName"], "MIT | BSD")
        self.write_notice(license_name="MIT | BSD")
        with self.assertRaises(ValueError):
            self.prepare()

    def test_fetched_dependency_requires_a_full_commit_in_notice(self):
        self.entry["sourceDirectory"] = "example-src"
        with self.assertRaises(ValueError):
            evidence.notice_components()

    def test_qt_metadata_only_retains_configured_modules_and_shared_settings(self):
        kit = self.root / "qt"
        paths = ["lib/cmake/Qt6/Qt6Config.cmake", "lib/cmake/Qt6Core/Qt6CoreConfig.cmake",
                 "lib/cmake/Qt6WebEngine/Qt6WebEngineConfig.cmake", "include/QtCore/header.h",
                 "mkspecs/qconfig.pri"]
        for name in paths:
            path = kit / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("metadata\n", encoding="utf-8")
        self.model["cache"] = {"Qt6_DIR": str(kit / "lib/cmake/Qt6"),
                               "Qt6Core_DIR": str(kit / "lib/cmake/Qt6Core")}
        self.assertEqual({path.relative_to(kit).as_posix() for path in evidence.qt_metadata(self.model, kit)},
                         {paths[0], paths[1], paths[4]})

    def test_manifest_reference_replaces_the_duplicate_payload_list(self):
        reference = {"artifact": "studio.zip", "path": "package-manifest.json", "sha256": "a" * 64}
        directory = self.prepare(payload_manifest=reference)
        receipt = evidence.read_json(directory / "receipt.json")
        self.assertEqual(receipt["payloadManifest"], reference)
        self.assertNotIn("payload", receipt)

    def test_failed_packaging_can_retry_without_touching_previous_outputs(self):
        self.output.mkdir()
        previous = self.output / "previous.zip"
        previous.write_bytes(b"previous")
        with self.assertRaisesRegex(RuntimeError, "installer failed"):
            with evidence.package_output(self.output) as staging:
                evidence.prepare("hub", self.payload, self.build, staging, "1.0.0", "test-build", model=self.model)
                (staging / "installer.exe").write_bytes(b"partial")
                raise RuntimeError("installer failed")
        self.assertEqual(list(self.output.iterdir()), [previous])
        with evidence.package_output(self.output) as staging:
            directory = evidence.prepare("hub", self.payload, self.build, staging, "1.0.0", "test-build",
                                         model=self.model)
            package = staging / "installer.exe"
            package.write_bytes(b"complete")
            evidence.finalize(directory, [package], self.payload)
        self.assertEqual(previous.read_bytes(), b"previous")
        self.assertEqual((self.output / "installer.exe").read_bytes(), b"complete")
        self.assertTrue((self.output / directory.name / "receipt.json").is_file())
        self.assertEqual(list(self.output.glob(".packaging-*")), [])

    def test_output_conflict_preserves_the_previous_release(self):
        self.output.mkdir()
        previous = self.output / "installer.exe"
        previous.write_bytes(b"previous")
        with self.assertRaises(ValueError):
            with evidence.package_output(self.output) as staging:
                (staging / "new-file.txt").write_text("new", encoding="utf-8")
                (staging / previous.name).write_bytes(b"replacement")
        self.assertEqual(previous.read_bytes(), b"previous")
        self.assertEqual(list(self.output.iterdir()), [previous])

    def test_payload_hashes_are_reused_by_binary_verification_and_finalization(self):
        package = self.root / "installer.exe"
        package.write_bytes(b"installer")
        with mock.patch.object(evidence, "sha256", wraps=evidence.sha256) as digest:
            directory = self.prepare()
            evidence.finalize(directory, [package], self.payload)
        self.assertEqual(sum(call.args[0] == self.payload / "app.exe" for call in digest.call_args_list), 1)
        self.assertEqual(sum(call.args[0] == package for call in digest.call_args_list), 1)

    def test_tracked_installer_inputs_reference_the_project_source(self):
        directory = self.prepare()
        custom = self.root / "payload.wxs"
        custom.write_text("generated input", encoding="utf-8")
        evidence.retain_packaging_inputs(directory, {"NOTICE": self.repo / "NOTICE", "payload.wxs": custom})
        receipt = evidence.read_json(directory / "receipt.json")
        self.assertEqual(receipt["packagingInputs"]["NOTICE"]["projectSource"], "NOTICE")
        self.assertFalse((directory / "packaging-inputs/NOTICE").exists())
        self.assertEqual((directory / "packaging-inputs/payload.wxs").read_text(), "generated input")

    def test_platform_specific_installer_input_bytes_are_retained(self):
        directory = self.prepare()
        source = self.repo / "NOTICE"
        source.write_bytes(source.read_bytes().replace(b"\n", b"\r\n"))
        evidence.retain_packaging_inputs(directory, {"NOTICE": source})
        self.assertEqual((directory / "packaging-inputs/NOTICE").read_bytes(), source.read_bytes())

    def test_generated_installer_input_is_retained_in_place(self):
        directory = self.prepare()
        generated = directory / "packaging-inputs/payload.wxs"
        generated.parent.mkdir()
        generated.write_text("generated input", encoding="utf-8")
        with mock.patch.object(evidence.shutil, "copyfile", side_effect=AssertionError("duplicate input copy")):
            evidence.retain_packaging_inputs(directory, {"payload.wxs": generated})
        receipt = evidence.read_json(directory / "receipt.json")
        self.assertEqual(receipt["packagingInputs"]["payload.wxs"], evidence.file_record(generated, directory))
        self.assertEqual(list(directory.rglob("*.wxs")), [generated])

    def test_embedded_msi_is_distinguished_from_the_downloadable_installer(self):
        directory = self.prepare()
        msi = self.root / "installer.msi"
        exe = self.root / "installer.exe"
        msi.write_bytes(b"msi")
        exe.write_bytes(b"exe containing msi")
        evidence.finalize(directory, [msi, exe], self.payload, intermediate_artifacts=(msi,))
        receipt = evidence.read_json(directory / "receipt.json")
        self.assertEqual([item["path"] for item in receipt["artifacts"]], [exe.name])
        self.assertEqual([item["path"] for item in receipt["intermediateArtifacts"]], [msi.name])

    def test_fetched_dependency_is_recorded_from_the_existing_build_checkout(self):
        source = self.build / "_deps/example-src"
        source.mkdir(parents=True)
        subprocess.run(["git", "init", "--quiet", str(source)], check=True)
        for key, value in (("user.email", "test@example.invalid"), ("user.name", "Test")):
            evidence.git(source, "config", key, value)
        (source / "example.c").write_text("int example(void) { return 1; }\n", encoding="utf-8")
        evidence.git(source, "add", ".")
        evidence.git(source, "commit", "--quiet", "-m", "source")
        revision = evidence.git(source, "rev-parse", "HEAD")
        entry = {**self.entry, "sourceDirectory": "example"}
        del entry["path"]
        self.inputs[:] = [entry]
        self.write_notice(f"1.0 (commit {revision})")
        self.commit()
        self.identity["gitCommit"] = self.run_git("rev-parse", "HEAD")
        self.model["paths"] = {source / f"example{index}.c" for index in range(50)}
        with mock.patch.object(evidence, "git", wraps=evidence.git) as git_calls:
            directory = self.prepare()
        self.assertTrue(all(call.args[1] in {"rev-parse", "status"} for call in git_calls.call_args_list))
        self.assertEqual(sum(call.args[:2] == (source, "rev-parse") for call in git_calls.call_args_list), 1)
        self.assertEqual(sum(call.args[:2] == (source, "status") for call in git_calls.call_args_list), 1)
        bom = evidence.read_json(directory / "sbom.cdx.json")
        self.assertEqual(bom["components"][0]["version"], revision)
        self.assertEqual(bom["components"][0]["externalReferences"][0]["url"], entry["url"])
        self.assertIn({"name": "release:source-dirty", "value": "false"}, bom["components"][0]["properties"])
        self.assertFalse((self.repo / ".temp").exists())
        (source / "example.c").write_text("modified", encoding="utf-8")
        self.identity["channel"] = "stable"
        directory = evidence.prepare("hub", self.payload, self.build, self.output, "1.0.0", "modified",
                                     model=self.model)
        receipt = evidence.read_json(directory / "receipt.json")
        self.assertTrue(receipt["sourceState"]["dependencyDirty"])
        self.assertEqual(receipt["checks"]["cleanMatchingSource"], "dirty")
        bom = evidence.read_json(directory / "sbom.cdx.json")
        self.assertIn({"name": "release:source-dirty", "value": "true"}, bom["components"][0]["properties"])
        evidence.git(source, "add", ".")
        evidence.git(source, "commit", "--quiet", "-m", "different source")
        with self.assertRaises(ValueError):
            self.prepare()

    def test_all_channels_record_dirty_sources_without_blocking_packaging(self):
        (self.dependency / "example.c").write_text("changed\n", encoding="utf-8")
        for channel in ("stable", "beta", "dev", "nightly"):
            self.identity["channel"] = channel
            with self.subTest(channel=channel):
                directory = evidence.prepare("hub", self.payload, self.build, self.output, "1.0.0", channel,
                                             model=self.model)
                evidence.finalize(directory, [self.payload / "app.exe"], self.payload)
                receipt = evidence.read_json(directory / "receipt.json")
                self.assertEqual(receipt["sourceState"], {"configuredDirty": False, "checkoutDirty": True})
                self.assertEqual(receipt["checks"]["cleanMatchingSource"], "dirty")
                self.assertEqual(receipt["checks"]["packageArtifactsPresent"], "passed")
                self.assertEqual(receipt["releaseApproval"], "pending")

    def test_all_channels_record_dirty_source_and_current_license_hashes(self):
        (self.dependency / "LICENSE").write_text("modified license", encoding="utf-8")
        self.identity["gitDirty"] = True
        for channel in ("stable", "beta", "dev", "nightly"):
            self.identity["channel"] = channel
            directory = evidence.prepare("hub", self.payload, self.build, self.output, "1.0.0", channel,
                                         model=self.model, binary_identity=self.identity.copy())
            receipt = evidence.read_json(directory / "receipt.json")
            self.assertEqual(receipt["sourceState"], {"configuredDirty": True, "checkoutDirty": True})
            self.assertEqual(receipt["checks"]["cleanMatchingSource"], "dirty")
            component = evidence.read_json(directory / "sbom.cdx.json")["components"][0]
            materials = evidence.json.loads(next(item["value"] for item in component["properties"]
                                                if item["name"] == "release:license-evidence"))
            self.assertEqual(materials[0]["sha256"], evidence.sha256(self.dependency / "LICENSE"))

    def test_missing_license_evidence_blocks_packaging(self):
        (self.dependency / "LICENSE").unlink()
        with self.assertRaises(ValueError):
            self.prepare()

    def test_unregistered_dependency_blocks_packaging(self):
        self.model["paths"].add(self.repo / "ThirdParty/unknown/library.c")
        with self.assertRaises(ValueError):
            self.prepare()

    def test_unreviewed_system_compression_supplier_blocks_packaging(self):
        self.entry["id"] = "minizip-ng"
        with self.assertRaises(ValueError):
            evidence.resolve_components(self.model, "hub")

    def test_apple_compression_is_recorded_as_a_system_library(self):
        self.entry["id"] = "minizip-ng"
        self.inputs.append({"id": "apple-libcompression", "notice": "Apple libcompression",
                            "scope": "system-library-not-redistributed", "url": "https://example.invalid/compression",
                            "evidence": []})
        self.write_notice(extra="| Apple libcompression | macOS SDK | Apple system library | Compression | System |\n")
        self.model["paths"].add(self.dependency / "mz_strm_libcomp.c")
        entries, _ = evidence.resolve_components(self.model, "hub")
        system = next(entry for entry in entries if entry["id"] == "apple-libcompression")
        self.assertEqual(system["scope"], "system-library-not-redistributed")

    def test_payload_with_unmatched_binary_is_rejected(self):
        (self.payload / "extra.dll").write_bytes(b"MZunregistered")
        with self.assertRaises(ValueError):
            self.prepare()

    def test_changed_binary_is_rejected_even_with_the_same_filename(self):
        (self.payload / "app.exe").write_bytes(b"MZchanged")
        with self.assertRaises(ValueError):
            self.prepare()

    def test_unmatched_import_library_is_rejected(self):
        (self.payload / "unknown.lib").write_bytes(b"!<arch>\nunknown")
        with self.assertRaises(ValueError):
            self.prepare()

    def test_macos_executable_without_extension_is_verified(self):
        (self.payload / "unknown").write_bytes(b"\xcf\xfa\xed\xfeunknown")
        with self.assertRaises(ValueError):
            self.prepare()

    def macos_deployment_fixture(self):
        self.identity["platform"] = "macos-arm64"
        bundle = self.build / "bin/Release/HarkenStudio.app"
        binary = bundle / "Contents/MacOS/HarkenStudio"
        binary.parent.mkdir(parents=True)
        binary.write_bytes(b"MZfixture")
        library = self.build / "bin/Release/libQWKCore.dylib"
        library.write_bytes(b"\xcf\xfa\xed\xfebuild-library")
        self.model["artifacts"] = [
            {"target": target, "type": kind, "name": path.name,
             "sha256": evidence.sha256(path), "path": path.relative_to(self.build).as_posix()}
            for target, kind, path in (("harken_editor", "EXECUTABLE", binary),
                                      ("QWKCore", "SHARED_LIBRARY", library))]
        tool = self.root / "macdeployqt"
        tool.write_bytes(b"deployment tool")
        deployed = bundle / "Contents/Frameworks/libQWKCore.dylib"
        return bundle, library, tool, deployed

    def run_macos_deployment(self, bundle, tool, transform):
        with mock.patch.object(evidence, "build_model", return_value=self.model), \
                mock.patch.object(deploy_macos_bundle.subprocess, "run", side_effect=transform):
            deploy_macos_bundle.deploy(self.build, bundle, tool, "Release")

    def test_macos_deployment_records_and_verifies_transformed_build_libraries(self):
        bundle, library, tool, deployed = self.macos_deployment_fixture()
        def transform(command, check):
            self.assertEqual(command, [str(tool), str(bundle), "-always-overwrite"])
            self.assertTrue(check)
            self.assertEqual(deployed.read_bytes(), library.read_bytes())
            deployed.write_bytes(b"\xcf\xfa\xed\xfedeployed-library")
        self.run_macos_deployment(bundle, tool, transform)
        report = evidence.read_macos_deployment(self.build, "studio", "Release", self.model)
        self.assertEqual(report["tool"], {"name": "macdeployqt", "sha256": evidence.sha256(tool)})
        self.assertEqual(report["libraries"][0]["sourceArtifact"], self.model["artifacts"][1])
        self.assertEqual(report["libraries"][0]["sha256"], evidence.sha256(deployed))
        origins = evidence.verify_payload_binaries(bundle.parent, self.model, None, set(), deployment=report)
        origin = next(record for record in origins if record["path"] == report["libraries"][0]["path"])
        self.assertEqual(origin["origin"], "CMake target via macdeployqt")
        with self.assertRaises(ValueError):
            evidence.verify_payload_binaries(bundle.parent, self.model, None, set())
        deployed.write_bytes(b"\xcf\xfa\xed\xfetampered-library")
        with self.assertRaises(ValueError):
            evidence.verify_payload_binaries(bundle.parent, self.model, None, set(), deployment=report)

    def test_macos_deployment_rejects_stale_or_mismatched_receipts(self):
        bundle, library, tool, deployed = self.macos_deployment_fixture()
        self.run_macos_deployment(bundle, tool, lambda *args, **kwargs: deployed.write_bytes(b"deployed"))
        path = evidence.macos_deployment_path(self.build, "studio", "Release")
        original = path.read_bytes()
        for field in ("commit", "platform", "source-hash", "target", "path", "duplicate"):
            with self.subTest(field=field):
                path.write_bytes(original)
                report = evidence.read_json(path)
                if field == "commit":
                    report["identity"]["gitCommit"] = "0" * 40
                elif field == "platform":
                    report["identity"]["platform"] = "windows-x64"
                elif field == "source-hash":
                    report["libraries"][0]["sourceArtifact"]["sha256"] = "0" * 64
                elif field == "target":
                    report["libraries"][0]["sourceArtifact"]["target"] = "unbuilt-library"
                elif field == "path":
                    report["libraries"][0]["path"] = "HarkenStudio.app/Contents/Frameworks/other.dylib"
                else:
                    report["libraries"].append(report["libraries"][0])
                evidence.write_json(path, report)
                with self.assertRaises(ValueError):
                    evidence.read_macos_deployment(self.build, "studio", "Release", self.model)

    def test_failed_macos_deployment_invalidates_the_previous_receipt(self):
        bundle, library, tool, deployed = self.macos_deployment_fixture()
        receipt = evidence.macos_deployment_path(self.build, "studio", "Release")
        evidence.write_json(receipt, {"previous": True})
        def fail(*args, **kwargs):
            raise subprocess.CalledProcessError(1, "macdeployqt")
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_macos_deployment(bundle, tool, fail)
        self.assertFalse(receipt.exists())

    def test_macos_deployment_rejects_source_changes_during_deployment(self):
        bundle, library, tool, deployed = self.macos_deployment_fixture()
        with self.assertRaises(ValueError):
            self.run_macos_deployment(bundle, tool, lambda *args, **kwargs: library.write_bytes(b"changed"))
        self.assertFalse(evidence.macos_deployment_path(self.build, "studio", "Release").exists())

    def test_payload_changes_during_packaging_are_rejected(self):
        directory = self.prepare()
        (self.payload / "app.exe").write_bytes(b"changed")
        with self.assertRaises(ValueError):
            evidence.finalize(directory, [], self.payload)

    def test_missing_artifact_never_marks_packaging_complete(self):
        directory = self.prepare()
        with self.assertRaises(ValueError):
            evidence.finalize(directory, [self.root / "missing.zip"], self.payload)
        self.assertEqual(evidence.read_json(directory / "receipt.json")["artifacts"], [])

    def test_existing_evidence_is_not_overwritten(self):
        directory = self.prepare()
        before = (directory / "receipt.json").read_bytes()
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertEqual((directory / "receipt.json").read_bytes(), before)

    def test_binary_identity_must_match_configured_commit(self):
        other = {**self.identity, "gitCommit": "0" * 40}
        with self.assertRaises(ValueError):
            self.prepare(binary_identity=other)

    def write_file_api_fixture(self):
        reply = self.build / ".cmake/api/v1/reply"
        target = {"id": "hub", "name": "harken_hub", "type": "EXECUTABLE",
                  "sources": [{"path": "ThirdParty/example/example.c"}],
                  "artifacts": [{"path": "bin/app.exe"}]}
        target_path = self.build / "bin/app.exe"
        target_path.parent.mkdir(parents=True)
        shutil.copyfile(self.payload / "app.exe", target_path)
        evidence.write_json(reply / "target.json", target)
        objects = {"codemodel": {"paths": {"source": str(self.repo)}, "configurations": [
            {"name": "Release", "targets": [{"id": "hub", "jsonFile": "target.json"}]}]},
            "cache": {"entries": []}, "toolchains": {"toolchains": []}}
        for kind, value in objects.items():
            evidence.write_json(reply / f"{kind}.json", value)
        evidence.write_json(reply / "index-test.json", {"objects": [
            {"kind": kind, "jsonFile": f"{kind}.json"} for kind in objects],
            "cmake": {"version": {"string": "3.31.6"}}})
        evidence.write_json(self.build / "generated/release-identity.json", self.identity)

    def test_file_api_resolves_only_product_dependency_graph(self):
        self.write_file_api_fixture()
        model = evidence.build_model(self.build, "hub", "Release")
        self.assertEqual(model["targets"], ["harken_hub"])
        self.assertIn(self.dependency / "example.c", model["paths"])
        self.assertEqual(model["artifacts"][0]["sha256"], evidence.sha256(self.payload / "app.exe"))

    def test_dirty_build_identity_is_preserved_after_worktree_is_clean(self):
        self.write_file_api_fixture()
        for channel in ("stable", "beta", "dev", "nightly"):
            evidence.write_json(self.build / "generated/release-identity.json",
                                {**self.identity, "gitDirty": True, "channel": channel})
            with self.subTest(channel=channel):
                directory = evidence.prepare("hub", self.payload, self.build, self.output, "1.0.0", channel)
                receipt = evidence.read_json(directory / "receipt.json")
                self.assertTrue(receipt["identity"]["gitDirty"])
                self.assertEqual(receipt["sourceState"], {"configuredDirty": True, "checkoutDirty": False})
                self.assertEqual(receipt["checks"]["cleanMatchingSource"], "dirty")

    def test_untracked_qt_log_is_recorded_without_blocking_release(self):
        (self.repo / "aqtinstall.log").write_text("Qt installation log\n", encoding="utf-8")
        self.assertEqual(self.run_git("status", "--porcelain"), "?? aqtinstall.log")
        self.write_file_api_fixture()
        for channel in ("stable", "beta"):
            evidence.write_json(self.build / "generated/release-identity.json",
                                {**self.identity, "gitDirty": True, "channel": channel})
            with self.subTest(channel=channel):
                directory = evidence.prepare("hub", self.payload, self.build, self.output, "1.0.0", channel)
                receipt = evidence.read_json(directory / "receipt.json")
                self.assertEqual(receipt["sourceState"], {"configuredDirty": True, "checkoutDirty": True})
                self.assertEqual(receipt["checks"]["cleanMatchingSource"], "dirty")

    def test_stale_build_commit_is_rejected(self):
        self.write_file_api_fixture()
        evidence.write_json(self.build / "generated/release-identity.json", {**self.identity, "gitCommit": "0" * 40})
        with self.assertRaises(ValueError):
            evidence.build_model(self.build, "hub", "Release")

    def test_sdk_cannot_mix_runtime_commits(self):
        evidence.write_json(self.root / "runtime/runtime-build.json", {"identity": {**self.identity, "gitCommit": "0" * 40}})
        with self.assertRaises(ValueError):
            evidence.read_runtime_receipts(self.root / "runtime", self.head)

    def test_sdk_requires_each_runtime_channel_to_match(self):
        channels = ("dev", "nightly", "beta", "stable")
        for channel in channels:
            first = {"identity": {**self.identity, "channel": channel},
                     "sourceState": {"configuredDirty": False, "checkoutDirty": False}}
            evidence.write_json(self.root / "runtime/first/runtime-build.json", first)
            for runtime_channel in channels:
                second = {**first, "identity": {**self.identity, "channel": runtime_channel}}
                evidence.write_json(self.root / "runtime/second/runtime-build.json", second)
                with self.subTest(channel=channel, runtime_channel=runtime_channel):
                    if channel == runtime_channel:
                        self.assertEqual(evidence.read_runtime_receipts(self.root / "runtime", self.head, channel),
                                         [first, second])
                    else:
                        with self.assertRaises(ValueError):
                            evidence.read_runtime_receipts(self.root / "runtime", self.head, channel)

    def test_all_channels_accept_dirty_runtime_receipts(self):
        for channel in ("stable", "beta", "dev", "nightly"):
            for configured_dirty, checkout_dirty in ((True, False), (False, True), (True, True)):
                receipt = {"identity": {**self.identity, "channel": channel, "gitDirty": configured_dirty},
                           "sourceState": {"configuredDirty": configured_dirty, "checkoutDirty": checkout_dirty}}
                evidence.write_json(self.root / "runtime/runtime-build.json", receipt)
                with self.subTest(channel=channel, configured=configured_dirty, checkout=checkout_dirty):
                    self.assertEqual(evidence.read_runtime_receipts(self.root / "runtime", self.head, channel), [receipt])

    def test_windows_qt_binary_must_match_the_actual_kit(self):
        kit = self.root / "qt"
        (kit / "bin").mkdir(parents=True)
        (kit / "bin/Qt6Core.dll").write_bytes(b"MZsupplier")
        deployed = self.payload / "Qt6Core.dll"
        shutil.copyfile(kit / "bin/Qt6Core.dll", deployed)
        records = evidence.verify_payload_binaries(self.payload, self.model, kit, set())
        self.assertIn("Qt kit", {record["origin"] for record in records})
        deployed.write_bytes(b"MZchanged")
        with self.assertRaises(ValueError):
            evidence.verify_payload_binaries(self.payload, self.model, kit, set())

    def test_qt_build_tools_are_not_accepted_as_runtime_dependencies(self):
        kit = self.root / "qt"
        (kit / "bin").mkdir(parents=True)
        (kit / "bin/moc.exe").write_bytes(b"MZtool")
        shutil.copyfile(kit / "bin/moc.exe", self.payload / "moc.exe")
        with self.assertRaises(ValueError):
            evidence.verify_payload_binaries(self.payload, self.model, kit, set())

    def test_android_receipt_retains_actual_ndk_notices(self):
        ndk = self.root / "ndk"
        ndk.mkdir()
        (ndk / "source.properties").write_text("Pkg.Revision = 27.2.12479018\n", encoding="utf-8")
        for name in ("NOTICE", "NOTICE.toolchain"):
            (ndk / name).write_text("Test license " + name, encoding="utf-8")
        model = {**self.model, "identity": {**self.identity, "platform": "android-arm64"},
                 "cache": {"CMAKE_ANDROID_NDK": str(ndk)}}
        output = self.root / "runtime/runtime-build.json"
        with mock.patch.object(evidence, "build_model", return_value=model):
            evidence.record_runtime(self.build, output, "Release")
        record = evidence.read_json(output)
        self.assertEqual(record["components"][-1]["version"], "27.2.12479018")
        self.assertEqual(len(record["notices"]), 2)
        for item in record["notices"]:
            self.assertEqual(item["sha256"], evidence.sha256(output.parent / item["path"]))

    def test_sdk_payload_version_must_match_the_source_checkout(self):
        for root, version in ((self.repo, "2.3.4"), (self.payload, "2.3.5")):
            plugin = root / "HarkenSDK/Unity/Harken"
            plugin.mkdir(parents=True)
            (plugin / "LICENSE.md").write_text("license\n", encoding="utf-8")
            (plugin / "NOTICE.md").write_text("notice\n", encoding="utf-8")
            evidence.write_json(plugin / "package.json", {"version": version})
        evidence.write_json(self.root / "runtime/runtime-build.json", {
            "identity": self.identity,
            "sourceState": {"configuredDirty": False, "checkoutDirty": False}})
        with mock.patch.object(evidence.collect_legal_texts, "REPO_ROOT", self.repo):
            with self.assertRaisesRegex(ValueError, "SDK version differs from the source checkout"):
                evidence.prepare_sdk(self.payload, self.root / "runtime", self.output)

    def test_sdk_retains_dirty_runtime_receipts_and_notices(self):
        sdk_source = self.repo / "HarkenSDK"
        sdk = self.payload / "HarkenSDK"
        for root in (sdk_source, sdk):
            root.mkdir(parents=True)
            plugin = root / "Unity/Harken"
            plugin.mkdir(parents=True)
            (plugin / "LICENSE.md").write_text("license\n", encoding="utf-8")
            (plugin / "NOTICE.md").write_text("notice\n", encoding="utf-8")
            evidence.write_json(plugin / "package.json", {"version": "2.3.4"})
        binaries = [sdk / "Unity/Harken/Runtime/Plugins/Windows/HarkenCore.dll"]
        for binary in binaries:
            binary.parent.mkdir(parents=True)
            shutil.copyfile(self.payload / "app.exe", binary)
        self.commit()
        commit = self.run_git("rev-parse", "HEAD")
        runtime = self.root / "runtime/android-arm64"
        runtime.mkdir(parents=True)
        notice = runtime / "Android-NDK-NOTICE.txt"
        notice.write_text("actual toolchain license\n", encoding="utf-8")
        receipt = {"identity": {**self.identity, "gitCommit": commit, "platform": "android-arm64", "channel": "stable"},
                   "sourceState": {"configuredDirty": False, "checkoutDirty": False},
                   "artifacts": self.model["artifacts"], "components": [], "toolchains": [],
                   "targets": ["harken_core_shared"], "cmakeVersion": "3.31.6",
                   "notices": [evidence.file_record(notice, runtime)]}
        evidence.write_json(runtime / "runtime-build.json", receipt)
        second_runtime = self.root / "runtime/android-armv7"
        second_runtime.mkdir()
        shutil.copyfile(notice, second_runtime / notice.name)
        evidence.write_json(second_runtime / "runtime-build.json", {
            **receipt, "identity": {**receipt["identity"], "platform": "android-armv7", "gitDirty": True},
            "sourceState": {"configuredDirty": True, "checkoutDirty": False}})
        with mock.patch.object(evidence.collect_legal_texts, "REPO_ROOT", self.repo), \
                mock.patch.object(evidence, "sha256", wraps=evidence.sha256) as digest, \
                mock.patch.object(evidence.collect_legal_texts, "verify_sdk",
                                  wraps=evidence.collect_legal_texts.verify_sdk) as verify_sdk:
            directory = evidence.prepare_sdk(self.payload, self.root / "runtime", self.output, "stable")
        verify_sdk.assert_called_once_with(sdk)
        for binary in binaries:
            self.assertEqual(sum(call.args[0] == binary for call in digest.call_args_list), 1)
        sums = dict(line.split("  ", 1)[::-1] for line in (self.payload / "SHA256SUMS.txt").read_text().splitlines())
        for binary in binaries:
            self.assertEqual(sums[binary.relative_to(self.payload).as_posix()], evidence.sha256(binary))
        self.assertNotIn("HarkenSDK/Unity/Harken/package.json", sums)
        evidence.finalize(directory, [], self.payload)
        package_receipt = evidence.read_json(directory / "receipt.json")
        self.assertEqual(package_receipt["identity"]["gitCommit"], commit)
        self.assertEqual(package_receipt["identity"]["version"], "2.3.4")
        self.assertTrue(package_receipt["identity"]["gitDirty"])
        self.assertTrue(package_receipt["sourceState"]["runtimeDirty"])
        self.assertEqual(package_receipt["checks"]["cleanMatchingSource"], "dirty")
        self.assertNotIn("sources", package_receipt)
        self.assertFalse((self.repo / ".temp").exists())
        self.assertEqual(len(evidence.read_runtime_receipts(self.payload, commit, "stable")), 2)
        self.assertEqual(list((sdk / "BuildEvidence").rglob("*.txt")), [])
        self.assertEqual(len(list(sdk.rglob(notice.name))), 1)
        for record in package_receipt["payload"]:
            self.assertEqual(record["sha256"], evidence.sha256(self.payload / record["path"]))
        plugin_notice = sdk / "Unity/Harken/Legal" / notice.name
        self.assertEqual(plugin_notice.read_bytes(), notice.read_bytes())
        plugin_notice.unlink()
        with self.assertRaises(ValueError):
            evidence.read_runtime_receipts(self.payload, commit, "stable")
        shutil.copyfile(notice, plugin_notice)
        plugin_notice.write_text("changed\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            evidence.read_runtime_receipts(self.payload, commit, "stable")

    def test_sdk_directory_can_include_its_own_evidence_without_an_extra_zip(self):
        with evidence.package_output(self.payload) as staging:
            directory = evidence.prepare("hub", self.payload, self.build, staging, "1.0.0", "test-build",
                                         model=self.model)
            evidence.finalize(directory, [], self.payload)
        receipt = evidence.read_json(self.payload / directory.name / "receipt.json")
        self.assertEqual(receipt["checks"]["payloadReadyForArtifactUpload"], "passed")
        self.assertEqual([item["path"] for item in receipt["payload"]], ["app.exe"])
        self.assertEqual(list(self.payload.glob("*.zip")), [])


if __name__ == "__main__":
    unittest.main()
