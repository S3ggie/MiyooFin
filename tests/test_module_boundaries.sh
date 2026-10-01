#!/bin/sh
# Fixture tests for tools/check-module-boundaries.sh: a forbidden edge fails,
# commented-out includes and allowed edges pass, per-file exceptions are exact,
# and system/sqlite header rules hold. Uses throwaway git repos only.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
CHECK="$ROOT/tools/check-module-boundaries.sh"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/miyoofin-boundaries.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

fail() { echo "module-boundaries test failed: $*" >&2; exit 1; }

# new_repo NAME  -> creates $TMP/NAME with the headers every case needs.
new_repo() {
    dir="$TMP/$1"
    mkdir -p "$dir/src/data" "$dir/src/net" "$dir/src/ui" "$dir/src/input" \
        "$dir/src/diagnostics" "$dir/src/library" "$dir/vendor/sqlite"
    : >"$dir/src/data/V.hpp"
    : >"$dir/src/net/N.hpp"
    : >"$dir/src/ui/U.hpp"
    : >"$dir/src/input/Action.hpp"
    : >"$dir/src/library/LibrarySync.hpp"
    : >"$dir/vendor/sqlite/sqlite3.h"
    echo "$dir"
}

# run_check DIR -> exit status of the checker over the repo at DIR.
run_check() {
    (cd "$1" && git init -q . && git add -A . && sh "$CHECK" "$1" >"$1/out.txt" 2>&1)
}

expect_ok() { run_check "$1" || { cat "$1/out.txt" >&2; fail "$2"; }; }
expect_fail() {
    if run_check "$1"; then fail "$2"; fi
    grep -q "$3" "$1/out.txt" || { cat "$1/out.txt" >&2; fail "$2 (wrong message)"; }
}

# allowed edge: net -> data
d=$(new_repo allowed)
printf '#include "../data/V.hpp"\n' >"$d/src/net/A.cpp"
expect_ok "$d" "allowed net -> data edge was rejected"

# forbidden edge: net -> ui
d=$(new_repo forbidden)
printf '#include "../ui/U.hpp"\n' >"$d/src/net/A.cpp"
expect_fail "$d" "forbidden net -> ui edge was accepted" "net must not depend on ui"

# a forbidden include inside comments never counts
d=$(new_repo comments)
printf '// #include "../ui/U.hpp"\n/* #include "../ui/U.hpp"\n   #include "../ui/U.hpp" */\n#include "../data/V.hpp"\n' >"$d/src/net/A.cpp"
expect_ok "$d" "commented-out includes were counted"

# per-file exception is exact: Action.hpp is allowed for diagnostics only
d=$(new_repo fileallow)
printf '#include "../input/Action.hpp"\n' >"$d/src/diagnostics/D.cpp"
expect_ok "$d" "diagnostics -> input/Action.hpp exception was rejected"
printf '#include "../input/Other.hpp"\n' >"$d/src/diagnostics/E.cpp"
: >"$d/src/input/Other.hpp"
expect_fail "$d" "diagnostics -> other input header was accepted" "diagnostics must not depend on input"

# LibrarySync is forbidden for ui even though ui may use library
d=$(new_repo forbids)
printf '#include "../library/LibrarySync.hpp"\n' >"$d/src/ui/S.cpp"
expect_fail "$d" "ui -> LibrarySync was accepted" "UI must go through LibraryCoordinator"

# system headers: SDL only in ui/app/input/image/main; curl only net/download
d=$(new_repo system)
printf '#include <SDL2/SDL.h>\n' >"$d/src/ui/R.cpp"
expect_ok "$d" "SDL in ui was rejected"
printf '#include <SDL2/SDL.h>\n' >"$d/src/net/R.cpp"
expect_fail "$d" "SDL in net was accepted" "net may not include SDL"
d=$(new_repo curl)
printf '#include <curl/curl.h>\n' >"$d/src/ui/C.cpp"
expect_fail "$d" "curl in ui was accepted" "ui may not include curl/"

# sqlite is catalog-only
d=$(new_repo sqlite)
printf '#include "../../vendor/sqlite/sqlite3.h"\n' >"$d/src/ui/Q.cpp"
expect_fail "$d" "sqlite in ui was accepted" "sqlite is catalog-only"

# unresolved includes are reported, not ignored
d=$(new_repo unresolved)
printf '#include "../net/Missing.hpp"\n' >"$d/src/ui/M.cpp"
expect_fail "$d" "unresolved include was ignored" "unresolved include"

# the real tree must pass
sh "$CHECK" "$ROOT" >/dev/null || fail "the repository itself violates the module table"

echo "module boundaries checker tests passed"
