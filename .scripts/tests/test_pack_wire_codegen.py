from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("pack_wire_codegen", ROOT / ".scripts/pack_wire_codegen.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def sample_layout() -> dict:
    return {
        "byteOrder": "little",
        "constants": {"capacity": 3},
        "kinds": [{"name": "row", "id": 7, "vintage": 2, "record": "row"}],
        "retiredKinds": [{"name": "retired", "id": 6}],
        "records": {
            "row": {
                "mode": "fixed",
                "fields": [
                    {"name": "tag", "type": "u8"},
                    {"name": "items", "type": "array", "of": {"type": "record", "record": "item"}, "length": "capacity"},
                    {"name": "identity", "type": "id"},
                    {"name": "reserved", "type": "u16", "reserved": True},
                ],
            },
            "item": {
                "mode": "fixed",
                "fields": [{"name": "value", "type": "u32"}, {"name": "weight", "type": "f32"}],
            },
        },
    }


def sample_bindings(layout: dict) -> dict:
    return {"records": {name: {"engine": name.capitalize(), "studio": name.capitalize()} for name in layout["records"]}}


class PackWireCodegenTest(unittest.TestCase):
    def test_setters_only_select_writable_scalar_fields(self) -> None:
        layout = sample_layout()
        bindings = sample_bindings(layout)
        bindings["setters"] = ["item.weight"]
        header = MODULE.render_header(layout, bindings, "engine")
        self.assertIn("StorePackItemWeight(std::uint8_t* out, float value)", header)
        self.assertIn("StoreLEF32(out + 4, static_cast<float>(value));", header)
        for fields in (["row.reserved"], ["row.items"], ["missing.field"], ["item.weight"] * 2):
            with self.subTest(fields=fields):
                bindings["setters"] = fields
                with self.assertRaises(ValueError):
                    MODULE.render_header(layout, bindings, "engine")

    def test_nested_records_and_arrays_have_packed_offsets(self) -> None:
        records = MODULE.record_layouts(sample_layout())
        self.assertEqual(records["item"]["bytes"], 8)
        self.assertEqual(records["row"]["bytes"], 43)
        self.assertEqual([offset for _, offset, _ in records["row"]["fields"]], [0, 1, 25, 41])

    def test_record_mapping_order_does_not_change_generated_bytes(self) -> None:
        layout = sample_layout()
        bindings = sample_bindings(layout)
        expected = MODULE.render_header(layout, bindings, "engine")
        layout["records"] = dict(reversed(list(layout["records"].items())))
        bindings["records"] = dict(reversed(list(bindings["records"].items())))
        self.assertEqual(MODULE.render_header(layout, bindings, "engine"), expected)
        self.assertNotIn("\r", expected)
        self.assertTrue(expected.endswith("\n"))

    def test_unsupported_layouts_are_rejected(self) -> None:
        cases = []
        def damaged(change):
            value = sample_layout()
            change(value)
            cases.append(value)
        damaged(lambda value: value.update(byteOrder="big"))
        damaged(lambda value: value.update(unrecognized=True))
        damaged(lambda value: value["constants"].update(capacity=True))
        damaged(lambda value: value["records"]["row"].update(mode="stream"))
        damaged(lambda value: value["records"]["row"]["fields"].append({"name": "tag", "type": "u16"}))
        damaged(lambda value: value["records"]["item"]["fields"][0].update(type="u24"))
        damaged(lambda value: value["records"]["item"]["fields"][0].update(type="u8", const=256))
        damaged(lambda value: value["records"]["item"]["fields"][0].update(reserved=True, const=0))
        damaged(lambda value: value["records"]["item"]["fields"][0].update(type="bytes", length=4, const="short"))
        damaged(lambda value: value["records"]["row"]["fields"][1].update(length="missing"))
        damaged(lambda value: value["records"]["row"]["fields"][1].update(length=0))
        damaged(lambda value: value["records"]["row"]["fields"][1].update(of={"type": []}))
        damaged(lambda value: value["records"]["row"]["fields"][1].update(of={"type": "record", "record": "missing"}))
        damaged(lambda value: value["records"]["row"]["fields"][1].update(live="identity"))
        damaged(lambda value: value["records"]["row"]["fields"][1].update(live={"count": "tag", "limit": 4, "subtract": 0}))
        damaged(lambda value: value["records"]["row"]["fields"][1].update(live={"count": "tag", "limit": 3, "subtract": -1}))
        damaged(lambda value: value["records"]["row"]["fields"][1].update(live={"count": "tag", "limit": 3, "subtract": True}))
        damaged(lambda value: value["kinds"].append({"name": "duplicate", "id": 7, "vintage": 3, "record": "item"}))
        damaged(lambda value: value["retiredKinds"].append({"name": "retiredCopy", "id": 7}))
        damaged(lambda value: value["constants"].update(rowBytes=10))
        for value in cases:
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    MODULE.validate_layout(value)

    def test_direct_and_indirect_record_cycles_are_rejected(self) -> None:
        for reference in ("row", "item"):
            layout = sample_layout()
            layout["records"]["item"]["fields"] = [{"name": "child", "type": "record", "record": reference}]
            with self.subTest(reference=reference):
                with self.assertRaisesRegex(ValueError, "recursive record"):
                    MODULE.validate_layout(layout)

    def test_binding_typos_and_non_members_are_rejected(self) -> None:
        layout = sample_layout()
        cases = []
        missing = sample_bindings(layout)
        missing["records"].pop("row")
        cases.append(missing)
        for member, value in (("row.missing", "value"), ("row.reserved", "value"), ("row.tag", "value;return"), ("row.tag", {"engine": "value"})):
            bindings = sample_bindings(layout)
            bindings["members"] = {member: value}
            cases.append(bindings)
        for bindings in cases:
            with self.subTest(bindings=bindings):
                with self.assertRaises(ValueError):
                    MODULE.render_header(layout, bindings, "engine")

    def test_scalar_records_and_target_member_bindings(self) -> None:
        layout = {
            "byteOrder": "little", "records": {
                "scalar": {"mode": "fixed", "fields": [{"name": "value", "type": "f32"}]},
                "mapped": {"mode": "fixed", "fields": [{"name": "value", "type": "u32"}]},
            },
        }
        bindings = {
            "records": {"scalar": {"engine": "float", "studio": "float"}, "mapped": {"engine": "Mapped", "studio": "Mapped"}},
            "members": {"scalar.value": "", "mapped.value": {"engine": "values[2].bits", "studio": "amount"}},
        }
        engine = MODULE.render_header(layout, bindings, "engine")
        studio = MODULE.render_header(layout, bindings, "studio")
        self.assertIn("decltype((*out))", engine)
        self.assertIn("out->values[2].bits", engine)
        self.assertNotIn("out->amount", engine)
        self.assertIn("out->amount", studio)

    def test_bounded_live_count_preserves_the_fixed_record_size(self) -> None:
        layout = sample_layout()
        original = MODULE.record_layouts(layout)["row"]["bytes"]
        layout["records"]["row"]["fields"][1]["live"] = {"count": "tag", "limit": 4, "subtract": 1}
        self.assertEqual(MODULE.record_layouts(layout)["row"]["bytes"], original)

    def test_source_scan_preserves_line_numbers_and_function_scope(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "layout.cpp"
            source.write_text('void Fingerprint() { StoreLE32(bytes, 1); }\n'
                              'void SerializeRow() {\n'
                              '    // StoreLE32(bytes, 2);\n'
                              '    const char* text = "LoadLE32(bytes)";\n'
                              '    StoreObjectId(bytes, id);\n'
                              '}\n', encoding="utf-8")
            errors = MODULE.scan_sources(root, {"layout.cpp": ("SerializeRow",)})
            self.assertEqual(len(errors), 1)
            self.assertIn("layout.cpp:5:", errors[0])
            self.assertEqual(len(MODULE.scan_sources(root, {"layout.cpp": None})), 2)
            self.assertTrue(MODULE.scan_sources(root, {"missing.cpp": None}))

    def test_check_detects_contract_output_and_source_drift(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            layout = sample_layout()
            contract = root / MODULE.CONTRACT
            bindings = root / MODULE.BINDINGS
            contract.parent.mkdir(parents=True)
            bindings.parent.mkdir(parents=True)
            contract.write_text(json.dumps({"x-harken-format": {"binaryLayout": layout}}), encoding="utf-8")
            bindings.write_text(json.dumps(sample_bindings(layout)), encoding="utf-8")
            source = root / "layout.cpp"
            source.write_text("void SerializeRow() {}\n", encoding="utf-8")
            with patch.object(MODULE, "SOURCE_SCANS", {"layout.cpp": None}):
                self.assertEqual(MODULE.generate(root), [])
                expected = {target: (root / path).read_bytes() for target, path in MODULE.OUTPUTS.items()}
                self.assertEqual(MODULE.generate(root, check=True), [])
                self.assertEqual(MODULE.generate(root), [])
                self.assertEqual(expected, {target: (root / path).read_bytes() for target, path in MODULE.OUTPUTS.items()})
                output = root / MODULE.OUTPUTS["engine"]
                output.write_bytes(output.read_bytes().replace(b"\n", b"\r\n"))
                self.assertEqual(len(MODULE.generate(root, check=True)), 1)
                MODULE.generate(root)
                layout["constants"]["capacity"] = 4
                contract.write_text(json.dumps({"x-harken-format": {"binaryLayout": layout}}), encoding="utf-8")
                self.assertEqual(len(MODULE.generate(root, check=True)), 2)
                MODULE.generate(root)
                source.write_text("void SerializeRow() { StoreLE32(bytes, 1); }\n", encoding="utf-8")
                errors = MODULE.generate(root, check=True)
                self.assertEqual(len(errors), 1)
                self.assertIn("handwritten Pack byte access", errors[0])


def stream_layout() -> dict:
    layout = sample_layout()
    layout["records"]["header"] = {"mode": "fixed", "fields": [
        {"name": "count", "type": "u32"}, {"name": "kind", "type": "u8"}]}
    layout["records"]["itemRow"] = {"mode": "stream", "fields": [
        {"name": "value", "type": "record", "record": "item"}]}
    layout["records"]["table"] = {"mode": "stream", "fields": [
        {"name": "header", "type": "record", "record": "header"},
        {"name": "items", "type": "rows", "record": "itemRow", "count": "count"},
        {"name": "optional", "type": "record", "record": "item", "when": {"field": "kind", "equals": 1}}]}
    layout["kinds"][0]["record"] = "table"
    return layout


class PackWireStreamCodegenTest(unittest.TestCase):
    def test_stream_controls_require_an_existing_earlier_unsigned_header_field(self) -> None:
        for control in ("missing", "weight"):
            layout = stream_layout()
            layout["records"]["table"]["fields"][1]["count"] = control
            with self.subTest(control=control):
                with self.assertRaises(ValueError):
                    MODULE.validate_layout(layout)
        layout = stream_layout()
        layout["records"]["table"]["fields"].reverse()
        with self.assertRaises(ValueError):
            MODULE.validate_layout(layout)
        layout = stream_layout()
        layout["records"]["header"]["fields"][0]["type"] = "f32"
        with self.assertRaises(ValueError):
            MODULE.validate_layout(layout)

    def test_stream_cycles_unknown_records_and_invalid_conditions_are_rejected(self) -> None:
        cases = []
        for replacement in ([], "missing", "table"):
            layout = stream_layout()
            layout["records"]["table"]["fields"][0]["record"] = replacement
            cases.append(layout)
        for replacement in ({"field": "kind", "equals": -1}, {"field": "kind", "equals": True}, {"field": "kind", "greater": 0}):
            layout = stream_layout()
            layout["records"]["table"]["fields"][2]["when"] = replacement
            cases.append(layout)
        for layout in cases:
            with self.subTest(layout=layout):
                with self.assertRaises(ValueError):
                    MODULE.validate_layout(layout)

    def test_borrowed_header_requires_an_explicit_fixed_context(self) -> None:
        layout = stream_layout()
        table = layout["records"]["table"]
        table["fields"].pop(0)
        with self.assertRaises(ValueError):
            MODULE.validate_layout(layout)
        table["context"] = "header"
        MODULE.validate_layout(layout)
        for context in ([], "missing", "itemRow"):
            table["context"] = context
            with self.subTest(context=context):
                with self.assertRaises(ValueError):
                    MODULE.validate_layout(layout)

    def test_stream_kind_and_unknown_binding_owner_are_not_lost(self) -> None:
        layout = stream_layout()
        bindings = sample_bindings(layout)
        header = MODULE.render_header(layout, bindings, "engine")
        self.assertIn("kPackEntryKindRow = 7u", header)
        self.assertIn("case kPackEntryKindRow:", header)
        for name, value in (("absent.count", "count"), ("table.count", {"engine": "count"})):
            bindings["members"] = {name: value}
            with self.subTest(name=name, value=value):
                with self.assertRaises(ValueError):
                    MODULE.render_header(layout, bindings, "engine")

    def test_null_or_malformed_layout_always_raises_value_error(self) -> None:
        for layout in (None, {}, {"records": None}, {"records": {"row": []}}):
            with self.subTest(layout=layout):
                with self.assertRaises(ValueError):
                    MODULE.validate_layout(layout)


if __name__ == "__main__":
    unittest.main()
