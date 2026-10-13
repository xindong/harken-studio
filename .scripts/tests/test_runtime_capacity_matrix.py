import unittest

import runtime_capacity_matrix as matrix


class CalibrationModelTest(unittest.TestCase):
    @staticmethod
    def pilots(mono_mean=5500.0):
        def record(mean):
            return {"measurements": {
                "pump": {"mean_us": mean, "p15_625_us": mean, "p50_us": mean},
                "update": {"mean_us": 1.0},
                "sync_serve": {"mean_us": 1.0},
            }}

        rows = {
            identifier: record(100.0)
            for _, identifiers in matrix.CALIBRATION_GROUPS
            for identifier in identifiers
        }
        rows["pitch.native.48000.stereo.p48.r16"] = record(3000.0)
        rows[matrix.MONO_PITCH_CONTROL] = record(mono_mean)
        return rows

    @staticmethod
    def catalog():
        return {"scenario_id": matrix.MONO_PITCH_CONTROL,
                "render_capacity": 64, "pitch_semitones": 48,
                "kind": "render", "mix_rate": 48000}

    def test_mono_uses_its_own_pilot_without_stereo_voice_scaling(self):
        catalog = self.catalog()
        _, decisions = matrix.estimate_calibrated_seconds(
            [catalog], self.pilots(), {}, 0.1, 1.0)
        decision = decisions[matrix.MONO_PITCH_CONTROL]
        self.assertEqual(15.0, matrix.timing_cap(catalog))
        self.assertAlmostEqual(5500.0 * 1.25 * 1584 / 1e6,
                               decision["formal_predicted_seconds"])
        self.assertFalse(decision["calibration_expected_screen"])

    def test_expected_screen_keeps_formal_cap_as_fallback(self):
        _, decisions = matrix.estimate_calibrated_seconds(
            [self.catalog()], self.pilots(12000.0), {}, 0.1, 1.0)
        decision = decisions[matrix.MONO_PITCH_CONTROL]
        self.assertTrue(decision["calibration_expected_screen"])
        self.assertAlmostEqual(decision["pilot_predicted_seconds"] + 15.0,
                               decision["total_predicted_seconds"])


if __name__ == "__main__":
    unittest.main()
