#!/usr/bin/env python3
import csv
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "telemetry"))

import decode  # noqa: E402


FIXTURES = ROOT / "tests" / "fixtures" / "telemetry"


class TelemetryDecoderTests(unittest.TestCase):
    def test_valid_fixture_decodes_all_records_and_exact_enum_semantics(self):
        result = decode.decode_file(FIXTURES / "valid_v1.mft")

        self.assertEqual(result["header"]["magic"], "MFT1")
        self.assertEqual(result["header"]["schema_version"], 1)
        self.assertEqual(result["header"]["header_size"], 80)
        self.assertEqual(result["header"]["platform"], "Host")
        self.assertEqual(
            [record["record_type"] for record in result["records"]],
            [
                "SystemSample",
                "FrameTimingSummary",
                "StateTransition",
                "WorkerSample",
                "NetworkRequest",
                "ArtworkSummary",
                "ArtworkDecode",
                "LibrarySync",
                "DownloadSample",
                "DownloadSegmentAttempt",
                "PlaybackEvent",
                "UiStall",
                "TelemetryHealth",
                "SessionEvent",
            ],
        )

        ui_stall = result["records"][11]["payload"]
        self.assertEqual(ui_stall["phase"], "Render")
        self.assertEqual(ui_stall["scope"], "MovieDetailsScreen::enter")
        state = result["records"][2]["payload"]
        self.assertEqual(state["state_kind"], "PlaybackState")
        self.assertEqual(state["current"], "Resuming")
        session = result["records"][13]["payload"]
        self.assertEqual(session["kind"], "TelemetryStarted")
        self.assertEqual(session["outcome"], "Success")
        self.assertEqual(session["value0"], 1000)
        self.assertEqual(session["value1"], 134217728)

    def test_valid_size_unknown_record_is_skipped(self):
        original = (FIXTURES / "valid_v1.mft").read_bytes()
        unknown = bytes.fromhex("63001400e7030000e803000000000000") + b"ABCD"
        result = decode.decode_bytes(original[:80] + unknown + original[80:])

        self.assertEqual(len(result["records"]), 14)
        self.assertEqual(result["records"][0]["record_type"], "SystemSample")

    def test_partial_tail_preserves_prior_complete_records(self):
        full = decode.decode_file(FIXTURES / "valid_v1.mft")
        truncated = decode.decode_file(FIXTURES / "truncated_v1.mft")

        self.assertEqual(len(truncated["records"]), len(full["records"]) - 1)
        self.assertEqual(
            truncated["records"][-1]["record_type"],
            "TelemetryHealth",
        )

    def test_json_and_csv_directory_exports(self):
        result = decode.decode_file(FIXTURES / "valid_v1.mft")
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            json_path = directory / "trace.json"
            decode.write_json(result, json_path)
            self.assertEqual(json.loads(json_path.read_text())["header"]["magic"], "MFT1")

            csv_directory = directory / "csv"
            decode.write_csv_directory(result, csv_directory)
            self.assertTrue((csv_directory / "header.json").exists())
            with (csv_directory / "SessionEvent.csv").open(newline="") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(rows[0]["kind"], "TelemetryStarted")


if __name__ == "__main__":
    unittest.main()
