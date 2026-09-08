#include "EpisodeBrowserScreen.hpp"
#include "../../cache/OfflineLibraryProjection.hpp"
#include "../BitmapFont.hpp"
#include "../../app/ScreenStack.hpp"
#include "../../app/UiDiagnostics.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../net/RouteRequest.hpp"
#include "../../cache/OfflineCatalog.hpp"
#include "../../cache/LibraryCache.hpp"
#include "../../diagnostics/PerformanceTelemetry.hpp"
#include "../../diagnostics/TelemetryGuards.hpp"
#include <cstdio>

namespace miyoofin {

// -------------------------------------------------------------------
// Layout constants — 640x480 framebuffer, two-panel design
// -------------------------------------------------------------------
static constexpr int FB_W           = 640;
static constexpr int FB_H           = 480;
static constexpr int BOTTOM_H       = 18;

// Left panel — episode text list
static constexpr int LEFT_X         = 16;
static constexpr int HEAD_Y         = 16;
static constexpr int LIST_Y         = 46;
static constexpr int LIST_W         = 285;
static constexpr int LIST_ROW_H     = 18;

// Right panel — placeholder thumbnail + metadata + buttons
static constexpr int THUMB_X        = 326;
static constexpr int THUMB_Y        = 38;
static constexpr int THUMB_W        = 288;
static constexpr int THUMB_H        = 162;
static constexpr int META_X         = 326;
static constexpr int META_Y         = THUMB_Y + THUMB_H + 6;
static constexpr int META_WRAP      = 34;

// Action buttons
static constexpr int BTN_W          = 78;
static constexpr int BTN_H          = 20;
static constexpr int BTN_Y          = FB_H - BOTTOM_H - BTN_H - 6;
static constexpr int BTN_PLAY_X     = 326;
static constexpr int BTN_EP_X       = 410;
static constexpr int BTN_SEASON_X   = 494;

// Yellow double-border focus colours
static constexpr Uint8 FOCUS_OR = 255, FOCUS_OG = 220, FOCUS_OB = 40;
static constexpr Uint8 FOCUS_IR = 255, FOCUS_IG = 255, FOCUS_IB = 120;
// -------------------------------------------------------------------
// Word-wrap helper (same algorithm as SeriesScreen)
// -------------------------------------------------------------------
static std::vector<std::string> wrapText(const char *text, int wrapCols)
{
    std::vector<std::string> lines;
    if (!text || !*text) return lines;

    std::string input(text);
    size_t pos = 0;

    while (pos < input.size()) {
        size_t newline = input.find('\n', pos);
        std::string para = (newline != std::string::npos)
            ? input.substr(pos, newline - pos)
            : input.substr(pos);

        while (!para.empty()) {
            if ((int)para.size() <= wrapCols) {
                lines.push_back(para);
                break;
            }
            size_t lastSpace = para.rfind(' ', wrapCols);
            if (lastSpace != std::string::npos && lastSpace > 0) {
                lines.push_back(para.substr(0, lastSpace));
                para = para.substr(lastSpace + 1);
            } else {
                lines.push_back(para.substr(0, wrapCols));
                para = para.substr(wrapCols);
            }
        }

        if (newline != std::string::npos)
            pos = newline + 1;
        else
            break;
    }

    return lines;
}

// Constructor
// -------------------------------------------------------------------
EpisodeBrowserScreen::EpisodeBrowserScreen(const Session &session,
                                           const MediaItem &series,
                                           const MediaItem &season,
                                           const std::string &initialEpisodeId, std::shared_ptr<DownloadManager> downloads, bool networkOffline, bool downloadedOnly)
    : m_session(session)
    , m_series(series)
    , m_season(season)
    , m_initialEpisodeId(initialEpisodeId)
    , m_downloads(std::move(downloads))
    , m_networkOffline(networkOffline)
    , m_downloadedOnly(downloadedOnly)
{
}

// -------------------------------------------------------------------
// Destructor — signal worker to stop, wake it, join (B5g1a)
// -------------------------------------------------------------------
EpisodeBrowserScreen::~EpisodeBrowserScreen()
{
    leave();
    if(m_fetchThread.joinable()) m_fetchThread.join();
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_workerStop = true;
    }
    m_workerCv.notify_one();
    if (m_workerThread.joinable())
        m_workerThread.join();
    freePreparedArtwork(m_episodeArtworkSurface);
}

// -------------------------------------------------------------------
// findEpisodeIndex — static helper for locating an episode by ID
// -------------------------------------------------------------------
int EpisodeBrowserScreen::findEpisodeIndex(
    const std::vector<MediaItem> &episodes,
    const std::string &episodeId)
{
    if (episodeId.empty()) return -1;
    for (int i = 0; i < (int)episodes.size(); ++i) {
        if (episodes[i].id == episodeId)
            return i;
    }
    return -1;
}

int EpisodeBrowserScreen::nextPrefetchIndex(
    int selected, int listScroll, int total, const std::set<int> &unavailable)
{
    if (selected < 0 || selected >= total) return -1;
    if (!unavailable.count(selected)) return selected;
    const int first = std::max(0, listScroll);
    const int last = std::min(total, first + LIST_VISIBLE);
    for (int distance = 1; distance < LIST_VISIBLE; ++distance) {
        const int ahead = selected + distance;
        if (ahead < last && !unavailable.count(ahead)) return ahead;
        const int behind = selected - distance;
        if (behind >= first && !unavailable.count(behind)) return behind;
    }
    return -1;
}

bool EpisodeBrowserScreen::artworkRequestCancelled(
    bool cancellationRequested, std::uint64_t requestGeneration,
    std::uint64_t currentGeneration)
{
    return cancellationRequested || requestGeneration != currentGeneration;
}

bool EpisodeBrowserScreen::shouldMarkArtworkFailed(bool success,
                                                    bool cancelled)
{
    return !success && !cancelled;
}

bool EpisodeBrowserScreen::advancePrefetchResume(
    bool &pending, int &delayUpdates)
{
    if (!pending) return false;
    if (delayUpdates > 0) {
        --delayUpdates;
        return false;
    }
    pending = false;
    return true;
}

// -------------------------------------------------------------------
// enter / leave
// -------------------------------------------------------------------
void EpisodeBrowserScreen::enter()
{
    UiDiagnostics::Scope scope("EpisodeBrowserScreen::enter");
    printf("[EpisodeBrowserScreen] enter series=%s season=%s\n",
           m_series.title.c_str(), m_season.title.c_str());
    if (m_episodes.empty() && m_loadState != LoadState::Error) {
        // ScreenStack::push invokes enter() on the SDL thread.  Catalog
        // parsing, download projection and persistence all belong to the
        // existing fetch worker; the loading screen is immediately usable.
        m_loadState = LoadState::Loading;
        fetchEpisodes(true);
    }
    else if (m_loadState == LoadState::Ready) {
        m_prefetchResumePending = false;
        m_prefetchResumeDelayUpdates = 0;
        {
            std::lock_guard<std::mutex> lock(m_workerMutex);
            m_workerPaused = false;
        }
        wakeArtworkWorker();
    }
}

void EpisodeBrowserScreen::leave()
{
    if(m_shutdownSignalled.exchange(true,std::memory_order_acq_rel))return;
    UiDiagnostics::Scope scope("EpisodeBrowserScreen::workerShutdown");
    printf("[EpisodeBrowserScreen] leave\n");
    m_fetchCancelled.store(true, std::memory_order_release);
    m_workerCancelled.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_workerStop = true;
    }
    m_workerCv.notify_one();
}

// -------------------------------------------------------------------
// fetchEpisodes — synchronous fetch via JellyfinApi
// -------------------------------------------------------------------
void EpisodeBrowserScreen::fetchEpisodes(bool loadCachedEpisodes)
{
    if(m_fetchThread.joinable()){std::lock_guard<std::mutex>g(m_fetchMutex);if(!m_fetchDone)return; m_fetchThread.join();} if(m_episodes.empty())m_loadState = LoadState::Loading;
    m_error.clear();
    {std::lock_guard<std::mutex>g(m_fetchMutex);m_fetchDone=false;m_cachedEpisodesDone=false;}
    m_fetchCancelled.store(false, std::memory_order_release);
    const Session s=m_session; const MediaItem seriesItem=m_series, seasonItem=m_season;
    const std::string sid=m_series.id, season=m_season.id;
    const bool networkOffline=m_networkOffline, downloadedOnly=m_downloadedOnly, loadCached=loadCachedEpisodes;
    const std::string catalogPath=OfflineCatalog::cachePath("cache",LibraryCache::scopeKey(s.serverUrl,s.userId));
    std::shared_ptr<DownloadManager> downloads=m_downloads;
    m_fetchThread=std::thread([this,s,seriesItem,seasonItem,sid,season,networkOffline,downloadedOnly,loadCached,catalogPath,downloads](){
        PerformanceTelemetry &telemetry=performanceTelemetry();
        telemetry.setWorkerActive(WorkerId::EpisodeFetch, true);
        telemetry.setWorkerQueueDepth(WorkerId::EpisodeFetch, 1);
        TelemetryTimer fetchTimer;
        bool telemetryCompleted = false;
        auto completeTelemetry = [&](Outcome outcome) noexcept {
            if (telemetryCompleted)
                return;
            telemetryCompleted = true;
            if (fetchTimer.active())
                (void)fetchTimer.elapsedUs();
            if (outcome == Outcome::Success)
                telemetry.addWorkerCompleted(WorkerId::EpisodeFetch);
            else if (outcome == Outcome::Cancelled)
                telemetry.addWorkerCancelled(WorkerId::EpisodeFetch);
            else
                telemetry.addWorkerFailed(WorkerId::EpisodeFetch);
            telemetry.setWorkerActive(WorkerId::EpisodeFetch, false);
            telemetry.setWorkerQueueDepth(WorkerId::EpisodeFetch, 0);
        };
        std::vector<MediaItem> cached;
        if(loadCached) {
            OfflineCatalogSnapshot catalog;
            OfflineCatalog::load(catalogPath,catalog,nullptr);
            if(m_fetchCancelled.load(std::memory_order_acquire)) {
                completeTelemetry(Outcome::Cancelled);
                return;
            }
            if(downloadedOnly) {
                LibrarySnapshot library;
                OfflineLibraryProjection projection(library,catalog,downloads?downloads->snapshot():DownloadSnapshot{});
                cached=projection.episodes(season);
            } else {
                auto it=catalog.episodesBySeason.find(season);
                if(it!=catalog.episodesBySeason.end()) cached=it->second;
            }
            {
                std::lock_guard<std::mutex>g(m_fetchMutex);
                m_cachedEpisodes=cached;
                m_cachedEpisodesDone=true;
            }
            if(networkOffline) {
                {
                    std::lock_guard<std::mutex>g(m_fetchMutex);
                    m_fetchOk=true;m_fetchEpisodes=std::move(cached);m_fetchDone=true;
                }
                completeTelemetry(Outcome::Success);
                return;
            }
        }
        if(m_fetchCancelled.load(std::memory_order_acquire)) {
            completeTelemetry(Outcome::Cancelled);
            return;
        }
        std::vector<MediaItem>v;std::string e;
        bool ok=RouteRequest(s).run([&](const std::string &base){return JellyfinApi::getEpisodes(base,s.accessToken,s.userId,s.deviceId,sid,season,v,e,&m_fetchCancelled);},e);
        // storeEpisodes reads, merges, serializes, fsyncs and renames the full
        // catalog.  Complete it here before publishing a cheap UI result.
        if(ok&&!m_fetchCancelled.load(std::memory_order_acquire)) {
            OfflineCatalog::storeEpisodes(catalogPath,seriesItem,seasonItem,v,nullptr);
            if (downloadedOnly) { OfflineCatalogSnapshot catalog; OfflineCatalog::load(catalogPath,catalog,nullptr); LibrarySnapshot library; OfflineLibraryProjection projection(library,catalog,downloads?downloads->snapshot():DownloadSnapshot{}); v=projection.episodes(season); }
        }
        {
            std::lock_guard<std::mutex>g(m_fetchMutex);
            m_fetchOk=ok;m_fetchEpisodes=std::move(v);m_fetchError=e;m_fetchDone=true;
        }
        completeTelemetry(m_fetchCancelled.load(std::memory_order_acquire)
            ? Outcome::Cancelled : (ok ? Outcome::Success : Outcome::Failure));
    });
}

void EpisodeBrowserScreen::publishEpisodes(std::vector<MediaItem> episodes)
{
    UiDiagnostics::Scope scope("EpisodeBrowserScreen::publishEpisodes");
    std::string selected=m_episodes.empty()?"":m_episodes[m_selectedEpisode].id;
    m_episodes=std::move(episodes);
    int index=findEpisodeIndex(m_episodes,selected);
    if(!m_initialSelectionApplied && !m_initialEpisodeId.empty()) {
        const int initial=findEpisodeIndex(m_episodes,m_initialEpisodeId);
        if(initial>=0) index=initial;
        m_initialSelectionApplied=true;
    }
    m_selectedEpisode=index>=0?index:0;
    m_loadState=LoadState::Ready;m_listScroll=0;m_overviewScroll=0;
    m_episodeArtwork={};m_episodeArtworkKey.clear();
    freePreparedArtwork(m_episodeArtworkSurface);
    std::vector<ArtworkJob> jobs;jobs.reserve(m_episodes.size());
    for(const auto&ep:m_episodes){ArtworkJob j;j.itemId=ep.id;auto t=ep.imageTags.find("Primary");if(t!=ep.imageTags.end()&&!t->second.empty()){j.imageTag=t->second;j.artworkKey=ep.id+":Primary:"+t->second+":288x162";}jobs.push_back(std::move(j));}
    {std::lock_guard<std::mutex>g(m_workerMutex);m_artworkJobs=std::move(jobs);m_failedKeys.clear();m_workerPaused=false;m_workerDecodedKey.clear();}
    clampListScroll();wakeArtworkWorker();
}

// -------------------------------------------------------------------
// clampListScroll
// -------------------------------------------------------------------
void EpisodeBrowserScreen::clampListScroll()
{
    int total = (int)m_episodes.size();
    if (total == 0) { m_listScroll = 0; return; }
    if (m_selectedEpisode < m_listScroll)
        m_listScroll = m_selectedEpisode;
    else if (m_selectedEpisode >= m_listScroll + LIST_VISIBLE)
        m_listScroll = m_selectedEpisode - LIST_VISIBLE + 1;
    if (m_listScroll < 0) m_listScroll = 0;
    int maxScroll = total - LIST_VISIBLE;
    if (maxScroll < 0) maxScroll = 0;
    if (m_listScroll > maxScroll) m_listScroll = maxScroll;
}

// -------------------------------------------------------------------
// handleAction
// -------------------------------------------------------------------
bool EpisodeBrowserScreen::handleAction(Action action)
{
    if (m_loadState == LoadState::Loading) {
        if (action == Action::Back) { m_stack->pop(); return true; }
        return false;
    }
    if (m_loadState == LoadState::Error) {
        switch (action) {
        case Action::Back:  m_stack->pop(); return true;
        case Action::Confirm: fetchEpisodes(); return true;
        default: return false;
        }
    }

    int total = (int)m_episodes.size();

    // Shoulder buttons for bio scrolling — works regardless of focus
    if (action == Action::PrevTab) {
        m_overviewScroll -= 3;
        if (m_overviewScroll < 0) m_overviewScroll = 0;
        return true;
    }
    if (action == Action::NextTab) {
        if (total > 0 && m_selectedEpisode >= 0
            && m_selectedEpisode < total)
        {
            const auto &ep = m_episodes[m_selectedEpisode];
            auto lines = wrapText(ep.overview.c_str(), META_WRAP);
            int overviewY = META_Y + BitmapFont::GLYPH_H + 2;
            bool hasMeta = (ep.parentIndexNumber > 0 && ep.indexNumber > 0)
                        || ep.runTimeTicks > 0 || ep.rating > 0.0f;
            if (hasMeta) overviewY += BitmapFont::GLYPH_H + 2;
            int vis = (BTN_Y - 4 - overviewY) / BitmapFont::GLYPH_H;
            if (vis < 1) vis = 1;
            int maxScroll = (int)lines.size() - vis;
            if (maxScroll < 0) maxScroll = 0;
            m_overviewScroll += 3;
            if (m_overviewScroll > maxScroll) m_overviewScroll = maxScroll;
        }
        return true;
    }

    if (action == Action::Back) { if(m_confirmDownload){m_confirmDownload=false;return true;} m_stack->pop(); return true; }
    if (m_confirmDownload) {
        handleDownloadConfirmation(action);
        return true;
    }
    // Y plans the whole displayed season.  Episodes have already been fetched,
    // so only the manager-owned planner performs network work from here.
    if (action == Action::ActionsMenu && m_downloads && !m_episodes.empty()) {
        requestSeasonDownloadPlan();
        return true;
    }

    // ----- EpisodeList focus -----
    if (m_focus == FocusArea::EpisodeList) {
        switch (action) {
        case Action::Up:
            if (m_selectedEpisode > 0) {
                m_selectedEpisode--;
                clampListScroll();
                m_overviewScroll = 0;
                clearSelectedEpisodeArtwork();
                wakeArtworkWorker();
            }
            return true;
        case Action::Down:
            if (m_selectedEpisode + 1 < total) {
                m_selectedEpisode++;
                clampListScroll();
                m_overviewScroll = 0;
                clearSelectedEpisodeArtwork();
                wakeArtworkWorker();
            }
            return true;
        case Action::Right:
            m_focus = FocusArea::ActionButtons;
            m_actionBtn = ActionButton::Play;
            return true;
        case Action::Confirm:
            m_focus = FocusArea::ActionButtons;
            m_actionBtn = ActionButton::Play;
            return true;
        default: return false;
        }
    }

    // ----- ActionButtons focus -----
    if (m_focus == FocusArea::ActionButtons) {
        switch (action) {
        case Action::Left:
            if (m_actionBtn == ActionButton::DownloadSeason)
                m_actionBtn = ActionButton::DownloadEpisode;
            else if (m_actionBtn == ActionButton::DownloadEpisode)
                m_actionBtn = ActionButton::Play;
            else
                m_focus = FocusArea::EpisodeList;
            return true;
        case Action::Right:
            if (m_actionBtn == ActionButton::Play)
                m_actionBtn = ActionButton::DownloadEpisode;
            else if (m_actionBtn == ActionButton::DownloadEpisode)
                m_actionBtn = ActionButton::DownloadSeason;
            return true;
        case Action::Confirm:
            if (m_actionBtn == ActionButton::Play) {
                if (m_selectedEpisode >= 0 && m_selectedEpisode < total) {
                    startSelectedEpisodePlayback();
                }
            } else {
                handleDownloadButtonAction();
            }
            return true;
        case Action::Up:
        case Action::Down:
            return true;
        default: return false;
        }
    }
    return false;
}

// -------------------------------------------------------------------
// update — load selected-episode artwork when state is Ready
// -------------------------------------------------------------------
void EpisodeBrowserScreen::update(Uint32 /*dt*/)
{
    std::vector<MediaItem> cached; bool cachedDone=false;
    {std::lock_guard<std::mutex>g(m_fetchMutex);if(m_cachedEpisodesDone){cached=std::move(m_cachedEpisodes);m_cachedEpisodesDone=false;cachedDone=true;}}
    if(cachedDone && (m_networkOffline || !cached.empty())) {
        UiDiagnostics::Scope scope("EpisodeBrowserScreen::publishCachedEpisodes");
        publishEpisodes(std::move(cached));
    }
    bool fetchDone; { std::lock_guard<std::mutex> g(m_fetchMutex); fetchDone=m_fetchDone; }
    if(fetchDone){UiDiagnostics::Scope scope("EpisodeBrowserScreen::publishFetchResult");std::vector<MediaItem>fresh;std::string err;bool ok;{std::lock_guard<std::mutex>g(m_fetchMutex);ok=m_fetchOk;fresh=std::move(m_fetchEpisodes);err=m_fetchError;m_fetchDone=false;}if(m_fetchThread.joinable())m_fetchThread.join();if(ok)publishEpisodes(std::move(fresh));else if(m_episodes.empty()){m_loadState=LoadState::Error;m_error=err;}}
    updatePlaybackState();

    if (m_loadState == LoadState::Ready)
        tryLoadSelectedEpisodeArtwork();
}

// -------------------------------------------------------------------
// tryLoadSelectedEpisodeArtwork — non-blocking (B5g1a)
// Main thread NEVER performs HTTP.  Selection changes are immediate
// for metadata.  Artwork uses cache-first + background worker.
// -------------------------------------------------------------------

} // namespace miyoofin
