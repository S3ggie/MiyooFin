#!/usr/bin/env python3
"""Summarize and correlate decoded MFT v1 telemetry on a desktop."""

import argparse
import csv
import json
import math
from pathlib import Path

import decode


def _records(decoded, record_type):
    return sorted(
        (record for record in decoded.get("records", [])
         if record.get("record_type") == record_type),
        key=lambda record: (record.get("monotonic_us", 0), record.get("sequence", 0)),
    )


def _rate(delta, interval_us):
    if interval_us <= 0:
        return 0.0
    return float(delta) * 1000000.0 / float(interval_us)


def _positive_delta(current, previous):
    if previous is None or current < previous:
        return 0
    return current - previous


def _stats(values):
    if not values:
        return {"count": 0, "total_us": 0, "average_us": 0.0,
                "minimum_us": 0, "maximum_us": 0, "p95_us": 0}
    ordered = sorted(values)
    percentile_index = min(len(ordered) - 1, int(math.ceil(len(ordered) * 0.95)) - 1)
    return {
        "count": len(values),
        "total_us": sum(values),
        "average_us": float(sum(values)) / len(values),
        "minimum_us": ordered[0],
        "maximum_us": ordered[-1],
        "p95_us": ordered[percentile_index],
    }


def _record_view(record):
    view = {
        "record_type": record.get("record_type"),
        "record_type_id": record.get("record_type_id"),
        "sequence": record.get("sequence"),
        "monotonic_us": record.get("monotonic_us"),
    }
    view.update(record.get("payload", {}))
    return view


def _group_key(payload, key):
    return payload.get(key, 0)


def _system_summary(records, cpu_count):
    samples = []
    previous = None
    read_total = 0
    write_total = 0
    for record in records:
        payload = record["payload"]
        timestamp = record["monotonic_us"]
        interval_us = timestamp - previous["monotonic_us"] if previous else 0
        read_delta = _positive_delta(
            payload.get("process_read_bytes_cumulative", 0),
            previous["payload"].get("process_read_bytes_cumulative") if previous else None,
        )
        write_delta = _positive_delta(
            payload.get("process_write_bytes_cumulative", 0),
            previous["payload"].get("process_write_bytes_cumulative") if previous else None,
        )
        read_total += read_delta
        write_total += write_delta
        row = _record_view(record)
        cpu_delta = _positive_delta(
            payload.get("process_cpu_us_cumulative", 0),
            previous["payload"].get("process_cpu_us_cumulative") if previous else None,
        )
        row.update({
            "monotonic_delta_us": interval_us,
            "read_bytes_delta": read_delta,
            "write_bytes_delta": write_delta,
            "read_bytes_per_sec": _rate(read_delta, interval_us),
            "write_bytes_per_sec": _rate(write_delta, interval_us),
            "cpu_percent_one_core": (
                float(cpu_delta) * 100.0 / float(interval_us) if interval_us > 0 else 0.0
            ),
        })
        if cpu_count and cpu_count > 0:
            row["cpu_percent_device"] = row["cpu_percent_one_core"] / cpu_count
        samples.append(row)
        previous = record

    rss_values = [record["payload"].get("rss_kib", 0) for record in records]
    peak_values = [record["payload"].get("peak_rss_kib", 0) for record in records]
    free_values = [record["payload"].get("free_storage_bytes", 0) for record in records]
    duration_us = records[-1]["monotonic_us"] - records[0]["monotonic_us"] if len(records) > 1 else 0
    return {
        "samples": samples,
        "rss": {
            "latest_kib": rss_values[-1] if rss_values else 0,
            "maximum_kib": max(rss_values) if rss_values else 0,
            "peak_rss_kib": max(peak_values) if peak_values else 0,
        },
        "io": {
            "read_bytes": read_total,
            "write_bytes": write_total,
            "read_bytes_per_sec": _rate(read_total, duration_us),
            "write_bytes_per_sec": _rate(write_total, duration_us),
        },
        "free_space": {
            "latest_bytes": free_values[-1] if free_values else 0,
            "minimum_bytes": min(free_values) if free_values else 0,
        },
    }


def _frame_summary(records):
    histogram = [0] * 9
    rows = []
    for record in records:
        payload = record["payload"]
        for index, value in enumerate(payload.get("histogram", [])):
            if index < len(histogram):
                histogram[index] += value
        rows.append(_record_view(record))
    return {
        "samples": rows,
        "histogram": histogram,
        "maximum_us": max((row.get("max_us", 0) for row in rows), default=0),
        "sample_count": sum(row.get("sample_count", 0) for row in rows),
        "stalls_over_50ms": sum(row.get("over_50ms_count", 0) for row in rows),
        "stalls_over_100ms": sum(row.get("over_100ms_count", 0) for row in rows),
    }


def _worker_summary(records):
    groups = {}
    rows = []
    for record in records:
        payload = record["payload"]
        worker_id = _group_key(payload, "worker_id")
        group = groups.setdefault(worker_id, {
            "worker_id": worker_id, "samples": 0, "latest_active": 0,
            "latest_queue_depth": 0, "maximum_queue_highwater": 0,
            "completed": 0, "failed": 0, "cancelled": 0,
        })
        group["samples"] += 1
        group["latest_active"] = payload.get("active", 0)
        group["latest_queue_depth"] = payload.get("queue_depth", 0)
        group["maximum_queue_highwater"] = max(
            group["maximum_queue_highwater"], payload.get("queue_highwater", 0)
        )
        group["completed"] += payload.get("completed_delta", 0)
        group["failed"] += payload.get("failed_delta", 0)
        group["cancelled"] += payload.get("cancelled_delta", 0)
        rows.append(_record_view(record))
    return {"samples": rows, "by_worker": list(groups.values())}


def _request_summary(records):
    groups = {}
    rows = []
    for record in records:
        payload = record["payload"]
        key = (payload.get("request_kind_id", 0), payload.get("request_kind", "Unknown"))
        groups.setdefault(key, []).append(payload.get("duration_us", 0))
        rows.append(_record_view(record))
    by_kind = []
    for (kind_id, kind), durations in sorted(groups.items()):
        row = {"request_kind_id": kind_id, "request_kind": kind}
        row.update(_stats(durations))
        by_kind.append(row)
    return {"records": rows, "by_request_kind": by_kind, "latency": _stats(
        [record["payload"].get("duration_us", 0) for record in records]
    )}


def _artwork_summary(summary_records, decode_records):
    total = {}
    for record in summary_records:
        for key, value in record["payload"].items():
            if key != "reserved" and isinstance(value, (int, float)):
                total[key] = total.get(key, 0) + value
    decode_durations = [record["payload"].get("duration_us", 0) for record in decode_records]
    return {
        "summary_records": [_record_view(record) for record in summary_records],
        "decode_records": [_record_view(record) for record in decode_records],
        "totals": total,
        "decode_latency": _stats(decode_durations),
    }


def _download_summary(sample_records, attempt_records):
    samples = [_record_view(record) for record in sample_records]
    attempts = [_record_view(record) for record in attempt_records]
    return {
        "samples": samples,
        "segment_attempts": attempts,
        "bytes": sum(row.get("bytes_delta", 0) for row in samples),
        "segments_completed": sum(row.get("segments_completed_delta", 0) for row in samples),
        "segment_retries": sum(row.get("segment_retries_delta", 0) for row in samples),
        "attempt_latency": _stats([row.get("duration_us", 0) for row in attempts]),
    }


def _playback_summary(records):
    rows = [_record_view(record) for record in records]
    by_stage = {}
    for row in rows:
        stage = row.get("stage", "Unknown")
        by_stage.setdefault(stage, []).append(row.get("duration_us", 0))
    return {
        "events": rows,
        "by_stage": [
            dict({"stage": stage}, **_stats(durations))
            for stage, durations in sorted(by_stage.items())
        ],
    }


def _health_summary(records):
    rows = [_record_view(record) for record in records]
    return {
        "records": rows,
        "latest": rows[-1] if rows else {},
        "maximum_queue_depth": max((row.get("queue_depth", 0) for row in rows), default=0),
        "dropped_records": max((row.get("dropped_records_cumulative", 0) for row in rows), default=0),
        "writer_errors": max((row.get("writer_errors_cumulative", 0) for row in rows), default=0),
    }


def _state_summary(records):
    rows = [_record_view(record) for record in records]
    latest = {}
    for row in rows:
        latest[row.get("state_kind", "Unknown")] = row
    return {"transitions": rows, "latest": latest}


def _session_summary(records):
    rows = [_record_view(record) for record in records]
    by_kind = {}
    for row in rows:
        by_kind[row.get("kind", "Unknown")] = by_kind.get(row.get("kind", "Unknown"), 0) + 1
    return {"events": rows, "by_kind": by_kind}


def _event_window(record):
    payload = record.get("payload", {})
    duration = payload.get("duration_us", 0)
    end = record.get("monotonic_us", 0)
    return max(0, end - duration), end


def _overlaps(record, start, end):
    record_start, record_end = _event_window(record)
    return record_start <= end and record_end >= start


def correlate(decoded, slow_frame_threshold_us=50000):
    """Associate slow/stall timestamps with state and nearby work."""
    records = sorted(decoded.get("records", []), key=lambda record: record.get("monotonic_us", 0))
    state_records = [record for record in records if record.get("record_type") == "StateTransition"]
    worker_records = [record for record in records if record.get("record_type") == "WorkerSample"]
    network_records = [record for record in records if record.get("record_type") == "NetworkRequest"]
    download_records = [record for record in records
                        if record.get("record_type") in ("DownloadSegmentAttempt", "DownloadSample")]
    targets = []
    for record in records:
        record_type = record.get("record_type")
        payload = record.get("payload", {})
        if record_type == "UiStall":
            targets.append((record, "UiStall", payload.get("duration_us", 0)))
        elif record_type == "FrameTimingSummary" and (
            payload.get("over_50ms_count", 0) or payload.get("over_100ms_count", 0)
            or payload.get("max_us", 0) >= slow_frame_threshold_us
        ):
            targets.append((record, "FrameTimingSummary", payload.get("max_us", 0)))

    correlations = []
    for target, source, duration in targets:
        timestamp = target.get("monotonic_us", 0)
        start = max(0, timestamp - duration)
        latest_state = {}
        for state in state_records:
            if state.get("monotonic_us", 0) <= timestamp:
                latest_state[state["payload"].get("state_kind", "Unknown")] = _record_view(state)
        latest_workers = {}
        for worker in worker_records:
            if worker.get("monotonic_us", 0) <= timestamp:
                worker_id = worker["payload"].get("worker_id", 0)
                latest_workers[worker_id] = _record_view(worker)
        correlations.append({
            "source": source,
            "monotonic_us": timestamp,
            "duration_us": duration,
            "state": latest_state,
            "workers": list(latest_workers.values()),
            "network": [_record_view(record) for record in network_records
                         if _overlaps(record, start, timestamp)],
            "downloads": [_record_view(record) for record in download_records
                           if _overlaps(record, start, timestamp)],
        })
    return correlations


def summarize(decoded, cpu_count=None):
    """Return a JSON-serializable summary derived from monotonic timestamps."""
    records = sorted(decoded.get("records", []), key=lambda record: record.get("monotonic_us", 0))
    timestamps = [record.get("monotonic_us", 0) for record in records]
    start = timestamps[0] if timestamps else 0
    end = timestamps[-1] if timestamps else 0
    result = {
        "header": decoded.get("header", {}),
        "timeline": {
            "start_monotonic_us": start,
            "end_monotonic_us": end,
            "duration_us": max(0, end - start),
            "record_count": len(records),
        },
        "system": _system_summary(_records(decoded, "SystemSample"), cpu_count),
        "frames": _frame_summary(_records(decoded, "FrameTimingSummary")),
        "workers": _worker_summary(_records(decoded, "WorkerSample")),
        "requests": _request_summary(_records(decoded, "NetworkRequest")),
        "artwork": _artwork_summary(
            _records(decoded, "ArtworkSummary"), _records(decoded, "ArtworkDecode")
        ),
        "downloads": _download_summary(
            _records(decoded, "DownloadSample"), _records(decoded, "DownloadSegmentAttempt")
        ),
        "playback": _playback_summary(_records(decoded, "PlaybackEvent")),
        "health": _health_summary(_records(decoded, "TelemetryHealth")),
        "state": _state_summary(_records(decoded, "StateTransition")),
        "session": _session_summary(_records(decoded, "SessionEvent")),
    }
    result["correlations"] = correlate(decoded)
    return result


_TABLE_FIELDS = {
    "timeline": ["record_type", "record_type_id", "sequence", "monotonic_us"],
    "system": ["monotonic_us", "monotonic_delta_us", "cpu_percent_one_core", "cpu_percent_device"],
    "frames": ["monotonic_us", "phase", "sample_count", "max_us", "over_50ms_count", "over_100ms_count"],
    "workers": ["monotonic_us", "worker_id", "active", "queue_depth", "queue_highwater"],
    "requests": ["request_kind_id", "request_kind", "count", "average_us", "minimum_us", "maximum_us", "p95_us"],
    "artwork": ["monotonic_us", "record_type", "context", "outcome", "duration_us"],
    "downloads": ["monotonic_us", "record_type", "active_downloads", "bytes_delta", "duration_us"],
    "playback": ["monotonic_us", "stage", "source", "duration_us", "playback_seq"],
    "health": ["monotonic_us", "queue_depth", "dropped_records_cumulative", "writer_errors_cumulative"],
    "state": ["monotonic_us", "state_kind", "previous", "current", "transition_seq"],
    "session": ["monotonic_us", "kind", "outcome", "value0", "value1"],
    "correlations": ["source", "monotonic_us", "duration_us", "state", "workers", "network", "downloads"],
}


def _json_value(value):
    if isinstance(value, (list, dict)):
        return json.dumps(value, sort_keys=True, separators=(",", ":"))
    return value


def _write_table(directory, name, rows):
    fields = list(_TABLE_FIELDS[name])
    for row in rows:
        for key in row:
            if key not in fields:
                fields.append(key)
    with (directory / (name + ".csv")).open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows({key: _json_value(value) for key, value in row.items()} for row in rows)


def _export_rows(summary):
    timeline = []
    for section in ("system", "frames", "workers", "requests", "artwork", "downloads",
                    "playback", "health", "state", "session"):
        for row in _rows_for_section(summary, section):
            timeline.append(row)
    artwork_rows = summary["artwork"]["summary_records"] + summary["artwork"]["decode_records"]
    download_rows = summary["downloads"]["samples"] + summary["downloads"]["segment_attempts"]
    return {
        "timeline": sorted(timeline, key=lambda row: row.get("monotonic_us", 0)),
        "system": summary["system"]["samples"],
        "frames": summary["frames"]["samples"],
        "workers": summary["workers"]["samples"],
        "requests": summary["requests"]["by_request_kind"],
        "artwork": artwork_rows,
        "downloads": download_rows,
        "playback": summary["playback"]["events"],
        "health": summary["health"]["records"],
        "state": summary["state"]["transitions"],
        "session": summary["session"]["events"],
        "correlations": summary["correlations"],
    }


def _rows_for_section(summary, section):
    if section == "system":
        return summary[section]["samples"]
    if section == "frames":
        return summary[section]["samples"]
    if section == "workers":
        return summary[section]["samples"]
    if section == "requests":
        return summary[section]["records"]
    if section == "artwork":
        return summary[section]["summary_records"] + summary[section]["decode_records"]
    if section == "downloads":
        return summary[section]["samples"] + summary[section]["segment_attempts"]
    if section == "playback":
        return summary[section]["events"]
    if section == "health":
        return summary[section]["records"]
    if section == "state":
        return summary[section]["transitions"]
    if section == "session":
        return summary[section]["events"]
    return []


def write_csv_exports(decoded, directory, cpu_count=None):
    """Write a summary JSON and stable, one-domain CSV tables."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    summary = summarize(decoded, cpu_count=cpu_count)
    (directory / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    for name, rows in _export_rows(summary).items():
        _write_table(directory, name, rows)
    return summary


def write_plots(summary, directory):
    """Optionally write simple plots when matplotlib is installed."""
    try:
        import matplotlib.pyplot as pyplot
    except ImportError:
        return False
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    samples = summary["system"]["samples"]
    if samples:
        x_values = [row["monotonic_us"] for row in samples]
        pyplot.figure()
        pyplot.plot(x_values, [row["rss_kib"] for row in samples])
        pyplot.xlabel("monotonic_us")
        pyplot.ylabel("RSS KiB")
        pyplot.title("Telemetry RSS")
        pyplot.savefig(directory / "rss.png")
        pyplot.close()
    return True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--json", dest="json_path", type=Path)
    parser.add_argument("--csv-dir", dest="csv_directory", type=Path)
    parser.add_argument("--cpu-count", type=int)
    parser.add_argument("--plot-dir", type=Path)
    args = parser.parse_args(argv)
    decoded = decode.decode_file(args.input)
    summary = summarize(decoded, cpu_count=args.cpu_count)
    if args.json_path:
        args.json_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    if args.csv_directory:
        write_csv_exports(decoded, args.csv_directory, cpu_count=args.cpu_count)
    if args.plot_dir and not write_plots(summary, args.plot_dir):
        print("matplotlib is not installed; plots were skipped")
    if not args.json_path and not args.csv_directory and not args.plot_dir:
        print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
