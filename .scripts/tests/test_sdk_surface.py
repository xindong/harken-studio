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


class InventoryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.write("HarkenEngine/Source/Tooling/Remote/wire/harken_wire_table.json",
                   json.dumps({"functions": []}))
        self.write("HarkenIntegration/Unity/Harken/Runtime/Core.asmdef", '{"name":"Core"}')

    def write(self, path, content):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")

    def source(self, name, content):
        self.write("HarkenIntegration/Unity/Harken/Runtime/" + name, content)

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
