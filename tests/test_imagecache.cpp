#include "test_support.hpp"

// Verify the pure victim-selection logic for the ImageCache LRU janitor.
// No filesystem access — just in-memory JanitorFileEntry vectors.

static void testSelectCacheVictimsEmpty()
{
    std::vector<ImageCache::JanitorFileEntry> entries;
    auto victims = ImageCache::selectCacheVictims(entries, 1000);
    CHECK(victims.empty());
}

static void testSelectCacheVictimsUnderCap()
{
    std::vector<ImageCache::JanitorFileEntry> entries = {
        {"a.jpg", 1000, 10},
        {"b.jpg", 2000, 20},
        {"c.jpg", 3000, 30},
    };
    // Total = 6000; target = 6000 → nothing to evict.
    auto victims = ImageCache::selectCacheVictims(entries, 6000);
    CHECK(victims.empty());
}

static void testSelectCacheVictimsOldestFirst()
{
    std::vector<ImageCache::JanitorFileEntry> entries = {
        {"new.jpg",  5000, 100},  // newest
        {"old.jpg",  3000, 10},   // oldest
        {"mid.jpg",  4000, 50},   // middle
    };
    // Total = 12000; target = 6000 → need to evict at least 6000 bytes.
    // Oldest first: old.jpg (3000) then mid.jpg (4000) → 7000 removed, remaining 5000.
    auto victims = ImageCache::selectCacheVictims(entries, 6000);
    CHECK(victims.size() == 2);
    // Verify oldest-first order.
    CHECK(entries[victims[0]].path == "old.jpg");
    CHECK(entries[victims[1]].path == "mid.jpg");
}

static void testSelectCacheVictimsExactEviction()
{
    std::vector<ImageCache::JanitorFileEntry> entries = {
        {"a.jpg", 4000, 10},
        {"b.jpg", 4000, 20},
        {"c.jpg", 4000, 30},
    };
    // Total = 12000; target = 8000 → evict exactly 4000.
    auto victims = ImageCache::selectCacheVictims(entries, 8000);
    CHECK(victims.size() == 1);
    CHECK(entries[victims[0]].path == "a.jpg");
}

static void testSelectCacheVictimsAllEvicted()
{
    std::vector<ImageCache::JanitorFileEntry> entries = {
        {"a.jpg", 10000, 10},
        {"b.jpg", 10000, 20},
    };
    // Total = 20000; target = 0 → evict everything.
    auto victims = ImageCache::selectCacheVictims(entries, 0);
    CHECK(victims.size() == 2);
    CHECK(entries[victims[0]].path == "a.jpg");
    CHECK(entries[victims[1]].path == "b.jpg");
}

static void testSelectCacheVictimsMtimeTie()
{
    std::vector<ImageCache::JanitorFileEntry> entries = {
        {"first.jpg", 1000, 50},   // same mtime as others
        {"second.jpg", 2000, 50},  // same mtime as others
        {"third.jpg", 3000, 50},   // same mtime as others
    };
    // All same mtime → stable_sort preserves input order.
    // Total = 6000; target = 2000 → need to evict 4000+ bytes.
    // Evict first (1000), second (2000), third (3000) = 6000 removed, remaining 0.
    auto victims = ImageCache::selectCacheVictims(entries, 2000);
    CHECK(victims.size() == 3);
    CHECK(entries[victims[0]].path == "first.jpg");
    CHECK(entries[victims[1]].path == "second.jpg");
    CHECK(entries[victims[2]].path == "third.jpg");
}

static void testJanitorConstants()
{
    // Verify cap constant is 128MB.
    CHECK(ImageCache::kMaxImageCacheBytes == 128LL * 1024 * 1024);
    // Verify hysteresis ratio is 90%.
    CHECK(ImageCache::kImageCacheHysteresisRatio > 0.89);
    CHECK(ImageCache::kImageCacheHysteresisRatio < 0.91);
    // Verify stale threshold is reasonable (1-30 seconds).
    CHECK(ImageCache::kImageCacheStaleSec >= 1);
    CHECK(ImageCache::kImageCacheStaleSec <= 30);
}

int main()
{
    testSelectCacheVictimsEmpty();
    testSelectCacheVictimsUnderCap();
    testSelectCacheVictimsOldestFirst();
    testSelectCacheVictimsExactEviction();
    testSelectCacheVictimsAllEvicted();
    testSelectCacheVictimsMtimeTie();
    testJanitorConstants();
    return miyoofin_test::finish("imagecache");
}
