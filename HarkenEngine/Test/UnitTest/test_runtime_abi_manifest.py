from __future__ import annotations

import copy
import importlib.util
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT_PATH = (
    REPO_ROOT
    / "HarkenEngine"
    / "Source"
    / "Tooling"
    / "Abi"
    / "gen_abi_manifest.py"
)
SPEC = importlib.util.spec_from_file_location(
    "gen_abi_manifest", SCRIPT_PATH
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class RuntimeAbiManifestTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header_path = (
            REPO_ROOT
            / "HarkenEngine"
            / "Source"
            / "Runtime"
            / "api"
            / "include"
            / "harken"
            / "Harken.h"
        )
        cls.table_path = (
            REPO_ROOT
            / "HarkenEngine"
            / "Source"
            / "Tooling"
            / "Remote"
            / "wire"
            / "harken_wire_table.json"
        )
        cls.header_text = (cls.header_path.with_name("ObjectId.h").read_text(encoding="utf-8") +
                           "\n" + cls.header_path.read_text(encoding="utf-8"))
        cls.header_contract = MODULE.parse_header_contract(
            cls.header_text
        )
        cls.table = MODULE.load_json_strict(cls.table_path)
        cls.trace_text = (
            REPO_ROOT
            / "HarkenEngine"
            / "Source"
            / "Runtime"
            / "api"
            / "src"
            / "TraceFormat.h"
        ).read_text(encoding="utf-8")

    def test_public_contract_is_complete(self) -> None:
        issues = MODULE.compare_completeness(
            self.header_contract,
            MODULE.table_contract(self.table),
        )
        self.assertEqual([], issues)

    def test_object_id_cplusplus_constructors_do_not_change_c_fields(self) -> None:
        bodies = MODULE.extract_struct_bodies(self.header_text)
        self.assertEqual(["high", "low"], MODULE.extract_struct_fields(bodies["VnObjectId"], "VnObjectId"))

    def test_variable_catalog_query_pointee_is_pinned(self) -> None:
        changed = MODULE.parse_header_contract(self.header_text.replace(
            "Vn_QueryVariableCatalog(VnVariableCatalogInfoV1* out)",
            "Vn_QueryVariableCatalog(uint64_t* out)",
        ))
        issues = MODULE.compare_completeness(changed, MODULE.table_contract(self.table))
        self.assertTrue(any("Vn_QueryVariableCatalog" in issue for issue in issues), issues)

    def test_missing_public_define_is_detected(self) -> None:
        incomplete = copy.deepcopy(self.table)
        del incomplete["api"]["defines"]["VN_MAX_EFFECT_TYPE_STATS"]
        issues = MODULE.compare_completeness(
            self.header_contract,
            MODULE.table_contract(incomplete),
        )
        self.assertTrue(
            any("VN_MAX_EFFECT_TYPE_STATS" in issue for issue in issues),
            issues,
        )

    def test_non_vnresult_public_function_is_detected(self) -> None:
        changed = MODULE.parse_header_contract(
            self.header_text + "\nuint32_t Vn_GetAbi(void);\n"
        )
        issues = MODULE.compare_completeness(
            changed,
            MODULE.table_contract(self.table),
        )
        self.assertTrue(any("Vn_GetAbi" in issue for issue in issues))

    def test_same_width_field_type_change_is_detected(self) -> None:
        changed_text = self.header_text.replace(
            "uint32_t sampleRate;", "float sampleRate;", 1
        )
        self.assertNotEqual(self.header_text, changed_text)
        changed = MODULE.parse_header_contract(changed_text)
        issues = MODULE.compare_completeness(
            changed,
            MODULE.table_contract(self.table),
        )
        self.assertTrue(
            any("VnConfig" in issue for issue in issues), issues
        )

    def test_pointer_pointee_change_is_detected(self) -> None:
        changed_text = self.header_text.replace(
            "const float* samplesInterleaved;",
            "const uint32_t* samplesInterleaved;",
            1,
        )
        changed = MODULE.parse_header_contract(changed_text)
        issues = MODULE.compare_completeness(
            changed,
            MODULE.table_contract(self.table),
        )
        self.assertTrue(any("VnPCMDesc" in issue for issue in issues))

    def test_callback_parameter_width_change_is_detected(self) -> None:
        changed_text = self.header_text.replace(
            "typedef void (*VnLogFn)(void* userData, uint32_t level, "
            "uint32_t code,",
            "typedef void (*VnLogFn)(void* userData, uint64_t level, "
            "uint32_t code,",
            1,
        )
        self.assertNotEqual(self.header_text, changed_text)
        changed = MODULE.parse_header_contract(changed_text)
        issues = MODULE.compare_completeness(
            changed,
            MODULE.table_contract(self.table),
        )
        self.assertTrue(any("VnLogFn" in issue for issue in issues))

    def test_scalar_typedef_width_change_is_detected(self) -> None:
        changed_text = self.header_text.replace(
            "typedef VnObjectId VnSoundId;",
            "typedef uint32_t VnSoundId;",
            1,
        )
        changed = MODULE.parse_header_contract(changed_text)
        issues = MODULE.compare_completeness(
            changed,
            MODULE.table_contract(self.table),
        )
        self.assertTrue(any("VnSoundId" in issue for issue in issues))

    def test_function_like_public_macro_fails_closed(self) -> None:
        with self.assertRaises(MODULE.ManifestError):
            MODULE.parse_header_contract(
                self.header_text + "\n#define VN_HELPER(x) (x)\n"
            )

    def test_implicit_public_enum_member_fails_closed(self) -> None:
        changed_text = self.header_text.replace(
            "VN_DEVICE_NULL = 1", "VN_DEVICE_NULL", 1
        )
        with self.assertRaises(MODULE.ManifestError):
            MODULE.parse_header_contract(changed_text)

    def test_unknown_public_typedef_style_fails_closed(self) -> None:
        with self.assertRaises(MODULE.ManifestError):
            MODULE.parse_header_contract(
                self.header_text + "\ntypedef unsigned long VnFutureId;\n"
            )

    def test_anonymous_public_struct_typedef_fails_closed(self) -> None:
        with self.assertRaises(MODULE.ManifestError):
            MODULE.parse_header_contract(
                self.header_text
                + "\ntypedef struct { uint32_t x; } VnFutureStruct;\n"
            )

    def test_named_public_enum_is_in_completeness_scan(self) -> None:
        changed = MODULE.parse_header_contract(
            self.header_text
            + "\nenum PublicKind { VN_FUTURE_KIND = 900 };\n"
        )
        issues = MODULE.compare_completeness(
            changed,
            MODULE.table_contract(self.table),
        )
        self.assertTrue(
            any("VN_FUTURE_KIND" in issue for issue in issues),
            issues,
        )

    def test_implicit_trace_call_id_fails_closed(self) -> None:
        changed_text = self.trace_text.replace(
            "kTraceCallUnregisterPCM = 2",
            "kTraceCallUnregisterPCM",
            1,
        )
        with self.assertRaises(MODULE.ManifestError):
            MODULE.parse_trace_contract(changed_text)

    def test_duplicate_json_keys_and_unsupported_target_fail_closed(
        self,
    ) -> None:
        with self.assertRaises(MODULE.ManifestError):
            MODULE.reject_duplicate_keys([("key", 1), ("key", 2)])
        changed = copy.deepcopy(self.table)
        changed["abi"]["pointerBytes"] = 4
        with self.assertRaises(MODULE.ManifestError):
            MODULE.validate_table_metadata(changed)

    def test_vintage_metadata_mutations_fail_closed(self) -> None:
        changed = copy.deepcopy(self.table)
        changed["abi"]["stridePolicies"]["VnFuture.rows"] = {
            "acceptedValues": [0, 13, 24]
        }
        with self.assertRaises(MODULE.ManifestError):
            MODULE.validate_table_metadata(changed)

        changed = copy.deepcopy(self.table)
        changed["abi"]["structSizePolicies"]["VnConfig"][
            "missingFields"
        ] = "arbitrary"
        with self.assertRaises(MODULE.ManifestError):
            MODULE.validate_table_metadata(changed)

        changed = copy.deepcopy(self.table)
        changed["abi"]["structSizePolicies"]["VnConfig"][
            "includeCurrentSize"
        ] = False
        with self.assertRaises(MODULE.ManifestError):
            MODULE.validate_table_metadata(changed)

    def test_struct_parser_handles_forward_definition_and_commas(self) -> None:
        source = """
            typedef struct VnForward VnForward;
            struct VnForward {
                float x, y;
                uint32_t values[2];
            };
        """
        bodies = MODULE.extract_struct_bodies(source)
        self.assertEqual(["VnForward"], list(bodies))
        self.assertEqual(
            ["x", "y", "values"],
            MODULE.extract_struct_fields(
                bodies["VnForward"], "VnForward"
            ),
        )

    def test_manifest_is_deterministic_and_has_expected_vintages(self) -> None:
        first = MODULE.build_manifest(REPO_ROOT)
        second = MODULE.build_manifest(REPO_ROOT)
        self.assertEqual(
            MODULE.canonical_bytes(first),
            MODULE.canonical_bytes(second),
        )
        policies = first["contract"]["nativeAbi"]["vintages"][
            "struct_size"
        ]
        self.assertEqual(
            [
                32,
                64,
                80,
                88,
                112,
                184,
                192,
                208,
                224,
                232,
                240,
                264,
                280,
                288,
                344,
                352,
            ],
            policies["VnConfig"]["acceptedSizes"],
        )
        self.assertEqual(
            [
                1832,
                2560,
                2568,
                2656,
                2680,
                2752,
                3144,
                3152,
                3160,
                3416,
                3536,
                4280,
                4288,
                4296,
                4336,
                4376,
                5400,
            ],
            policies["VnSnapshot"]["acceptedSizes"],
        )
        self.assertEqual(
            [64, 72, 88],
            policies["VnPlayDesc"]["acceptedSizes"],
        )
        self.assertEqual(
            [608, 612, 796],
            policies["VnAttenuationDesc"]["acceptedSizes"],
        )
        self.assertEqual(
            "polymorphic-base",
            policies["VnEffectDesc"]["mode"],
        )
        self.assertEqual(
            "explicit-non-standalone",
            policies["VnEffectDesc"]["source"],
        )
        self.assertNotIn(
            "acceptedSizes",
            policies["VnEffectDesc"],
        )
        self.assertEqual(
            {}, first["contract"]["nativeAbi"]["vintages"]["stride"]
        )
        self.assertEqual(
            "exact-current", policies["VnPCMDesc"]["mode"]
        )
        self.assertEqual("default", policies["VnPCMDesc"]["source"])
        self.assertEqual(
            {"nativeAbiSha256", "remoteWireSha256", "traceFormatSha256"},
            set(first["sectionSha256"]),
        )

    def test_dumpbin_parser_ignores_non_export_rows(self) -> None:
        output = """
              ordinal hint RVA      name
                    1    0 000110D0 Vn_Init = @ILT+10(Vn_Init)
                    2    1 00011120 Vn_Term
            Summary
                1000 .data
        """
        self.assertEqual(
            ["Vn_Init", "Vn_Term"],
            MODULE.parse_dumpbin_exports(output),
        )


if __name__ == "__main__":
    unittest.main()
