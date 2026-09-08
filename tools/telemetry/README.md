# MFT v1 desktop decoder

`decode.py` is an independent standard-library decoder for the fixed-layout MFT v1
trace format. It validates the file header, decodes known records, skips unknown
records with valid sizes, stops safely on impossible sizes, and ignores a partial
tail after preserving complete records.

Examples:

```sh
python3 tools/telemetry/decode.py trace.mft --json trace.json
python3 tools/telemetry/decode.py trace.mft --csv-dir trace-csv
```

The CSV export writes one file per record type plus `header.json`. No third-party
Python package is required.

`analyze.py` builds desktop summaries from the decoded monotonic timeline. It derives
one-core CPU percentages, optional whole-device CPU share, I/O rates, memory and
free-space summaries, frame stalls/histograms, worker/request/artwork/download/
playback/health summaries, and correlations between slow timestamps and current
state or overlapping work. Its core uses only the Python standard library.

Examples:

```sh
python3 tools/telemetry/analyze.py trace.mft --json trace-summary.json
python3 tools/telemetry/analyze.py trace.mft --csv-dir trace-analysis --cpu-count 4
```

Plots are optional and only attempted when explicitly requested with `--plot-dir`;
matplotlib is never required for decoding or analysis.
