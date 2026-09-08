#!/usr/bin/env python3
"""Decode MFT v1 traces using only Python's standard library."""

import argparse
import csv
import json
import struct
from pathlib import Path

import schema


class MftDecodeError(ValueError):
    pass


def _enum(values, value):
    return schema.enum_name(values, value)


def _header(data):
    if len(data) < schema.FILE_HEADER_SIZE:
        raise MftDecodeError("file header is truncated")
    if data[:4] != schema.MAGIC:
        raise MftDecodeError("invalid MFT magic")
    version, header_size = struct.unpack_from("<HH", data, 4)
    if version != schema.SCHEMA_VERSION:
        raise MftDecodeError("unsupported MFT schema version {}".format(version))
    if header_size != schema.FILE_HEADER_SIZE:
        raise MftDecodeError("invalid MFT header size {}".format(header_size))
    values = struct.unpack_from(schema.FILE_HEADER_FORMAT, data, 8)
    names = (
        "flags", "pid", "session_nonce", "start_monotonic_us",
        "optional_wall_time_s", "sample_interval_ms", "free_space_interval_ms",
        "rotation_index", "writer_buffer_bytes", "app_version_packed",
        "build_commit32", "platform_id", "reserved0", "reserved1",
    )
    header = {"magic": "MFT1", "schema_version": version, "header_size": header_size}
    header.update(dict(zip(names, values)))
    header["platform"] = _enum(schema.PLATFORM_IDS, header["platform_id"])
    return header


def _unpack(fmt, payload):
    expected = struct.calcsize(fmt)
    if len(payload) != expected:
        raise MftDecodeError("record payload size {} does not match {}".format(len(payload), expected))
    return struct.unpack(fmt, payload)


def _decode_payload(record_type, payload):
    if record_type == 1:
        values = _unpack("<7Q4I", payload)
        names = (
            "process_cpu_us_cumulative", "rss_kib", "peak_rss_kib",
            "process_read_bytes_cumulative", "process_write_bytes_cumulative",
            "free_storage_bytes", "telemetry_logical_bytes_written",
            "dropped_records_cumulative", "free_storage_sample_age_ms",
            "validity_flags", "reserved",
        )
        return dict(zip(names, values))
    if record_type == 2:
        phase, interval, count, total, maximum, over_50, over_100 = struct.unpack_from("<B3xIIQIII", payload)
        histogram = struct.unpack_from("<9I", payload, 32)
        reserved = struct.unpack_from("<I", payload, 68)[0]
        return {
            "phase_id": phase,
            "phase": _enum(schema.FRAME_PHASES, phase),
            "interval_us": interval,
            "sample_count": count,
            "total_us": total,
            "max_us": maximum,
            "over_50ms_count": over_50,
            "over_100ms_count": over_100,
            "histogram": list(histogram),
            "reserved": reserved,
        }
    if record_type == 3:
        state_kind, _, previous, current, _, transition = _unpack("<BBHHHI", payload)
        return {
            "state_kind_id": state_kind,
            "state_kind": _enum(schema.STATE_KINDS, state_kind),
            "previous_id": previous,
            "previous": _state_name(state_kind, previous),
            "current_id": current,
            "current": _state_name(state_kind, current),
            "transition_seq": transition,
        }
    if record_type == 4:
        values = _unpack("<HBB6I", payload)
        return {
            "worker_id": values[0], "active": values[1],
            "queue_depth": values[3], "queue_highwater": values[4],
            "completed_delta": values[5], "failed_delta": values[6],
            "cancelled_delta": values[7], "reserved": values[8],
        }
    if record_type == 5:
        values = _unpack("<HBBQIIQQBBBB", payload)
        return {
            "request_kind_id": values[0], "request_kind": _enum(schema.REQUEST_KINDS, values[0]),
            "route_kind_id": values[1], "route_kind": _enum(schema.ROUTE_KINDS, values[1]),
            "method_id": values[2], "method": _enum(schema.HTTP_METHODS, values[2]),
            "duration_us": values[3], "http_status": values[4], "curl_code": values[5],
            "rx_payload_bytes": values[6], "tx_body_bytes": values[7],
            "attempt": values[8], "cancelled": values[9], "truncated": values[10],
            "fallback_attempt": values[11],
        }
    if record_type == 6:
        values = _unpack("<6I2Q2IQ2I", payload)
        names = (
            "cache_probe_hits", "cache_probe_misses", "cache_read_success", "cache_read_failure",
            "cache_write_success", "cache_write_failure", "compressed_read_bytes",
            "compressed_written_bytes", "decode_count", "decode_failures", "decode_total_us",
            "decode_max_us", "reserved",
        )
        return dict(zip(names, values))
    if record_type == 7:
        context, outcome, _, duration, compressed, decoded, reserved = _unpack("<BBHQQQI", payload)
        return {
            "context_id": context, "context": _enum(schema.ARTWORK_CONTEXTS, context),
            "outcome_id": outcome, "outcome": _enum(schema.OUTCOMES, outcome),
            "duration_us": duration, "compressed_input_bytes": compressed,
            "decoded_rgba_bytes": decoded, "reserved": reserved,
        }
    if record_type == 8:
        duration, outcome, saved, _, views, media, changed, requests, reserved = _unpack("<QBBH5I", payload)
        return {
            "duration_us": duration, "outcome_id": outcome, "outcome": _enum(schema.OUTCOMES, outcome),
            "cache_saved": saved, "views_count": views, "media_count": media,
            "changed_hierarchy_count": changed, "request_count": requests, "reserved": reserved,
        }
    if record_type == 9:
        values = _unpack("<IIQQ4I", payload)
        names = (
            "active_downloads", "queued_downloads", "bytes_delta", "bytes_per_sec",
            "segments_completed_delta", "segment_retries_delta", "planner_queue_depth", "reserved",
        )
        return dict(zip(names, values))
    if record_type == 10:
        values = _unpack("<IIHHBBBBQQIIQ", payload)
        return {
            "telemetry_job_seq": values[0], "segment_ordinal": values[1],
            "attempt_number": values[2], "retry_delay_ms": values[3],
            "route_kind_id": values[4], "route_kind": _enum(schema.ROUTE_KINDS, values[4]),
            "outcome_id": values[5], "outcome": _enum(schema.OUTCOMES, values[5]),
            "retry_planned": values[6], "reserved0": values[7], "duration_us": values[8],
            "payload_bytes": values[9], "http_status": values[10], "curl_code": values[11],
            "reserved1": values[12],
        }
    if record_type == 11:
        stage, source, child_exit, _, duration, exit_code, reserved, sequence = _unpack("<BBBBQiII", payload)
        return {
            "stage_id": stage, "stage": _enum(schema.PLAYBACK_STAGES, stage),
            "source_id": source, "source": _enum(schema.PLAYBACK_SOURCES, source),
            "child_exit_kind_id": child_exit, "child_exit_kind": _enum(schema.CHILD_EXIT_KINDS, child_exit),
            "duration_us": duration, "child_exit_code": exit_code, "reserved": reserved,
            "playback_seq": sequence,
        }
    if record_type == 12:
        edge, _, screen, tab, action, phase, duration, scope, worker_mask, reserved = _unpack("<BBHHHB3xQHHQ", payload)
        return {
            "edge_id": edge, "edge": _enum(schema.STALL_EDGES, edge),
            "screen_id": screen, "screen": _enum(schema.SCREEN_IDS, screen),
            "tab_id": tab, "tab": _enum(schema.TAB_IDS, tab),
            "action_id": action, "action": _enum(schema.ACTION_IDS, action),
            "phase_id": phase, "phase": _enum(schema.UI_PHASE_IDS, phase),
            "duration_us": duration, "scope_id": scope, "scope": _enum(schema.UI_SCOPES, scope),
            "worker_mask": worker_mask, "reserved": reserved,
        }
    if record_type == 13:
        values = _unpack("<8I", payload)
        names = (
            "queue_depth", "queue_highwater", "dropped_records_cumulative", "writer_errors_cumulative",
            "rotation_count", "sampling_late_count", "buffered_bytes", "reserved",
        )
        return dict(zip(names, values))
    if record_type == 14:
        kind, outcome, _, value0, value1 = _unpack("<BBHIQ", payload)
        return {
            "kind_id": kind, "kind": _enum(schema.SESSION_EVENT_KINDS, kind),
            "outcome_id": outcome, "outcome": _enum(schema.OUTCOMES, outcome),
            "value0": value0, "value1": value1,
        }
    raise MftDecodeError("unknown record type {}".format(record_type))


def _state_name(state_kind, value):
    if state_kind == 1:
        return _enum(schema.SCREEN_IDS, value)
    if state_kind == 2:
        return _enum(schema.TAB_IDS, value)
    if state_kind == 3:
        return _enum(schema.ACTION_IDS, value)
    if state_kind == 4:
        return _enum(schema.PLAYBACK_STATES, value)
    return "Unknown({})".format(value)


def decode_bytes(data):
    header = _header(data)
    records = []
    warnings = []
    offset = schema.FILE_HEADER_SIZE
    while offset < len(data):
        remaining = len(data) - offset
        if remaining < schema.RECORD_HEADER_SIZE:
            warnings.append("partial record header ignored")
            break
        record_type, record_size, sequence, monotonic = struct.unpack_from(
            schema.RECORD_HEADER_FORMAT, data, offset
        )
        if record_size < schema.RECORD_HEADER_SIZE or record_size > schema.MAX_RECORD_SIZE:
            warnings.append("impossible record size at offset {}".format(offset))
            break
        end = offset + record_size
        if end > len(data):
            warnings.append("partial record tail ignored")
            break
        definition = schema.RECORD_TYPES.get(record_type)
        if definition is not None:
            name, expected_size = definition
            if record_size != expected_size:
                warnings.append("invalid {} size at offset {}".format(name, offset))
                break
            payload = data[offset + schema.RECORD_HEADER_SIZE:end]
            records.append({
                "record_type_id": record_type,
                "record_type": name,
                "record_size": record_size,
                "sequence": sequence,
                "monotonic_us": monotonic,
                "payload": _decode_payload(record_type, payload),
            })
        else:
            warnings.append("unknown record type {} skipped".format(record_type))
        offset = end
    return {"header": header, "records": records, "warnings": warnings}


def decode_file(path):
    return decode_bytes(Path(path).read_bytes())


def write_json(decoded, path):
    Path(path).write_text(json.dumps(decoded, indent=2, sort_keys=True) + "\n")


def write_csv_directory(decoded, directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "header.json").write_text(json.dumps(decoded["header"], indent=2, sort_keys=True) + "\n")
    by_type = {}
    for record in decoded["records"]:
        by_type.setdefault(record["record_type"], []).append(record)
    for record_type, records in by_type.items():
        rows = []
        fieldnames = ["record_type_id", "record_type", "record_size", "sequence", "monotonic_us"]
        for record in records:
            row = {key: value for key, value in record.items() if key != "payload"}
            row.update(record["payload"])
            for key in row:
                if key not in fieldnames:
                    fieldnames.append(key)
            rows.append(row)
        with (directory / (record_type + ".csv")).open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=fieldnames, extrasaction="ignore")
            writer.writeheader()
            writer.writerows(rows)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--json", dest="json_path", type=Path)
    parser.add_argument("--csv-dir", dest="csv_directory", type=Path)
    args = parser.parse_args(argv)
    decoded = decode_file(args.input)
    if args.json_path:
        write_json(decoded, args.json_path)
    if args.csv_directory:
        write_csv_directory(decoded, args.csv_directory)
    if not args.json_path and not args.csv_directory:
        print(json.dumps(decoded, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
