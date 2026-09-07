static void testBuildImageUrlPrimary()
{
    std::printf("[test] buildImageUrl — Primary\n");
    std::string url = buildImageUrl(
        "https://jellyfin.example.com", "abc123",
        ImageType::Primary, "tag42", 300, 200);
    CHECK(url.find("/Items/abc123/Images/Primary") != std::string::npos);
    CHECK(url.find("format=jpg") != std::string::npos);
    CHECK(url.find("maxWidth=300") != std::string::npos);
    CHECK(url.find("maxHeight=200") != std::string::npos);
    CHECK(url.find("quality=80") != std::string::npos);
    CHECK(url.find("tag=tag42") != std::string::npos);
    std::printf("[test] buildImageUrl Primary OK\n");
}

// -------------------------------------------------------------------
// B5a Test: buildImageUrl — Thumb image URL
// -------------------------------------------------------------------
static void testBuildImageUrlThumb()
{
    std::printf("[test] buildImageUrl — Thumb\n");
    std::string url = buildImageUrl(
        "http://192.168.1.50:8096", "item999",
        ImageType::Thumb, "thumbTag", 150, 100);
    CHECK(url.find("/Items/item999/Images/Thumb") != std::string::npos);
    CHECK(url.find("maxWidth=150") != std::string::npos);
    CHECK(url.find("tag=thumbTag") != std::string::npos);
    std::printf("[test] buildImageUrl Thumb OK\n");
}

// -------------------------------------------------------------------
// B5a Test: imageTypeToString and cache key differences
// -------------------------------------------------------------------
static void testImageTypeAndCacheKeys()
{
    std::printf("[test] imageTypeToString + cache keys differ\n");
    CHECK_EQ(std::string(imageTypeToString(ImageType::Primary)), "Primary");
    CHECK_EQ(std::string(imageTypeToString(ImageType::Thumb)), "Thumb");
    std::string p = ImageCache::cacheFilename("i", ImageType::Primary, "t", 100, 200);
    std::string t = ImageCache::cacheFilename("i", ImageType::Thumb, "t", 100, 200);
    CHECK(p != t);
    CHECK(p.find("_Primary_") != std::string::npos);
    CHECK(t.find("_Thumb_") != std::string::npos);
    std::printf("[test] imageTypeToString + cache keys OK\n");
}

// -------------------------------------------------------------------
// B5a Test: Cache filename determinism and contents
// -------------------------------------------------------------------
static void testCacheFilename()
{
    std::printf("[test] Cache filename\n");
    std::string a = ImageCache::cacheFilename("abc", ImageType::Primary, "tag", 100, 200);
    std::string b = ImageCache::cacheFilename("abc", ImageType::Primary, "tag", 100, 200);
    CHECK_EQ(a, b);
    CHECK(a.find("100x200") != std::string::npos);
    CHECK(a.find("abc") != std::string::npos);
    CHECK(a.find("tag") != std::string::npos);
    CHECK(a.size() >= 4 && a.substr(a.size()-4) == ".jpg");
    std::printf("[test] Cache filename OK\n");
}

// -------------------------------------------------------------------
// B5a Test: Cache write/read roundtrip
// -------------------------------------------------------------------
static void testCacheWriteRead()
{
    std::printf("[test] Cache write/read roundtrip\n");
    ImageCache::setCacheDir("test_cache_images/");
    const unsigned char data[] = {0x01, 0x02, 0x03, 0xFF, 0xFE};
    CHECK(ImageCache::writeToCache("item1", ImageType::Primary, "t", 50, 50, data, sizeof(data)));
    CHECK(ImageCache::isCached("item1", ImageType::Primary, "t", 50, 50));
    auto rb = ImageCache::readCached("item1", ImageType::Primary, "t", 50, 50);
    CHECK(rb.size() == sizeof(data));
    CHECK(std::memcmp(rb.data(), data, sizeof(data)) == 0);
    std::remove(ImageCache::cachePath("item1", ImageType::Primary, "t", 50, 50).c_str());
    ::rmdir("test_cache_images");
    ImageCache::setCacheDir("cache/images/");
    std::printf("[test] Cache write/read OK\n");
}

// -------------------------------------------------------------------
// B5a Test: JPEG decode valid fixture
// -------------------------------------------------------------------
static void testJpegDecodeValid()
{
    std::printf("[test] JPEG decode valid fixture\n");
    auto img = ImageDecoder::decodeJpegFile("tests/fixtures/tiny.jpg");
    CHECK(!img.empty());
    CHECK(img.width == 4);
    CHECK(img.height == 4);
    CHECK(img.pixels.size() == 4u * 4u * 4u);
    bool allZero = true;
    for (auto b : img.pixels) { if (b != 0) { allZero = false; break; } }
    CHECK(!allZero);
    std::printf("[test] JPEG decode valid OK\n");
}

// -------------------------------------------------------------------
// B5a Test: JPEG decode fails safely on invalid data
// -------------------------------------------------------------------
static void testJpegDecodeInvalid()
{
    std::printf("[test] JPEG decode invalid data\n");
    const unsigned char garbage[] = {0x00, 0x01, 0x02, 0x03};
    CHECK(ImageDecoder::decodeJpeg(garbage, sizeof(garbage)).empty());
    CHECK(ImageDecoder::decodeJpeg(nullptr, 0).empty());
    CHECK(ImageDecoder::decodeJpeg(nullptr, 10).empty());
    CHECK(ImageDecoder::decodeJpegFile("nonexistent.jpg").empty());
    std::printf("[test] JPEG decode invalid OK\n");
}

// -------------------------------------------------------------------
// B5a Test: BinaryHttpResponse defaults and ok()
// -------------------------------------------------------------------
static void testBinaryHttpResponse()
{
    std::printf("[test] BinaryHttpResponse\n");
    BinaryHttpResponse r;
    CHECK(r.status == 0);
    CHECK(r.data.empty());
    CHECK(!r.truncated);
    CHECK(!r.ok());
    r.status = 200; r.data = {0xFF, 0xD8};
    CHECK(r.ok());
    r.status = 404;
    CHECK(!r.ok());
    r.status = 200; r.truncated = true;
    CHECK(!r.ok());
    std::printf("[test] BinaryHttpResponse OK\n");
}

// ===================================================================
// B5b tests — Selected artwork loading logic
// ===================================================================

// B5b: Missing Primary tag → no artwork request expected
static void testNoPrimaryTagNoArtwork()
{
    std::printf("[test] B5b: no Primary tag\n");
    MediaItem item;
    item.id = "test-item-1";
    CHECK(item.imageTags.find("Primary") == item.imageTags.end());
    auto it = item.imageTags.find("Primary");
    CHECK(!(it != item.imageTags.end() && !it->second.empty()));
    std::printf("[test] B5b: no Primary tag OK\n");
}

// B5b: Primary tag present produces correct identity key
static void testArtworkIdentityKey()
{
    std::printf("[test] B5b: artwork identity key\n");
    MediaItem item;
    item.id = "movie-42";
    item.imageTags["Primary"] = "abc123";
    auto it = item.imageTags.find("Primary");
    CHECK(it != item.imageTags.end());
    std::string key = item.id + ":" + it->second;
    CHECK_EQ(key, "movie-42:abc123");

    MediaItem item2;
    item2.id = "movie-99";
    item2.imageTags["Primary"] = "abc123";
    std::string key2 = item2.id + ":" + item2.imageTags["Primary"];
    CHECK(key != key2);
    std::printf("[test] B5b: artwork identity key OK\n");
}

// B5b: Same selection does not re-trigger load
static void testArtworkLoadGuard()
{
    std::printf("[test] B5b: repeat selection guard\n");
    std::string loadedId;
    bool attempted = false;

    MediaItem item;
    item.id = "item-7";
    item.imageTags["Primary"] = "tag-aaa";
    std::string key = item.id + ":" + item.imageTags["Primary"];
    bool shouldLoad = !(attempted && loadedId == key);
    CHECK(shouldLoad);

    loadedId = key;
    attempted = true;
    shouldLoad = !(attempted && loadedId == key);
    CHECK(!shouldLoad);

    item.id = "item-8";
    key = item.id + ":" + item.imageTags["Primary"];
    shouldLoad = !(attempted && loadedId == key);
    CHECK(shouldLoad);
    std::printf("[test] B5b: repeat selection guard OK\n");
}

// B5b: Cached JPEG can be decoded
static void testCachedJpegDecodeRoundtrip()
{
    std::printf("[test] B5b: cached JPEG decode roundtrip\n");
    FILE *f = std::fopen("tests/fixtures/tiny.jpg", "rb");
    CHECK(f != nullptr);
    std::fseek(f, 0, SEEK_END);
    long fsize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> jpegBytes(static_cast<size_t>(fsize));
    std::fread(jpegBytes.data(), 1, jpegBytes.size(), f);
    std::fclose(f);

    ImageCache::setCacheDir("test_b5b_cache/");
    CHECK(ImageCache::writeToCache("b5b-test", ImageType::Primary,
        "roundtrip", 72, 98, jpegBytes.data(), jpegBytes.size()));
    CHECK(ImageCache::isCached("b5b-test", ImageType::Primary, "roundtrip", 72, 98));
    auto cached = ImageCache::readCached("b5b-test", ImageType::Primary, "roundtrip", 72, 98);
    CHECK(!cached.empty());
    auto decoded = ImageDecoder::decodeJpeg(cached.data(), cached.size());
    CHECK(!decoded.empty());

    std::remove("test_b5b_cache/b5b-test_Primary_roundtrip_72x98.jpg");
    ::rmdir("test_b5b_cache");
    ImageCache::setCacheDir("cache/images/");
    std::printf("[test] B5b: cached JPEG decode roundtrip OK\n");
}

// B5b: Failed/missing load leaves empty state
static void testFailedLoadLeavesEmpty()
{
    std::printf("[test] B5b: failed load leaves empty\n");
    MediaItem item;
    item.id = "item-bad";
    item.imageTags["Primary"] = "";
    auto it = item.imageTags.find("Primary");
    CHECK(!(it != item.imageTags.end() && !it->second.empty()));

    MediaItem item2;
    item2.id = "item-none";
    it = item2.imageTags.find("Primary");
    CHECK(!(it != item2.imageTags.end() && !it->second.empty()));

    const unsigned char garbage[] = {0xFF, 0x00, 0x00, 0x00};
    CHECK(ImageDecoder::decodeJpeg(garbage, sizeof(garbage)).empty());
    std::printf("[test] B5b: failed load leaves empty OK\n");
}

// B5b: Artwork URL uses correct dimensions (72×98)
static void testArtworkUrlDimensions()
{
    std::printf("[test] B5b: artwork URL dimensions\n");
    std::string url = buildImageUrl("http://server:8096", "item-1",
        ImageType::Primary, "tag-abc", 72, 98);
    CHECK(url.find("maxWidth=72") != std::string::npos);
    CHECK(url.find("maxHeight=98") != std::string::npos);
    CHECK(url.find("quality=80") != std::string::npos);
    CHECK(url.find("tag=tag-abc") != std::string::npos);
    std::printf("[test] B5b: artwork URL dimensions OK\n");
}
// B5c1: Movie artwork box is 64×96
static void testMovieArtworkBox()
{
    std::printf("[test] B5c1: movie artwork box = 64x96\n");
    MediaItem m;
    m.type = "movie";
    auto box = artworkBoxSize(m);
    CHECK(box.w == 64);
    CHECK(box.h == 96);
    std::printf("[test] B5c1: movie artwork box OK\n");
}

// B5c1: Show artwork box is 64×96
static void testShowArtworkBox()
{
    std::printf("[test] B5c1: show artwork box = 64x96\n");
    MediaItem s;
    s.type = "show";
    auto box = artworkBoxSize(s);
    CHECK(box.w == 64);
    CHECK(box.h == 96);
    std::printf("[test] B5c1: show artwork box OK\n");
}

// B5c1: Episode artwork box is 128×72
static void testEpisodeArtworkBox()
{
    std::printf("[test] B5c1: episode artwork box = 128x72\n");
    MediaItem e;
    e.type = "episode";
    auto box = artworkBoxSize(e);
    CHECK(box.w == 128);
    CHECK(box.h == 72);
    std::printf("[test] B5c1: episode artwork box OK\n");
}

// B5c1: Anything-else artwork box is 64×96
static void testOtherArtworkBox()
{
    std::printf("[test] B5c1: other type artwork box = 64x96\n");
    MediaItem x;
    x.type = "folder";
    auto box = artworkBoxSize(x);
    CHECK(box.w == 64);
    CHECK(box.h == 96);
    std::printf("[test] B5c1: other type artwork box OK\n");
}

// B5c1: Metadata X starts after artwork box + gap
static void testMetadataXFollowsBoxWidth()
{
    std::printf("[test] B5c1: metadata X follows box width\n");
    // ART_X = 8, gap = 10, so mx = 8 + boxW + 10
    MediaItem movie;
    movie.type = "movie";
    auto mbox = artworkBoxSize(movie);
    int mxMovie = 8 + mbox.w + 10;  // 82
    CHECK(mxMovie == 82);

    MediaItem episode;
    episode.type = "episode";
    auto ebox = artworkBoxSize(episode);
    int mxEpisode = 8 + ebox.w + 10;  // 146
    CHECK(mxEpisode == 146);

    // Episode metadata starts further right than movie metadata
    CHECK(mxEpisode > mxMovie);
    std::printf("[test] B5c1: metadata X follows box width OK\n");
}

// -------------------------------------------------------------------
// B5d1: Row card geometry and scrolling tests
// -------------------------------------------------------------------

// B5d1: Movie row card is 64×96
static void testMovieRowCard()
{
    std::printf("[test] B5d1: movie row card = 64x96\n");
    MediaItem m;
    m.type = "movie";
    auto box = artworkBoxSize(m);
    CHECK(box.w == 64);
    CHECK(box.h == 96);
    CHECK(rowStripHeight() == 96);
    std::printf("[test] B5d1: movie row card OK\n");
}

// B5d1: Show row card is 64×96
static void testShowRowCard()
{
    std::printf("[test] B5d1: show row card = 64x96\n");
    MediaItem s;
    s.type = "show";
    auto box = artworkBoxSize(s);
    CHECK(box.w == 64);
    CHECK(box.h == 96);
    std::printf("[test] B5d1: show row card OK\n");
}

// B5d1: Episode row card is 128×72
static void testEpisodeRowCard()
{
    std::printf("[test] B5d1: episode row card = 128x72\n");
    MediaItem e;
    e.type = "episode";
    auto box = artworkBoxSize(e);
    CHECK(box.w == 128);
    CHECK(box.h == 72);
    std::printf("[test] B5d1: episode row card OK\n");
}

// B5d1: Mixed-width card X positions
static void testMixedWidthPositions()
{
    std::printf("[test] B5d1: mixed-width X positions\n");
    MediaItem movie;  movie.type = "movie";   // w=64
    MediaItem episode; episode.type = "episode"; // w=128
    std::vector<MediaItem> row = {movie, episode, movie};

    // startX=4, gap=6
    CHECK(cardXPosition(row, 0) == 4);                // movie
    CHECK(cardXPosition(row, 1) == 4 + 64 + 6);      // 74, episode
    CHECK(cardXPosition(row, 2) == 4 + 64 + 6 + 128 + 6); // 208, movie

    // Total width: right edge of last card
    int totalW = cardXPosition(row, 3); // past-the-end X = startX + all widths + gaps
    CHECK(totalW == 4 + 64 + 6 + 128 + 6 + 64 + 6); // 278
    // totalRowWidth excludes trailing gap
    CHECK(totalRowWidth(row) == totalW - 6); // 272
    std::printf("[test] B5d1: mixed-width X positions OK\n");
}

// B5d1: Scrolling right keeps selected card visible
static void testScrollRightKeepsVisible()
{
    std::printf("[test] B5d1: scroll right keeps selected visible\n");
    MediaItem m; m.type = "movie"; // w=64
    std::vector<MediaItem> row(15, m); // 15 movies, each 64 wide

    // Card 14 starts at 4 + 14*(64+6) = 4 + 980 = 984
    // Right edge = 984 + 64 = 1048.  Viewport = 640.
    // Needed scroll = 1048 - 640 = 408
    int scroll = clampCardScroll(row, 14, 0, 640);
    CHECK(scroll == 408);
    // Verify the card is now visible: screenX = 984 - 408 = 576, right = 640
    CHECK(984 - scroll >= 0);
    CHECK(984 + 64 - scroll <= 640);
    std::printf("[test] B5d1: scroll right keeps selected visible OK\n");
}

// B5d1: Scrolling back left decreases pixel scroll
static void testScrollLeftDecreases()
{
    std::printf("[test] B5d1: scroll back left decreases scroll\n");
    MediaItem m; m.type = "movie"; // w=64
    std::vector<MediaItem> row(15, m);

    // Start with card 14 visible (scroll = 408)
    int scroll = clampCardScroll(row, 14, 0, 640);
    CHECK(scroll == 408);

    // Move to card 0: cardX(0) = 4. Need scroll = 4 so screenX = 0.
    scroll = clampCardScroll(row, 0, scroll, 640);
    CHECK(scroll == 4);
    CHECK(scroll < 408); // scroll decreased
    std::printf("[test] B5d1: scroll back left decreases OK\n");
}

// B5d1: Scroll never becomes negative
static void testScrollNeverNegative()
{
    std::printf("[test] B5d1: scroll never negative\n");
    MediaItem m; m.type = "movie";
    std::vector<MediaItem> row(5, m);

    // activeCard = 0, currentScroll = -50
    int scroll = clampCardScroll(row, 0, -50, 640);
    CHECK(scroll >= 0);

    // activeCard = 2, currentScroll = 0 (small row, fits on screen)
    scroll = clampCardScroll(row, 2, 0, 640);
    CHECK(scroll >= 0);
    CHECK(scroll == 0); // card 2 at 4+2*70=144, right=208, fits

    std::printf("[test] B5d1: scroll never negative OK\n");
}

// B5d1: No card-index / pixel-offset confusion
static void testScrollIsPixelNotIndex()
{
    std::printf("[test] B5d1: scroll is pixel offset, not card index\n");
    MediaItem m; m.type = "movie"; // w=64
    std::vector<MediaItem> row(20, m);

    // Card 19 at 4 + 19*70 = 1334, right edge = 1398
    // Needed scroll = 1398 - 640 = 758
    int scroll = clampCardScroll(row, 19, 0, 640);
    CHECK(scroll == 758);
    // 758 is a pixel value, far larger than any card index (0-19)
    CHECK(scroll > 19);

    // Verify: card 19 screenX = 1334 - 758 = 576, right = 640. Visible.
    CHECK(1334 - scroll >= 0);
    CHECK(1334 + 64 - scroll <= 640);

    std::printf("[test] B5d1: scroll is pixel offset OK\n");
}

// ===================================================================
// B5d2a tests — Row artwork loading state (no rendering)
// ===================================================================

#include <map>

// Helper: simulate visible scheduling (same RAM-cache filtering as Home).
static std::vector<std::string> pickCandidates(
    const std::vector<MediaItem> &items,
    const std::map<std::string, RowArtworkStatus> &statusMap)
{
    std::vector<std::string> candidates;
    for (const auto &item : items) {
        std::string key = buildRowArtworkKey(item);
        if (key.empty()) continue;
        if (statusMap.find(key) == statusMap.end())
            candidates.push_back(std::move(key));
    }
    return candidates;
}

// B5d2a: Movie row artwork uses Primary at 64x96
static void testMovieRowKeyPrimary()
{
    std::printf("[test] B5d2a: movie row key -> Primary 64x96\n");
    MediaItem m;
    m.id = "m1"; m.type = "movie";
    m.imageTags["Primary"] = "tagA";
    std::string key = buildRowArtworkKey(m);
    CHECK(!key.empty());
    CHECK(key.find("Primary") != std::string::npos);
    CHECK(key.find("Thumb") == std::string::npos);
    CHECK(key.find("64x96") != std::string::npos);
    CHECK(key == "m1:Primary:tagA:64x96");
    std::printf("[test] B5d2a: movie row key -> Primary 64x96 OK\n");
}

// B5d2a: Episode row artwork uses Primary at 128x72
static void testEpisodeRowKeyPrimary()
{
    std::printf("[test] B5d2a: episode row key -> Primary 128x72\n");
    MediaItem e;
    e.id = "e1"; e.type = "episode";
    e.imageTags["Primary"] = "tagB";
    std::string key = buildRowArtworkKey(e);
    CHECK(!key.empty());
    CHECK(key.find("Primary") != std::string::npos);
    CHECK(key.find("128x72") != std::string::npos);
    CHECK(key == "e1:Primary:tagB:128x72");
    std::printf("[test] B5d2a: episode row key -> Primary 128x72 OK\n");
}

// B5d2a: No Primary tag → empty key (skip that item)
static void testNoPrimaryTagEmptyKey()
{
    std::printf("[test] B5d2a: no Primary tag -> empty key\n");
    MediaItem m;
    m.id = "x1"; m.type = "movie";
    // no imageTags at all
    CHECK(buildRowArtworkKey(m).empty());
    std::printf("[test] B5d2a: no Primary tag -> empty key OK\n");
}

// B5d2a: Same key does not load twice (already in map → not a candidate)
static void testSameKeyNotLoadedTwice()
{
    std::printf("[test] B5d2a: same key not loaded twice\n");
    MediaItem m;
    m.id = "m2"; m.type = "movie";
    m.imageTags["Primary"] = "t1";
    std::string key = buildRowArtworkKey(m);

    std::map<std::string, RowArtworkStatus> sm;
    sm[key] = RowArtworkStatus::Loaded;

    std::vector<MediaItem> items = {m};
    CHECK(pickCandidates(items, sm).empty());

    sm[key] = RowArtworkStatus::Failed;
    CHECK(pickCandidates(items, sm).empty());

    std::printf("[test] B5d2a: same key not loaded twice OK\n");
}

// All visible RAM misses are scheduled in the same update.
static void testAllVisibleCandidatesScheduledPerCycle()
{
    std::printf("[test] B5d2a: all visible candidates per cycle\n");
    std::vector<MediaItem> items;
    for (int i = 0; i < 5; ++i) {
        MediaItem m;
        char id[32]; std::snprintf(id, sizeof(id), "item-%d", i);
        m.id = id; m.type = "movie";
        char tag[32]; std::snprintf(tag, sizeof(tag), "t%d", i);
        m.imageTags["Primary"] = tag;
        items.push_back(m);
    }
    std::map<std::string, RowArtworkStatus> sm;

    auto candidates = pickCandidates(items, sm);
    CHECK(candidates.size() == items.size());
    sm[candidates[1]] = RowArtworkStatus::Loaded;
    sm[candidates[3]] = RowArtworkStatus::Failed;
    candidates = pickCandidates(items, sm);
    CHECK(candidates.size() == items.size() - 2);
    CHECK(candidates[0] == "item-0:Primary:t0:64x96");
    CHECK(candidates[1] == "item-2:Primary:t2:64x96");
    CHECK(candidates[2] == "item-4:Primary:t4:64x96");

    std::printf("[test] B5d2a: all visible candidates per cycle OK\n");
}

// B5e1a: Season item with IndexNumber
static void testSeasonIndexNumber()
{
    std::printf("[test] B5e1a: Season IndexNumber parsing\n");
    std::string j = R"({"Id":"sn1","Name":"Season 1","Type":"Season","IndexNumber":1})";
    auto item = JellyfinApi::jsonToMediaItem(j);
    CHECK_EQ(item.id, "sn1");
    CHECK_EQ(item.title, "Season 1");
    CHECK(item.indexNumber == 1);
    std::printf("[test] B5e1a: Season IndexNumber parsing OK\n");
}

// B5e1a: Type "Season" normalised to "season"
static void testSeasonTypeNormalization()
{
    std::printf("[test] B5e1a: Season type normalization\n");
    std::string j = R"({"Id":"sn2","Name":"Season 2","Type":"Season","IndexNumber":2})";
    auto item = JellyfinApi::jsonToMediaItem(j);
    CHECK_EQ(item.type, "season");
    CHECK(item.indexNumber == 2);
    std::printf("[test] B5e1a: Season type normalization OK\n");
}

// B5e2a: Episode JSON parsing — all episode metadata fields
static void testEpisodeJsonParsing()
{
    std::printf("[test] B5e2a: Episode JSON parsing\n");
    std::string j = R"({"Id":"ep1","Name":"Pilot","Type":"Episode","
        R"("IndexNumber":3,"ParentIndexNumber":2,"RunTimeTicks":6600000000,""
        R"("SeriesName":"Example Show","SeriesId":"series123","SeasonId":"season456"})";
    auto item = JellyfinApi::jsonToMediaItem(j);
    CHECK_EQ(item.id, "ep1");
    CHECK_EQ(item.title, "Pilot");
    CHECK_EQ(item.type, "episode");
    CHECK(item.indexNumber == 3);
    CHECK(item.parentIndexNumber == 2);
    CHECK(item.runTimeTicks == 6600000000LL);
    CHECK_EQ(item.seriesName, "Example Show");
    CHECK_EQ(item.seriesId, "series123");
    CHECK_EQ(item.seasonId, "season456");
    std::printf("[test] B5e2a: Episode JSON parsing OK\n");
}

// B5e2a: Episode type "Episode" normalised to "episode"
static void testEpisodeTypeNormalization()
{
    std::printf("[test] B5e2a: Episode type normalization\n");
    std::string j = R"({"Id":"ep2","Name":"S2E1","Type":"Episode","IndexNumber":1,"ParentIndexNumber":2})";
    auto item = JellyfinApi::jsonToMediaItem(j);
    CHECK_EQ(item.type, "episode");
    CHECK(item.indexNumber == 1);
    CHECK(item.parentIndexNumber == 2);
    std::printf("[test] B5e2a: Episode type normalization OK\n");
}

// B5e2a: ticksToMinutes helper
static void testTicksToMinutes()
{
    std::printf("[test] B5e2a: ticksToMinutes\n");
    CHECK(ticksToMinutes(0) == 0);
    CHECK(ticksToMinutes(-100) == 0);

    // 6600000000 ticks = 660 seconds = 11 minutes exactly
    CHECK(ticksToMinutes(6600000000LL) == 11);

    // 3300000000 ticks = 330 seconds = 5.5 min -> rounds to 6
    CHECK(ticksToMinutes(3300000000LL) == 6);

    // 2700000000 ticks = 270 seconds = 4.5 min -> rounds to 5
    CHECK(ticksToMinutes(2700000000LL) == 5);

    // 600000000 ticks = 60 seconds = 1 min
    CHECK(ticksToMinutes(600000000LL) == 1);

    // 300000000 ticks = 30 seconds = 0.5 min -> rounds to 1
    CHECK(ticksToMinutes(300000000LL) == 1);

    std::printf("[test] B5e2a: ticksToMinutes OK\n");
}

// B5e2a: Episode defaults (fields absent in JSON)
static void testEpisodeDefaults()
{
    std::printf("[test] B5e2a: Episode field defaults\n");
    std::string j = R"({"Id":"ep3","Name":"Intro","Type":"Episode"})";
    auto item = JellyfinApi::jsonToMediaItem(j);
    CHECK_EQ(item.type, "episode");
    CHECK(item.indexNumber == 0);
    CHECK(item.parentIndexNumber == 0);
    CHECK(item.runTimeTicks == 0LL);
    CHECK(item.seriesName.empty());
    CHECK(item.seriesId.empty());
    CHECK(item.seasonId.empty());
    std::printf("[test] B5e2a: Episode field defaults OK\n");
}

// B5e3b: findEpisodeIndex — target found at correct index
static void testFindEpisodeIndexFound()
{
    std::printf("[test] B5e3b: findEpisodeIndex found\n");
    std::vector<MediaItem> eps;
    MediaItem a; a.id = "A"; a.indexNumber = 1; eps.push_back(a);
    MediaItem b; b.id = "B"; b.indexNumber = 2; eps.push_back(b);
    MediaItem c; c.id = "C"; c.indexNumber = 3; eps.push_back(c);

    CHECK(EpisodeBrowserScreen::findEpisodeIndex(eps, "B") == 1);
    CHECK(EpisodeBrowserScreen::findEpisodeIndex(eps, "A") == 0);
    CHECK(EpisodeBrowserScreen::findEpisodeIndex(eps, "C") == 2);
    std::printf("[test] B5e3b: findEpisodeIndex found OK\n");
}

// B5e3b: findEpisodeIndex — unknown target returns -1
static void testFindEpisodeIndexNotFound()
{
    std::printf("[test] B5e3b: findEpisodeIndex not found\n");
    std::vector<MediaItem> eps;
    MediaItem a; a.id = "A"; eps.push_back(a);
    MediaItem b; b.id = "B"; eps.push_back(b);
    MediaItem c; c.id = "C"; eps.push_back(c);

    CHECK(EpisodeBrowserScreen::findEpisodeIndex(eps, "Z") == -1);
    CHECK(EpisodeBrowserScreen::findEpisodeIndex(eps, "D") == -1);
    std::printf("[test] B5e3b: findEpisodeIndex not found OK\n");
}

// B5e3b: findEpisodeIndex — empty target returns -1
static void testFindEpisodeIndexEmpty()
{
    std::printf("[test] B5e3b: findEpisodeIndex empty target\n");
    std::vector<MediaItem> eps;
    MediaItem a; a.id = "A"; eps.push_back(a);
    MediaItem b; b.id = "B"; eps.push_back(b);

    CHECK(EpisodeBrowserScreen::findEpisodeIndex(eps, "") == -1);
    std::printf("[test] B5e3b: findEpisodeIndex empty target OK\n");
}

// B5e3b: findEpisodeIndex — empty list returns -1
static void testFindEpisodeIndexEmptyList()
{
    std::printf("[test] B5e3b: findEpisodeIndex empty list\n");
    std::vector<MediaItem> eps;

    CHECK(EpisodeBrowserScreen::findEpisodeIndex(eps, "A") == -1);
    CHECK(EpisodeBrowserScreen::findEpisodeIndex(eps, "") == -1);
    std::printf("[test] B5e3b: findEpisodeIndex empty list OK\n");
}

// B5g1b: visible episode-artwork scheduling
static void testEpisodePrefetchScheduler()
{
    std::printf("[test] B5g1b: visible prefetch scheduler\n");
    std::set<int> unavailable;

    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(5, 0, 20, unavailable) == 5);
    unavailable.insert(5);
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(5, 0, 20, unavailable) == 6);
    unavailable.insert(6);
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(5, 0, 20, unavailable) == 4);

    unavailable.clear();
    unavailable.insert(5);
    unavailable.insert(6);
    unavailable.insert(4);
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(5, 0, 30, unavailable) == 7);

    unavailable.clear();
    unavailable.insert(12);
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(12, 10, 40, unavailable) == 13);
    unavailable.insert(13);
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(12, 10, 40, unavailable) == 11);

    unavailable.clear();
    for (int i = 10; i < 32; ++i) unavailable.insert(i);
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(12, 10, 40, unavailable) == -1);

    unavailable.clear();
    unavailable.insert(500);
    for (int i = 489; i < 511; ++i) unavailable.insert(i);
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(500, 489, 1000, unavailable) == -1);

    // A distant selected episode is not pulled into the old viewport.
    unavailable.clear();
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(900, 889, 1000, unavailable) == 900);
    std::printf("[test] B5g1b: visible prefetch scheduler OK\n");
}

static void testEpisodeArtworkPreemption()
{
    std::printf("[test] episode artwork request preemption\n");

    // A selection change invalidates an in-flight prefetch request.
    CHECK(EpisodeBrowserScreen::artworkRequestCancelled(false, 10, 11));
    // libcurl cancellation is also an intentional cancellation, never a
    // cache-key failure.
    CHECK(EpisodeBrowserScreen::artworkRequestCancelled(true, 10, 10));
    CHECK(!EpisodeBrowserScreen::shouldMarkArtworkFailed(false, true));
    CHECK(EpisodeBrowserScreen::shouldMarkArtworkFailed(false, false));

    // Once the stale request is dropped, recomputing from the latest
    // selection puts its cached/selected thumbnail ahead of any prefetch.
    std::set<int> unavailable;
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(14, 4, 30, unavailable) == 14);
    std::printf("[test] episode artwork request preemption OK\n");
}

static void testEpisodePrefetchPlaybackResume()
{
    std::printf("[test] B5g1b: playback resume countdown\n");
    bool pending = true;
    int delayUpdates = 1;

    CHECK(!EpisodeBrowserScreen::advancePrefetchResume(
        pending, delayUpdates));
    CHECK(pending);
    CHECK(delayUpdates == 0);

    CHECK(EpisodeBrowserScreen::advancePrefetchResume(
        pending, delayUpdates));
    CHECK(!pending);
    CHECK(delayUpdates == 0);

    CHECK(!EpisodeBrowserScreen::advancePrefetchResume(
        pending, delayUpdates));
    std::printf("[test] B5g1b: playback resume countdown OK\n");
}

// B5f2: PlaybackRequest — valid movie writes expected fields

