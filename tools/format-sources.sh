#!/bin/sh
# Prints the NUL-delimited first-party C/C++ source list that formatting may touch.
# Single source of truth for format-check.sh and format.sh: vendored, imported and
# generated code (third_party, vendor, player/, ...) is outside the roots or excluded.
set -eu
script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
cd "$script_dir/.."
git ls-files -z -- 'src/**' 'include/**' 'tools/**' 'tests/**' | python3 -c '
import os
import sys

extensions = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
roots = ("src", "include", "tools", "tests")
excluded_directories = {
    "external",
    "generated",
    "imported",
    "third-party",
    "third_party",
    "vendor",
}
excluded_files = {
    "src/image/stb_image_impl.cpp",
}
for raw_path in sys.stdin.buffer.read().split(b"\0"):
    if not raw_path:
        continue
    path = os.fsdecode(raw_path)
    parts = path.split("/")
    if parts[0] not in roots or not any(path.endswith(extension) for extension in extensions):
        continue
    if path in excluded_files:
        continue
    if any(part.casefold() in excluded_directories for part in parts[1:]):
        continue
    sys.stdout.buffer.write(raw_path + b"\0")
'
