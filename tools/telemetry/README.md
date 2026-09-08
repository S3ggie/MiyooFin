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
