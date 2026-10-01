#!/bin/sh
# Enforce the module dependency table for first-party sources under src/.
#
# Checks DIRECT #include edges only (transitive use is not visible here).
# Quoted includes are resolved the way the compiler does: the includer's
# directory first, then the repo root, then include/. Comments are stripped so
# prose and commented-out includes never count.
#
# Usage:
#   tools/check-module-boundaries.sh [repo-root]            enforce, exit 1 on violation
#   tools/check-module-boundaries.sh --print-edges [root]   print "from -> to (count)"
#
# The table below is the single place that states what may depend on what.
# To add an edge, add it here with a reason; the check is what keeps the
# layering from eroding.
set -eu

mode=check
if [ "${1:-}" = "--print-edges" ]; then
    mode=edges
    shift
fi
script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
repo_root=${1:-$(CDPATH= cd "$script_dir/.." && pwd)}
cd "$repo_root"

git ls-files -z -- 'src/**' | MODE="$mode" python3 -c '
import os
import re
import sys
from collections import defaultdict

mode = os.environ["MODE"]
EXT = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}

# module -> modules it may include. Same-module includes are always allowed.
# "data" is a leaf that every module may use.
ALLOWED = {
    "data": set(),
    "input": set(),
    "image": {"diagnostics"},
    "diagnostics": set(),
    "net": {"diagnostics"},
    "cache": {"net", "diagnostics"},
    "catalog": {"diagnostics"},
    "download": {"net", "cache", "diagnostics", "library"},
    "library": {"catalog", "net", "cache", "diagnostics", "download"},
    "playback": {"cache", "download"},
    "update": {"net"},
    "ui": {"net", "library", "download", "cache", "playback", "update",
           "diagnostics", "image", "input", "app"},
    "app": {"ui", "net", "library", "download", "cache", "playback", "update",
            "diagnostics", "image", "input", "catalog"},
    "main": {"app", "update", "diagnostics"},
}
for allowed in ALLOWED.values():
    allowed.add("data")

# (from-module, to-file) edges that are allowed ONLY for that exact header.
FILE_ALLOWS = {
    ("diagnostics", "src/input/Action.hpp"): "diagnostics records input actions",
    ("catalog", "src/cache/LibraryCache.hpp"): "CatalogCompatibility legacy snapshot surface",
    ("download", "src/library/LibraryCoordinator.hpp"): "download planning uses the coordinator",
    ("download", "src/library/LibraryQuery.hpp"): "download planning uses the coordinator query",
    ("library", "src/download/DownloadStore.hpp"): "offline reconstruction reads the store",
    ("library", "src/download/DownloadTypes.hpp"): "offline query value types",
}

# (from-module, to-file) edges that are never allowed even if the module is.
FILE_FORBIDS = {
    ("ui", "src/library/LibrarySync.hpp"): "UI must go through LibraryCoordinator",
    ("app", "src/library/LibrarySync.hpp"): "App must go through LibraryCoordinator",
}

# Third-party / system headers that only certain modules may include.
SYSTEM_ONLY_IN = {
    "SDL": {"ui", "app", "input", "image", "main"},
    "curl/": {"net", "download"},
    "sqlite3": {"catalog"},
}

# (from-file, header prefix) exceptions to SYSTEM_ONLY_IN / the sqlite rule.
SYSTEM_FILE_ALLOWS = {
    ("src/app/App.cpp", "curl/"): "process-wide curl_global_init/cleanup",
    ("src/main.cpp", "vendor/sqlite"): "sets sqlite3_temp_directory before any DB opens",
}

INCLUDE = re.compile(r"^\s*#\s*include\s*([\"<])([^\">]+)[\">]")


def strip_comments(text):
    out = []
    in_block = False
    for line in text.splitlines():
        buf = []
        i = 0
        while i < len(line):
            if in_block:
                end = line.find("*/", i)
                if end < 0:
                    i = len(line)
                else:
                    in_block = False
                    i = end + 2
            elif line.startswith("/*", i):
                in_block = True
                i += 2
            elif line.startswith("//", i):
                break
            else:
                buf.append(line[i])
                i += 1
        out.append("".join(buf))
    return out


def module_of(path):
    parts = path.split("/")
    if parts[0] != "src":
        return None
    if len(parts) == 2:
        return "main"
    return parts[1]


files = [f for f in sys.stdin.read().split("\0")
         if f and os.path.splitext(f)[1] in EXT]
edges = defaultdict(int)
violations = []

for path in files:
    src_mod = module_of(path)
    if src_mod is None:
        continue
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = strip_comments(fh.read())
    for lineno, line in enumerate(lines, 1):
        m = INCLUDE.match(line)
        if not m:
            continue
        kind, target = m.groups()
        if kind == "<":
            for prefix, mods in SYSTEM_ONLY_IN.items():
                if (target.startswith(prefix) and src_mod not in mods
                        and (path, prefix) not in SYSTEM_FILE_ALLOWS):
                    violations.append((path, lineno, "<%s>" % target,
                                       "%s may not include %s" % (src_mod, prefix)))
            continue
        candidates = [
            os.path.normpath(os.path.join(os.path.dirname(path), target)),
            os.path.normpath(target),
            os.path.normpath(os.path.join("include", target)),
        ]
        resolved = next((c for c in candidates if os.path.exists(c)), None)
        if resolved is None:
            violations.append((path, lineno, target, "unresolved include"))
            continue
        if (resolved.startswith("vendor/sqlite") and src_mod != "catalog"
                and (path, "vendor/sqlite") not in SYSTEM_FILE_ALLOWS):
            violations.append((path, lineno, resolved, "sqlite is catalog-only"))
        dst_mod = module_of(resolved)
        if dst_mod is None or dst_mod == src_mod:
            continue
        edges[(src_mod, dst_mod)] += 1
        if (src_mod, resolved) in FILE_FORBIDS:
            violations.append((path, lineno, resolved,
                               FILE_FORBIDS[(src_mod, resolved)]))
        elif dst_mod not in ALLOWED[src_mod] and (src_mod, resolved) not in FILE_ALLOWS:
            violations.append((path, lineno, resolved,
                               "%s must not depend on %s" % (src_mod, dst_mod)))

if mode == "edges":
    for (a, b), n in sorted(edges.items()):
        print("%s -> %s (%d)" % (a, b, n))
    sys.exit(0)

if violations:
    print("module boundary violations:")
    for path, lineno, target, why in violations:
        print("  %s:%d includes %s: %s" % (path, lineno, target, why))
    sys.exit(1)
print("module boundaries OK (%d files, %d module edges)" % (len(files), len(edges)))
'
