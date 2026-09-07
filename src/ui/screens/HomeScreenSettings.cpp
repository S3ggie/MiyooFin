#include "HomeScreen.hpp"
#include "../BitmapFont.hpp"
#include "../Theme.hpp"
#include "../../download/DownloadSupport.hpp"
#include "miyoofin/version.hpp"
#include <ctime>

namespace miyoofin {

static constexpr int SETTINGS_VISIBLE_ROWS = 6;
static std::int64_t wallClockMs(){return (std::int64_t)std::time(nullptr)*1000;}
static std::string compactSyncAge(std::int64_t timestamp)
{
    if (timestamp <= 0) return "Never";
    const std::int64_t elapsed=std::max<std::int64_t>(0,wallClockMs()-timestamp);
    if (elapsed < 60LL*1000) return "Now";
    if (elapsed < 60LL*60*1000) return std::to_string(elapsed/(60LL*1000))+"m ago";
    if (elapsed < 24LL*60*60*1000) return std::to_string(elapsed/(60LL*60*1000))+"h ago";
    return std::to_string(elapsed/(24LL*60*60*1000))+"d ago";
}

HomeScreen::SettingsRowAction HomeScreen::settingsRowAction(int row)
{ return homeSettingsRowAction(row); }

std::vector<HomeScreen::SettingsAddressRow> HomeScreen::settingsAddressRows(const Session &session)
{ return homeSettingsAddressRows(session); }

int HomeScreen::settingsRowCount(const Session &session)
{ return homeSettingsRowCount(session); }

HomeScreen::SettingsRowAction HomeScreen::settingsRowAction(int row, const Session &session)
{ return homeSettingsRowAction(row, session); }

void HomeScreen::drawSettingsTab(SDL_Surface *fb)
{
    BitmapFont::fillRect(fb, 0, 25, 640, 437, 24, 24, 32, 255);
    struct SettingRow { std::string section; std::string value; };
    std::vector<SettingRow> rows={{"Offline Mode", m_session.manualOfflineMode ? "ON" : "OFF"}};
    for (const SettingsAddressRow &row:settingsAddressRows(m_session)) rows.push_back({row.section,row.value});
    rows.insert(rows.end(), {
        {"Last API Route", lastApiRouteValue()},
        {"Account", m_userName.empty() ? "Unknown" : m_userName},
        {"LIBRARY", "Last Sync: " + compactSyncAge(m_syncState.lastSuccessfulMs)},
        {"DOWNLOADS", "Local " + formatBytes(m_downloadSnapshot.localBytes) + " | Free " + formatBytes(m_downloadSnapshot.freeBytes)},
        {"DIAGNOSTICS", "UI Stall Logger Enabled"},
        {"ABOUT", std::string(APP_NAME) + " " + VERSION_STR},
        {"ACCOUNT", "Log Out"}
    });
    static constexpr int ROW_H=68, TOP=34;
    for (int visible=0; visible<SETTINGS_VISIBLE_ROWS; ++visible) {
        const int index=m_settingsScroll+visible;
        if (index >= (int)rows.size()) break;
        const int y=TOP+visible*ROW_H;
        const bool selected=index==m_settingsSelected;
        if (selected)
            BitmapFont::fillRect(fb,8,y-2,624,ROW_H-4,Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,70);
        BitmapFont::drawString(fb,16,y,rows[index].section.c_str(),Theme::ACCENT_R,Theme::ACCENT_G,
            Theme::ACCENT_B,24,24,32);
        std::string value=rows[index].value;
        static constexpr size_t MAX_CHARS=72;
        if (value.size()>MAX_CHARS) value=value.substr(0,MAX_CHARS-3)+"...";
        BitmapFont::drawString(fb,16,y+20,value.c_str(),Theme::TEXT_R,Theme::TEXT_G,
            Theme::TEXT_B,24,24,32);
        BitmapFont::fillRect(fb,16,y+ROW_H-8,608,1,48,48,58,255);
    }
}

} // namespace miyoofin
