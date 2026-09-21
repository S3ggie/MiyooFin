#!/bin/sh
set -eu

root=${1:-.}

python3 - "$root" <<'PY'
import bisect
import os
import re
import sys


root = os.path.abspath(sys.argv[1])
source_root = os.path.join(root, "src")


def mask_cpp(source):
    """Remove comments and literals while preserving offsets and newlines."""
    masked = list(source)
    i = 0
    length = len(source)
    while i < length:
        if source.startswith("//", i):
            end = source.find("\n", i)
            if end == -1:
                end = length
            for pos in range(i, end):
                masked[pos] = " "
            i = end
            continue
        if source.startswith("/*", i):
            end = source.find("*/", i + 2)
            end = length if end == -1 else end + 2
            for pos in range(i, end):
                if source[pos] != "\n":
                    masked[pos] = " "
            i = end
            continue
        if source[i] in ('"', "'"):
            quote = source[i]
            i += 1
            while i < length:
                if source[i] == "\\":
                    masked[i] = " "
                    if i + 1 < length and source[i + 1] != "\n":
                        masked[i + 1] = " "
                    i += 2
                    continue
                closing = source[i] == quote
                if source[i] != "\n":
                    masked[i] = " "
                i += 1
                if closing:
                    break
            continue
        if source[i] == "R" and i + 1 < length and source[i + 1] == '"':
            delimiter_end = source.find("(", i + 2)
            if delimiter_end != -1:
                delimiter = source[i + 2:delimiter_end]
                closing = ")" + delimiter + '"'
                end = source.find(closing, delimiter_end + 1)
                end = length if end == -1 else end + len(closing)
                for pos in range(i, end):
                    if source[pos] != "\n":
                        masked[pos] = " "
                i = end
                continue
        i += 1
    return "".join(masked)


qualified_sync = r"(?<![:\w])(?:(?:::)?[A-Za-z_]\w*\s*::\s*)*LibrarySync"
patterns = (
    ("allocator construction", re.compile(
        r"\bmake_(?:shared|unique)\s*<\s*" + qualified_sync + r"\s*>")),
    ("new-expression construction", re.compile(
        r"\bnew\s+(?:(?:\([^;]*\)\s*)+)?" + qualified_sync + r"\b")),
    ("direct local/value construction", re.compile(
        r"^[ \t]*(?:(?:const|volatile|static|thread_local|mutable)\s+)*"
        + qualified_sync
        + r"\s+[A-Za-z_]\w*\s*(?P<initializer>[({=])", re.MULTILINE)),
    ("direct value expression", re.compile(
        r"(?:\breturn\b|\b(?:auto|[A-Za-z_]\w*)\s*=|[({,:=])\s*"
        + qualified_sync
        + r"\s*(?=[({])")),
    ("standalone temporary construction", re.compile(
        r"^[ \t]*" + qualified_sync + r"\s*(?=[({])", re.MULTILINE)),
)


def function_body_depth(source, offset):
    """Approximate whether an offset is inside a function body.

    This keeps a function declaration such as `LibrarySync makeSync(...)`
    from being mistaken for a local object declaration, while still checking
    the same syntax inside an actual function body.
    """
    stack = []
    for position, character in enumerate(source[:offset]):
        if character == "{":
            prefix = source[max(0, position - 512):position].rstrip()
            tail = prefix[prefix.rfind(";") + 1:]
            tail = tail[tail.rfind("{") + 1:]
            control = re.search(r"\b(?:if|for|while|switch|catch)\s*\(", tail)
            function_body = bool(
                re.search(r"\)\s*(?:const|volatile|noexcept(?:\s*\([^)]*\))?"
                          r"|override|final|&|&&|->[^{}]+)?\s*$", tail)
                and not control
            )
            stack.append(function_body)
        elif character == "}" and stack:
            stack.pop()
    return sum(stack)


def class_body_depth(source, offset):
    """Approximate whether an offset is inside a class/struct body."""
    stack = []
    for position, character in enumerate(source[:offset]):
        if character == "{":
            prefix = source[max(0, position - 512):position].rstrip()
            tail = prefix[prefix.rfind(";") + 1:]
            tail = tail[tail.rfind("{") + 1:]
            class_body = bool(re.search(r"\b(?:class|struct|union)\b[^{}]*$", tail))
            stack.append(class_body)
        elif character == "}" and stack:
            stack.pop()
    return sum(stack)


def looks_like_function_declaration(source, opening):
    """Distinguish a namespace-scope function declaration from an object."""
    depth = 0
    closing = None
    for position in range(opening, len(source)):
        if source[position] == "(":
            depth += 1
        elif source[position] == ")":
            depth -= 1
            if depth == 0:
                closing = position
                break
    if closing is None:
        return False

    suffix = source[closing + 1:].lstrip()
    if not re.match(r"(?:;|\{|const\b|volatile\b|noexcept\b|override\b|"
                    r"final\b|&|&&|->|requires\b)", suffix):
        return False
    parameters = source[opening + 1:closing].strip()
    if not parameters or parameters == "void":
        return True
    parameter_pattern = re.compile(
        r"^(?:(?:const|volatile|unsigned|signed|short|long)\s+)*"
        r"(?:(?:::)?[A-Za-z_]\w*(?:\s*::\s*[A-Za-z_]\w*)*)"
        r"(?:\s*<[^()]*>)?\s*[&*]*\s+[A-Za-z_]\w*"
        r"(?:\s*=.*)?$")
    parts = [part.strip() for part in parameters.split(",")]
    return all(parameter_pattern.match(part) for part in parts)


violations = []
if os.path.isdir(source_root):
    for directory, _, filenames in os.walk(source_root):
        for filename in filenames:
            if not filename.endswith((".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx")):
                continue
            path = os.path.join(directory, filename)
            relative = os.path.relpath(path, root)
            if relative == os.path.join("src", "library", "LibraryCoordinator.cpp"):
                continue
            with open(path, encoding="utf-8") as source_file:
                source = source_file.read()
            masked = mask_cpp(source)
            line_starts = [0]
            line_starts.extend(pos + 1 for pos, char in enumerate(source) if char == "\n")
            for description, pattern in patterns:
                for match in pattern.finditer(masked):
                    if (description == "standalone temporary construction"
                            and (re.search(r"LibrarySync\s*::\s*LibrarySync\s*$",
                                           match.group(0))
                                 or (class_body_depth(masked, match.start())
                                     and not function_body_depth(masked, match.start())))):
                        continue
                    if (description == "direct local/value construction"
                            and match.group("initializer") == "("
                            and function_body_depth(masked, match.start()) == 0
                            and looks_like_function_declaration(
                                masked, match.end() - 1)):
                        continue
                    line = bisect.bisect_right(line_starts, match.start())
                    original_line = source.splitlines()[line - 1] if source.splitlines() else ""
                    violations.append((relative, line, description, original_line.strip()))

if violations:
    print("LibrarySync construction guard violation:")
    for path, line, description, original_line in violations:
        print(f"{path}:{line}: {description}: {original_line}")
    sys.exit(1)
PY
