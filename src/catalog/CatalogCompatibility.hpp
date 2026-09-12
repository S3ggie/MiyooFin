#ifndef MIYOOFIN_CATALOG_COMPATIBILITY_HPP
#define MIYOOFIN_CATALOG_COMPATIBILITY_HPP

// Compatibility-only declarations for the legacy LibrarySnapshot bridge.
// This header owns the legacy type dependency; normal catalog consumers use
// bounded page/query APIs instead.
#include "../cache/LibraryCache.hpp"

namespace miyoofin { class CatalogDb; }

#endif
