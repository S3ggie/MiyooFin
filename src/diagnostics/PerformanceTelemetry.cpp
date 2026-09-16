#include "PerformanceTelemetry.hpp"
#include "PerformanceTelemetryInternal.hpp"

#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <thread>
#include <unistd.h>
#include <cstring>

#include "TelemetryClock.hpp"
#include "miyoofin/version.hpp"

namespace miyoofin {
// Static schema contracts retained across telemetry translation-unit splits: read_media_page_dequeued, read_media_page_ready.

#if defined(MIYOOFIN_TELEMETRY_HOST_TEST)
void PerformanceTelemetry::setTestHooks(const TestHooks &hooks) noexcept
{
    telemetry_internal::g_testHooks = hooks;
}

void PerformanceTelemetry::clearTestHooks() noexcept
{
    telemetry_internal::g_testHooks = TestHooks{};
}

void PerformanceTelemetry::setSchemaVersionForTest(uint16_t version) noexcept
{
    telemetry_internal::g_testSchemaVersion = version;
}
#endif

ScreenId PerformanceTelemetry::screenIdFromDiagnosticName(const char *name) noexcept
{
    if (name == nullptr)
        return ScreenId::Other;
    if (std::strcmp(name, "StartupScreen") == 0)
        return ScreenId::Startup;
    if (std::strcmp(name, "ServerEntryScreen") == 0)
        return ScreenId::ServerEntry;
    if (std::strcmp(name, "ConnectScreen") == 0)
        return ScreenId::Connect;
    if (std::strcmp(name, "LoginScreen") == 0)
        return ScreenId::Login;
    if (std::strcmp(name, "AuthCheckScreen") == 0)
        return ScreenId::AuthCheck;
    if (std::strcmp(name, "HomeScreen") == 0)
        return ScreenId::Home;
    if (std::strcmp(name, "SeriesScreen") == 0)
        return ScreenId::Series;
    if (std::strcmp(name, "EpisodeBrowserScreen") == 0)
        return ScreenId::EpisodeBrowser;
    if (std::strcmp(name, "MovieDetailsScreen") == 0)
        return ScreenId::MovieDetails;
    if (std::strcmp(name, "InputDiagnosticsScreen") == 0)
        return ScreenId::InputDiagnostics;
    return ScreenId::Other;
}

TabId PerformanceTelemetry::tabIdFromDiagnosticName(const char *name) noexcept
{
    if (name == nullptr || std::strcmp(name, "other") == 0)
        return TabId::Other;
    if (std::strcmp(name, "n/a") == 0)
        return TabId::NotApplicable;
    if (std::strcmp(name, "Home") == 0)
        return TabId::Home;
    if (std::strcmp(name, "Movies") == 0)
        return TabId::Movies;
    if (std::strcmp(name, "Shows") == 0)
        return TabId::Shows;
    if (std::strcmp(name, "Downloads") == 0)
        return TabId::Downloads;
    if (std::strcmp(name, "Settings") == 0)
        return TabId::Settings;
    return TabId::Other;
}

ActionId PerformanceTelemetry::actionIdFromAction(Action action) noexcept
{
    switch (action) {
        case Action::None: return ActionId::None;
        case Action::Up: return ActionId::Up;
        case Action::Down: return ActionId::Down;
        case Action::Left: return ActionId::Left;
        case Action::Right: return ActionId::Right;
        case Action::Confirm: return ActionId::Confirm;
        case Action::Back: return ActionId::Back;
        case Action::Search: return ActionId::Search;
        case Action::ActionsMenu: return ActionId::ActionsMenu;
        case Action::PrevTab: return ActionId::PrevTab;
        case Action::NextTab: return ActionId::NextTab;
        case Action::PrevPage: return ActionId::PrevPage;
        case Action::NextPage: return ActionId::NextPage;
        case Action::Settings: return ActionId::Settings;
        case Action::Menu: return ActionId::Menu;
        case Action::Exit: return ActionId::Exit;
        case Action::Raw: return ActionId::Raw;
    }
    return ActionId::Other;
}

PerformanceTelemetry &performanceTelemetry() noexcept
{
    static PerformanceTelemetry instance;
    return instance;
}

} // namespace miyoofin

#endif
