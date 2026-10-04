#ifndef MIYOOFIN_TEST_UI_MODELS_SUPPORT_HPP
#define MIYOOFIN_TEST_UI_MODELS_SUPPORT_HPP

// Shared fixtures for the test_ui_models* groups. Helpers are inline so a group that does not use
// one does not warn.

#include "../test_support.hpp"
#include "../../src/ui/ConnectionMonitor.hpp"
#include "../../src/ui/HomeTabs.hpp"
#include "../../src/ui/HomeSyncState.hpp"
#include "../../src/ui/HomeSettingsModel.hpp"
#include "../../src/ui/HomeArtworkPlan.hpp"

// Pure Home/UI model coverage: sync scheduling, settings rows, tab
// projection, and artwork planning. These models drive screen behavior but
// are independent of SDL rendering, so they are tested directly here.

static inline MediaItem makeHomeItem(const std::string& id, const std::string& title,
                                     const std::string& type = "")
{
    MediaItem item;
    item.id = id;
    item.title = title;
    item.type = type;
    return item;
}

// --- HomeDownloadsState: Downloads-tab selection, expansion and confirmation.

static inline DownloadItem homeDlMovie(const std::string& id)
{
    DownloadItem item;
    item.itemId = id;
    item.itemType = "movie";
    item.title = "Movie " + id;
    item.state = DownloadState::Complete;
    return item;
}

static inline DownloadItem homeDlEpisode(const std::string& id, int number)
{
    DownloadItem item;
    item.itemId = id;
    item.itemType = "episode";
    item.title = "Episode " + id;
    item.seriesId = "a";
    item.seriesName = "Alpha Show";
    item.seasonId = "a1";
    item.seasonName = "Season 1";
    item.seasonNumber = 1;
    item.episodeNumber = number;
    item.state = DownloadState::Complete;
    return item;
}

static inline DownloadSnapshot homeDlSnapshot()
{
    DownloadSnapshot snapshot;
    snapshot.items = {homeDlMovie("m1"), homeDlEpisode("e1", 1), homeDlEpisode("e2", 2)};
    return snapshot;
}

// --- HomeArtworkCache: bounded LRU of decoded row artwork + card surfaces.

static inline DecodedImage homeArtworkImage(int w, int h)
{
    DecodedImage image;
    image.width = w;
    image.height = h;
    image.pixels.assign(static_cast<size_t>(w) * h * 4, 0x7F);
    return image;
}

// Writes an executable fake axp_test and returns its path.
static inline std::string writeFakeAxp(const std::string& dir, const std::string& body)
{
    const std::string path = dir + "/axp_test";
    std::FILE* f = std::fopen(path.c_str(), "w");
    CHECK(f != nullptr);
    if (f) {
        std::fputs("#!/bin/sh\n", f);
        std::fputs(body.c_str(), f);
        std::fclose(f);
        ::chmod(path.c_str(), 0755);
    }
    return path;
}

// Drives update() until `done` holds or ~3s pass (the probe runs on a worker).
template <typename Done>
static bool pumpBattery(BatteryMonitor& monitor, unsigned stepMs, Done done)
{
    for (int i = 0; i < 300 && !done(); ++i) {
        monitor.update(stepMs);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

// --- ConnectionMonitor: polling link status behind "<user> connected" -------

// Drives update() with small real-time sleeps until `done` holds (~3s max).
template <typename Done>
static bool pumpLink(ConnectionMonitor& monitor, unsigned stepMs, Done done)
{
    for (int i = 0; i < 300 && !done(); ++i) {
        monitor.update(stepMs);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    return done();
}

#endif
