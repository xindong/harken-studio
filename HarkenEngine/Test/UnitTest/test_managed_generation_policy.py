"""Audience decisions must survive removal from the observed public surface."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "gen_binding", ROOT / "HarkenEngine/Source/Tooling/Binding/gen_binding.py")
binding = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(binding)


class GenerationPolicyTests(unittest.TestCase):
    def setUp(self):
        self.table = json.loads(Path(binding.TABLE_PATH).read_text(encoding="utf-8"))
        self.policy = json.loads(Path(binding.SDK_POLICY_PATH).read_text(encoding="utf-8"))

    def validate(self, policy, table=None):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "policy.json"
            path.write_text(json.dumps(policy), encoding="utf-8")
            with patch.object(binding, "SDK_POLICY_PATH", path):
                return binding.managed_policy(self.table if table is None else table)

    def test_unclassified_new_native_type_fails_generation(self):
        table = copy.deepcopy(self.table)
        table["api"]["enums"]["VnNewToolEnum"] = {"backing": "u32", "values": {}}
        with self.assertRaisesRegex(ValueError, "VnNewToolEnum"):
            self.validate(self.policy, table)

    def test_unclassified_new_constant_fails_generation(self):
        table = copy.deepcopy(self.table)
        table["api"]["defines"]["VN_NEW_LIMIT"] = 1
        with self.assertRaisesRegex(ValueError, "VN_NEW_LIMIT"):
            self.validate(self.policy, table)

    def test_missing_or_stale_decisions_are_rejected(self):
        for category in ("types", "constants"):
            for stale in (False, True):
                with self.subTest(category=category, stale=stale):
                    policy = copy.deepcopy(self.policy)
                    entries = policy["managed_generation"][category]
                    if stale:
                        entries["UnrecognizedSymbol"] = next(iter(entries.values()))
                    else:
                        del entries[next(iter(entries))]
                    with self.assertRaises(ValueError):
                        self.validate(policy)

    def test_invalid_disposition_fails_closed(self):
        group = self.policy["managed_generation"]["types"]["HarkenNative"]
        self.policy["groups"][group]["disposition"] = "tombstone"
        with self.assertRaisesRegex(ValueError, "Invalid managed disposition"):
            self.validate(self.policy)

    def test_visibility_does_not_depend_on_public_inventory(self):
        for group in self.policy["groups"].values():
            group["entries"] = {}
        policy = self.validate(self.policy)
        self.assertEqual("internal", binding.managed_access(policy, "types", "HarkenNative"))
        self.assertEqual("public", binding.managed_access(policy, "types", "VnResult"))

    def test_excluded_native_contracts_do_not_change_sdk_outputs(self):
        baseline_policy = self.validate(self.policy)
        baseline = binding.managed_projection(self.table, baseline_policy)
        table = copy.deepcopy(self.table)
        table["types"]["VnMonitorBoundaryProbe"] = {"fields": [["sequence", "u64"]]}
        table["api"]["enums"]["VnMonitorBoundaryEnum"] = {
            "backing": "u32", "values": {"VN_MONITOR_BOUNDARY_VALUE": 91}}
        table["api"]["callbacks"]["VnMonitorBoundaryCallback"] = {"params": [
            {"name": "context", "kind": "ptr", "ctype": "void*"}]}
        table["api"]["defines"]["VN_MONITOR_BOUNDARY_LIMIT"] = {"type": "u32", "value": 97}
        table["api"]["defines"]["VN_MONITOR_STUDIO_DEFAULT_GAME_OBJECT_ID"]["value"] = 103
        table["api"]["results"]["values"]["VN_MONITOR_BOUNDARY_RESULT"] = -991
        table["functions"].append({"name": "Vn_MonitorBoundaryProbe", "params": [
            {"name": "desc", "wire": "desc", "type": "VnMonitorBoundaryProbe", "ctype": "const VnMonitorBoundaryProbe*"}]})
        table.setdefault("binding_overloads", []).append({"name": "Vn_ToolBoundaryAlias",
            "entryPoint": "Vn_MonitorBoundaryProbe", "params": []})
        policy = self.validate(self.policy, table)
        projected = binding.managed_projection(table, policy)
        for emit in (binding.emit_types, binding.emit_native, binding.emit_abi_cs):
            with self.subTest(output=emit.__name__):
                self.assertEqual(emit(baseline, baseline_policy), emit(projected, policy))
        self.assertIn("VnMonitorBoundaryProbe", binding.emit_cpp_pin(table))

    def test_sdk_dependencies_cannot_cross_excluded_native_boundary(self):
        for dependency in ("field", "function"):
            with self.subTest(dependency=dependency):
                table = copy.deepcopy(self.table)
                if dependency == "field":
                    table["types"]["VnConfig"]["fields"].append(["monitor", {"struct": "VnMonitorRecordingDesc"}])
                else:
                    table["functions"].append({"name": "Vn_SdkBoundaryProbe", "params": [
                        {"name": "desc", "wire": "desc", "type": "VnMonitorRecordingDesc", "ctype": "const VnMonitorRecordingDesc*"}]})
                with self.assertRaisesRegex(ValueError, "SDK projection crosses excluded native type"):
                    self.validate(self.policy, table)


if __name__ == "__main__":
    unittest.main()
