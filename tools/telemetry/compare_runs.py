#!/usr/bin/env python3
"""Compare repeated A/B/C telemetry summaries using standard-library math."""

import argparse
import json
import statistics
from pathlib import Path


def _read(path):
    return json.loads(Path(path).read_text())


def _mean(values):
    return statistics.median(values) if values else None


def _metric_values(summary):
    system = summary.get("system", {})
    samples = system.get("samples", [])
    frames = summary.get("frames", {})
    requests = summary.get("requests", {}).get("latency", {})
    artwork = summary.get("artwork", {})
    downloads = summary.get("downloads", {})
    playback = summary.get("playback", {})
    health = summary.get("health", {})
    return {
        "timeline.duration_us": summary.get("timeline", {}).get("duration_us"),
        "system.cpu_percent_one_core": _mean(
            [row.get("cpu_percent_one_core", 0.0) for row in samples[1:]]
        ),
        "system.rss_latest_kib": system.get("rss", {}).get("latest_kib"),
        "system.rss_peak_kib": system.get("rss", {}).get("peak_rss_kib"),
        "system.read_bytes_per_sec": system.get("io", {}).get("read_bytes_per_sec"),
        "system.write_bytes_per_sec": system.get("io", {}).get("write_bytes_per_sec"),
        "system.free_storage_bytes": system.get("free_space", {}).get("minimum_bytes"),
        "frames.maximum_us": frames.get("maximum_us"),
        "frames.stalls_over_50ms": frames.get("stalls_over_50ms"),
        "frames.stalls_over_100ms": frames.get("stalls_over_100ms"),
        "requests.average_latency_us": requests.get("average_us"),
        "requests.p95_latency_us": requests.get("p95_us"),
        "artwork.decode_average_us": artwork.get("decode_latency", {}).get("average_us"),
        "downloads.bytes": downloads.get("bytes"),
        "downloads.segment_retries": downloads.get("segment_retries"),
        "playback.event_count": len(playback.get("events", [])),
        "health.dropped_records": health.get("dropped_records"),
        "health.writer_errors": health.get("writer_errors"),
        "correlations.count": len(summary.get("correlations", [])),
    }


def _group(paths):
    values_by_metric = {}
    for path in paths:
        for metric, value in _metric_values(_read(path)).items():
            if value is not None:
                values_by_metric.setdefault(metric, []).append(value)
    result = {}
    for metric, values in values_by_metric.items():
        result[metric] = {
            "repetitions": len(values),
            "median": statistics.median(values),
            "minimum": min(values),
            "maximum": max(values),
            "spread": max(values) - min(values),
            "values": values,
        }
    return result


def _delta(left, right):
    if left is None or right is None:
        return None
    return right - left


def _percent_delta(left, right):
    if left is None or right is None or left == 0:
        return None
    return (right - left) * 100.0 / left


def compare_runs(a_paths, b_paths, c_paths):
    """Return repeated-run medians and A-to-B/B-to-C observer deltas."""
    groups = {"A": _group(a_paths), "B": _group(b_paths), "C": _group(c_paths)}
    metrics = sorted(set().union(*(set(group) for group in groups.values())))
    comparisons = {}
    for metric in metrics:
        a = groups["A"].get(metric, {}).get("median")
        b = groups["B"].get(metric, {}).get("median")
        c = groups["C"].get(metric, {}).get("median")
        comparisons[metric] = {
            "A": a,
            "B": b,
            "C": c,
            "B_minus_A": _delta(a, b),
            "C_minus_B": _delta(b, c),
            "C_minus_A": _delta(a, c),
            "B_vs_A_percent": _percent_delta(a, b),
            "C_vs_B_percent": _percent_delta(b, c),
        }
    return {"groups": groups, "comparisons": comparisons}


def _sample_summary(cpu, rss, retries):
    return {
        "timeline": {"duration_us": 1000},
        "system": {
            "samples": [{"cpu_percent_one_core": 0}, {"cpu_percent_one_core": cpu}],
            "rss": {"latest_kib": rss, "peak_rss_kib": rss},
            "io": {"read_bytes_per_sec": 0, "write_bytes_per_sec": 0},
            "free_space": {"minimum_bytes": 100},
        },
        "frames": {"maximum_us": 10, "stalls_over_50ms": 0, "stalls_over_100ms": 0},
        "requests": {"latency": {"average_us": 20, "p95_us": 20}},
        "artwork": {"decode_latency": {"average_us": 30}},
        "downloads": {"bytes": 40, "segment_retries": retries},
        "playback": {"events": []}, "health": {"dropped_records": 0, "writer_errors": 0},
        "correlations": [],
    }


def self_test():
    import tempfile

    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)
        paths = {}
        for name, summary in {
            "a1": _sample_summary(10, 100, 1),
            "a2": _sample_summary(12, 110, 1),
            "b1": _sample_summary(11, 120, 2),
            "c1": _sample_summary(20, 130, 3),
        }.items():
            path = directory / (name + ".json")
            path.write_text(json.dumps(summary))
            paths[name] = path
        result = compare_runs(
            [paths["a1"], paths["a2"]], [paths["b1"]], [paths["c1"]]
        )
        cpu = result["comparisons"]["system.cpu_percent_one_core"]
        assert cpu["A"] == 11
        assert cpu["B_minus_A"] == 0
        assert cpu["C_minus_B"] == 9
        assert result["groups"]["A"]["system.rss_latest_kib"]["spread"] == 10
    print("compare_runs self-test OK")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--a", nargs="+", type=Path, help="A (telemetry disabled) summaries")
    parser.add_argument("--b", nargs="+", type=Path, help="B (compiled-in, runtime off) summaries")
    parser.add_argument("--c", nargs="+", type=Path, help="C (compiled-in, runtime on) summaries")
    parser.add_argument("--json", dest="json_path", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        self_test()
        return 0
    if not args.a or not args.b or not args.c:
        parser.error("--a, --b, and --c are required unless --self-test is used")
    result = compare_runs(args.a, args.b, args.c)
    text = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.json_path:
        args.json_path.write_text(text)
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
