from __future__ import annotations

import json
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / ".scripts" / "runtime_callback_closure.py"
SPEC = importlib.util.spec_from_file_location(
    "runtime_callback_closure", SCRIPT
)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def node(
    node_id: str,
    qualified: str,
    source: str = "HarkenEngine/Source/Runtime/render/src/Test.cpp",
    body: str = "a" * 64,
):
    return MODULE.Node(
        id=node_id,
        usr="usr:" + node_id,
        mangled="",
        qualified_name=qualified,
        display_name=qualified.rsplit("::", 1)[-1] + "()",
        source=source,
        line=1,
        column=1,
        body_sha256=body,
        scope="repository",
        features={
            "hasLoop": False,
            "hasTry": False,
            "hasThrow": False,
            "hasNew": False,
            "hasDelete": False,
            "raiiDeclarations": [],
        },
    )


def classification(body: str = "a" * 64) -> dict[str, str]:
    return {
        "bodySha256": body,
        "allocation": "none",
        "blocking": "none",
        "io": "none",
        "exception": "no-direct-throw",
        "loopBound": "no-loop",
        "configurationBound": "release-runtime-configuration",
        "threadQos": "callback-or-render-worker",
        "evidenceOwner": "m2-wp0-ast",
        "cost": "constant",
    }


class RuntimeCallbackClosureTest(unittest.TestCase):
    def test_dependent_member_call_binds_receiver_type_only(self) -> None:
        class FakeType:
            def __init__(self, spelling: str, declaration=None, element=None):
                self.spelling = spelling
                self._declaration = declaration
                self._element = element

            def get_canonical(self):
                return self

            def get_declaration(self):
                return self._declaration

            def get_array_element_type(self):
                return self._element

        class FakeCursor:
            def __init__(
                self,
                kind: str,
                spelling: str = "",
                children=(),
                referenced=None,
                type=None,
            ):
                self.kind = SimpleNamespace(name=kind)
                self.spelling = spelling
                self._children = list(children)
                self.referenced = referenced
                self.type = type

            def get_children(self):
                return iter(self._children)

            def walk_preorder(self):
                yield self
                for child in self._children:
                    yield from child.walk_preorder()

        owner = FakeCursor("STRUCT_DECL", "BiquadState")
        owner.semantic_parent = SimpleNamespace(
            kind=SimpleNamespace(name="NAMESPACE"),
            spelling="harken",
            semantic_parent=SimpleNamespace(
                kind=SimpleNamespace(name="TRANSLATION_UNIT"),
                spelling="",
                semantic_parent=None,
            ),
        )
        field = FakeCursor("FIELD_DECL", "targetState")
        field.type = FakeType(
            "BiquadState[Channels]",
            element=FakeType("BiquadState", declaration=owner),
        )
        receiver_member = FakeCursor(
            "MEMBER_REF_EXPR", "targetState", referenced=field
        )
        receiver = FakeCursor(
            "ARRAY_SUBSCRIPT_EXPR", children=(receiver_member,)
        )
        member = FakeCursor(
            "CXX_DEPENDENT_SCOPE_MEMBER_EXPR",
            "Reset",
            children=(receiver,),
        )
        call = FakeCursor(
            "CALL_EXPR",
            children=(member,),
        )
        reset = node("biquad-reset", "harken::BiquadState::Reset")
        selection = node(
            "selection-reset", "harken::SelectionMemoryTableV1::Reset"
        )
        result = MODULE.resolve_unresolved_call(
            call, {reset.id: reset, selection.id: selection}
        )
        self.assertEqual(result.kind, "ast-receiver-member")
        self.assertEqual(result.targets, (reset,))

    def test_free_call_ignores_member_shaped_arguments(self) -> None:
        class FakeToken:
            def __init__(self, spelling: str):
                self.spelling = spelling

        class FakeCursor:
            def __init__(
                self,
                kind: str,
                spelling: str = "",
                children=(),
                tokens=(),
            ):
                self.kind = SimpleNamespace(name=kind)
                self.spelling = spelling
                self._children = list(children)
                self._tokens = [FakeToken(token) for token in tokens]

            def get_children(self):
                return iter(self._children)

            def get_tokens(self):
                return iter(self._tokens)

            def walk_preorder(self):
                yield self
                for child in self._children:
                    yield from child.walk_preorder()

        overloaded = FakeCursor(
            "OVERLOADED_DECL_REF", "BiquadCoeffsEqual"
        )
        callee = FakeCursor("DECL_REF_EXPR", children=(overloaded,))
        argument = FakeCursor("MEMBER_REF_EXPR", "targetCoeffs")
        call = FakeCursor(
            "CALL_EXPR",
            children=(callee, argument),
            tokens=(
                "BiquadCoeffsEqual",
                "(",
                "transition",
                ".",
                "targetCoeffs",
                ",",
                "targetCoeffs",
                ")",
            ),
        )
        target = node("biquad-equal", "harken::BiquadCoeffsEqual")

        result = MODULE.resolve_unresolved_call(call, {target.id: target})

        self.assertEqual(result.kind, "ast-repository-overload-set")
        self.assertEqual(result.targets, (target,))

    def test_nested_dependent_field_binds_element_owner_only(self) -> None:
        class FakeToken:
            def __init__(self, spelling: str):
                self.spelling = spelling

        class FakeType:
            def __init__(
                self,
                spelling: str,
                declaration=None,
                element=None,
                pointee=None,
                kind: str = "RECORD",
            ):
                self.spelling = spelling
                self._declaration = declaration
                self._element = element
                self._pointee = pointee
                self.kind = SimpleNamespace(name=kind)

            def get_canonical(self):
                return self

            def get_declaration(self):
                return self._declaration

            def get_array_element_type(self):
                return self._element

            def get_pointee(self):
                return self._pointee

        class FakeCursor:
            def __init__(
                self,
                kind: str,
                spelling: str = "",
                children=(),
                referenced=None,
                type=None,
                tokens=(),
            ):
                self.kind = SimpleNamespace(name=kind)
                self.spelling = spelling
                self._children = list(children)
                self.referenced = referenced
                self.type = type
                self._tokens = [FakeToken(token) for token in tokens]

            def get_children(self):
                return iter(self._children)

            def get_tokens(self):
                return iter(self._tokens)

            def walk_preorder(self):
                yield self
                for child in self._children:
                    yield from child.walk_preorder()

        namespace = SimpleNamespace(
            kind=SimpleNamespace(name="NAMESPACE"),
            spelling="harken",
            semantic_parent=SimpleNamespace(
                kind=SimpleNamespace(name="TRANSLATION_UNIT"),
                spelling="",
                semantic_parent=None,
            ),
        )
        state_owner = FakeCursor("STRUCT_DECL", "BiquadState")
        state_owner.semantic_parent = namespace
        state_type = FakeType("BiquadState", declaration=state_owner)
        state_field = FakeCursor(
            "FIELD_DECL",
            "targetState",
            type=FakeType("BiquadState[Channels]", element=state_type),
        )
        transition_owner = FakeCursor(
            "STRUCT_DECL", "BiquadTransitionState", children=(state_field,)
        )
        transition_owner.semantic_parent = namespace
        transition_value_type = FakeType(
            "BiquadTransitionState<Channels>", declaration=transition_owner
        )
        transition_type = FakeType(
            "BiquadTransitionState<Channels> &",
            pointee=transition_value_type,
            kind="LVALUEREFERENCE",
        )
        transition_parameter = FakeCursor(
            "DECL_REF_EXPR", "transition", type=transition_type
        )
        field_access = FakeCursor(
            "MEMBER_REF_EXPR",
            children=(transition_parameter,),
            tokens=("transition", ".", "targetState"),
        )
        receiver = FakeCursor(
            "ARRAY_SUBSCRIPT_EXPR",
            children=(field_access,),
            tokens=("transition", ".", "targetState", "[", "ch", "]"),
        )
        callee = FakeCursor(
            "MEMBER_REF_EXPR",
            children=(receiver,),
            tokens=(
                "transition",
                ".",
                "targetState",
                "[",
                "ch",
                "]",
                ".",
                "Reset",
            ),
        )
        call = FakeCursor(
            "CALL_EXPR",
            children=(callee,),
            tokens=(
                "transition",
                ".",
                "targetState",
                "[",
                "ch",
                "]",
                ".",
                "Reset",
                "(",
                ")",
            ),
        )
        state_reset = node("state-reset", "harken::BiquadState::Reset")
        transition_reset = node(
            "transition-reset", "harken::BiquadTransitionState::Reset"
        )

        result = MODULE.resolve_unresolved_call(
            call,
            {
                state_reset.id: state_reset,
                transition_reset.id: transition_reset,
            },
        )

        self.assertEqual(result.kind, "ast-receiver-member")
        self.assertEqual(result.targets, (state_reset,))

    def test_rejects_toolchain_drift(self) -> None:
        root = Path("C:/repo").resolve(strict=False)
        cache = {
            "CMAKE_HOME_DIRECTORY": str(root),
            "CMAKE_GENERATOR": MODULE.EXPECTED_GENERATOR,
            "CMAKE_GENERATOR_PLATFORM": MODULE.EXPECTED_PLATFORM,
            "CMAKE_GENERATOR_TOOLSET": "v143,version=14.43.0",
            "CMAKE_SYSTEM_VERSION": MODULE.EXPECTED_WINDOWS_SDK,
            "HARKEN_BUILD_RT_QUALIFICATION": "ON",
            **MODULE.REQUIRED_PROBE_CACHE,
        }
        with self.assertRaises(MODULE.ClosureError):
            MODULE.validate_cache_contract(cache, root)

    def test_rejects_parse_diagnostics(self) -> None:
        with self.assertRaises(MODULE.ClosureError):
            MODULE.reject_parse_diagnostics(
                [{
                    "severity": 3,
                    "spelling": "parse failed",
                    "source": "Test.cpp",
                    "line": 1,
                    "column": 1,
                }]
            )

    def test_rejects_missing_root(self) -> None:
        nodes = {
            "other": node(
                "other",
                "harken::MixerLoop::RenderBlock",
                "HarkenEngine/Source/Runtime/render/src/Other.cpp",
            )
        }
        with self.assertRaises(MODULE.ClosureError):
            MODULE.resolve_roots(nodes)

    def test_rejects_missing_translation_unit(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            project = root / "harken_core.vcxproj"
            include = (
                root / "HarkenEngine/Source/Runtime/render/include"
            )
            include.mkdir(parents=True)
            missing = (
                root / "HarkenEngine/Source/Runtime/render/src/Missing.cpp"
            )
            project.write_text(
                f"""<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <WindowsTargetPlatformVersion>{MODULE.EXPECTED_WINDOWS_SDK}</WindowsTargetPlatformVersion>
    <PlatformToolset>v143</PlatformToolset>
  </PropertyGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'">
    <ClCompile>
      <AdditionalIncludeDirectories>{include}</AdditionalIncludeDirectories>
      <PreprocessorDefinitions>WIN32;_WINDOWS;NDEBUG</PreprocessorDefinitions>
      <LanguageStandard>stdcpp20</LanguageStandard>
      <RuntimeLibrary>MultiThreadedDLL</RuntimeLibrary>
      <ExceptionHandling>Sync</ExceptionHandling>
    </ClCompile>
  </ItemDefinitionGroup>
  <ItemGroup><ClCompile Include="{missing}" /></ItemGroup>
</Project>""",
                encoding="utf-8",
            )
            with self.assertRaises(MODULE.ClosureError):
                MODULE.project_release_configuration(project, root)

    def test_rejects_per_file_compile_metadata(self) -> None:
        item = MODULE.et.fromstring(
            """
<ClCompile Include="Test.cpp">
  <PreprocessorDefinitions Condition="'$(Configuration)'=='Release'">
    FILE_ONLY
  </PreprocessorDefinitions>
</ClCompile>
"""
        )
        with self.assertRaises(MODULE.ClosureError):
            MODULE.reject_per_file_compile_metadata(item)

    def test_cmake_inventory_excludes_probe_source_when_all_off(self) -> None:
        off_cache = {
            key: "OFF" for key in MODULE.REQUIRED_PROBE_CACHE
        }
        sources, record = MODULE.qualification_source_inventory(
            REPO_ROOT / "HarkenEngine" / "CMakeLists.txt",
            off_cache,
            REPO_ROOT,
        )
        relative = {
            path.relative_to(REPO_ROOT).as_posix()
            for path in sources
        }
        probe = (
            "HarkenEngine/Source/Runtime/core/src/RtProbe.cpp"
        )
        self.assertNotIn(probe, relative)
        self.assertIn(probe, record["conditionalProbeSources"])
        self.assertFalse(record["probesEnabled"])

        on_cache = dict(off_cache)
        on_cache["HARKEN_RT_PROBE_DEVICE_CALLBACK"] = "ON"
        on_sources, on_record = MODULE.qualification_source_inventory(
            REPO_ROOT / "HarkenEngine" / "CMakeLists.txt",
            on_cache,
            REPO_ROOT,
        )
        self.assertIn(
            probe,
            {
                path.relative_to(REPO_ROOT).as_posix()
                for path in on_sources
            },
        )
        self.assertTrue(on_record["probesEnabled"])

    def test_rejects_missing_or_unexpected_qualification_tu(self) -> None:
        root = Path("C:/repo")
        expected = [root / "Base.cpp", root / "Owned.cpp"]
        with self.assertRaisesRegex(MODULE.ClosureError, 'owned\\.cpp'):
            MODULE.validate_tu_inventory(
                expected, [root / "Base.cpp"]
            )
        with self.assertRaisesRegex(MODULE.ClosureError, 'extra\\.cpp'):
            MODULE.validate_tu_inventory(
                expected,
                [
                    root / "Base.cpp",
                    root / "Owned.cpp",
                    root / "Extra.cpp",
                ],
            )

    def test_rejects_unclassified_and_stale_nodes(self) -> None:
        nodes = {
            "a": node("a", "harken::A"),
            "b": node("b", "harken::B"),
        }
        manifest = {"classifications": {"a": classification()}}
        with self.assertRaises(MODULE.ClosureError):
            MODULE.validate_classifications(
                manifest, {"a", "b"}, nodes
            )
        manifest["classifications"]["stale"] = classification()
        with self.assertRaises(MODULE.ClosureError):
            MODULE.validate_classifications(
                manifest, {"a"}, {"a": nodes["a"]}
            )

    def test_rejects_duplicate_reachable_compiler_identity(self) -> None:
        first = node("first", "harken::A")
        duplicate = node("duplicate", "harken::A")
        duplicate.usr = first.usr
        duplicate.mangled = first.mangled
        nodes = {first.id: first, duplicate.id: duplicate}
        manifest = {
            "classifications": {
                first.id: classification(),
                duplicate.id: classification(),
            }
        }
        with self.assertRaises(MODULE.ClosureError):
            MODULE.validate_classifications(
                manifest, set(nodes), nodes
            )

    def test_rejects_body_digest_drift(self) -> None:
        nodes = {"a": node("a", "harken::A", body="b" * 64)}
        manifest = {"classifications": {"a": classification()}}
        with self.assertRaises(MODULE.ClosureError):
            MODULE.validate_classifications(manifest, {"a"}, nodes)

    def test_rejects_source_dependency_digest_drift(self) -> None:
        MODULE.validate_source_dependency_digest(
            {"sourceDependencyDigest": "a" * 64}, "a" * 64
        )
        with self.assertRaises(MODULE.ClosureError):
            MODULE.validate_source_dependency_digest(
                {"sourceDependencyDigest": "a" * 64}, "b" * 64
            )

    def test_rejects_uncontrolled_classification_value(self) -> None:
        nodes = {"a": node("a", "harken::A")}
        entry = classification()
        entry["blocking"] = "CERTIFIED-SAFE"
        manifest = {"classifications": {"a": entry}}
        with self.assertRaises(MODULE.ClosureError):
            MODULE.validate_classifications(manifest, {"a"}, nodes)

    def test_rejects_forbidden_callback_reachable_operation(self) -> None:
        nodes = {"a": node("a", "operator new")}
        for field, value in (
            ("allocation", "forbidden-system-allocation"),
            ("blocking", "forbidden-lock"),
            ("io", "forbidden-file-io"),
        ):
            with self.subTest(field=field):
                entry = classification()
                entry[field] = value
                manifest = {"classifications": {"a": entry}}
                with self.assertRaises(MODULE.ClosureError):
                    MODULE.validate_classifications(
                        manifest, {"a"}, nodes
                    )

    def test_external_mutating_owners_are_not_prefix_whitelisted(self) -> None:
        for name in (
            "std::vector::push_back",
            "std::vector::reserve",
            "std::unique_ptr::reset",
        ):
            with self.subTest(name=name):
                leaf = node("external", name)
                leaf.scope = "external"
                entry = MODULE.external_leaf_classification(leaf)
                self.assertEqual(
                    entry["allocation"],
                    "forbidden-system-allocation",
                )
        leaf = node("external", "std::vector::future_method")
        leaf.scope = "external"
        self.assertEqual(
            MODULE.external_leaf_classification(leaf)["blockers"],
            ["RT-B007"],
        )

    def test_virtual_method_target_is_never_static_direct(self) -> None:
        class Kind:
            name = "CXX_METHOD"

        class Target:
            kind = Kind()

            @staticmethod
            def is_virtual_method() -> bool:
                return True

        self.assertTrue(MODULE.is_virtual_method_target(Target()))

    def test_internal_loop_defaults_to_open_blocker(self) -> None:
        loop = node("loop", "harken::Loop")
        loop.features["hasLoop"] = True
        entry = MODULE.internal_node_classification(loop)
        self.assertEqual(entry["loopBound"], "unknown-blocker")
        self.assertEqual(entry["cost"], "unknown-blocker")
        self.assertEqual(entry["blockers"], ["RT-B008"])

    def test_external_wait_yield_and_transcendentals_are_explicit(self) -> None:
        cases = (
            (
                "std::_Atomic_storage::wait",
                "blocking",
                "scheduler-wait",
                "RT-B002",
            ),
            (
                "std::this_thread::yield",
                "blocking",
                "scheduler-yield",
                "RT-B002",
            ),
            (
                "sin",
                "cost",
                "crt-transcendental-open",
                "RT-B006",
            ),
            (
                "pow",
                "cost",
                "crt-transcendental-open",
                "RT-B006",
            ),
        )
        for name, field, expected, blocker in cases:
            with self.subTest(name=name):
                leaf = node("external", name)
                leaf.scope = "external"
                entry = MODULE.external_leaf_classification(leaf)
                self.assertEqual(entry[field], expected)
                self.assertIn(blocker, entry["blockers"])

    def test_unknown_external_leaf_is_open_blocker(self) -> None:
        leaf = node("external", "vendor::OpaqueCall")
        leaf.scope = "external"
        entry = MODULE.external_leaf_classification(leaf)
        self.assertEqual(entry["cost"], "unknown-blocker")
        self.assertEqual(entry["blockers"], ["RT-B007"])

    def test_rejects_stale_overlay_site(self) -> None:
        nodes = {
            "caller": node("caller", "harken::Caller"),
            "target": node("target", "harken::VoicePool::CascadeThunk"),
        }
        site = MODULE.DynamicSite(
            caller="caller",
            kind="CALL_EXPR",
            expression="fn(user, index)",
            member="fn",
            source="HarkenEngine/Source/Runtime/render/src/Test.cpp",
            line=10,
            column=5,
        )
        manifest = {
            "overlays": [{
                "name": "stale",
                "targetDerivation": "voice-cascade-thunk",
                "sites": [{
                    "source": site.source,
                    "line": 11,
                    "column": site.column,
                    "kind": site.kind,
                    "member": site.member,
                }],
            }]
        }
        with self.assertRaises(MODULE.ClosureError):
            MODULE.apply_overlays(
                nodes,
                [site],
                manifest,
                {},
            )

    def test_compiler_identity_overlay_binds_one_exact_target_and_fails_closed(
        self,
    ) -> None:
        caller = node("caller", "harken::Caller")
        target = node(
            "target",
            "std::_Atomic_storage::load",
            "C:/Program Files/Microsoft Visual Studio/2022/Community/"
            "VC/Tools/MSVC/14.42.34433/include/atomic",
        )
        target.usr = "usr:atomic-uint64-load"
        target.mangled = "?load@atomic-uint64"
        target.line = 1051
        target.column = 22
        site = MODULE.DynamicSite(
            caller=caller.id,
            kind="CALL_EXPR",
            expression="source [ i ] . load ( std :: memory_order_seq_cst )",
            member="memory_order_seq_cst",
            source=(
                "HarkenEngine/Source/Runtime/core/src/CompletionProgress.cpp"
            ),
            line=53,
            column=20,
        )
        target_identity = {
            "id": target.id,
            "usr": target.usr,
            "mangled": target.mangled,
            "qualifiedName": target.qualified_name,
            "source": target.source,
            "line": target.line,
            "column": target.column,
        }
        overlay = {
            "name": "completion-progress-atomic-load",
            "targetDerivation": "compiler-identity",
            "targetIdentity": target_identity,
            "sites": [{
                "source": site.source,
                "line": site.line,
                "column": site.column,
                "kind": site.kind,
                "member": site.member,
                "expression": site.expression,
            }],
        }
        edges, unknown, reports = MODULE.apply_overlays(
            {caller.id: caller, target.id: target},
            [site],
            {"overlays": [overlay]},
            {},
        )
        self.assertEqual(len(edges), 1)
        self.assertEqual(edges[0].callee, target.id)
        self.assertEqual(unknown, [])
        self.assertEqual(reports[0]["targetCount"], 1)
        with self.assertRaises(MODULE.ClosureError):
            MODULE.apply_overlays(
                {caller.id: caller},
                [site],
                {"overlays": [overlay]},
                {},
            )

    def test_implicit_raii_requires_ast_constructor_evidence(self) -> None:
        source = "HarkenEngine/Source/Runtime/render/src/Test.cpp"
        caller = node("caller", "harken::MixerLoop::RenderBlock", source)
        constructor = node(
            "constructor",
            "harken::memguard::AudioThreadScope::AudioThreadScope",
            "HarkenEngine/Source/Runtime/core/include/AllocGuard.h",
        )
        destructor = node(
            "destructor",
            "harken::memguard::AudioThreadScope::~AudioThreadScope",
            "HarkenEngine/Source/Runtime/core/include/AllocGuard.h",
        )
        nodes = {
            item.id: item for item in (caller, constructor, destructor)
        }
        declaration = {
            "name": "audioScope",
            "type": "harken::memguard::AudioThreadScope",
            "source": source,
            "line": 10,
            "column": 5,
            "constructor": (
                "harken::memguard::AudioThreadScope::AudioThreadScope"
            ),
            "constructorSite": {"line": 10, "column": 5},
        }
        caller.features["raiiDeclarations"] = [declaration]
        overlay = {
            "name": "audio-thread-scope-mix-destroy",
            "targetDerivation": (
                "symbol:"
                "harken::memguard::AudioThreadScope::~AudioThreadScope"
            ),
            "sites": [],
            "implicitCaller": {
                "id": caller.id,
                "qualifiedName": caller.qualified_name,
                "source": caller.source,
                "bodySha256": caller.body_sha256,
            },
            "implicitDeclaration": declaration,
        }
        with self.assertRaises(MODULE.ClosureError):
            MODULE.materialize_implicit_raii_sites(
                [overlay], nodes, []
            )
        direct = MODULE.Edge(
            caller=caller.id,
            callee=constructor.id,
            kind="ast-direct",
            source=source,
            line=10,
            column=5,
        )
        _, edges, reports = MODULE.materialize_implicit_raii_sites(
            [overlay], nodes, [direct]
        )
        self.assertEqual(len(edges), 1)
        self.assertEqual(
            edges[0].kind, "overlay-implicit-raii-destructor"
        )
        self.assertEqual(
            reports[0]["evidence"],
            "cxx-raii-destructor-from-ast-constructor",
        )
        with self.assertRaises(MODULE.ClosureError):
            MODULE.reject_unmodeled_implicit_destructors(
                [], nodes, {caller.id}
            )
        MODULE.reject_unmodeled_implicit_destructors(
            [overlay], nodes, {caller.id}
        )

    def test_timeline_sink_authority_derivation_is_exact_and_fail_closed(
        self,
    ) -> None:
        target = node(
            "target",
            "harken::VoiceTimelineSink::BeginQuantum",
            "HarkenEngine/Source/Runtime/render/include/harken/render/"
            "VoiceTimelineSink.h",
        )
        method = {
            "name": "BeginQuantum",
            "target": {
                "id": target.id,
                "usr": target.usr,
                "mangled": target.mangled,
                "qualifiedName": target.qualified_name,
                "source": target.source,
                "line": target.line,
                "column": target.column,
            },
        }
        authority = {"methods": [method]}
        with self.assertRaises(MODULE.ClosureError):
            MODULE.derive_timeline_executor_sink_targets(
                {}, {}, authority, "BeginQuantum"
            )
        with self.assertRaises(MODULE.ClosureError):
            MODULE.derive_timeline_executor_sink_targets(
                {"a": target, "b": target},
                {},
                authority,
                "BeginQuantum",
            )

    def test_realtime_worker_domain_authority_is_exact_and_fail_closed(
        self,
    ) -> None:
        source = "HarkenEngine/Source/Runtime/api/src/DeviceBackend.cpp"
        factory = node("factory", "harken::DeviceBackend::WorkerDomain", source)
        factory.line = 914
        factory.column = 37
        join = node("join", "harken::DeviceBackend::Impl::JoinWorkerDomain", source)
        join.line = 157
        join.column = 47
        leave = node("leave", "harken::DeviceBackend::Impl::LeaveWorkerDomain", source)
        leave.line = 225
        leave.column = 27
        authority = {
            "factory": MODULE.node_full_identity(factory),
            "assignments": [
                {
                    "field": "join",
                    "site": {
                        "source": source,
                        "line": 920,
                        "column": 5,
                        "tokens": "domain . ops . join = & Impl :: JoinWorkerDomain",
                    },
                    "target": MODULE.node_full_identity(join),
                },
                {
                    "field": "leave",
                    "site": {
                        "source": source,
                        "line": 921,
                        "column": 5,
                        "tokens": "domain . ops . leave = & Impl :: LeaveWorkerDomain",
                    },
                    "target": MODULE.node_full_identity(leave),
                },
            ],
            "bindings": [
                {
                    "field": "context",
                    "site": {
                        "source": source,
                        "line": 922,
                        "column": 5,
                        "tokens": "domain . context = m_impl",
                    },
                },
                {
                    "field": "generation",
                    "site": {
                        "source": source,
                        "line": 923,
                        "column": 5,
                        "tokens": "domain . generation = m_impl -> domainGeneration",
                    },
                },
            ],
        }
        nodes = {item.id: item for item in (factory, join, leave)}
        self.assertEqual(
            MODULE.derive_realtime_worker_domain_target(nodes, authority, "join"),
            [join.id],
        )
        with self.assertRaises(MODULE.ClosureError):
            MODULE.derive_realtime_worker_domain_target(
                {key: value for key, value in nodes.items() if key != join.id},
                authority,
                "join",
            )
        with self.assertRaises(MODULE.ClosureError):
            MODULE.derive_realtime_worker_domain_target(
                nodes,
                {
                    **authority,
                    "assignments": [
                        authority["assignments"][0],
                        authority["assignments"][0],
                    ],
                },
                "join",
            )

    def test_avrt_pointer_field_authority_binds_exact_system_target_and_fails_closed(
        self,
    ) -> None:
        source = "HarkenEngine/Source/Runtime/api/src/DeviceBackend.cpp"
        target = node(
            "avrt-set",
            "AvSetMmThreadCharacteristicsA",
            "<compiler-external-avrt>",
        )
        target.scope = "external"
        target.features["externalAuthority"] = "avrt-system-field"
        field = {
            "usr": "usr:ma-context-avrt-set",
            "qualifiedName": "ma_context::AvSetMmThreadCharacteristicsA",
            "source": "HarkenEngine/ThirdParty/miniaudio/miniaudio.h",
            "line": 7372,
            "column": 13,
            "type": "ma_proc",
        }
        authority = {
            "families": [{
                "name": "set",
                "field": field,
                "target": MODULE.node_full_identity(target),
                "bindings": [{
                    "owner": {
                        "qualifiedName": "harken::DeviceBackend::Impl::JoinWorkerDomain",
                    },
                    "variable": {"name": "setCharacteristics"},
                    "site": {
                        "source": source,
                        "line": 187,
                        "column": 5,
                        "tokens": "auto setCharacteristics = reinterpret_cast < AvSetMmThreadCharacteristicsFn > ( impl -> context . wasapi . AvSetMmThreadCharacteristicsA )",
                    },
                }],
                "callSites": [{
                    "source": source,
                    "line": 192,
                    "column": 21,
                    "tokens": "setCharacteristics ( \"Games\" , & taskIndex )",
                }],
            }],
        }
        self.assertEqual(
            MODULE.derive_avrt_system_target(
                {target.id: target}, authority, "set"
            ),
            [target.id],
        )
        with self.assertRaises(MODULE.ClosureError):
            MODULE.derive_avrt_system_target({}, authority, "set")
        with self.assertRaises(MODULE.ClosureError):
            MODULE.derive_avrt_system_target(
                {target.id: target, "duplicate": target}, authority, "set"
            )
        with self.assertRaises(MODULE.ClosureError):
            MODULE.derive_avrt_system_target(
                {target.id: target}, authority, "revert"
            )

        candidate = node("candidate", "harken::ExactCallable")
        nodes: dict[str, MODULE.Node] = {}

        self.assertIs(
            MODULE.register_compiler_node(nodes, candidate, "exact-callable"),
            candidate,
        )
        self.assertEqual(nodes, {candidate.id: candidate})
        self.assertIs(
            MODULE.register_compiler_node(nodes, candidate, "exact-callable"),
            candidate,
        )
        with self.assertRaises(MODULE.ClosureError):
            MODULE.register_compiler_node(
                {candidate.id: candidate, "duplicate": candidate},
                candidate,
                "exact-callable",
            )

    def test_spin_until_jobs_drained_authority_is_exact_and_fail_closed(
        self,
    ) -> None:
        source = "HarkenEngine/Source/Runtime/render/src/RenderWorkerPool.cpp"
        template = node("template", "harken::RenderWorkerPool::SpinUntil", source)
        template.line = 1125
        template.column = 33
        instantiation = node("instantiation", "harken::RenderWorkerPool::SpinUntil", source)
        instantiation.line = 1125
        instantiation.column = 33
        instantiation.mangled = "spin-instantiation"
        caller = node("caller", "harken::RenderWorkerPool::Run", source)
        caller.line = 1194
        caller.column = 47
        target = node(
            "target",
            "harken::RenderWorkerPool::Run::(lambda at F:\\HarkenStudio\\HarkenEngine\\Source\\Runtime\\render\\src\\RenderWorkerPool.cpp:1463:30)::operator()",
            source,
        )
        target.line = 1463
        target.column = 30
        target.mangled = "lambda-operator"
        parameter_usr = "usr:ready"
        variable_usr = "usr:jobsDrained"
        authority = {
            "template": MODULE.node_full_identity(template),
            "instantiation": MODULE.node_full_identity(instantiation),
            "caller": MODULE.node_full_identity(caller),
            "lambdaTarget": MODULE.node_full_identity(target),
            "readyCall": {
                "source": source,
                "line": 1128,
                "column": 13,
                "tokens": "ready ( )",
                "parameter": {
                    "usr": parameter_usr,
                    "qualifiedName": "harken::RenderWorkerPool::SpinUntil::ready",
                    "source": source,
                    "line": 1125,
                    "column": 49,
                },
            },
            "lambdaVariable": {
                "name": "jobsDrained",
                "usr": variable_usr,
                "qualifiedName": "harken::RenderWorkerPool::Run::jobsDrained",
                "source": source,
                "line": 1463,
                "column": 16,
                "type": "const (lambda at F:\\HarkenStudio\\HarkenEngine\\Source\\Runtime\\render\\src\\RenderWorkerPool.cpp:1463:30)",
            },
            "callSites": [
                {
                    "source": source,
                    "line": 1483,
                    "column": 13,
                    "tokens": "SpinUntil ( jobsDrained , deadlineAt , drainYields )",
                    "argument": {
                        "name": "jobsDrained",
                        "usr": variable_usr,
                        "source": source,
                        "line": 1463,
                        "column": 16,
                    },
                },
                {
                    "source": source,
                    "line": 1511,
                    "column": 17,
                    "tokens": "SpinUntil ( jobsDrained , deadlineAt , drainYields )",
                    "argument": {
                        "name": "jobsDrained",
                        "usr": variable_usr,
                        "source": source,
                        "line": 1463,
                        "column": 16,
                    },
                },
            ],
        }
        nodes = {item.id: item for item in (template, instantiation, caller, target)}
        self.assertEqual(MODULE.derive_spin_until_target(nodes, authority), [target.id])
        with self.assertRaises(MODULE.ClosureError):
            MODULE.derive_spin_until_target(
                {key: value for key, value in nodes.items() if key != target.id},
                authority,
            )
        with self.assertRaises(MODULE.ClosureError):
            MODULE.derive_spin_until_target(
                {**nodes, "duplicate": target}, authority
            )
        with self.assertRaises(MODULE.ClosureError):
            MODULE.derive_spin_until_target(
                nodes, {**authority, "callSites": authority["callSites"][:1]}
            )

    def test_digest_sink_authority_is_test_only_and_fail_closed(self) -> None:
        authority = {
            "bodySha256": "a" * 64,
            "method": "Fold",
            "ownerTarget": "harken_runtime_tests",
            "qualifiedName": "(anonymous namespace)::DigestSink",
            "setDigestSinkCallCount": 2,
            "source": "HarkenEngine/Test/UnitTest/TimelineConvergenceTest.cpp",
            "targetKind": "TimelineDigestSink::Fold",
        }
        with self.assertRaises(MODULE.ClosureError):
            MODULE.validate_digest_sink_test_authority(
                REPO_ROOT, authority
            )
        for source in (
            "HarkenEngine/Source/Runtime/DigestSink.cpp",
            "HarkenStudio/Test/UnitTest/DigestSink.cpp",
            "HarkenHub/Test/UnitTest/DigestSink.cpp",
            "HarkenSDK/Test/ContractTest/DigestSink.cpp",
        ):
            with self.subTest(source=source):
                with self.assertRaisesRegex(MODULE.ClosureError, "not test-only"):
                    MODULE.validate_digest_sink_test_authority(
                        REPO_ROOT, {**authority, "source": source}
                    )

    def test_dynamic_blocker_requires_exact_site_and_open_taxonomy(self) -> None:
        site = MODULE.DynamicSite(
            caller="reachable",
            kind="CALL_EXPR",
            expression="hook()",
            member="hook",
            source="HarkenEngine/Source/Runtime/render/src/Test.cpp",
            line=10,
            column=5,
        )
        blocker = {
            "source": site.source,
            "line": site.line,
            "column": site.column,
            "kind": site.kind,
            "member": site.member,
            "expression": site.expression,
            "blocker": "RT-B007",
            "evidence": "No exact target or ownership proof.",
        }
        reports = MODULE.validate_dynamic_blockers([site], [blocker])
        self.assertEqual(reports[0]["status"], "OPEN")
        self.assertEqual(reports[0]["blocker"], "RT-B007")
        with self.assertRaises(MODULE.ClosureError):
            MODULE.validate_dynamic_blockers(
                [site], [{**blocker, "line": 11}]
            )

if __name__ == "__main__":
    unittest.main()
