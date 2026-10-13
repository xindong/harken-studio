from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "sdk_surface", ROOT / "HarkenEngine/Source/Tooling/Binding/sdk_surface.py")
surface = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(surface)


def cs(source):
    return surface.managed_surface(source.encode(), "fixture.cs")


def cpp(source):
    return surface.unreal_surface(source.encode(), "fixture.h")


def policy(rows, **changes):
    group = {"audience": "gameplay", "disposition": "supported",
             "rationale": "Fixture API", "consumer_contract": "Fixture consumer",
             "entries": list(rows)}
    group.update(changes)
    return {"schema_version": 1, "groups": {"fixture": group}}


class ManagedSurfaceTest(unittest.TestCase):
    def test_define_undef_order_changes_availability(self):
        tail = '\n#if FEATURE\npublic class C {}\n#endif'
        before = cs('#define FEATURE\n#undef FEATURE' + tail)
        after = cs('#undef FEATURE\n#define FEATURE' + tail)
        self.assertTrue(surface.validate(policy(before), after))

    def test_platform_availability_is_part_of_contract(self):
        before = cs('public class C { public void Play() {} }')
        after = cs('public class C {\n#if UNITY_EDITOR\npublic void Play() {}\n#endif\n}')
        self.assertTrue(surface.validate(policy(before), after))
        player = cs('public class C {\n#if UNITY_EDITOR\n#else\npublic void Play() {}\n#endif\n}')
        self.assertTrue(surface.validate(policy(after), player))

    def test_file_local_compilation_symbol_changes_are_recorded(self):
        before = cs('#define FEATURE\npublic class C {\n#if FEATURE\npublic void F() {}\n#endif\n}')
        after = cs('#undef FEATURE\npublic class C {\n#if FEATURE\npublic void F() {}\n#endif\n}')
        self.assertTrue(surface.validate(policy(before), after))

    def test_auto_property_order_and_private_storage_are_frozen(self):
        before = cs('public struct S { public int A {get;set;} public long B {get;set;} }')
        after = cs('public struct S { public long B {get;set;} public int A {get;set;} }')
        extended = cs('public struct S { public int A {get;set;} public long B {get;set;} private int Tail {get;set;} }')
        self.assertTrue(surface.validate(policy(before), after))
        self.assertTrue(surface.validate(policy(before), extended))

    def test_enum_reordering_changes_implicit_values(self):
        before = cs('public enum E { Real, Null }')
        after = cs('public enum E { Null, Real }')
        self.assertTrue(surface.validate(policy(before), after))

    def test_struct_field_order_and_private_layout_fields_are_frozen(self):
        before = cs('public struct S { public int First; public long Second; }')
        after = cs('public struct S { public long Second; public int First; }')
        extended = cs('public struct S { public int First; public long Second; private int Tail; }')
        self.assertTrue(surface.validate(policy(before), after))
        self.assertTrue(surface.validate(policy(before), extended))

    def test_partial_implicit_access_uses_the_merged_type(self):
        before = cs("public partial class C {} partial class C {}")
        after = cs("public partial class C {} partial class C { public void CreateBus() {} }")
        self.assertTrue(any("CreateBus" in error for error in surface.validate(policy(before), after)))

    def test_new_public_member_is_not_approved_by_its_public_type(self):
        old = cs("namespace N { public class C { public void Play() {} } }")
        new = cs("namespace N { public class C { public void Play() {} public void CreateBus() {} } }")
        errors = surface.validate(policy(old), new)
        self.assertEqual(1, len(errors))
        self.assertIn("Unclassified SDK declaration", errors[0])
        self.assertIn("CreateBus", errors[0])

    def test_private_container_and_private_protected_do_not_leak(self):
        rows = cs("public class C { private class P { public int Hidden; } "
                  "private protected int Hidden2; protected int Visible; }")
        self.assertFalse(any("Hidden" in key or "::private" in key for key in rows))
        self.assertTrue(any("Visible" in key for key in rows))

    def test_interface_implicit_public_and_explicit_private(self):
        rows = cs("public interface I { void Play(); private void Hidden() {} }")
        self.assertTrue(any("Play" in key for key in rows))
        self.assertFalse(any("Hidden" in key for key in rows))

    def test_nested_type_delegate_property_and_overloads_are_individual(self):
        rows = cs("namespace N { public class C { public struct S { public int X; } "
                  "public delegate void D(S v); public int P { get; private set; } "
                  "public void F(int a) {} public void F(string a) {} } }")
        self.assertEqual(2, sum(" void F " in key for key in rows))
        self.assertTrue(any("N.C.D" in key for key in rows))
        self.assertTrue(any("N.C.S::public int X" in key for key in rows))
        self.assertTrue(any("private set" in key for key in rows))

    def test_expression_and_method_body_changes_are_not_api_changes(self):
        a = cs("public class C { public int F() { return 1; } public int P => 1; }")
        b = cs("public class C { public int F() { return 2; } public int P => 2; }")
        self.assertEqual(set(a), set(b))

    def test_method_local_condition_changes_are_not_api_changes(self):
        source = ('public class C { public int F() {\n#if UNITY_EDITOR\n'
                  'return 1;\n#elif FEATURE\nreturn 2;\n#else\nreturn 3;\n#endif\n} }')
        self.assertEqual(set(cs(source)), set(cs(source.replace('UNITY_EDITOR', '!UNITY_EDITOR'))))
        self.assertEqual(set(cs(source)), set(cs('public class C { public int F() { return 3; } }')))

    def test_method_local_nullable_state_remains_part_of_context(self):
        source = ('public class C { public void F() {\n#if FEATURE\n'
                  '#nullable enable\n#endif\n} public string Value; }')
        self.assertTrue(surface.validate(policy(cs(source)), cs(source.replace('FEATURE', '!FEATURE'))))

    def test_file_scoped_namespace_and_comments(self):
        rows = cs("namespace N; /* public class Fake {} */ public class C {}")
        self.assertTrue(any("/N.C::" in key for key in rows))
        self.assertFalse(any("Fake" in key for key in rows))

    def test_generic_arity_keeps_distinct_type_members_separate(self):
        old = cs("public class C<T> { public void F() {} } public class C<T,U> {}")
        new = cs("public class C<T> { public void F() {} } public class C<T,U> { public void F() {} }")
        errors = surface.validate(policy(old), new)
        self.assertEqual(1, len(errors))
        self.assertIn("C`2", errors[0])

    def test_both_platform_branches_are_classified(self):
        rows = cs("public class C {\n#if UNITY_EDITOR\n public void Editor() {}\n"
                  "#else\n public void Player() {}\n#endif\n}")
        self.assertTrue(any("Editor" in key for key in rows))
        self.assertTrue(any("Player" in key for key in rows))

    def test_friend_assembly_and_alias_changes_are_detected(self):
        a = cs('using X = A; [assembly: InternalsVisibleTo("Tools")] public class C {}')
        b = cs('using X = B; [assembly: InternalsVisibleTo("Game")] public class C {}')
        errors = surface.validate(policy(a), b)
        self.assertEqual(4, len(errors))

    def test_moving_type_between_existing_assemblies_is_detected(self):
        source = b"public class C { public void F() {} }"
        a = surface.managed_surface(source, "fixture.cs", "Harken.Core")
        b = surface.managed_surface(source, "fixture.cs", "Harken.Unity.Editor")
        self.assertEqual(4, len(surface.validate(policy(a), b)))

    def test_invalid_public_declaration_fails_closed(self):
        with self.assertRaises(surface.SurfaceError):
            cs("public class C { public void Missing( } ")

    def test_unsafe_body_grammar_does_not_hide_following_public_method(self):
        rows = cs("public class C { private unsafe void T() { ref VnNotification n = "
                  "ref *(VnNotification*)notification.ToPointer(); sink.Handler(in n, sink.State); } "
                  "public void Visible() {} }")
        self.assertTrue(any("Visible" in key for key in rows))


class UnrealSurfaceTest(unittest.TestCase):
    def test_moving_type_across_existing_pack_boundary_is_detected(self):
        declaration = 'struct S { char A; int B; };'
        before = cpp('#pragma pack(push, 1)\n' + declaration + '\n#pragma pack(pop)')
        after = cpp(declaration + '\n#pragma pack(push, 1)\n#pragma pack(pop)')
        self.assertTrue(surface.validate(policy(before), after))

    def test_preprocessor_context_does_not_freeze_method_body_or_offsets(self):
        before = cpp('#pragma once\nstruct S { int F() { return 1; } };')
        after = cpp('// Comment shifts offsets\n#pragma once\nstruct S { int F() { return 123; } };')
        self.assertEqual(set(before), set(after))

    def test_method_local_condition_changes_are_not_api_changes(self):
        source = ('struct S { int F() {\n#if WITH_EDITOR\nreturn 1;\n'
                  '#elif FEATURE\nreturn 2;\n#else\nreturn 3;\n#endif\n} };')
        self.assertEqual(set(cpp(source)), set(cpp(source.replace('WITH_EDITOR', '!WITH_EDITOR'))))
        self.assertEqual(set(cpp(source)), set(cpp('struct S { int F() { return 3; } };')))

    def test_method_local_macro_state_preserves_its_conditional_branches(self):
        source = ('struct S { void F() {\n#if WITH_EDITOR\nint x = 1;\n'
                  '#elif FEATURE\nint x = 2;\n#else\n#define PUBLIC_TYPE long\n#endif\n}\n'
                  'PUBLIC_TYPE Value; };')
        for before, after in [('WITH_EDITOR', '!WITH_EDITOR'), ('FEATURE', '!FEATURE'),
                              ('long', 'int')]:
            with self.subTest(before=before):
                self.assertTrue(surface.validate(policy(cpp(source)), cpp(source.replace(before, after))))

    def test_define_undef_order_changes_availability(self):
        tail = '\n#if FEATURE\nstruct C {};\n#endif'
        before = cpp('#define FEATURE 1\n#undef FEATURE' + tail)
        after = cpp('#undef FEATURE\n#define FEATURE 1' + tail)
        self.assertTrue(surface.validate(policy(before), after))

    def test_include_order_and_pack_directives_are_recorded(self):
        before = cpp('#include "A.h"\n#include "B.h"\nstruct C { int X; };')
        after = cpp('#include "B.h"\n#include "A.h"\nstruct C { int X; };')
        packed = cpp('#include "A.h"\n#include "B.h"\n#pragma pack(push, 1)\nstruct C { int X; };\n#pragma pack(pop)')
        self.assertTrue(surface.validate(policy(before), after))
        self.assertTrue(surface.validate(policy(before), packed))

    def test_platform_availability_is_part_of_contract(self):
        before = cpp('struct C { void Play(); };')
        after = cpp('struct C {\n#if WITH_EDITOR\nvoid Play();\n#endif\n};')
        self.assertTrue(surface.validate(policy(before), after))
        player = cpp('struct C {\n#if WITH_EDITOR\n#else\nvoid Play();\n#endif\n};')
        self.assertTrue(surface.validate(policy(after), player))

    def test_conditionally_inherited_access_preserves_platform_identity(self):
        before = cpp('class C {\n#if WITH_EDITOR\npublic:\n#else\nprivate:\n#endif\nvoid F(); };')
        after = cpp('class C {\n#if WITH_EDITOR\nprivate:\n#else\npublic:\n#endif\nvoid F(); };')
        self.assertTrue(surface.validate(policy(before), after))

    def test_ifdef_and_ifndef_are_distinct(self):
        before = cpp('#ifdef FEATURE\nstruct C {};\n#endif')
        after = cpp('#ifndef FEATURE\nstruct C {};\n#endif')
        self.assertTrue(surface.validate(policy(before), after))

    def test_three_way_conditional_access_observes_every_branch(self):
        rows = cpp('class C {\n#if A\nprivate:\n#elif B\nprivate:\n#else\n'
                   'public: void Play();\n#endif\nvoid Following(); };')
        self.assertTrue(any('Play' in key for key in rows))
        self.assertTrue(any('Following' in key for key in rows))

    def test_elif_access_does_not_hide_alternative_public_branch(self):
        rows = cpp('class C { public:\n#if A\nprivate:\n#elif B\nprivate:\n#else\n'
                   'void Player();\n#endif\n};')
        self.assertTrue(any('Player' in key for key in rows))

    def test_enum_reordering_changes_implicit_values(self):
        before = cpp('UENUM() enum class E { Real, Null };')
        after = cpp('UENUM() enum class E { Null, Real };')
        self.assertTrue(surface.validate(policy(before), after))

    def test_struct_field_order_and_private_layout_fields_are_frozen(self):
        before = cpp('struct S { int First; long Second; };')
        after = cpp('struct S { long Second; int First; };')
        extended = cpp('struct S { int First; long Second; private: int Tail; };')
        self.assertTrue(surface.validate(policy(before), after))
        self.assertTrue(surface.validate(policy(before), extended))

    def test_private_reflected_property_is_part_of_blueprint_surface(self):
        before = cpp('UCLASS() class C { GENERATED_BODY() private: };')
        after = cpp('UCLASS() class C { GENERATED_BODY() private: '
                    'UPROPERTY(BlueprintReadWrite, meta=(AllowPrivateAccess="true")) int32 NewValue; };')
        self.assertTrue(any("NewValue" in error for error in surface.validate(policy(before), after)))

    def test_enum_trailing_metadata_is_observed(self):
        before = cpp('UENUM(BlueprintType) enum class E { Value UMETA(Hidden), Other };')
        after = cpp('UENUM(BlueprintType) enum class E { Value UMETA(DisplayName="Value"), Other };')
        self.assertTrue(surface.validate(policy(before), after))

    def test_comment_does_not_detach_function_metadata(self):
        before = cpp('class C { public: UFUNCTION(BlueprintPure) /* explanation */ void Play(); };')
        after = cpp('class C { public: UFUNCTION(BlueprintCallable) /* explanation */ void Play(); };')
        self.assertTrue(surface.validate(policy(before), after))

    def test_using_base_member_expands_surface(self):
        before = cpp('class C : public Base { public: };')
        after = cpp('class C : public Base { public: using Base::CreateBus; };')
        self.assertTrue(any("CreateBus" in error for error in surface.validate(policy(before), after)))

    def test_unattached_reflection_macro_fails_closed(self):
        with self.assertRaises(surface.SurfaceError):
            cpp('struct C {}; UFUNCTION(BlueprintCallable)')

    def test_class_access_and_struct_default(self):
        rows = cpp("class C { int Hidden; public: int F(int x) const; protected: int P; "
                   "private: int Hidden2; }; struct S { int X; };")
        self.assertFalse(any("Hidden" in key for key in rows))
        self.assertTrue(any("int F" in key for key in rows))
        self.assertTrue(any("int X" in key for key in rows))

    def test_new_blueprint_callable_and_metadata_change_are_detected(self):
        old = cpp('UCLASS() class HARKENRUNTIME_API C { GENERATED_BODY() public: '
                  'UFUNCTION(BlueprintPure) void Play(); };')
        new = cpp('UCLASS() class HARKENRUNTIME_API C { GENERATED_BODY() public: '
                  'UFUNCTION(BlueprintCallable) void Play(); };')
        errors = surface.validate(policy(old), new)
        self.assertEqual(2, len(errors))
        self.assertTrue(any("BlueprintCallable" in error for error in errors))

    def test_uparam_and_deprecation_are_part_of_signature(self):
        rows = cpp('class C { public: UE_DEPRECATED(5.0, "Use F") '
                   'void Old(UPARAM(ref) Handle& Value); };')
        self.assertTrue(any("UPARAM(ref)" in key and "UE_DEPRECATED" in key for key in rows))

    def test_inline_body_is_not_part_of_contract(self):
        a = cpp("namespace N { struct C { int F() { return 1; } }; }")
        b = cpp("namespace N { struct C { int F() { return 2; } }; }")
        self.assertEqual(set(a), set(b))

    def test_conditional_members_are_both_observed(self):
        rows = cpp("struct S {\n#if WITH_EDITOR\nint Editor;\n#else\nint Player;\n#endif\n};")
        self.assertTrue(any("Editor" in key for key in rows))
        self.assertTrue(any("Player" in key for key in rows))

    def test_conditional_access_cannot_hide_player_only_public_api(self):
        rows = cpp("class C {\n#if WITH_EDITOR\nprivate:\n#else\npublic:\n#endif\nvoid Play(); };")
        self.assertTrue(any("Play" in key for key in rows))

    def test_public_member_inside_access_branch_is_observed(self):
        rows = cpp("class C {\n#if WITH_EDITOR\npublic: void Editor();\n#endif\n};")
        self.assertTrue(any("Editor" in key for key in rows))

    def test_missing_macro_end_fails_closed(self):
        with self.assertRaises(surface.SurfaceError):
            cpp("UCLASS(unclosed class C {};")

    def test_comments_and_strings_do_not_become_unreal_macros(self):
        rows = cpp('/* UCLASS(unclosed */ struct S { const char* Value = "UFUNCTION("; };')
        self.assertTrue(any("Value" in key for key in rows))

    def test_export_marker_is_not_silently_erased(self):
        a = cpp("class HARKENRUNTIME_API C {};")
        b = cpp("class C {};")
        self.assertEqual(2, len(surface.validate(policy(a), b)))

    def test_unmodelled_template_fails_closed(self):
        with self.assertRaises(surface.SurfaceError):
            cpp("template<typename T> struct S { T Value; };")


class InventoryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.write("HarkenEngine/Source/Tooling/Remote/wire/harken_wire_table.json",
                   json.dumps({"functions": []}))
        self.write("HarkenSDK/Unity/Harken/Runtime/Core.asmdef", '{"name":"Core"}')

    def write(self, path, content):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")

    def source(self, name, content):
        self.write("HarkenSDK/Unity/Harken/Runtime/" + name, content)

    def test_unreal_alternative_header_extensions_are_inventoried(self):
        before = surface.inventory(self.root)
        for index, extension in enumerate(('.hpp', '.hh', '.hxx', '.inl', '.ipp', '.inc', '.HPP')):
            with self.subTest(extension=extension):
                filename = f'New{index}{extension}'
                self.write('HarkenSDK/Unreal/Harken/Source/HarkenRuntime/Public/' + filename,
                           'struct Extra { void CreateBus(); };')
                errors = surface.validate(policy(before), surface.inventory(self.root))
                self.assertTrue(any('CreateBus' in error and filename in error for error in errors))

    def test_unmodelled_unreal_public_file_fails_closed(self):
        self.write('HarkenSDK/Unreal/Harken/Source/HarkenRuntime/Public/New.custom',
                   'void CreateBus();')
        with self.assertRaises(surface.SurfaceError):
            surface.inventory(self.root)

    def test_partial_access_across_files_is_observed(self):
        self.source("A.cs", "namespace N { public partial class C {} }")
        self.source("B.cs", "namespace N { partial class C {} }")
        before = surface.inventory(self.root)
        self.source("B.cs", "namespace N { partial class C { public void CreateBus() {} } }")
        after = surface.inventory(self.root)
        self.assertTrue(any("CreateBus" in error for error in surface.validate(policy(before), after)))

    def test_partial_access_does_not_cross_assemblies_or_private_containers(self):
        self.source("A.cs", "namespace N { public partial class C {} }")
        self.source("Other/Other.asmdef", '{"name":"Other"}')
        self.source("Other/B.cs", "namespace N { partial class C { public int Hidden; } }")
        self.source("C.cs", "class Outer { public partial class C { public int Hidden2; } }")
        self.assertFalse(any("Hidden" in key for key in surface.inventory(self.root)))

    def test_nested_partial_access_reaches_fixed_point(self):
        self.source("A.cs", "partial class C { partial class D { public int Value; } }")
        self.source("B.cs", "public partial class C { public partial class D {} }")
        self.assertTrue(any("Value" in key for key in surface.inventory(self.root)))

    def test_moving_declaration_between_alias_contexts_is_detected(self):
        declaration = "public class C { public Id Value; }"
        self.source("A.cs", "using Id = System.Int32; " + declaration)
        self.source("B.cs", "using Id = System.String;")
        before = surface.inventory(self.root)
        self.source("A.cs", "using Id = System.Int32;")
        self.source("B.cs", "using Id = System.String; " + declaration)
        self.assertTrue(surface.validate(policy(before), surface.inventory(self.root)))

    def test_moving_alias_between_namespaces_is_detected(self):
        self.source("A.cs", "namespace A { using Id = System.Int32; } namespace B {}")
        before = surface.inventory(self.root)
        self.source("A.cs", "namespace A {} namespace B { using Id = System.Int32; }")
        self.assertTrue(surface.validate(policy(before), surface.inventory(self.root)))


class PolicyTest(unittest.TestCase):
    def test_native_addition_and_removal_are_detected(self):
        old = {"native:Vn_Old": {"native_public": True}}
        new = {"native:Vn_New": {"native_public": True}}
        errors = surface.validate(policy(old), new)
        self.assertEqual(2, len(errors))

    def test_resurrected_native_tombstone_is_rejected(self):
        retired = {"native:Vn_Retired": {"native_public": False}}
        p = policy(retired, audience="internal", disposition="tombstone")
        self.assertEqual([], surface.validate(p, retired))
        self.assertTrue(surface.validate(p, {"native:Vn_Retired": {"native_public": True}}))

    def test_unexplained_compatibility_is_rejected(self):
        p = policy({}, disposition="compatibility")
        self.assertIn("Missing retirement condition", surface.validate(p, {})[0])

    def test_duplicate_classification_is_rejected(self):
        rows = cs("public class C {}")
        p = policy(rows)
        p["groups"]["second"] = dict(p["groups"]["fixture"])
        self.assertTrue(any("Multiply classified" in e for e in surface.validate(p, rows)))


if __name__ == "__main__":
    unittest.main()
