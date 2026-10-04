#include "MusicScreen.hpp"
#include "MusicScreenInternal.hpp"
#include "../../music/MusicApi.hpp"
#include "../../music/MusicPaths.hpp"
#include "../../music/MusicParse.hpp"
#include "../UiKit.hpp"
#include "TextEntryScreen.hpp"
#include "../ClockSettings.hpp"
#include "../../app/ScreenStack.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace miyoofin {

using namespace music_screen_detail;

void MusicScreen::refreshSettingsRows(MusicPane& pane)
{
    auto action = [](const std::string& title, const std::string& subtitle,
                     const std::string& right) {
        MusicRow r;
        r.kind = MusicRow::Kind::Action;
        r.id = title;
        r.title = title;
        r.subtitle = subtitle;
        r.right = right;
        return r;
    };
    const int stream = m_settings ? m_settings->streamKbps.load() : 192;
    const int download = m_settings ? m_settings->downloadKbps.load() : 192;
    pane.rows = {
        action("Enter MiyooFin", "Press A to enter MiyooFin", ""),
        action("Streaming quality", "Used when playing from the server",
               std::to_string(stream) + " kbps"),
        action("Download quality", "Used for offline music", std::to_string(download) + " kbps"),
        action("Album view", "Albums as a list or a grid of covers",
               m_settings && m_settings->albumGrid.load() ? "Grid" : "List"),
        action("Clock format", "12 or 24 hour clock in the header",
               ClockSettings::instance().hour24() ? "24-hour" : "12-hour"),
        action("Time zone", "Where the header clock gets its time zone",
               ClockSettings::instance().zoneSummary()),
        action("Clear music cache", "Streamed tracks and covers (downloads stay)",
               m_cacheClearArmed ? "Press A again" : ""),
        action("Server",
               m_session.routes().lan.empty() ? m_session.routes().pub : m_session.routes().lan,
               ""),
        action("Account",
               m_session.userName + (m_session.manualOfflineMode ? " (offline mode)" : ""), "")};
    pane.requested = true;
    pane.loadedOnce = true;
    clampPane(pane);
}

void MusicScreen::settingsAction(int index)
{
    if (index == 0) {
        m_videoModeRequested = true;
    } else if (index == 1 && m_settings) {
        m_settings->streamKbps.store(music::MusicSettings::nextKbps(m_settings->streamKbps.load()));
        m_settings->save(kSettingsFile);
    } else if (index == 2 && m_settings) {
        m_settings->downloadKbps.store(
            music::MusicSettings::nextKbps(m_settings->downloadKbps.load()));
        m_settings->save(kSettingsFile);
    } else if (index == 3 && m_settings) {
        m_settings->albumGrid.store(!m_settings->albumGrid.load());
        m_settings->save(kSettingsFile);
        for (MusicPane& p : m_tabs[static_cast<int>(MusicTab::Library)].roots)
            clampPane(p);
        markDirty();
    } else if (index == 4) {
        ClockSettings& clock = ClockSettings::instance();
        clock.setHour24(!clock.hour24());
        clock.save("clock-settings.txt");
    } else if (index == 5) {
        m_zonePick = true;
        std::vector<std::string> items = {"Automatic (from network)", "Device setting"};
        for (const ClockZone& z : clockZones())
            items.push_back(z.label);
        m_picker.open("Time zone", items);
    } else if (index == 6) {
        if (!m_cacheClearArmed) {
            m_cacheClearArmed = true;
            m_toast = "Press A again to clear the music cache";
            m_toastUntil = m_clock + 3000;
            return;
        }
        m_cacheClearArmed = false;
        // Remove cached tracks, covers and lists (downloads are untouched).
        // On a worker thread; the track playing and the one queued next are left alone.
        std::set<std::string> keep;
        if (m_player)
            for (const std::string& path : m_player->inUsePaths())
                keep.insert(path);
        m_library->clearCaches(std::move(keep));
        m_toast = "Music cache cleared";
        m_toastUntil = m_clock + 2000;
    }
}

} // namespace miyoofin
