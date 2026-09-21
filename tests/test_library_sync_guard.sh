#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/src/library" "$tmp/tests"
cat > "$tmp/src/library/Allowed.cpp" <<'EOF'
#include "LibrarySync.hpp"
#include <library/LibrarySync.hpp>
class LibrarySync;
LibrarySync *pointer;
LibrarySync &reference = *pointer;
std::shared_ptr<LibrarySync> member;
library::LibrarySync::LibrarySync(Session session, std::shared_ptr<CatalogDb> db,
                                   std::uint64_t epoch) {}
library::LibrarySync makeLibrarySync(Session session);
const char *text = "std::make_shared<LibrarySync>(session, db, epoch)";
LibrarySyncSchedule schedule;
EOF
cat > "$tmp/src/library/LibraryCoordinator.cpp" <<'EOF'
auto sync = std::make_shared<LibrarySync>(session, db, epoch);
library::LibrarySync(session, db, epoch);
auto placement = new (storage) LibrarySync(session, db, epoch);
EOF
cat > "$tmp/tests/AllowedTest.cpp" <<'EOF'
auto sync = std::make_shared<LibrarySync>(session, db, epoch);
LibrarySync local(session, db, epoch);
library::LibrarySync(session, db, epoch);
auto placement = new (storage) LibrarySync(session, db, epoch);
EOF

sh "$root/tools/check-library-sync-construction.sh" "$tmp"

cat > "$tmp/src/library/Forbidden.cpp" <<'EOF'
auto shared = std::make_shared<LibrarySync>(session, db, epoch);
auto unique = std::make_unique<library::LibrarySync>(session, db, epoch);
auto allocated = new LibrarySync(session, db, epoch);
library::LibrarySync global(session, db, epoch);
void forbidden() {
library::LibrarySync local(session, db, epoch);
library::LibrarySync braced{session, db, epoch};
auto value = LibrarySync(session, db, epoch);
library::LibrarySync(session, db, epoch);
auto placement = new (storage) LibrarySync(session, db, epoch);
}
EOF

if sh "$root/tools/check-library-sync-construction.sh" "$tmp" > "$tmp/diagnostics" 2>&1; then
    echo "LibrarySync guard accepted forbidden construction" >&2
    exit 1
fi
grep -q 'Forbidden.cpp' "$tmp/diagnostics"
grep -q 'LibrarySync construction guard violation' "$tmp/diagnostics"
echo 'LibrarySync construction guard tests passed'
