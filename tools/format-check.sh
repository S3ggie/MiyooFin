#!/bin/sh
set -eu

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
repo_root=$(CDPATH= cd "$script_dir/.." && pwd)
cd "$repo_root"

# The formatting boundary is first-party implementation and test code plus
# the small standalone tools. Vendored/imported/generated code and shell/
# python tooling remain outside this check. git ls-files keeps the source set
# explicit and prevents untracked build output from being scanned. Keep the
# intermediate list NUL-delimited so tracked paths remain safe even if they
# contain whitespace.
source_list=$(mktemp "${TMPDIR:-/tmp}/miyoofin-format.XXXXXX")
trap 'rm -f "$source_list"' EXIT HUP INT TERM
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
' >"$source_list"

if [ ! -s "$source_list" ]; then
    echo "format check failed: no first-party C/C++ sources found"
    exit 1
fi

clang_format=${CLANG_FORMAT:-clang-format}
if command -v "$clang_format" >/dev/null 2>&1; then
    python3 - "$source_list" "$clang_format" <<'PY'
import os
import subprocess
import sys

with open(sys.argv[1], "rb") as handle:
    sources = [os.fsdecode(path) for path in handle.read().split(b"\0") if path]

for source in sources:
    subprocess.run(
        [sys.argv[2], "--dry-run", "--Werror", "--style=file", source],
        check=True,
    )
PY
    echo "clang-format check passed"
    exit 0
fi

# Minimal CI images may not include clang-format.  In that case, check only
# the narrow class of regressions this guard owns: multiple statements on one
# physical line or a non-empty braced control body on one line.  The scanner
# masks C/C++ comments, quoted strings, character literals, and raw strings
# first, so prose and string data cannot trigger the fallback.  Single
# statement bodies and loop-header semicolons are intentionally outside this
# fallback's scope; clang-format is the authoritative check when installed.
python3 - "$source_list" <<'PY'
import re
import os
import sys


def mask_non_code(text):
    masked = list(text)
    i = 0
    size = len(text)
    while i < size:
        if text.startswith("//", i):
            end = text.find("\n", i)
            if end < 0:
                end = size
            for position in range(i, end):
                masked[position] = " "
            i = end
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = size if end < 0 else end + 2
            for position in range(i, end):
                if text[position] != "\n":
                    masked[position] = " "
            i = end
        elif text[i] in ('"', "'"):
            quote = text[i]
            i += 1
            while i < size:
                if text[i] == "\\":
                    masked[i] = " "
                    if i + 1 < size and text[i + 1] != "\n":
                        masked[i + 1] = " "
                    i += 2
                else:
                    escaped = text[i] == quote
                    masked[i] = " "
                    i += 1
                    if escaped:
                        break
        elif text.startswith("R\"", i):
            delimiter_end = text.find("(", i + 2)
            if delimiter_end < 0:
                i += 2
                continue
            delimiter = text[i + 2:delimiter_end]
            terminator = ")" + delimiter + '"'
            end = text.find(terminator, delimiter_end + 1)
            end = size if end < 0 else end + len(terminator)
            for position in range(i, end):
                if text[position] != "\n":
                    masked[position] = " "
            i = end
        else:
            i += 1
    return "".join(masked)


def matching_paren(line, opening):
    depth = 0
    for position in range(opening, len(line)):
        if line[position] == "(":
            depth += 1
        elif line[position] == ")":
            depth -= 1
            if depth == 0:
                return position
    return None


def matching_brace(line, opening):
    depth = 0
    for position in range(opening, len(line)):
        if line[position] == "{":
            depth += 1
        elif line[position] == "}":
            depth -= 1
            if depth == 0:
                return position
    return None


def has_inline_braced_control(line):
    control = re.compile(r"\b(if|else|for|while|switch)\b")
    for match in control.finditer(line):
        keyword = match.group(1)
        if keyword == "else":
            after = line[match.end():].lstrip()
        else:
            opening = line.find("(", match.end())
            if opening < 0:
                continue
            closing = matching_paren(line, opening)
            if closing is None:
                continue
            after = line[closing + 1:].lstrip()
        if after.startswith("{") and "}" in after[1:]:
            body = after[1:after.find("}", 1)].strip()
            if body:
                return True
    return False


def remove_control_headers(line):
    masked = list(line)
    control = re.compile(r"\b(for|while|if|switch)\b")
    for match in control.finditer(line):
        opening = line.find("(", match.end())
        if opening < 0:
            continue
        closing = matching_paren(line, opening)
        if closing is None:
            continue
        for position in range(match.start(), closing + 1):
            masked[position] = " "
    return "".join(masked)


def remove_single_statement_lambdas(line):
    masked = list(line)
    lambda_start = re.compile(
        r"\[[^]]*\]\s*(?:\([^)]*\))?\s*(?:mutable\s*)?\{"
    )
    for match in lambda_start.finditer(line):
        opening = line.rfind("{", match.start(), match.end())
        closing = matching_brace(line, opening)
        if closing is None:
            continue
        body = line[opening + 1:closing]
        if ";" in body and body.count(";") == 1 and not re.search(
            r"\b(if|else|for|while|switch)\b", body
        ):
            for position in range(opening, closing + 1):
                masked[position] = " "
    return "".join(masked)


with open(sys.argv[1], "rb") as handle:
    sources = [os.fsdecode(path) for path in handle.read().split(b"\0") if path]
violations = []
for source in sources:
    with open(source, encoding="utf-8") as handle:
        masked_lines = mask_non_code(handle.read()).splitlines()
    for number, line in enumerate(masked_lines, 1):
        if has_inline_braced_control(line):
            violations.append(f"{source}:{number}: inline braced control flow")
            continue
        # clang-format may wrap a loop header before its closing parenthesis.
        # Its semicolons are still header syntax, not multiple statements.
        incomplete_control = re.search(r"\b(for|while|if|switch)\s*\(", line)
        if incomplete_control:
            opening = line.find("(", incomplete_control.start())
            if matching_paren(line, opening) is None:
                continue
        if line.count(";") >= 2:
            # A for-loop header has two semicolons by definition.  Removing
            # its parenthesized header avoids treating that syntax as a body.
            without_headers = remove_single_statement_lambdas(
                remove_control_headers(line)
            )
            if without_headers.count(";") >= 2:
                violations.append(f"{source}:{number}: multiple statements")

if violations:
    print("format fallback failed:")
    print("\n".join(violations))
    raise SystemExit(1)

print("SKIP: clang-format is not installed; narrow format fallback passed")
PY
