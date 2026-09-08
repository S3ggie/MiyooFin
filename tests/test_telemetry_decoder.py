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
import analyze  # noqa: E402
import compare_runs  # noqa: E402


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

    def test_analysis_represents_all_domains_and_derives_rates(self):
        decoded = decode.decode_file(FIXTURES / "valid_v1.mft")
        summary = analyze.summarize(decoded, cpu_count=4)

        self.assertEqual(summary["timeline"]["record_count"], 14)
        self.assertAlmostEqual(
            summary["system"]["samples"][0]["cpu_percent_one_core"], 0.0
        )
        self.assertIn("cpu_percent_device", summary["system"]["samples"][0])
        for section in (
            "system", "frames", "workers", "requests", "artwork", "downloads",
            "playback", "health", "state", "session", "correlations",
        ):
            self.assertIn(section, summary)
        self.assertEqual(summary["frames"]["histogram"], [1, 2, 3, 4, 5, 6, 7, 8, 9])
        self.assertEqual(summary["requests"]["latency"]["maximum_us"], 123)
        self.assertEqual(summary["downloads"]["bytes"], 3000)

    def test_analysis_cpu_io_and_grouping_math(self):
        def record(record_type, timestamp, payload):
            return {
                "record_type": record_type,
                "record_type_id": 1,
                "record_size": 0,
                "sequence": timestamp,
                "monotonic_us": timestamp,
                "payload": payload,
            }

        decoded = {"header": {}, "records": [
            record("SystemSample", 1000, {
                "process_cpu_us_cumulative": 100,
                "process_read_bytes_cumulative": 1000,
                "process_write_bytes_cumulative": 2000,
                "rss_kib": 10, "peak_rss_kib": 12, "free_storage_bytes": 900,
            }),
            record("SystemSample", 3000, {
                "process_cpu_us_cumulative": 1100,
                "process_read_bytes_cumulative": 5000,
                "process_write_bytes_cumulative": 2600,
                "rss_kib": 20, "peak_rss_kib": 22, "free_storage_bytes": 800,
            }),
            record("NetworkRequest", 2500, {
                "request_kind_id": 16, "request_kind": "Artwork", "duration_us": 1000,
            }),
        ]}
        summary = analyze.summarize(decoded, cpu_count=2)
        sample = summary["system"]["samples"][1]
        self.assertEqual(sample["monotonic_delta_us"], 2000)
        self.assertAlmostEqual(sample["cpu_percent_one_core"], 50.0)
        self.assertAlmostEqual(sample["cpu_percent_device"], 25.0)
        self.assertEqual(sample["read_bytes_delta"], 4000)
        self.assertEqual(summary["system"]["io"]["write_bytes"], 600)

    def test_analysis_correlates_state_and_overlapping_work(self):
        def record(record_type, timestamp, payload):
            return {
                "record_type": record_type,
                "record_type_id": 0,
                "sequence": timestamp,
                "monotonic_us": timestamp,
                "payload": payload,
            }

        decoded = {"header": {}, "records": [
            record("StateTransition", 100, {
                "state_kind": "Screen", "current": "Home",
            }),
            record("WorkerSample", 105, {
                "worker_id": 9, "active": 1, "queue_depth": 2,
            }),
            record("NetworkRequest", 190, {
                "request_kind": "Artwork", "duration_us": 50,
            }),
            record("DownloadSegmentAttempt", 195, {
                "duration_us": 40, "outcome": "Success",
            }),
            record("UiStall", 200, {
                "duration_us": 100, "scope": "Screen::update",
            }),
        ]}
        correlations = analyze.correlate(decoded)
        self.assertEqual(len(correlations), 1)
        self.assertEqual(correlations[0]["state"]["Screen"]["current"], "Home")
        self.assertEqual(len(correlations[0]["workers"]), 1)
        self.assertEqual(len(correlations[0]["network"]), 1)
        self.assertEqual(len(correlations[0]["downloads"]), 1)

    def test_analysis_writes_normalized_domain_exports(self):
        decoded = decode.decode_file(FIXTURES / "valid_v1.mft")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            analyze.write_csv_exports(decoded, output, cpu_count=2)
            for name in (
                "summary.json", "system.csv", "frames.csv", "workers.csv",
                "requests.csv", "artwork.csv", "downloads.csv", "playback.csv",
                "health.csv", "state.csv", "session.csv", "correlations.csv",
            ):
                self.assertTrue((output / name).exists(), name)
            with (output / "requests.csv").open(newline="") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(rows[0]["request_kind"], "Artwork")

    def test_compare_runs_reports_medians_spread_and_observer_deltas(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            summaries = []
            for name, cpu in (("a1", 10), ("a2", 14), ("b", 12), ("c", 22)):
                summary = compare_runs._sample_summary(cpu, 100, 0)
                path = directory / (name + ".json")
                path.write_text(json.dumps(summary))
                summaries.append(path)
            result = compare_runs.compare_runs(
                summaries[:2], [summaries[2]], [summaries[3]]
            )
            metric = result["comparisons"]["system.cpu_percent_one_core"]
            self.assertEqual(metric["A"], 12)
            self.assertEqual(metric["B_minus_A"], 0)
            self.assertEqual(metric["C_minus_B"], 10)
            self.assertEqual(
                result["groups"]["A"]["system.cpu_percent_one_core"]["spread"], 4
            )


if __name__ == "__main__":
    unittest.main()
