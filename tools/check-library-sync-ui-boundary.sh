#!/bin/sh
set -eu

root=${1:-.}

python3 - "$root" <<'PY'
import os
import re
import sys


root = os.path.abspath(sys.argv[1])
production_roots = tuple(
    os.path.join(root, "src", directory) for directory in ("ui", "app"))
include = re.compile(
    r'^\s*#\s*include\s*[<"][^">]*LibrarySync\.hpp[">]')


def without_comments(source):
    result = []
    index = 0
    in_block = False
    while index < len(source):
        if in_block:
            if source.startswith("*/", index):
                result.extend("  ")
                index += 2
                in_block = False
            else:
                result.append("\n" if source[index] == "\n" else " ")
                index += 1
        elif source.startswith("//", index):
            result.extend("  ")
            index += 2
            while index < len(source) and source[index] != "\n":
                result.append(" ")
                index += 1
        elif source.startswith("/*", index):
            result.extend("  ")
            index += 2
            in_block = True
        else:
            result.append(source[index])
            index += 1
    return "".join(result)


violations = []
for production_root in production_roots:
    if not os.path.isdir(production_root):
        continue
    for directory, _, filenames in os.walk(production_root):
        for filename in filenames:
            if not filename.endswith((".c", ".cc", ".cpp", ".cxx",
                                      ".h", ".hh", ".hpp", ".hxx")):
                continue
            path = os.path.join(directory, filename)
            with open(path, encoding="utf-8") as source_file:
                source = source_file.read()
            for line_number, line in enumerate(
                    without_comments(source).splitlines(), 1):
                if include.search(line):
                    violations.append((os.path.relpath(path, root),
                                       line_number, line.strip()))

if violations:
    print("LibrarySync UI/application boundary violation:")
    for path, line, text in violations:
        print(f"{path}:{line}: production UI/application includes "
              f"LibrarySync.hpp: {text}")
    sys.exit(1)
PY
