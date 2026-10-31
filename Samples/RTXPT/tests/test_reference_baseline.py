"""CPU checks for the numeric capture reader; actual readback is tested on the GPU."""
import math
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from capture_reference_baseline import difference, main, read_pfm


class ReferenceCaptureReaderTests(unittest.TestCase):
    def test_endian_and_hdr_values(self):
        expected = [0.0, 2.0, -0.5]
        with tempfile.TemporaryDirectory() as folder:
            for endian, scale in (("<", -1), (">", 1)):
                path = Path(folder) / "frame.pfm"
                path.write_bytes(f"PF\n1 1\n{scale}\n".encode() + struct.pack(endian + "3f", *expected))
                width, height, values = read_pfm(path)
                self.assertEqual((1, 1), (width, height))
                self.assertEqual(expected, list(values))

    def test_rejects_truncated_nonfinite_and_invalid_dimensions(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "frame.pfm"
            for header, values in ((b"PF\n1 1\n-1\n", [1.0, 2.0]),
                                   (b"PF\n1 1\n-1\n", [1.0, math.nan, 0.0]),
                                   (b"PF\n0 1\n-1\n", [])):
                with self.subTest(header=header, values=values):
                    path.write_bytes(header + struct.pack("<" + "f" * len(values), *values))
                    with self.assertRaises(ValueError):
                        read_pfm(path)

    def test_difference_detects_changed_signal_and_size(self):
        self.assertEqual({"rmse": 1.0, "p99_abs": 1.0, "max_abs": 1.0},
                         difference((1, 1, [0, 1, 2]), (1, 1, [1, 2, 3])))
        with self.assertRaises(ValueError):
            difference((1, 1, [0, 0, 0]), (2, 1, [0] * 6))

    def test_preflight_failure_records_failed_manifest(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "evidence"
            with patch("sys.argv", ["capture", "--exe", str(Path(folder) / "missing.exe"), "--output", str(output)]):
                with self.assertRaises(FileNotFoundError):
                    main()
            manifest = json.loads((output / "manifest.json").read_text())
            self.assertEqual("failed", manifest["status"])
            self.assertEqual({}, manifest["runs"])


if __name__ == "__main__":
    unittest.main()
