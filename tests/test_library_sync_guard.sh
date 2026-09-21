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

mkdir -p "$tmp/src/ui" "$tmp/src/app"
cat > "$tmp/src/ui/Allowed.cpp" <<'EOF'
// #include "../../library/LibrarySync.hpp"
LibrarySyncSchedule schedule;
library::LibraryCoordinator *coordinator;
EOF
cat > "$tmp/src/app/Allowed.cpp" <<'EOF'
#include "../../library/LibraryCoordinator.hpp"
LibrarySyncSchedule schedule;
library::LibraryCoordinator *coordinator;
EOF
cat > "$tmp/src/library/Coordinator.cpp" <<'EOF'
#include "LibrarySync.hpp"
EOF
cat > "$tmp/tests/AllowedTest.cpp" <<'EOF'
#include "../src/library/LibrarySync.hpp"
EOF
sh "$root/tools/check-library-sync-ui-boundary.sh" "$tmp"

cat > "$tmp/src/ui/Forbidden.cpp" <<'EOF'
#include "../../library/LibrarySync.hpp"
EOF
if sh "$root/tools/check-library-sync-ui-boundary.sh" "$tmp" > "$tmp/ui-diagnostics" 2>&1; then
    echo "LibrarySync UI boundary guard accepted forbidden include" >&2
    exit 1
fi
grep -q 'Forbidden.cpp' "$tmp/ui-diagnostics"
grep -q 'LibrarySync UI/application boundary violation' "$tmp/ui-diagnostics"

cat > "$tmp/src/app/Forbidden.cpp" <<'EOF'
#include "../../library/LibrarySync.hpp"
EOF
if sh "$root/tools/check-library-sync-ui-boundary.sh" "$tmp" > "$tmp/app-ui-diagnostics" 2>&1; then
    echo "LibrarySync UI/application boundary guard accepted forbidden app include" >&2
    exit 1
fi
grep -q 'Forbidden.cpp' "$tmp/app-ui-diagnostics"
grep -q 'LibrarySync UI/application boundary violation' "$tmp/app-ui-diagnostics"
echo 'LibrarySync construction and UI boundary guard tests passed'
